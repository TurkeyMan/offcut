#include "local_ops.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <numeric>
#include <set>
#include "ai_json.h"
#include "director_helpers.h"
#include "transform.h"
namespace offcut
{
namespace
{
using detail::json_at;
using detail::json_reader;
using detail::json_value;
using mask = std::vector<uint8_t>;
[[noreturn]] void invalid(const std::string& message)
{
    throw std::runtime_error(message);
}
bool null(const json_value& v)
{
    return v.kind == json_value::scalar && v.text == "null";
}
const json_value& member(const json_value& v, const std::string& key)
{
    if (v.kind != json_value::object)
        invalid("Expected an operation object");
    const auto it = v.members.find(key);
    if (it == v.members.end())
        invalid("Missing " + key);
    return it->second;
}
std::string text(const json_value& v)
{
    if (v.kind != json_value::string)
        invalid("Expected a string");
    return v.text;
}
std::string optional_text(const json_value& v, const char* key)
{
    const auto* p = json_at(v, {key});
    return !p || null(*p) ? std::string() : text(*p);
}
double number(const json_value& v)
{
    if (v.kind != json_value::scalar)
        invalid("Expected a number");
    size_t end = 0;
    double x = 0;
    try
    {
        x = std::stod(v.text, &end);
    }
    catch (...)
    {
        invalid("Expected a finite number");
    }
    if (end != v.text.size() || !std::isfinite(x))
        invalid("Expected a finite number");
    return x;
}
uint64_t integer(const json_value& v, uint64_t maximum)
{
    const double x = number(v);
    if (x < 0 || x != std::floor(x) || x > double(maximum))
        invalid("Count is outside the supported range");
    return uint64_t(x);
}
std::array<uint8_t, 3> colour(const std::string& value)
{
    if (value.size() != 7 || value[0] != '#')
        invalid("Use a palette colour as #RRGGBB");
    std::array<uint8_t, 3> out{};
    for (size_t i = 0; i < 6; ++i)
    {
        const auto c = value[i + 1];
        const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (n < 0)
            invalid("Invalid palette colour");
        out[i / 2] = uint8_t(out[i / 2] * 16 + n);
    }
    return out;
}
int sheet_id(const std::string& value, size_t count)
{
    if (value.size() < 2 || value.front() != 's')
        invalid("Use a sheet id such as s2");
    size_t end = 0;
    int id = 0;
    try
    {
        id = std::stoi(value.substr(1), &end);
    }
    catch (...)
    {
        invalid("Invalid sheet id");
    }
    if (end != value.size() - 1 || id < 1 || id > int(count) || std::to_string(id) != value.substr(1))
        invalid("Sheet is outside the palette");
    return id - 1;
}
template <class F>
void neighbours(int p, int w, int h, F f)
{
    const int x = p % w, y = p / w;
    if (x)
        f(p - 1);
    if (x + 1 < w)
        f(p + 1);
    if (y)
        f(p - w);
    if (y + 1 < h)
        f(p + w);
}
std::vector<std::vector<int>> components(const mask& pixels, int w, int h)
{
    mask seen(pixels.size());
    std::vector<std::vector<int>> out;
    for (int i = 0; i < int(pixels.size()); ++i)
        if (pixels[size_t(i)] && !seen[size_t(i)])
        {
            auto& q = out.emplace_back();
            q.push_back(i);
            seen[size_t(i)] = 1;
            for (size_t at = 0; at < q.size(); ++at)
                neighbours(q[at], w, h,
                           [&](int p)
                           {
                               if (pixels[size_t(p)] && !seen[size_t(p)])
                               {
                                   seen[size_t(p)] = 1;
                                   q.push_back(p);
                               }
                           });
        }
    return out;
}
mask reaches_frame(const mask& pixels, int w, int h)
{
    mask reached(pixels.size());
    std::vector<int> q;
    for (int i = 0; i < int(pixels.size()); ++i)
        if (pixels[size_t(i)] && (i % w == 0 || i / w == 0 || i % w == w - 1 || i / w == h - 1))
        {
            reached[size_t(i)] = 1;
            q.push_back(i);
        }
    for (size_t at = 0; at < q.size(); ++at)
        neighbours(q[at], w, h,
                   [&](int p)
                   {
                       if (pixels[size_t(p)] && !reached[size_t(p)])
                       {
                           reached[size_t(p)] = 1;
                           q.push_back(p);
                       }
                   });
    return reached;
}
// Same clipped, separable square/rectangle opening used by thin_features.
mask opened(const mask& pixels, int w, int h, int kx, int ky)
{
    const auto pass = [&](const mask& in, bool rows, bool erode, int k)
    {
        mask out(in.size());
        std::vector<int> sums(size_t(std::max(w, h)) + 1);
        const int a = k / 2, b = k - 1 - a, before = erode ? a : b, after = erode ? b : a;
        for (int line = 0; line < (rows ? h : w); ++line)
        {
            const int n = rows ? w : h;
            const auto at = [&](int x) { return rows ? size_t(line) * w + x : size_t(x) * w + line; };
            sums[0] = 0;
            for (int x = 0; x < n; ++x)
                sums[size_t(x) + 1] = sums[size_t(x)] + (in[at(x)] ? 1 : 0);
            for (int x = 0; x < n; ++x)
            {
                const int lo = std::max(0, x - before), hi = std::min(n - 1, x + after);
                const int count = sums[size_t(hi) + 1] - sums[size_t(lo)];
                out[at(x)] = erode ? count == hi - lo + 1 : count > 0;
            }
        }
        return out;
    };
    return pass(pass(pass(pass(pixels, true, true, kx), false, true, ky), true, false, kx), false, false, ky);
}
struct finding
{
    std::string kind;
    int sheet = 0;
    double x = 0, y = 0, area = 0;
};
struct selection
{
    int tone = 0;
    mask pixels;
};
class editor
{
public:
    const local_ops_input& input;
    const int w, h;
    const frame_geometry geometry;
    double image_scale = 1, image_left = 0, image_top = 0;
    std::vector<uint8_t> initial, tones;
    std::map<std::string, finding> findings;
    std::map<std::string, mask> bindings;
    std::set<std::string> removed;
    std::optional<sheet_result> original_sheets, current_sheets;
    editor(const local_ops_input& in) : input(in), w(in.image.width), h(in.image.height), geometry(dimensions(in.options))
    {
        if (!in.image.valid() || in.palette.size() < 2 || in.palette.size() > 12 || in.palette.size() != size_t(in.options.layers))
            invalid("Local operations need a valid flat raster and one palette colour per sheet");
        // Match illustration import's centre-cover nearest-neighbour geometry.
        image_scale = std::max(double(geometry.w) / w, double(geometry.h) / h);
        image_left = (w - geometry.w / image_scale) / 2;
        image_top = (h - geometry.h / image_scale) / 2;
        std::map<std::array<uint8_t, 3>, int> lookup;
        for (size_t i = 0; i < in.palette.size(); ++i)
            if (!lookup.emplace(in.palette[i], int(i)).second)
                invalid("Palette colours must be distinct");
        initial.resize(size_t(w) * h);
        for (size_t i = 0; i < initial.size(); ++i)
        {
            const size_t p = i * 4;
            if (in.image.rgba[p + 3] != 255)
                invalid("Local operations need an opaque, palette-snapped image");
            const auto found = lookup.find({in.image.rgba[p], in.image.rgba[p + 1], in.image.rgba[p + 2]});
            if (found == lookup.end())
                invalid("Image contains a colour outside the palette");
            initial[i] = uint8_t(found->second);
        }
        tones = initial;
        if (!in.measurements.empty())
        {
            const auto json = json_reader(in.measurements).parse();
            if (json.kind != json_value::object)
                invalid("Measurements must be a JSON object");
            if (const auto* list = json_at(json, {"findings"}))
            {
                if (list->kind != json_value::array)
                    invalid("Measurement findings must be an array");
                for (const auto& f : list->items)
                {
                    if (f.kind != json_value::object)
                        invalid("Invalid measurement finding");
                    const auto id = text(member(f, "id"));
                    finding item;
                    item.kind = text(member(f, "kind"));
                    item.sheet = int(integer(member(f, "sheet"), in.palette.size())) - 1;
                    const auto& at = member(f, "at_mm");
                    if (item.sheet < 0 || at.kind != json_value::array || at.items.size() != 2)
                        invalid("Invalid measurement coordinates");
                    item.x = number(at.items[0]) * working_scale(in.options);
                    item.y = number(at.items[1]) * working_scale(in.options);
                    item.area = number(member(f, "area_mm2"));
                    if (item.area < 0 || !findings.emplace(id, item).second)
                        invalid("Invalid or duplicate measurement id");
                }
            }
        }
    }
    bool frame(int p) const
    {
        const int x = p % w, y = p / w;
        const double cx = (x + .5 - image_left) * image_scale, cy = (y + .5 - image_top) * image_scale;
        return cx < geometry.left || cx >= geometry.w - geometry.right || cy < geometry.top || cy >= geometry.h - geometry.bottom;
    }
    size_t source_pixel(int x, int y) const
    {
        const int sx = std::clamp(int(std::floor(image_left + (x + .5) / image_scale)), 0, w - 1),
                  sy = std::clamp(int(std::floor(image_top + (y + .5) / image_scale)), 0, h - 1);
        return size_t(sy) * w + sx;
    }
    int palette_index(const std::string& hex) const
    {
        const auto c = colour(hex);
        const auto it = std::ranges::find(input.palette, c);
        if (it == input.palette.end())
            invalid("Destination colour is not in the palette");
        return int(it - input.palette.begin());
    }
    tone_map working(const std::vector<uint8_t>& source) const
    {
        tone_map map{geometry.w, geometry.h, std::vector<uint8_t>(size_t(geometry.w) * geometry.h)};
        for (int y = 0; y < geometry.h; ++y)
            for (int x = 0; x < geometry.w; ++x)
                map.indices[size_t(y) * geometry.w + x] = source[source_pixel(x, y)];
        return map;
    }
    sheet_result sheets(const std::vector<uint8_t>& source) const
    {
        auto options = input.options;
        options.connect = false;
        options.smoothing = 0;
        return build_sheets(working(source), options);
    }
    const mask& measurement(const std::string& id)
    {
        if (removed.contains(id))
            invalid(id + " was removed by an earlier operation");
        if (const auto it = bindings.find(id); it != bindings.end())
            return it->second;
        const auto found = findings.find(id);
        if (found == findings.end())
            invalid(id + " not found");
        const auto& f = found->second;
        if (f.kind != "floating" && f.kind != "thin")
            invalid(id + " is not a supported piece measurement");
        if (!original_sheets)
            original_sheets = sheets(initial);
        const auto& card = original_sheets->layers[size_t(f.sheet)];
        auto pixels = card.solid;
        if (f.kind == "thin")
        {
            const int k = std::max(2, int(std::lround(input.options.bridge_mm * working_scale(input.options))));
            const auto wide = opened(pixels, geometry.w, geometry.h, k, k);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = pixels[i] && !wide[i] && !border(int(i) % geometry.w, int(i) / geometry.w, geometry);
        }
        const auto groups = components(pixels, geometry.w, geometry.h);
        const std::vector<int>* best = nullptr;
        double best_cost = std::numeric_limits<double>::infinity();
        const double tolerance = std::max(3., .11 * working_scale(input.options));
        for (const auto& group : groups)
        {
            if (f.kind == "floating" && std::ranges::any_of(group, [&](int p) { return border(p % geometry.w, p / geometry.w, geometry); }))
                continue;
            double x = 0, y = 0;
            for (int p : group)
            {
                x += p % geometry.w;
                y += p / geometry.w;
            }
            x /= group.size();
            y /= group.size();
            const double distance = (x - f.x) * (x - f.x) + (y - f.y) * (y - f.y);
            if (distance > tolerance * tolerance)
                continue;
            const double area = group.size() / std::pow(working_scale(input.options), 2);
            const double cost = distance + std::pow((area - f.area) / std::max(1., f.area), 2);
            if (cost < best_cost)
            {
                best_cost = cost;
                best = &group;
            }
        }
        if (!best)
            invalid(id + " no longer matches a measured piece in this picture");
        mask work(pixels.size()), native(initial.size());
        for (int p : *best)
            work[size_t(p)] = 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const int p = y * w + x;
                const int sx = std::clamp(int(std::floor((x + .5 - image_left) * image_scale)), 0, geometry.w - 1),
                          sy = std::clamp(int(std::floor((y + .5 - image_top) * image_scale)), 0, geometry.h - 1);
                native[size_t(p)] = initial[size_t(p)] == f.sheet && work[size_t(sy) * geometry.w + sx] && !frame(p);
            }
        if (std::ranges::none_of(native, [](auto x) { return x != 0; }))
            invalid(id + " has no visible pixels on its measured sheet");
        return bindings.emplace(id, std::move(native)).first->second;
    }
    selection select(const std::string& target)
    {
        selection out;
        out.pixels.resize(tones.size());
        if (target.starts_with('m'))
        {
            out.pixels = measurement(target);
            std::vector<size_t> counts(input.palette.size());
            for (size_t i = 0; i < tones.size(); ++i)
                if (out.pixels[i])
                    ++counts[tones[i]];
            out.tone = int(std::max_element(counts.begin(), counts.end()) - counts.begin());
            for (size_t i = 0; i < tones.size(); ++i)
                out.pixels[i] = out.pixels[i] && tones[i] == out.tone;
        }
        else
        {
            out.tone = target.starts_with('#') ? palette_index(target) : sheet_id(target, input.palette.size());
            for (size_t i = 0; i < tones.size(); ++i)
                out.pixels[i] = tones[i] == out.tone && !frame(int(i));
        }
        if (std::ranges::none_of(out.pixels, [](auto x) { return x != 0; }))
            invalid("Target " + target + " has no visible interior pixels");
        return out;
    }
    bool supported_after(const mask& region, int depth)
    {
        if (!current_sheets)
            current_sheets = sheets(tones);
        auto card = current_sheets->layers[size_t(depth)].solid;
        mask added(card.size());
        for (int y = 0; y < geometry.h; ++y)
            for (int x = 0; x < geometry.w; ++x)
            {
                const size_t p = size_t(y) * geometry.w + x;
                added[p] = region[source_pixel(x, y)];
                if (added[p])
                    card[p] = 1;
            }
        const auto reached = reaches_frame(card, geometry.w, geometry.h);
        for (size_t i = 0; i < added.size(); ++i)
            if (added[i] && !reached[i])
                return false;
        return true;
    }
    int destination(const json_value& op, int fallback) const
    {
        const auto sheet = optional_text(op, "sheet"), hex = optional_text(op, "colour");
        int result = sheet.empty() ? fallback : sheet_id(sheet, input.palette.size());
        if (!hex.empty())
        {
            const int c = palette_index(hex);
            if (!sheet.empty() && result != c)
                invalid("Destination sheet and colour disagree");
            result = c;
        }
        return result;
    }
    mask thickening(const selection& target, double mm) const
    {
        if (!std::isfinite(mm) || mm < input.options.bridge_mm || mm > std::min(input.options.width_mm, input.options.height_mm))
            invalid("Thickening width must be at least Min. width and fit the frame");
        const int kx = std::max(1, int(std::ceil(mm * working_scale(input.options) / image_scale))), ky = kx;
        mask own(tones.size());
        for (size_t i = 0; i < own.size(); ++i)
            own[i] = tones[i] == target.tone;
        const auto wide = opened(own, w, h, kx, ky);
        std::vector<int> horizontal(own.size()), vertical(own.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w;)
            {
                if (!own[size_t(y) * w + x])
                {
                    ++x;
                    continue;
                }
                const int begin = x;
                while (x < w && own[size_t(y) * w + x])
                    ++x;
                for (int j = begin; j < x; ++j)
                    horizontal[size_t(y) * w + j] = x - begin;
            }
        for (int x = 0; x < w; ++x)
            for (int y = 0; y < h;)
            {
                if (!own[size_t(y) * w + x])
                {
                    ++y;
                    continue;
                }
                const int begin = y;
                while (y < h && own[size_t(y) * w + x])
                    ++y;
                for (int j = begin; j < y; ++j)
                    vertical[size_t(j) * w + x] = y - begin;
            }
        std::vector<int> diff(size_t(w + 1) * (h + 1));
        const auto add = [&](int x, int y, int value) { diff[size_t(y) * (w + 1) + x] += value; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t p = size_t(y) * w + x;
                if (!target.pixels[p] || wide[p])
                    continue;
                const int dx = std::max(0, kx - horizontal[p]), dy = std::max(0, ky - vertical[p]);
                const int x0 = std::max(0, x - dx / 2), x1 = std::min(w, x + (dx - dx / 2) + 1), y0 = std::max(0, y - dy / 2),
                          y1 = std::min(h, y + (dy - dy / 2) + 1);
                add(x0, y0, 1);
                add(x1, y0, -1);
                add(x0, y1, -1);
                add(x1, y1, 1);
            }
        mask owner(own.size()), seen(own.size());
        std::vector<int> q;
        for (const auto& group : components(own, w, h))
        {
            const bool selected = std::ranges::any_of(group, [&](int p) { return target.pixels[size_t(p)] != 0; });
            for (int p : group)
            {
                owner[size_t(p)] = selected;
                seen[size_t(p)] = 1;
                q.push_back(p);
            }
        }
        for (size_t at = 0; at < q.size(); ++at)
            neighbours(q[at], w, h,
                       [&](int p)
                       {
                           if (!seen[size_t(p)])
                           {
                               seen[size_t(p)] = 1;
                               owner[size_t(p)] = owner[size_t(q[at])];
                               q.push_back(p);
                           }
                       });
        mask grow(own.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t p = size_t(y) * (w + 1) + x;
                if (x)
                    diff[p] += diff[p - 1];
                if (y)
                    diff[p] += diff[p - (w + 1)];
                if (x && y)
                    diff[p] -= diff[p - (w + 2)];
                const size_t pixel = size_t(y) * w + x;
                grow[pixel] = diff[p] > 0 && owner[pixel] && tones[pixel] > target.tone && !frame(int(pixel));
            }
        return grow;
    }
};
}  // namespace
local_ops_result apply_local_ops(const local_ops_input& input)
{
    editor edit(input);
    const auto ops = json_reader(input.ops).parse();
    if (ops.kind != json_value::array)
        invalid("Local operations must be a JSON array");
    if (ops.items.size() > 256)
        invalid("Too many local operations");
    local_ops_result result;
    result.image = input.image;
    for (size_t i = 0; i < ops.items.size(); ++i)
    {
        const auto& op = ops.items[i];
        std::string name = "Operation " + std::to_string(i + 1);
        try
        {
            name = text(member(op, "op"));
            if (name == "set_sheet_count" || name == "set_card" || name == "extend_to_frame" || name == "extract")
            {
                result.skipped.push_back(name + " needs a project-level or UI step; the Auto loop handles it");
                continue;
            }
            if (name != "push_back" && name != "show_through" && name != "merge_colours" && name != "map_colour" && name != "remove" && name != "thicken")
                invalid("Unknown local operation");
            const auto target = text(member(op, "target"));
            auto selected = edit.select(target);
            auto pixels = selected.pixels;
            int destination = selected.tone;
            std::string description;
            if (name == "push_back")
            {
                const auto explicit_sheet = optional_text(op, "sheet");
                const int begin = explicit_sheet.empty() ? selected.tone + 1 : sheet_id(explicit_sheet, input.palette.size());
                if (begin <= selected.tone)
                    invalid("Push back needs a deeper sheet");
                destination = -1;
                for (int depth = begin; depth < int(input.palette.size()); ++depth)
                {
                    if (edit.supported_after(pixels, depth))
                    {
                        destination = depth;
                        break;
                    }
                    if (!explicit_sheet.empty())
                        break;
                }
                if (destination < 0)
                    invalid("No requested deeper sheet can support the piece");
                description = "Pushed back " + target + " to sheet " + std::to_string(destination + 1);
            }
            else if (name == "show_through")
            {
                destination = edit.destination(op, int(input.palette.size()) - 1);
                if (destination <= selected.tone)
                    invalid("Show through needs a deeper sheet or the backing");
                description = "Showed " + target + " through to sheet " + std::to_string(destination + 1);
            }
            else if (name == "merge_colours" || name == "map_colour")
            {
                if (optional_text(op, "sheet").empty() && optional_text(op, "colour").empty())
                    invalid("Choose a destination sheet or palette colour");
                destination = edit.destination(op, selected.tone);
                if (destination == selected.tone)
                    invalid("Target already uses that colour");
                for (size_t p = 0; p < pixels.size(); ++p)
                    pixels[p] = edit.tones[p] == selected.tone && !edit.frame(int(p));
                description = "Recoloured all interior pixels of " + target + " to sheet " + std::to_string(destination + 1);
            }
            else if (name == "remove")
            {
                std::vector<size_t> count(input.palette.size());
                for (int p = 0; p < int(pixels.size()); ++p)
                    if (pixels[size_t(p)])
                        neighbours(p, edit.w, edit.h,
                                   [&](int q)
                                   {
                                       if (!pixels[size_t(q)] && edit.tones[size_t(q)] != selected.tone)
                                           ++count[edit.tones[size_t(q)]];
                                   });
                destination = -1;
                size_t best = 0;
                for (int tone = 0; tone < int(count.size()); ++tone)
                    if (count[size_t(tone)] && count[size_t(tone)] >= best)
                    {
                        best = count[size_t(tone)];
                        destination = tone;
                    }
                if (destination < 0)
                    invalid("Target has no surrounding colour to merge into");
                description = "Removed " + target + " into its surrounding colour on sheet " + std::to_string(destination + 1);
            }
            else
            {
                double width = input.options.bridge_mm;
                if (const auto* value = json_at(op, {"width_mm"}); value && !null(*value))
                    width = number(*value);
                pixels = edit.thickening(selected, width);
                destination = selected.tone;
                description = std::format("Thickened the narrow parts of {} toward {:g} mm", target, width);
            }
            size_t changed = 0;
            for (size_t p = 0; p < pixels.size(); ++p)
                if (pixels[p] && edit.tones[p] != destination)
                {
                    edit.tones[p] = uint8_t(destination);
                    ++changed;
                }
            if (!changed)
                invalid("No safe visible pixels would change");
            edit.current_sheets.reset();
            if (name == "remove" && target.starts_with('m'))
                edit.removed.insert(target);
            result.applied.push_back(description + " (" + std::to_string(changed) + " pixels)");
        }
        catch (const std::exception& e)
        {
            result.skipped.push_back(name + ": " + e.what());
        }
    }
    for (size_t p = 0; p < edit.tones.size(); ++p)
        if (edit.tones[p] != edit.initial[p])
        {
            ++result.changed_pixels;
            std::copy(input.palette[edit.tones[p]].begin(), input.palette[edit.tones[p]].end(), result.image.rgba.begin() + p * 4);
        }
    return result;
}
namespace
{
using path = std::vector<std::string>;
path pointer(const std::string& value)
{
    path out;
    if (value.empty())
        return out;
    if (value[0] != '/')
        invalid("JSON Patch path must be a JSON pointer");
    size_t begin = 1;
    for (;;)
    {
        const auto end = value.find('/', begin);
        std::string token;
        for (size_t i = begin; i < (end == std::string::npos ? value.size() : end); ++i)
        {
            char c = value[i];
            if (c == '~')
            {
                if (++i >= value.size() || (value[i] != '0' && value[i] != '1'))
                    invalid("Invalid JSON pointer escape");
                c = value[i] == '0' ? '~' : '/';
            }
            token += c;
        }
        out.push_back(token);
        if (out.size() > 64)
            invalid("JSON Patch path is too deep");
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return out;
}
size_t array_index(const std::string& token, size_t size, bool adding)
{
    if (adding && token == "-")
        return size;
    if (token.empty() || (token.size() > 1 && token[0] == '0') || token.find_first_not_of("0123456789") != std::string::npos)
        invalid("Invalid JSON Patch array index");
    size_t n = 0;
    for (char c : token)
    {
        if (n > size / 10 + 1)
            invalid("JSON Patch array index is out of range");
        n = n * 10 + size_t(c - '0');
    }
    if (n > size || (!adding && n == size))
        invalid("JSON Patch array index is out of range");
    return n;
}
json_value& locate(json_value& root, const path& parts, size_t count)
{
    auto* node = &root;
    for (size_t i = 0; i < count; ++i)
    {
        if (node->kind == json_value::object)
        {
            auto it = node->members.find(parts[i]);
            if (it == node->members.end())
                invalid("JSON Patch path does not exist");
            node = &it->second;
        }
        else if (node->kind == json_value::array)
            node = &node->items[array_index(parts[i], node->items.size(), false)];
        else
            invalid("JSON Patch path traverses a scalar");
    }
    return *node;
}
json_value* existing(json_value& root, const path& parts)
{
    try
    {
        return &locate(root, parts, parts.size());
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}
void erase(json_value& root, const path& parts)
{
    if (parts.empty())
    {
        root = {};
        root.text = "null";
        return;
    }
    auto& parent = locate(root, parts, parts.size() - 1);
    if (parent.kind == json_value::object)
    {
        if (!parent.members.erase(parts.back()))
            invalid("JSON Patch remove path does not exist");
    }
    else if (parent.kind == json_value::array)
        parent.items.erase(parent.items.begin() + array_index(parts.back(), parent.items.size(), false));
    else
        invalid("JSON Patch parent is a scalar");
}
void put(json_value& root, const path& parts, json_value value, bool replacing)
{
    if (parts.empty())
    {
        root = std::move(value);
        return;
    }
    auto& parent = locate(root, parts, parts.size() - 1);
    if (parent.kind == json_value::object)
    {
        if (replacing && !parent.members.contains(parts.back()))
            invalid("JSON Patch replace path does not exist");
        parent.members[parts.back()] = std::move(value);
    }
    else if (parent.kind == json_value::array)
    {
        const auto index = array_index(parts.back(), parent.items.size(), !replacing);
        if (replacing)
            parent.items[index] = std::move(value);
        else
            parent.items.insert(parent.items.begin() + index, std::move(value));
    }
    else
        invalid("JSON Patch parent is a scalar");
}
bool equal(const json_value& a, const json_value& b)
{
    if (a.kind != b.kind)
        return false;
    if (a.kind == json_value::object)
    {
        if (a.members.size() != b.members.size())
            return false;
        for (const auto& [key, value] : a.members)
        {
            const auto it = b.members.find(key);
            if (it == b.members.end() || !equal(value, it->second))
                return false;
        }
        return true;
    }
    if (a.kind == json_value::array)
    {
        if (a.items.size() != b.items.size())
            return false;
        for (size_t i = 0; i < a.items.size(); ++i)
            if (!equal(a.items[i], b.items[i]))
                return false;
        return true;
    }
    if (a.text == b.text)
        return true;
    if (a.kind == json_value::scalar)
    {
        try
        {
            return number(a) == number(b);
        }
        catch (...)
        {
        }
    }
    return false;
}
// The director schema carries patch values as strings. Text fields stay
// literal; structural/numeric fields decode their JSON representation.
json_value patch_value(json_value& root, const path& parts, const json_value& value)
{
    if (value.kind != json_value::string)
        return value;
    const auto* old = existing(root, parts);
    if (old && old->kind == json_value::string)
        return value;
    if (old)
        return json_reader(value.text).parse();
    const std::string field = parts.empty() ? "" : parts.back();
    const std::string parent = parts.size() > 1 ? parts[parts.size() - 2] : "";
    const bool structural = parts.empty() || field == "arrangement" || field == "support" || field == "style" || field == "local" || field == "sheets" ||
                            field == "features" || field == "palette" || field == "ops" || field == "splits" || field == "risks" || field == "avoid" ||
                            field == "scene_directions" || field == "through" || field == "extends_behind" || field == "evidence" || field == "plan_version" ||
                            field == "sheet_count" || field == "depth" || field == "contact_mm" || field == "count" || field == "width_mm" ||
                            field == "seeds" || parent == "seeds" || field == "hidden_support" || parent == "sheets" || parent == "features" ||
                            parent == "palette" || parent == "ops" || parent == "splits";
    return structural ? json_reader(value.text).parse() : value;
}
}  // namespace
std::string apply_plan_patch(const std::string& plan_json, const std::string& patch_json)
{
    validate_director_plan(plan_json, 32);
    auto root = json_reader(plan_json).parse();
    const auto patch = json_reader(patch_json).parse();
    if (patch.kind != json_value::array || patch.items.size() > 256)
        invalid("Plan patch must be an array of at most 256 operations");
    for (size_t i = 0; i < patch.items.size(); ++i)
        try
        {
            const auto& op = patch.items[i];
            const auto name = text(member(op, "op"));
            const auto parts = pointer(text(member(op, "path")));
            if (name == "add" || name == "replace")
            {
                auto value = patch_value(root, parts, member(op, "value"));
                put(root, parts, std::move(value), name == "replace");
            }
            else if (name == "remove")
                erase(root, parts);
            else if (name == "test")
            {
                const auto value = patch_value(root, parts, member(op, "value"));
                if (!equal(locate(root, parts, parts.size()), value))
                    invalid("JSON Patch test failed");
            }
            else if (name == "move" || name == "copy")
            {
                const auto from = pointer(text(member(op, "from")));
                auto value = locate(root, from, from.size());
                if (name == "move")
                {
                    if (parts.size() > from.size() && std::equal(from.begin(), from.end(), parts.begin()))
                        invalid("Cannot move a value inside itself");
                    if (from == parts)
                        continue;
                    erase(root, from);
                }
                put(root, parts, std::move(value), false);
            }
            else
                invalid("Unknown JSON Patch operation: " + name);
            if (detail::json_dump(root, "").size() > 4 * 1024 * 1024)
                invalid("Patched plan is too large");
        }
        catch (const std::exception& e)
        {
            invalid("Plan patch operation " + std::to_string(i + 1) + ": " + e.what());
        }
    const auto result = detail::json_dump(root, "");
    validate_director_plan(result, 32);
    return result;
}
double auto_round_score(const std::string& source)
{
    try
    {
        const auto root = json_reader(source).parse();
        const auto floating = integer(member(root, "floating"), (1u << 22) - 1), thin = integer(member(root, "thin"), (1u << 22) - 1);
        const auto& tones = member(root, "tones");
        const auto found = integer(member(tones, "found"), 127), wanted = integer(member(tones, "wanted"), 127);
        if (!found || !wanted)
            invalid("Tone counts are unknown");
        uint64_t unsupported = 0;
        if (const auto* pixels = json_at(root, {"floating_pixels"}))
            unsupported = integer(*pixels, (1u << 24) - 1);
        else
        {
            double area = 0;
            size_t count = 0;
            if (const auto* total = json_at(root, {"floating_area_mm2"}))
                area = number(*total);
            else
            {
                const auto& findings = member(root, "findings");
                if (findings.kind != json_value::array)
                    invalid("Unknown floating area");
                for (const auto& f : findings.items)
                    if (text(member(f, "kind")) == "floating")
                    {
                        const double a = number(member(f, "area_mm2"));
                        if (a < 0)
                            invalid("Invalid area");
                        area += a;
                        ++count;
                    }
                if (count != floating)
                    invalid("Truncated floating findings cannot be ranked");
            }
            double units = 10;
            const auto* frame = json_at(root, {"frame_mm"});
            const auto* canvas = json_at(root, {"canvas_px"});
            if (frame && canvas)
            {
                if (frame->kind != json_value::array || canvas->kind != json_value::array || frame->items.size() != 2 || canvas->items.size() != 2)
                    invalid("Invalid measurement geometry");
                const double fw = number(frame->items[0]), fh = number(frame->items[1]), w = number(canvas->items[0]), h = number(canvas->items[1]);
                if (fw <= 0 || fh <= 0 || w <= 0 || h <= 0)
                    invalid("Invalid measurement geometry");
                // Working pixels are square. The shorter canvas axis is
                // rounded to an integer, so its separate ratio loses area.
                const double scale = std::max(w, h) / std::max(fw, fh);
                units = scale * scale;
            }
            const double area_pixels = std::round(area * units);
            if (area < 0 || !std::isfinite(area_pixels) || area_pixels > double((1u << 24) - 1))
                invalid("Unsupported area is outside the supported range");
            unsupported = std::max(uint64_t(area_pixels), floating);
        }
        if ((floating == 0) != (unsupported == 0) || unsupported < floating)
            invalid("Inconsistent floating measurements");
        // 24 + 22 + 7 bits fit exactly in double's 53-bit integer mantissa.
        // Bounded lower terms cannot outweigh one unit of a higher term.
        return double((unsupported * (uint64_t(1) << 22) + thin) * 128 + (found > wanted ? found - wanted : 0));
    }
    catch (...)
    {
        return std::numeric_limits<double>::infinity();
    }
}
}  // namespace offcut
