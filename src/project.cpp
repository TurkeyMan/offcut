#include "project.h"
#include <algorithm>
#include <bit>
#include "transform.h"
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace offcut
{
bool raster::valid() const
{
    return width > 0 && height > 0 && width <= 2048 && height <= 2048 && rgba.size() == size_t(width) * height * 4;
}
std::string_view layer_type_name(layer_type t)
{
    switch (t)
    {
        case layer_type::image:
            return "Image";
        case layer_type::colour_regions:
            return "Colour regions";
        case layer_type::mask:
            return "Mask";
        case layer_type::text:
            return "Text";
        case layer_type::sketch:
            return "Sketch";
        case layer_type::scenery:
            return "Scenery";
        case layer_type::ai_illustration:
            return "AI illustration";
    }
    return "Layer";
}
void project::assign_layer_ids()
{
    uint32_t next = 1;
    for (const auto& l : layers)
        next = std::max(next, l.id + 1);
    for (auto& l : layers)
        if (!l.id)
            l.id = next++;
}
int project::find_layer(uint32_t id) const
{
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == id)
            return int(i);
    return -1;
}
std::string utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
std::filesystem::path from_utf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
frame_borders borders(const transform_options& o)
{
    if (o.uniform_border)
        return {o.border_mm, o.border_mm, o.border_mm, o.border_mm};
    return {o.border_top_mm, o.border_bottom_mm, o.border_left_mm, o.border_right_mm};
}
std::span<const frame_preset> frame_sizes()
{
    static constexpr frame_preset sizes[] = {
        {"4 × 6\"", 101.6, 152.4}, {"5 × 7\"", 127, 177.8},
        {"8 × 10\"", 203.2, 254},  {"11 × 14\"", 279.4, 355.6},
        {"8 × 8\"", 203.2, 203.2}, {"12 × 12\"", 304.8, 304.8},
        {"A5", 148, 210},          {"A4", 210, 297},
        {"A3", 297, 420},          {"Custom", 0, 0},
    };
    return sizes;
}
void set_frame_size(transform_options& o, int preset, bool landscape)
{
    if (preset < 0 || preset >= int(frame_sizes().size()))
        throw std::runtime_error("Unknown frame size.");
    o.size_preset = preset;
    o.landscape = landscape;
    if (preset != 9)
    {
        o.width_mm = frame_sizes()[preset].width_mm;
        o.height_mm = frame_sizes()[preset].height_mm;
    }
    else if ((o.width_mm > o.height_mm) != landscape)
    {
        std::swap(o.width_mm, o.height_mm);
    }
    if (preset != 9 && landscape)
        std::swap(o.width_mm, o.height_mm);
}
void validate(const transform_options& o)
{
    const auto b = borders(o);
    if (!std::isfinite(o.width_mm) || !std::isfinite(o.height_mm) || !std::isfinite(o.border_mm) || !std::isfinite(o.bridge_mm) || o.width_mm < 30 ||
        o.width_mm > 1000 || o.height_mm < 30 || o.height_mm > 1000 || o.border_mm < 2 || !std::isfinite(b.top) || !std::isfinite(b.bottom) ||
        !std::isfinite(b.left) || !std::isfinite(b.right) || std::min({b.top, b.bottom, b.left, b.right}) < 2 || b.left + b.right >= o.width_mm ||
        b.top + b.bottom >= o.height_mm || o.bridge_mm < .5 || o.bridge_mm > 10 || o.layers < 2 || o.layers > 12 || o.smoothing < 0 || o.smoothing > 6 ||
        int(o.placement) < 0 || int(o.placement) > 2 || o.touch_edges > 15 || o.size_preset < 0 || o.size_preset > 9 || !std::isfinite(o.layer_gap_mm) ||
        o.layer_gap_mm < 0 || o.layer_gap_mm > 50)
        throw std::runtime_error("Use 30–1000 mm dimensions, borders that leave an opening, and 2–12 layers.");
}
namespace
{
void number(std::ostream& out, uint32_t n)
{
    for (int i = 0; i < 4; ++i)
        out.put(char(n >> (8 * i)));
}
uint32_t number(std::istream& in)
{
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
    {
        const int c = in.get();
        if (c < 0)
            throw std::runtime_error("Truncated project.");
        n |= uint32_t(c) << (8 * i);
    }
    return n;
}
void bytes(std::ostream& out, const std::vector<uint8_t>& b)
{
    number(out, uint32_t(b.size()));
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
std::vector<uint8_t> bytes(std::istream& in, size_t limit)
{
    const auto n = number(in);
    if (n > limit)
        throw std::runtime_error("Project field exceeds its size limit.");
    std::vector<uint8_t> b(n);
    in.read(reinterpret_cast<char*>(b.data()), n);
    if (!in)
        throw std::runtime_error("Truncated project.");
    return b;
}
// Transforms keep their exact values: positions may be negative and need
// more precision than the hundredths used for frame dimensions.
void real(std::ostream& out, double v)
{
    const auto bits = std::bit_cast<uint64_t>(v);
    number(out, uint32_t(bits));
    number(out, uint32_t(bits >> 32));
}
double real(std::istream& in)
{
    const uint64_t low = number(in), high = number(in);
    const double v = std::bit_cast<double>(low | high << 32);
    if (!std::isfinite(v))
        throw std::runtime_error("Invalid number in project.");
    return v;
}
void string(std::ostream& out, const std::string& s)
{
    bytes(out, {s.begin(), s.end()});
}
std::string string(std::istream& in)
{
    const auto b = bytes(in, 1 << 20);  // plans and measurements are JSON
    return {b.begin(), b.end()};
}
constexpr uint32_t version_tag = 0x31524556;  // "VER1"
constexpr uint32_t palette_tag = 0x314C4150;  // "PAL1": each version's requested palette
constexpr uint32_t auto_tag = 0x31545541;     // "AUT1": Auto's rounds, Until and remembered consent
}  // namespace
void save_project(const project& p, const std::filesystem::path& path)
{
    validate(p.options);
    // A sibling temporary protects the previous project from incomplete writes.
    auto temp = path;
    temp += ".writing";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot write project.");
    out.write("OFFCUT03", 8);
    string(out, p.name);
    string(out, p.instructions);
    const auto& o = p.options;
    for (double v : {o.width_mm, o.height_mm, o.border_mm, o.bridge_mm})
        number(out, uint32_t(std::lround(v * 100)));
    number(out, o.layers);
    number(out, o.smoothing);
    number(out, o.connect);
    number(out, p.stale);
    for (double v : {o.border_top_mm, o.border_bottom_mm, o.border_left_mm, o.border_right_mm})
        number(out, uint32_t(std::lround(v * 100)));
    number(out, o.uniform_border);
    // OFFCUT03: the placement new images start with, no longer a setting of
    // the composite. Each image keeps its own transform below.
    number(out, uint32_t(o.placement));
    number(out, o.touch_edges);
    number(out, o.size_preset);
    number(out, o.landscape);
    number(out, uint32_t(std::lround(o.layer_gap_mm * 100)));
    string(out, p.ai_style);
    number(out, uint32_t(p.layers.size()));
    for (const auto& l : p.layers)
    {
        number(out, uint32_t(l.type));
        number(out, l.id);
        number(out, l.derived_from);
        string(out, l.name);
        number(out, l.visible);
        number(out, l.image.width);
        number(out, l.image.height);
        bytes(out, l.image.rgba);
        const auto t = effective_transform(l, o);
        for (double v : {t.x_mm, t.y_mm, t.mm_per_pixel, t.rotation_deg, t.crop.x, t.crop.y, t.crop.w, t.crop.h})
            real(out, v);
        string(out, l.notes);
        number(out, l.version);
        number(out, uint32_t(l.versions.size()));
        for (const auto& v : l.versions)
        {
            number(out, v.id);
            number(out, v.parent);
            number(out, uint32_t(v.state));
            string(out, v.prompt);
            string(out, v.review);
            number(out, v.image.width);
            number(out, v.image.height);
            bytes(out, v.image.rgba);
        }
    }
    number(out, uint32_t(p.cuts.size()));
    for (const auto& c : p.cuts)
    {
        string(out, c.name);
        number(out, c.visible);
        number(out, c.width);
        number(out, c.height);
        bytes(out, c.solid);
    }
    // OFFCUT03's tail: what each AI version records beyond its pixels and
    // prompt, and the recent instructions. Files without it still load.
    number(out, version_tag);
    for (const auto& l : p.layers)
        for (const auto& v : l.versions)
        {
            number(out, v.previous);
            for (const auto* s : {&v.task, &v.instructions, &v.style, &v.notes, &v.prompt_files, &v.account, &v.provider, &v.model, &v.created})
                string(out, *s);
            real(out, v.seconds);
            string(out, v.usage);
            number(out, uint32_t(v.issues + 1));
            string(out, v.findings);
            string(out, v.request);
            string(out, v.run);
            number(out, v.round);
            real(out, v.score);
            for (const auto* s : {&v.measurements, &v.plan, &v.verdict, &v.ops, &v.explanation, &v.chips})
                string(out, *s);
            for (const auto* u : {&v.illustrator, &v.director})
            {
                real(out, u->seconds);
                real(out, double(u->input_tokens));
                real(out, double(u->output_tokens));
            }
        }
    number(out, uint32_t(p.recent_instructions.size()));
    for (const auto& s : p.recent_instructions)
        string(out, s);
    number(out, palette_tag);
    for (const auto& l : p.layers)
        for (const auto& v : l.versions)
            string(out, v.palette);
    number(out, auto_tag);
    number(out, uint32_t(p.auto_rounds));
    number(out, uint32_t(p.auto_until));
    string(out, p.auto_consent);
    out.close();
    if (!out)
        throw std::runtime_error("Project write failed.");
    // Keep replacement portable; the backup is restored if the final rename fails.
    auto backup = path;
    backup += ".backup";
    std::error_code ec;
    const bool existed = std::filesystem::exists(path);
    if (existed)
    {
        std::filesystem::rename(path, backup, ec);
        if (ec)
            throw std::runtime_error("Cannot replace project; check for an existing .backup file.");
    }
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        if (existed)
            std::filesystem::rename(backup, path);
        throw std::runtime_error("Cannot finish saving project.");
    }
    if (existed)
        std::filesystem::remove(backup, ec);
}
namespace
{
void check_transform(const layer& l)
{
    const auto& t = l.transform;
    const auto& c = t.crop;
    if (!(t.mm_per_pixel > 1e-6 && t.mm_per_pixel < 1000) || std::abs(t.x_mm) > 1e5 || std::abs(t.y_mm) > 1e5 || std::abs(t.rotation_deg) > 1e5 ||
        c.x < 0 || c.y < 0 || c.w < 1 || c.h < 1 || c.x + c.w > l.image.width + 1e-9 || c.y + c.h > l.image.height + 1e-9)
        throw std::runtime_error("Invalid image placement in project.");
}
}  // namespace
project load_project(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    char magic[8]{};
    in.read(magic, 8);
    const std::string version(magic, 8);
    if (version != "OFFCUT01" && version != "OFFCUT02" && version != "OFFCUT03")
        throw std::runtime_error("This is not an Offcut project.");
    const bool v3 = version == "OFFCUT03";
    project p;
    p.path = path;
    p.name = string(in);
    p.instructions = string(in);
    auto& o = p.options;
    o.width_mm = number(in) / 100.;
    o.height_mm = number(in) / 100.;
    o.border_mm = number(in) / 100.;
    o.bridge_mm = number(in) / 100.;
    o.layers = int(number(in));
    o.smoothing = int(number(in));
    o.connect = number(in) != 0;
    p.stale = number(in) != 0;
    if (version != "OFFCUT01")
    {
        o.border_top_mm = number(in) / 100.;
        o.border_bottom_mm = number(in) / 100.;
        o.border_left_mm = number(in) / 100.;
        o.border_right_mm = number(in) / 100.;
        o.uniform_border = number(in) != 0;
        o.placement = placement_mode(number(in));
        o.touch_edges = number(in);
        o.size_preset = int(number(in));
        o.landscape = number(in) != 0;
    }
    else
    {
        o.border_top_mm = o.border_bottom_mm = o.border_left_mm = o.border_right_mm = o.border_mm;
        o.landscape = o.width_mm > o.height_mm;
    }
    if (v3)
    {
        o.layer_gap_mm = number(in) / 100.;
        p.ai_style = string(in);
    }
    validate(o);
    const auto count = number(in);
    if (count > 32)
        throw std::runtime_error("Too many source images.");
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        layer s;
        if (v3)
        {
            const auto type = number(in);
            if (type > uint32_t(layer_type::ai_illustration))
                throw std::runtime_error("Unknown layer type in project.");
            s.type = layer_type(type);
            s.id = number(in);
            s.derived_from = number(in);
        }
        s.name = string(in);
        s.visible = number(in) != 0;
        s.image.width = int(number(in));
        s.image.height = int(number(in));
        s.image.rgba = bytes(in, 2048 * 2048 * 4);
        total += s.image.rgba.size();
        if (!s.image.valid() || total > 128 * 1024 * 1024)
            throw std::runtime_error("Invalid image in project.");
        if (v3)
        {
            auto& t = s.transform;
            for (double* v : {&t.x_mm, &t.y_mm, &t.mm_per_pixel, &t.rotation_deg, &t.crop.x, &t.crop.y, &t.crop.w, &t.crop.h})
                *v = real(in);
            check_transform(s);
            s.notes = string(in);
            s.version = number(in);
            const auto versions = number(in);
            if (versions > 64)
                throw std::runtime_error("Too many versions of an item.");
            for (uint32_t k = 0; k < versions; ++k)
            {
                item_version v;
                v.id = number(in);
                v.parent = number(in);
                const auto state = number(in);
                if (state > uint32_t(review_state::answered))
                    throw std::runtime_error("Unknown review state in project.");
                v.state = review_state(state);
                v.prompt = string(in);
                v.review = string(in);
                v.image.width = int(number(in));
                v.image.height = int(number(in));
                v.image.rgba = bytes(in, 2048 * 2048 * 4);
                total += v.image.rgba.size();
                if (!v.image.valid() || total > 256 * 1024 * 1024 || v.id == 0 || v.parent >= v.id)
                    throw std::runtime_error("Invalid item version in project.");
                s.versions.push_back(std::move(v));
            }
            if (s.version != 0 && std::ranges::none_of(s.versions, [&](const item_version& v) { return v.id == s.version; }))
                throw std::runtime_error("An item uses a version it does not have.");
        }
        p.layers.push_back(std::move(s));
    }
    {
        std::vector<uint32_t> ids;
        for (const auto& l : p.layers)
            if (l.id)
            {
                if (std::ranges::find(ids, l.id) != ids.end())
                    throw std::runtime_error("Duplicate layer id in project.");
                ids.push_back(l.id);
            }
    }
    const auto cuts = number(in);
    if (cuts > 32)
        throw std::runtime_error("Too many cut layers.");
    for (uint32_t i = 0; i < cuts; ++i)
    {
        cut_layer c;
        c.name = string(in);
        c.visible = number(in) != 0;
        c.width = int(number(in));
        c.height = int(number(in));
        c.solid = bytes(in, 640 * 640);
        if (c.width < 1 || c.height < 1 || c.width > 640 || c.height > 640 || c.solid.size() != size_t(c.width) * c.height)
            throw std::runtime_error("Invalid cut layer.");
        for (auto b : c.solid)
            if (b > 1)
                throw std::runtime_error("Invalid mask.");
        if (!p.cuts.empty() && (p.cuts.front().width != c.width || p.cuts.front().height != c.height))
            throw std::runtime_error("Layer dimensions differ.");
        p.cuts.push_back(std::move(c));
    }
    if (version == "OFFCUT02" && in.peek() != std::char_traits<char>::eof())
    {
        o.layer_gap_mm = number(in) / 100.;
        validate(o);
    }
    bool tail = false;
    if (v3 && in.peek() != std::char_traits<char>::eof())
    {
        if (number(in) != version_tag)
            throw std::runtime_error("Unknown data at the end of the project.");
        tail = true;
        for (auto& l : p.layers)
            for (auto& v : l.versions)
            {
                v.previous = number(in);
                for (auto* s : {&v.task, &v.instructions, &v.style, &v.notes, &v.prompt_files, &v.account, &v.provider, &v.model, &v.created})
                    *s = string(in);
                v.seconds = real(in);
                v.usage = string(in);
                v.issues = int(std::min<uint32_t>(number(in), 1u << 30)) - 1;
                v.findings = string(in);
                v.request = string(in);
                v.run = string(in);
                v.round = number(in);
                v.score = real(in);
                for (auto* s : {&v.measurements, &v.plan, &v.verdict, &v.ops, &v.explanation, &v.chips})
                    *s = string(in);
                for (auto* u : {&v.illustrator, &v.director})
                {
                    u->seconds = real(in);
                    u->input_tokens = uint64_t(std::clamp(real(in), 0., 1e15));
                    u->output_tokens = uint64_t(std::clamp(real(in), 0., 1e15));
                }
            }
        const auto recent = number(in);
        if (recent > 10)
            throw std::runtime_error("Too many recent instructions in project.");
        for (uint32_t i = 0; i < recent; ++i)
            p.recent_instructions.push_back(string(in));
        if (in.peek() != std::char_traits<char>::eof())
        {
            if (number(in) != palette_tag)
                throw std::runtime_error("Unknown data at the end of the project.");
            for (auto& l : p.layers)
                for (auto& v : l.versions)
                    v.palette = string(in);
        }
        if (in.peek() != std::char_traits<char>::eof())
        {
            if (number(in) != auto_tag)
                throw std::runtime_error("Unknown data at the end of the project.");
            p.auto_rounds = int(std::clamp<uint32_t>(number(in), 1, 5));
            p.auto_until = int(std::min<uint32_t>(number(in), 2));
            p.auto_consent = string(in);
        }
    }
    // Before the Send dialog, the project's one "AI instructions" field was
    // sent with every request: it becomes the first recent instruction.
    if (!tail && !p.instructions.empty())
        p.recent_instructions = {p.instructions};
    // Before OFFCUT03 the placement applied to every image as it was drawn.
    // Fix it into each image's own transform, so the project looks the same.
    if (!v3)
        for (auto& l : p.layers)
            l.transform = place_image(l, o, o.placement, o.touch_edges);
    p.assign_layer_ids();
    p.selected_layer = p.layers.empty() ? -1 : int(p.layers.size()) - 1;
    if (!p.cuts.empty())
    {
        p.view = view_mode::assembled;
        p.step = work_step::sheets;
    }
    return p;
}
}  // namespace offcut
