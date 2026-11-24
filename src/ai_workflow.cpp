#include "ai_workflow.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "director_helpers.h"
#include "export.h"
#include "versions.h"
namespace offcut
{
namespace
{
std::string now_iso()
{
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}
std::string new_run_id()
{
    static std::atomic_uint serial{0};
    return std::format("{:x}-{}", std::chrono::system_clock::now().time_since_epoch().count() / 10000, serial++);
}
const layer& item_of(const project& p, uint32_t id)
{
    const int i = p.find_layer(id);
    if (i < 0)
        throw std::runtime_error("That item is no longer in the project.");
    return p.layers[size_t(i)];
}
const raster& version_pixels(const layer& l, uint32_t version)
{
    if (!version)
        return l.image;
    const auto* v = find_version(l, version);
    if (!v)
        throw std::runtime_error("That version is no longer in the item's thread.");
    return v->image;
}
// What goes over the wire: at most 1024 px on the long side (image models
// work at about that size, and providers charge for input pixels).
std::vector<uint8_t> upload_png(const raster& r)
{
    constexpr int limit = 1024;
    if (std::max(r.width, r.height) <= limit)
        return encode_png(r);
    const double k = double(limit) / std::max(r.width, r.height);
    return encode_png(fit_raster(r, std::max(1, int(std::lround(r.width * k))), std::max(1, int(std::lround(r.height * k)))));
}
std::string joined(const std::vector<std::string>& parts)
{
    std::string out;
    for (const auto& s : parts)
        out += (out.empty() ? "" : "; ") + s;
    return out;
}
}  // namespace
prepared_ai_upload prepare_ai_upload(const project& p)
{
    if (std::ranges::none_of(p.layers, [](const auto& s) { return s.visible && s.image.valid(); }))
        throw std::runtime_error("Add at least one visible source image before generating with AI.");
    prepared_ai_upload out;
    out.task = ai_task::reimagine;
    out.options = p.options;
    out.run = new_run_id();
    out.frame_width_mm = p.options.width_mm;
    out.frame_height_mm = p.options.height_mm;
    out.words = reimagine_words(p);
    out.composition = flatten(p.layers, p.options);
    out.width = out.composition.width;
    out.height = out.composition.height;
    out.images.push_back({"composition.png", upload_png(out.composition)});
    for (size_t i = 0; i < p.layers.size(); ++i)
    {
        const auto& source = p.layers[i];
        if (source.visible)
            out.images.push_back({std::format("reference-{:02}.png", i + 1), upload_png(cropped(current_pixels(source), effective_transform(source, p.options).crop))});
    }
    assemble(out);
    return out;
}
prepared_ai_upload prepare_refinement(const project& p, uint32_t item, uint32_t version, ai_task task, const std::string& instructions)
{
    const auto& l = item_of(p, item);
    prepared_ai_upload out;
    out.task = task;
    out.options = p.options;
    out.item = item;
    out.frame_width_mm = p.options.width_mm;
    out.frame_height_mm = p.options.height_mm;
    out.words.task = task;
    out.words.style = p.ai_style;
    out.words.notes = l.notes;
    out.words.instructions = instructions;
    const auto add_original = [&]
    {
        if (l.image.valid())
            out.images.push_back({"original.png", upload_png(l.image)});
    };
    if (task == ai_task::try_again)
    {
        // Same plan or prompt, a new drawing from the same input, as a sibling.
        const auto* v = version ? find_version(l, version) : nullptr;
        if (!v)
            throw std::runtime_error("Try again needs an AI result.");
        out.parent = v->parent;
        out.resend = v->prompt;
        out.prompt_files = v->prompt_files;
        out.palette = v->palette;
        out.run = v->run;
        out.round = v->round + 1;
        out.words.style = v->style;
        out.words.notes = v->notes;
        out.words.instructions = v->instructions;
        out.plan = v->plan;
        const auto& input = version_pixels(l, v->parent);
        out.images.push_back({v->parent ? std::format("version-{}.png", v->parent) : std::string("composition.png"), upload_png(input)});
        if (v->parent)
            add_original();
        out.width = input.width;
        out.height = input.height;
    }
    else
    {
        // Stateless refinement: the version itself, the original as a second
        // reference, earlier rounds summarised, and what validation found.
        out.parent = version;
        const auto* v = version ? find_version(l, version) : nullptr;
        out.run = v ? v->run : new_run_id();
        out.round = v ? v->round + 1 : 1;
        const auto& input = version_pixels(l, version);
        out.images.push_back({version ? std::format("version-{}.png", version) : std::string("original.png"), upload_png(input)});
        if (version)
            add_original();
        out.width = input.width;
        out.height = input.height;
        out.words.done = joined(thread_requests(l, version));
        if (v)
            out.words.issues = v->findings;
    }
    assemble(out);
    return out;
}
void assemble(prepared_ai_upload& u)
{
    if (!u.resend.empty())
    {
        u.prompt = u.resend;
        return;
    }
    if (!u.plan.empty())
    {
        const auto plan = validate_director_plan(u.plan, 32);
        const auto a = assemble_prompt(prompts(), u.options, u.words, &plan);
        std::string change;
        if (u.task != ai_task::reimagine && u.parent && u.words.instructions.find_first_not_of(" \t\r\n") != std::string::npos)
        {
            change = "Change the first attached image, the drawing made from these directions, and keep the rest of it as it is: " + u.words.instructions;
            if (!u.refine_marks.empty())
                change += "\nWhere to look: " + u.refine_marks + ".";
            change += "\n\n";
        }
        u.prompt = change + a.text;
        u.prompt_files = a.files;
        u.palette = a.palette;
        return;
    }
    const auto a = assemble_prompt(prompts(), u.options, u.words);
    u.prompt = a.text;
    u.prompt_files = a.files;
    u.palette = a.palette;
}
void set_ai_instructions(prepared_ai_upload& u, const std::string& text)
{
    u.words.instructions = text;
    assemble(u);
}
void choose_ai_account(prepared_ai_upload& u, const ai_account& a)
{
    u.endpoint = account_image_endpoint(a, u.frame_width_mm, u.frame_height_mm);
    u.account_id = a.id;
    u.account_name = a.name;
    if (u.task != ai_task::reimagine && supports_input_fidelity(u.endpoint))
        u.endpoint.input_fidelity = "high";
    if (const auto x = u.endpoint.size.find('x'); x != std::string::npos)
    {
        u.words.width = std::atoi(u.endpoint.size.c_str());
        u.words.height = std::atoi(u.endpoint.size.c_str() + x + 1);
    }
    assemble(u);
}
validation rebuild_sheets(project& p)
{
    auto built = make_sheets(p.layers, p.options);
    p.cuts = std::move(built.layers);
    p.selected_cut = std::clamp(p.selected_cut, 0, std::max(0, int(p.cuts.size()) - 1));
    p.stale = false;
    p.dirty = true;
    auto v = validate_sheets(p.cuts, p.options, built.removed);
    v.tones_found = built.tones_found;
    v.tones_wanted = built.tones_found ? p.options.layers : 0;
    return v;
}
namespace
{
// Keeps the usage figures Offcut can read from the provider's JSON.
void read_usage(item_version::role_usage& u, const std::string& json)
{
    const auto number = [&](std::string_view key) -> uint64_t
    {
        const auto at = json.find("\"" + std::string(key) + "\"");
        if (at == std::string::npos)
            return 0;
        const auto colon = json.find(':', at);
        return colon == std::string::npos ? 0 : std::strtoull(json.c_str() + colon + 1, nullptr, 10);
    };
    u.input_tokens = number("input_tokens");
    u.output_tokens = number("output_tokens");
}
}  // namespace
int apply_ai_result(project& p, const prepared_ai_upload& u, const ai_result& r)
{
    int index = -1;
    if (u.task == ai_task::reimagine && !u.item)
    {
        // A new item over its sources, which are hidden, not deleted.
        layer item;
        int n = 1;
        for (const auto& l : p.layers)
            n += l.type == layer_type::ai_illustration;
        item.name = n == 1 ? "AI illustration" : std::format("AI illustration {}", n);
        item.type = layer_type::ai_illustration;
        item.image = u.composition;
        item.notes = u.words.notes;
        for (auto it = p.layers.rbegin(); it != p.layers.rend(); ++it)
            if (it->visible && !item.derived_from)
                item.derived_from = it->id;
        for (auto& l : p.layers)
            l.visible = false;
        item.transform.mm_per_pixel = 1 / working_scale(p.options);
        item.transform.x_mm = item.image.width * item.transform.mm_per_pixel / 2;
        item.transform.y_mm = item.image.height * item.transform.mm_per_pixel / 2;
        item.transform.crop = full_crop(item.image);
        p.layers.push_back(std::move(item));
        p.assign_layer_ids();
        index = int(p.layers.size()) - 1;
    }
    else
    {
        index = p.find_layer(u.item);
        if (index < 0)
            throw std::runtime_error("The item this result belongs to was removed.");
    }
    auto& l = p.layers[size_t(index)];
    item_version v;
    v.parent = u.parent;
    v.image = r.image;
    v.prompt = u.prompt;
    v.task = std::string(ai_task_name(u.task));
    v.instructions = u.words.instructions;
    v.style = u.words.style;
    v.notes = u.words.notes;
    v.prompt_files = u.prompt_files;
    v.palette = u.palette;
    v.account = u.account_id;
    v.provider = u.account_name;
    v.model = u.endpoint.model;
    v.created = now_iso();
    v.seconds = r.seconds;
    v.usage = r.usage;
    v.request = utf8(r.folder);
    v.run = u.run;
    v.round = u.round;
    v.plan = u.plan;
    v.illustrator.seconds = r.seconds;
    read_usage(v.illustrator, r.usage);
    const auto id = add_version(l, std::move(v));
    const auto found = rebuild_sheets(p);
    auto* added = find_version(p.layers[size_t(index)], id);
    added->issues = found.count();
    added->findings = describe_findings(found, p.options);
    added->score = found.score(p.options);
    added->measurements = measurements_json(found, p.options);
    p.selected_layer = index;
    p.step = work_step::colours;
    p.view = view_mode::assembled;
    p.activity = std::format("AI result {}: {}.", id, found.count() ? std::format("{} {} to review", found.count(), found.count() == 1 ? "issue" : "issues")
                                                                       : std::string("no issues found"));
    return index;
}
}  // namespace offcut
