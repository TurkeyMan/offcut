#include "prompts.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include "transform.h"
#ifdef _WIN32
#include <windows.h>
#endif
namespace offcut
{
std::string_view ai_task_name(ai_task t)
{
    switch (t)
    {
        case ai_task::reimagine:
            return "Reimagine";
        case ai_task::refine:
            return "Refine";
        case ai_task::try_again:
            return "Try again";
        case ai_task::fix_support:
            return "Fix with AI";
        case ai_task::describe_scene:
            return "Describe a scene";
        case ai_task::describe_element:
            return "Describe an element";
    }
    return "AI";
}
std::string_view ai_task_file(ai_task t)
{
    switch (t)
    {
        case ai_task::reimagine:
            return "reimagine.txt";
        case ai_task::refine:
        case ai_task::try_again:
            return "refine.txt";
        case ai_task::fix_support:
            return "fix-support.txt";
        case ai_task::describe_scene:
            return "describe-scene.txt";
        case ai_task::describe_element:
            return "describe-element.txt";
    }
    return "reimagine.txt";
}
const std::vector<std::string>& prompt_file_names()
{
    static const std::vector<std::string> names{"brief.txt",          "reimagine.txt",        "refine.txt",           "fix-support.txt",
                                                "describe-scene.txt", "describe-element.txt", "preferences.txt",      "output.txt",
                                                "review-chips.txt",   "director-plan.txt",    "director-critique.txt"};
    return names;
}
const std::string& prompt_library::text(const std::string& name) const
{
    static const std::string none;
    const auto it = files.find(name);
    return it == files.end() ? none : it->second;
}
std::string prompt_library::version(const std::string& name) const
{
    // FNV-1a: stable across builds and platforms, enough to tell edits apart.
    uint32_t h = 2166136261u;
    for (unsigned char c : text(name))
        h = (h ^ c) * 16777619u;
    return std::format("{}@{:08x}", name, h);
}
std::string strip_prompt_comments(std::string_view in)
{
    std::string out;
    size_t at = 0;
    while (at <= in.size())
    {
        auto end = in.find('\n', at);
        if (end == std::string_view::npos)
            end = in.size();
        auto line = in.substr(at, end - at);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (!line.starts_with('#'))
        {
            out += line;
            out += '\n';
        }
        at = end + 1;
    }
    // Blank lines at either end are layout, not text.
    const auto first = out.find_first_not_of(" \t\n");
    if (first == std::string::npos)
        return {};
    out.erase(0, out.rfind('\n', first) == std::string::npos ? 0 : out.rfind('\n', first) + 1);
    out.erase(out.find_last_not_of(" \t\n") + 1);
    return out;
}
prompt_library load_prompt_library(const std::filesystem::path& folder)
{
    prompt_library lib;
    lib.folder = folder;
    for (const auto& name : prompt_file_names())
    {
        std::string text;
        bool read = false;
        if (!folder.empty())
            if (std::ifstream in(folder / name, std::ios::binary); in)
            {
                std::ostringstream s;
                s << in.rdbuf();
                if (in.good() || in.eof())
                {
                    text = s.str();
                    if (text.starts_with("\xEF\xBB\xBF"))  // a BOM from Notepad
                        text.erase(0, 3);
                    read = true;
                }
            }
        if (!read)
        {
            text = builtin_prompt_text(name);
            if (!folder.empty())
                lib.fallbacks.push_back(name);
        }
        lib.files[name] = strip_prompt_comments(text);
    }
    return lib;
}
std::filesystem::path find_prompt_folder()
{
    std::error_code ec;
    if (const char* env = std::getenv("OFFCUT_PROMPTS_DIR"); env && *env)
        return from_utf8(env);
#ifdef OFFCUT_SOURCE_DIR
    if (const auto source = from_utf8(OFFCUT_SOURCE_DIR) / "data" / "prompts"; std::filesystem::is_directory(source, ec))
        return source;
#endif
#ifdef _WIN32
    wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (n > 0 && n < std::size(buffer))
        if (const auto beside = std::filesystem::path(buffer).parent_path() / "data" / "prompts"; std::filesystem::is_directory(beside, ec))
            return beside;
#endif
    return {};
}
const prompt_library& prompts()
{
    static const prompt_library lib = load_prompt_library(find_prompt_folder());
    return lib;
}
std::vector<std::string> default_planes(int sheets)
{
    // Template route: a sensible stacking for the sheet count, near to far.
    // The frame and foreground in front, the subject in the middle depths,
    // distant scenery behind, plain sky or backing last.
    const std::string front = "the frame border, and the nearest foreground: grass or foliage along the bottom, reaching in from the sides";
    const std::string sky = "plain sky or background, one flat colour, nothing in it";
    if (sheets <= 2)
        return {front + ", with the subject standing in it as one silhouette", sky};
    if (sheets == 3)
        return {front, "the subject, standing on the ground", sky};
    std::vector<std::string> planes{front};
    if (sheets >= 6)
    {
        planes.push_back("the subject's nearest parts (front legs, a raised paw, the face)");
        planes.push_back("the rest of the subject, standing on the ground");
    }
    else
        planes.push_back("the subject, standing on the ground");
    static const char* scenery[] = {"a low hill or bank behind the subject, running off both sides", "distant hills or trees", "far hills, paler",
                                    "the farthest hills, palest", "high clouds meeting the top border"};
    for (int i = 0; int(planes.size()) < sheets - 1; ++i)
        planes.push_back(scenery[std::min(i, 4)]);
    planes.push_back(sky);
    return planes;
}
std::string list_planes(const std::vector<std::string>& planes, const std::vector<std::array<uint8_t, 3>>& palette)
{
    std::string out;
    for (size_t i = 0; i < planes.size(); ++i)
    {
        std::string label = i == 0 ? "front" : i + 1 == planes.size() ? "back" : "";
        if (i < palette.size())
            label += std::format("{}colour #{:02x}{:02x}{:02x}", label.empty() ? "" : ", ", palette[i][0], palette[i][1], palette[i][2]);
        out += std::format("{}- {}{}: {}", i ? "\n" : "", i + 1, label.empty() ? std::string() : " (" + label + ")", planes[i]);
    }
    return out;
}
std::vector<std::array<uint8_t, 3>> default_palette(int sheets)
{
    using rgb = std::array<uint8_t, 3>;
    const rgb front{0x1e, 0x4d, 0x2b}, sky{0xcf, 0xe6, 0xf2};
    // Middle planes: far apart in hue, and in lightness from their neighbours.
    static const rgb middle[] = {{0xc8, 0x74, 0x2a}, {0x8f, 0xb0, 0x4a}, {0x6b, 0x4f, 0x8a}, {0xe0, 0xc0, 0x5a}, {0x4f, 0x8f, 0xb0}, {0xb0, 0x4a, 0x5a}};
    std::vector<rgb> out{front};
    for (size_t i = 0; int(out.size()) < std::max(sheets, 2) - 1; ++i)
        out.push_back(middle[i % std::size(middle)]);
    out.push_back(sky);
    return out;
}
std::string palette_text(const std::vector<std::array<uint8_t, 3>>& palette)
{
    std::string out;
    for (const auto& c : palette)
        out += std::format("{}#{:02x}{:02x}{:02x}", out.empty() ? "" : " ", c[0], c[1], c[2]);
    return out;
}
std::vector<std::array<uint8_t, 3>> parse_palette(std::string_view text)
{
    std::vector<std::array<uint8_t, 3>> out;
    const auto hex = [](char ch) {
        return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    };
    for (size_t at = 0; (at = text.find('#', at)) != std::string_view::npos; at += 7)
    {
        if (at + 7 > text.size())
            return {};
        std::array<uint8_t, 3> c{};
        for (size_t k = 0; k < 3; ++k)
        {
            const int hi = hex(text[at + 1 + 2 * k]), lo = hex(text[at + 2 + 2 * k]);
            if (hi < 0 || lo < 0)
                return {};
            c[k] = uint8_t(hi * 16 + lo);
        }
        out.push_back(c);
    }
    return out;
}
namespace
{
bool blank(std::string_view s)
{
    return s.find_first_not_of(" \t\r") == std::string_view::npos;
}
std::string mm(double v)
{
    return std::format("{:g} mm", std::round(v * 10) / 10);
}
}  // namespace
std::string fill_prompt(std::string_view text, const std::map<std::string, std::string>& values)
{
    std::string out;
    size_t at = 0;
    while (at < text.size())
    {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos)
            end = text.size();
        const auto line = text.substr(at, end - at);
        at = end + 1;
        std::string filled;
        bool keep = true, had_placeholder = false;
        for (size_t i = 0; i < line.size();)
        {
            const auto close = line[i] == '{' ? line.find('}', i) : std::string_view::npos;
            if (close == std::string_view::npos)
            {
                filled += line[i++];
                continue;
            }
            auto name = line.substr(i + 1, close - i - 1);
            const bool optional = name.ends_with('?');
            if (optional)
                name.remove_suffix(1);
            const auto it = values.find(std::string(name));
            if (it == values.end())
            {
                // Not ours: kept as written, so a typo shows in the prompt.
                filled += line.substr(i, close - i + 1);
                i = close + 1;
                continue;
            }
            had_placeholder = true;
            if (blank(it->second) && !optional)
                keep = false;
            filled += it->second;
            i = close + 1;
        }
        if (keep && !(had_placeholder && blank(filled)))
        {
            out += filled;
            out += '\n';
        }
    }
    while (out.ends_with('\n'))
        out.pop_back();
    return out;
}
std::map<std::string, std::string> prompt_values(const transform_options& o, const prompt_words& w)
{
    const auto b = borders(o);
    std::map<std::string, std::string> v;
    v["sheets"] = std::to_string(o.layers);
    v["frame"] = std::format("{:g} × {:g} mm", std::round(o.width_mm * 10) / 10, std::round(o.height_mm * 10) / 10);
    v["opening"] = std::format("{:g} × {:g} mm", std::round((o.width_mm - b.left - b.right) * 10) / 10, std::round((o.height_mm - b.top - b.bottom) * 10) / 10);
    v["borders"] = b.top == b.bottom && b.top == b.left && b.top == b.right
                       ? mm(b.top) + " wide on every side"
                       : std::format("top {}, bottom {}, left {}, right {}", mm(b.top), mm(b.bottom), mm(b.left), mm(b.right));
    v["gap"] = mm(o.layer_gap_mm);
    v["min_width"] = mm(o.bridge_mm);
    // The image is the whole frame, cover-cropped: pixels per mm along the
    // tighter side.
    // Unknown size: what an OpenAI-style edit is asked for (account_image_endpoint).
    const double width_px = w.width > 0 ? w.width : o.width_mm > o.height_mm * 1.05 ? 1536 : 1024;
    const double height_px = w.height > 0 ? w.height : o.height_mm > o.width_mm * 1.05 ? 1536 : 1024;
    const double px_per_mm = std::max(width_px / o.width_mm, height_px / o.height_mm);
    v["min_width_px"] = std::to_string(std::max(1, int(std::lround(o.bridge_mm * px_per_mm))));
    v["size"] = w.width > 0 && w.height > 0 ? std::format("{} × {} pixels", w.width, w.height)
                                            : std::format("in the frame's proportions ({:g} : {:g})", o.width_mm, o.height_mm);
    if (b.top == b.bottom && b.top == b.left && b.top == b.right)
        v["borders_px"] = std::format(", about {} pixels wide", std::max(1, int(std::lround(b.top * px_per_mm))));
    else
        v["borders_px"] = std::format(", about {} pixels at the top, {} at the bottom, {} left and {} right", std::lround(b.top * px_per_mm),
                                      std::lround(b.bottom * px_per_mm), std::lround(b.left * px_per_mm), std::lround(b.right * px_per_mm));

    const auto planes = w.planes.empty() ? default_planes(o.layers) : w.planes;
    const auto palette = w.palette.empty() ? default_palette(int(planes.size())) : w.palette;
    v["planes"] = list_planes(planes, palette);
    v["palette"] = palette_text(palette);
    v["style"] = w.style;
    v["notes"] = w.notes;
    v["instructions"] = w.instructions;
    v["done"] = w.done;
    v["issues"] = w.issues;
    v["placement"] = w.placement;
    v["references"] = w.references;
    v["description"] = w.description;
    v["area"] = w.area;
    v["background"] = w.background;
    return v;
}
assembled_prompt assemble_prompt(const prompt_library& lib, const transform_options& o, const prompt_words& w, const ai_plan* plan)
{
    auto values = prompt_values(o, w);
    std::vector<std::string> parts, used;
    const auto trimmed = [](std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    };
    if (plan)
    {
        // Compiled from the plan: the shadowbox seen straight on, its depth
        // planes near to far, its directions, what to avoid, its palette.
        std::string directions = std::format(
            "Draw the flat front design of a layered paper shadowbox, seen straight on: {} flat paper layers, each depth one flat colour, nearer layers "
            "overlapping further ones. The attached picture is a subject reference only: do not copy its line work, outlines, shading or style.",
            plan->planes.empty() ? o.layers : int(plan->planes.size()));
        if (!plan->planes.empty())
            directions += "\nLayers, near to far:\n" + list_planes(plan->planes);
        for (const auto& d : plan->scene_directions)
            if (!trimmed(d).empty())
                directions += (directions.empty() ? "" : "\n") + trimmed(d);
        std::string avoid;
        for (const auto& a : plan->avoid)
            if (!trimmed(a).empty())
                avoid += (avoid.empty() ? "" : "; ") + trimmed(a);
        if (!avoid.empty())
            directions += (directions.empty() ? "Avoid: " : "\nAvoid: ") + avoid + ".";
        if (!plan->palette.empty())
        {
            std::string colours;
            for (const auto& c : plan->palette)
                colours += std::format("{}#{:02x}{:02x}{:02x}", colours.empty() ? "" : ", ", c[0], c[1], c[2]);
            directions += (directions.empty() ? "" : "\n") + std::format("Use exactly these {} colours, front to back: {}.", plan->palette.size(), colours);
            values["sheets"] = std::to_string(plan->palette.size());
            values["palette"] = palette_text(plan->palette);
        }
        parts.push_back(directions);
    }
    else
    {
        const std::string task_file(ai_task_file(w.task));
        parts.push_back(fill_prompt(lib.text(task_file), values));
        used.push_back(task_file);
        const auto style = trimmed(w.style), notes = trimmed(w.notes), instructions = trimmed(w.instructions);
        if (!style.empty() || !notes.empty() || !instructions.empty())
        {
            std::string user = fill_prompt(lib.text("preferences.txt"), values);
            used.push_back("preferences.txt");
            const auto add = [&](const char* heading, const std::string& text)
            {
                if (text.empty())
                    return;
                if (!user.empty())
                    user += '\n';
                user += heading;
                user += text.find('\n') == std::string::npos ? " " : "\n";
                user += text;
            };
            add("Style:", style);
            add("About this subject:", notes);
            add("Instructions for this request:", instructions);
            parts.push_back(user);
        }
    }
    parts.push_back(fill_prompt(lib.text("output.txt"), values));
    used.push_back("output.txt");
    assembled_prompt out;
    out.palette = values["palette"];
    for (const auto& part : parts)
        if (!blank(part))
        {
            if (!out.text.empty())
                out.text += "\n\n";
            out.text += part;
        }
    out.text += '\n';
    for (const auto& name : used)
    {
        if (!out.files.empty())
            out.files += ' ';
        out.files += lib.version(name);
    }
    if (plan)
        out.files += " plan";
    return out;
}
std::vector<review_chip> parse_review_chips(std::string_view text)
{
    std::vector<review_chip> chips;
    const auto trim = [](std::string_view s)
    {
        const auto a = s.find_first_not_of(" \t\r");
        if (a == std::string_view::npos)
            return std::string();
        return std::string(s.substr(a, s.find_last_not_of(" \t\r") - a + 1));
    };
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line))
    {
        if (line.starts_with('#') || blank(line))
            continue;
        std::vector<std::string> fields;
        size_t at = 0;
        for (int i = 0; i < 3; ++i)
        {
            const auto bar = line.find('|', at);
            if (bar == std::string::npos)
                break;
            fields.push_back(trim(std::string_view(line).substr(at, bar - at)));
            at = bar + 1;
        }
        if (fields.size() != 3)
            continue;
        review_chip c{fields[0], fields[1], fields[2], trim(std::string_view(line).substr(at))};
        if (!c.id.empty() && !c.menu.empty() && !c.label.empty() && !c.text.empty())
            chips.push_back(std::move(c));
    }
    return chips;
}
std::string chip_instruction(const std::vector<review_chip>& chips, const std::vector<chip_choice>& chosen, const transform_options& o)
{
    auto values = prompt_values(o, {});
    std::string out;
    for (const auto& c : chosen)
    {
        const auto it = std::ranges::find(chips, c.id, &review_chip::id);
        if (it == chips.end())
            continue;
        values["element"] = c.element.empty() ? std::string("something in the scene") : c.element;
        const auto text = fill_prompt(it->text, values);
        if (text.empty())
            continue;
        if (!out.empty())
            out += ' ';
        out += text;
    }
    return out;
}
std::string chip_review(const std::vector<review_chip>& chips, const std::vector<chip_choice>& chosen)
{
    std::string out;
    for (const auto& c : chosen)
    {
        const auto it = std::ranges::find(chips, c.id, &review_chip::id);
        std::string label = it == chips.end() ? c.id : it->menu + ' ' + it->label;
        if (label.ends_with("…"))
        {
            label.resize(label.size() - std::string_view("…").size());
            if (!c.element.empty())
                label += ' ' + c.element;
        }
        std::ranges::transform(label, label.begin(), [](char ch) { return ch >= 'A' && ch <= 'Z' ? char(ch - 'A' + 'a') : ch; });
        if (!out.empty())
            out += "; ";
        out += label;
    }
    return out;
}
std::string chip_ids(const std::vector<chip_choice>& chosen)
{
    std::string out;
    for (const auto& c : chosen)
    {
        if (!out.empty())
            out += ' ';
        out += c.id;
        if (!c.element.empty())
        {
            auto element = c.element;
            std::ranges::replace(element, ' ', '_');
            out += '=' + element;
        }
    }
    return out;
}
}  // namespace offcut
