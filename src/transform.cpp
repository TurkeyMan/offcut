#include "transform.h"
#include <algorithm>
#include <map>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include "prompts.h"
#include "versions.h"

namespace offcut
{
frame_geometry dimensions(const transform_options& o)
{
    validate(o);
    const double scale = 640. / std::max(o.width_mm, o.height_mm);
    const auto b = borders(o);
    frame_geometry g{std::max(20, int(std::lround(o.width_mm * scale))), std::max(20, int(std::lround(o.height_mm * scale))),
                     std::max(1, int(std::ceil(b.left * scale))),        std::max(1, int(std::ceil(b.top * scale))),
                     std::max(1, int(std::ceil(b.right * scale))),       std::max(1, int(std::ceil(b.bottom * scale)))};
    if (g.opening_width() < 1 || g.opening_height() < 1)
        throw std::runtime_error("The frame leaves no opening at this working resolution.");
    return g;
}
bool border(int x, int y, frame_geometry g)
{
    return x < g.left || y < g.top || x >= g.w - g.right || y >= g.h - g.bottom;
}
subject_box detect_subject(const raster& image)
{
    if (!image.valid())
        return {};
    const auto count = size_t(image.width) * image.height;
    size_t transparent = 0;
    std::vector<int> samples[3];
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
        {
            const auto i = (size_t(y) * image.width + x) * 4;
            if (image.rgba[i + 3] < 128)
                ++transparent;
            if ((x == 0 || y == 0 || x == image.width - 1 || y == image.height - 1) && image.rgba[i + 3] >= 128)
                for (int ch = 0; ch < 3; ++ch)
                    samples[ch].push_back(image.rgba[i + ch]);
        }
    const bool alpha_background = transparent > count / 100;
    int background[3] = {255, 255, 255};
    for (int ch = 0; ch < 3; ++ch)
        if (!samples[ch].empty())
        {
            auto& values = samples[ch];
            std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
            background[ch] = values[values.size() / 2];
        }
    const auto close_to_background = [&](size_t i)
    {
        int distance = 0;
        for (int ch = 0; ch < 3; ++ch)
        {
            const int delta = int(image.rgba[i + ch]) - background[ch];
            distance += delta * delta;
        }
        return distance <= 35 * 35;
    };
    size_t edge_count = 0, matching = 0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            if (x == 0 || y == 0 || x == image.width - 1 || y == image.height - 1)
            {
                const auto i = (size_t(y) * image.width + x) * 4;
                if (image.rgba[i + 3] >= 128)
                {
                    ++edge_count;
                    if (close_to_background(i))
                        ++matching;
                }
            }
    const bool plain_background = edge_count && matching * 10 >= edge_count * 9;
    subject_box box{image.width, image.height, 0, 0};
    size_t subject = 0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
        {
            const auto i = (size_t(y) * image.width + x) * 4;
            if (image.rgba[i + 3] < 128)
                continue;
            const double lum = (.2126 * image.rgba[i] + .7152 * image.rgba[i + 1] + .0722 * image.rgba[i + 2]) / 255.;
            if (!alpha_background && (lum > .92 || (plain_background && close_to_background(i))))
                continue;
            box.left = std::min(box.left, x);
            box.top = std::min(box.top, y);
            box.right = std::max(box.right, x + 1);
            box.bottom = std::max(box.bottom, y + 1);
            ++subject;
        }
    box.found = subject > 0;
    box.isolated = box.found && (alpha_background || (plain_background && subject * 100 < count * 95));
    return box;
}
transform_options new_project_options(const raster& image)
{
    transform_options o;
    set_frame_size(o, 2, image.width > image.height);
    if (detect_subject(image).isolated)
        o.placement = placement_mode::touch_edges;
    return o;
}
namespace
{
std::vector<std::vector<int>> components(const cut_layer& c)
{
    std::vector<uint8_t> seen(c.solid.size());
    std::vector<std::vector<int>> out;
    for (int start = 0; start < int(c.solid.size()); ++start)
    {
        if (!c.solid[start] || seen[start])
            continue;
        auto& q = out.emplace_back();
        q.push_back(start);
        seen[start] = 1;
        for (size_t k = 0; k < q.size(); ++k)
        {
            const int i = q[k], x = i % c.width, y = i / c.width;
            for (int j : {x > 0 ? i - 1 : -1, x + 1 < c.width ? i + 1 : -1, y > 0 ? i - c.width : -1, y + 1 < c.height ? i + c.width : -1})
                if (j >= 0 && !seen[j] && c.solid[j])
                {
                    seen[j] = 1;
                    q.push_back(j);
                }
        }
    }
    return out;
}
// Card that doesn't reach the frame can't hold itself up, and a bridge thin
// enough not to spoil the artwork is too weak to carry it, so it's removed.
// Tonal layers are cumulative: what's removed from one usually still shows
// from a layer behind, where it's part of a shape that does reach the frame.
piece_note note(const std::vector<int>& part, int width, int sheet)
{
    double x = 0, y = 0;
    for (int i : part)
        x += i % width, y += i / width;
    return {sheet, x / part.size(), y / part.size(), part.size()};
}
size_t frame_and_settle(cut_layer& c, const transform_options& o, std::vector<piece_note>* notes = nullptr, int sheet = 0)
{
    size_t removed = 0;
    const auto g = dimensions(o);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (border(x, y, g))
                c.solid[y * g.w + x] = 1;
    for (const auto& component : components(c))
    {
        if (std::ranges::any_of(component, [&](int i) { return border(i % g.w, i / g.w, g); }))
            continue;
        // Specks always go; larger floating pieces only when asked.
        if (o.connect || component.size() < size_t((o.smoothing + 1) * (o.smoothing + 1)))
        {
            ++removed;
            if (notes)
                notes->push_back(note(component, g.w, sheet));
            for (int i : component)
                c.solid[i] = 0;
        }
    }
    return removed;
}
std::vector<float> luminance(const raster& r, int smoothing)
{
    std::vector<float> gray(size_t(r.width) * r.height);
    for (size_t i = 0; i < gray.size(); ++i)
        gray[i] = (.2126f * r.rgba[i * 4] + .7152f * r.rgba[i * 4 + 1] + .0722f * r.rgba[i * 4 + 2]) / 255.f;
    std::vector<float> temp(gray.size());
    for (int pass = 0; pass < smoothing; ++pass)
    {
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x)
            {
                float sum = 0;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = std::clamp(x + dx, 0, r.width - 1), yy = std::clamp(y + dy, 0, r.height - 1);
                        sum += gray[yy * r.width + xx];
                        ++count;
                    }
                temp[y * r.width + x] = sum / count;
            }
        gray.swap(temp);
    }
    return gray;
}
// Squared Euclidean distance via separable lower envelopes of parabolas;
// retain the argmin so hidden pixels know which visible depth owns the edge.
// Algorithm: Felzenszwalb/Huttenlocher, https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf
constexpr double far_away = 1e20;
void distance_line(const std::vector<double>& cost, std::vector<double>& distance, std::vector<int>& nearest)
{
    const int n = int(cost.size());
    distance.assign(n, far_away);
    nearest.assign(n, -1);
    std::vector<int> sites(n);
    std::vector<double> starts(n + 1);
    int last = -1;
    for (int q = 0; q < n; ++q)
    {
        if (cost[q] >= far_away / 2)
            continue;
        double crossing = -far_away;
        while (last >= 0)
        {
            const int p = sites[last];
            crossing = ((cost[q] + double(q) * q) - (cost[p] + double(p) * p)) / (2. * (q - p));
            if (crossing > starts[last])
                break;
            --last;
        }
        ++last;
        sites[last] = q;
        starts[last] = last == 0 ? -far_away : crossing;
        starts[last + 1] = far_away;
    }
    if (last < 0)
        return;
    int active = 0;
    for (int p = 0; p < n; ++p)
    {
        while (active < last && starts[active + 1] < p)
            ++active;
        const int q = sites[active];
        nearest[p] = q;
        distance[p] = double(p - q) * (p - q) + cost[q];
    }
}
struct depth_distance
{
    std::vector<double> squared;
    std::vector<int> nearest;
};
depth_distance distance_to_depth(const tone_map& map, int depth)
{
    const size_t n = map.indices.size();
    std::vector<double> horizontal(n), cost, dist;
    std::vector<int> row_source(n), nearest;
    cost.resize(map.width);
    for (int y = 0; y < map.height; ++y)
    {
        for (int x = 0; x < map.width; ++x)
            cost[x] = map.indices[size_t(y) * map.width + x] >= depth ? 0 : far_away;
        distance_line(cost, dist, nearest);
        for (int x = 0; x < map.width; ++x)
        {
            const size_t p = size_t(y) * map.width + x;
            horizontal[p] = dist[x];
            row_source[p] = nearest[x] < 0 ? -1 : y * map.width + nearest[x];
        }
    }
    depth_distance out{std::vector<double>(n), std::vector<int>(n, -1)};
    cost.resize(map.height);
    for (int x = 0; x < map.width; ++x)
    {
        for (int y = 0; y < map.height; ++y)
            cost[y] = horizontal[size_t(y) * map.width + x];
        distance_line(cost, dist, nearest);
        for (int y = 0; y < map.height; ++y)
        {
            const size_t p = size_t(y) * map.width + x;
            out.squared[p] = dist[y];
            if (nearest[y] >= 0)
                out.nearest[p] = row_source[size_t(nearest[y]) * map.width + x];
        }
    }
    return out;
}
void restore_hidden_support(cut_layer& limited, const tone_map& map, int depth, frame_geometry g)
{
    const auto parts = components(limited);
    const auto attached = [&](const auto& part) { return std::ranges::any_of(part, [&](int p) { return border(p % g.w, p / g.w, g); }); };
    if (std::ranges::all_of(parts, attached))
        return;
    cut_layer cumulative{"", g.w, g.h, std::vector<uint8_t>(map.indices.size())};
    for (size_t p = 0; p < map.indices.size(); ++p)
        cumulative.solid[p] = map.indices[p] <= depth;
    const auto full = components(cumulative);
    std::vector<int> owner(map.indices.size(), -1);
    for (int i = 0; i < int(full.size()); ++i)
        for (int p : full[i])
            owner[p] = i;
    std::vector<uint8_t> restore(full.size());
    for (const auto& part : parts)
        if (!attached(part))
        {
            const int i = owner[part.front()];
            if (i >= 0 && !restore[i] && attached(full[i]))
                restore[i] = 1;
        }
    for (size_t i = 0; i < full.size(); ++i)
        if (restore[i])
            for (int p : full[i])
                if (map.indices[p] < depth)
                    limited.solid[p] = 1;
}
}  // namespace
size_t composite_mismatch(const std::vector<cut_layer>& sheets, const tone_map& map)
{
    size_t n = 0;
    for (size_t p = 0; p < map.indices.size(); ++p)
    {
        int front = -1;
        for (int i = 0; i < int(sheets.size()) && front < 0; ++i)
            if (p < sheets[size_t(i)].solid.size() && sheets[size_t(i)].solid[p])
                front = i;
        n += front != map.indices[p];
    }
    return n;
}
sheet_result build_sheets(const tone_map& input, const transform_options& o)
{
    const auto g = dimensions(o);
    if (input.width != g.w || input.height != g.h || input.indices.size() != size_t(g.w) * g.h)
        throw std::runtime_error("Tone map must match the working canvas.");
    auto map = input;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
        {
            auto& tone = map.indices[size_t(y) * g.w + x];
            if (tone >= o.layers)
                throw std::runtime_error("Tone index exceeds the layer count.");
            if (border(x, y, g))
                tone = 0;
        }
    sheet_result result;
    const double reach = o.layer_gap_mm * 640. / std::max(o.width_mm, o.height_mm);
    for (int depth = 0; depth < o.layers; ++depth)
    {
        cut_layer c{"Layer " + std::to_string(depth + 1), g.w, g.h, std::vector<uint8_t>(map.indices.size())};
        if (depth == o.layers - 1)
            std::fill(c.solid.begin(), c.solid.end(), uint8_t(1));
        else
        {
            depth_distance distance;
            if (depth > 0)
                distance = distance_to_depth(map, depth);
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                {
                    const size_t p = size_t(y) * g.w + x;
                    const int tone = map.indices[p];
                    bool solid = border(x, y, g) || tone == depth;
                    if (!solid && tone < depth && depth > 0)
                    {
                        const double radius = (depth - tone) * reach;
                        const int nearest = distance.nearest[p];
                        solid = nearest >= 0 && map.indices[nearest] == depth && distance.squared[p] <= radius * radius;
                    }
                    c.solid[p] = solid;
                }
            if (depth > 0)
                restore_hidden_support(c, map, depth, g);
        }
        result.removed_per_layer.push_back(frame_and_settle(c, o, &result.removed, depth));
        result.layers.push_back(std::move(c));
    }
#ifndef NDEBUG
    // The visible picture is locked: stacked front to back, the sheets show
    // the tone map exactly, except where pieces were removed (specks, or
    // floating card with "Remove floating pieces" on).
    size_t removed_pixels = 0;
    for (const auto& r : result.removed)
        removed_pixels += r.pixels;
    if (composite_mismatch(result.layers, map) > removed_pixels)
        throw std::logic_error("Sheets changed the visible picture beyond the pieces removed.");
#endif
    return result;
}
rect_mm opening_mm(const transform_options& o)
{
    const auto b = borders(o);
    return {b.left, b.top, o.width_mm - b.right, o.height_mm - b.bottom};
}
double working_scale(const transform_options& o)
{
    return 640. / std::max(o.width_mm, o.height_mm);
}
pixel_rect full_crop(const raster& r)
{
    return {0, 0, double(r.width), double(r.height)};
}
raster cropped(const raster& r, const pixel_rect& c)
{
    const int x0 = std::clamp(int(std::floor(c.x)), 0, r.width), y0 = std::clamp(int(std::floor(c.y)), 0, r.height);
    const int x1 = std::clamp(int(std::ceil(c.x + c.w)), x0, r.width), y1 = std::clamp(int(std::ceil(c.y + c.h)), y0, r.height);
    raster out{x1 - x0, y1 - y0, {}};
    out.rgba.reserve(size_t(out.width) * out.height * 4);
    for (int y = y0; y < y1; ++y)
    {
        const auto start = r.rgba.begin() + (size_t(y) * r.width + x0) * 4;
        out.rgba.insert(out.rgba.end(), start, start + size_t(out.width) * 4);
    }
    return out;
}
namespace
{
pixel_rect crop_of(const layer& l)
{
    const auto& c = l.transform.crop;
    return c.w > 0 && c.h > 0 ? c : full_crop(l.image);
}
constexpr double pi = 3.14159265358979323846;
}  // namespace
image_transform place_image(const layer& l, const transform_options& o, placement_mode mode, unsigned edges)
{
    const auto g = dimensions(o);
    const auto crop = crop_of(l);
    const double aw = crop.w, ah = crop.h;
    const double opening_w = g.opening_width(), opening_h = g.opening_height();
    // Working-canvas pixels: k per source pixel, the crop's top-left at (ox, oy).
    double k = std::min(opening_w / aw, opening_h / ah);
    if (mode == placement_mode::fill)
        k = std::max(opening_w / aw, opening_h / ah);
    double ox = g.left + (opening_w - aw * k) / 2, oy = g.top + (opening_h - ah * k) / 2;
    if (mode == placement_mode::touch_edges)
    {
        auto box = crop.x == 0 && crop.y == 0 && crop.w == l.image.width && crop.h == l.image.height ? detect_subject(current_pixels(l))
                                                                                                     : detect_subject(cropped(current_pixels(l), crop));
        if (!box.found)
            box = {0, 0, int(aw), int(ah), true, false};
        const double c = o.bridge_mm * working_scale(o);
        const bool left = edges & touch_left, right = edges & touch_right, top = edges & touch_top, bottom = edges & touch_bottom;
        // Fit the subject, not its transparent/background padding. Leave a
        // two-pixel gap at unselected edges so a limiting axis does not
        // accidentally touch another bar. A single contact translates the
        // subject past that edge, using the extra space outside the opening.
        const double inset_x = std::min(2., opening_w / 4), inset_y = std::min(2., opening_h / 4);
        const double available_w = opening_w + (left ? c : -inset_x) + (right ? c : -inset_x);
        const double available_h = opening_h + (top ? c : -inset_y) + (bottom ? c : -inset_y);
        k = std::min(available_w / (box.right - box.left), available_h / (box.bottom - box.top));
        if (left && right)
            k = (opening_w + 2 * c) / (box.right - box.left);
        if (top && bottom)
        {
            const double vertical = (opening_h + 2 * c) / (box.bottom - box.top);
            k = left && right ? std::max(k, vertical) : vertical;
        }
        ox = g.left + opening_w / 2 - (box.left + box.right) * k / 2;
        oy = g.top + opening_h / 2 - (box.top + box.bottom) * k / 2;
        if (left && !right)
            ox = g.left - c - box.left * k;
        else if (right && !left)
            ox = g.w - g.right + c - box.right * k;
        if (top && !bottom)
            oy = g.top - c - box.top * k;
        else if (bottom && !top)
            oy = g.h - g.bottom + c - box.bottom * k;
    }
    const double s = working_scale(o);
    image_transform t;
    t.crop = crop;
    t.mm_per_pixel = k / s;
    t.x_mm = (ox + aw * k / 2) / s;
    t.y_mm = (oy + ah * k / 2) / s;
    return t;
}
image_transform effective_transform(const layer& l, const transform_options& o)
{
    if (l.transform.placed())
    {
        auto t = l.transform;
        if (!(t.crop.w > 0 && t.crop.h > 0))
            t.crop = full_crop(l.image);
        return t;
    }
    return place_image(l, o, o.placement, o.touch_edges);
}
point_mm to_frame(const image_transform& t, double u, double v)
{
    const double a = t.rotation_deg * pi / 180, cs = std::cos(a), sn = std::sin(a);
    const double lx = (u - (t.crop.x + t.crop.w / 2)) * t.mm_per_pixel, ly = (v - (t.crop.y + t.crop.h / 2)) * t.mm_per_pixel;
    return {t.x_mm + lx * cs - ly * sn, t.y_mm + lx * sn + ly * cs};
}
point_mm to_source(const image_transform& t, point_mm p)
{
    const double a = t.rotation_deg * pi / 180, cs = std::cos(a), sn = std::sin(a);
    const double dx = p.x - t.x_mm, dy = p.y - t.y_mm;
    const double lx = dx * cs + dy * sn, ly = -dx * sn + dy * cs;
    return {lx / t.mm_per_pixel + t.crop.x + t.crop.w / 2, ly / t.mm_per_pixel + t.crop.y + t.crop.h / 2};
}
std::array<point_mm, 4> corners(const image_transform& t)
{
    const auto& c = t.crop;
    return {to_frame(t, c.x, c.y), to_frame(t, c.x + c.w, c.y), to_frame(t, c.x + c.w, c.y + c.h), to_frame(t, c.x, c.y + c.h)};
}
rect_mm bounds(const image_transform& t)
{
    const auto c = corners(t);
    rect_mm r{c[0].x, c[0].y, c[0].x, c[0].y};
    for (const auto& p : c)
    {
        r.left = std::min(r.left, p.x);
        r.right = std::max(r.right, p.x);
        r.top = std::min(r.top, p.y);
        r.bottom = std::max(r.bottom, p.y);
    }
    return r;
}
double width_mm(const image_transform& t)
{
    return t.crop.w * t.mm_per_pixel;
}
double height_mm(const image_transform& t)
{
    return t.crop.h * t.mm_per_pixel;
}
bool contains(const image_transform& t, point_mm p)
{
    if (!t.placed())
        return false;
    const auto s = to_source(t, p);
    return s.x >= t.crop.x && s.x < t.crop.x + t.crop.w && s.y >= t.crop.y && s.y < t.crop.y + t.crop.h;
}
int hit_test(const std::vector<layer>& layers, const transform_options& o, point_mm p)
{
    for (int i = int(layers.size()) - 1; i >= 0; --i)
        if (layers[i].visible && layers[i].image.valid() && contains(effective_transform(layers[i], o), p))
            return i;
    return -1;
}
image_transform with_crop(const image_transform& t, pixel_rect crop)
{
    auto out = t;
    out.crop = crop;
    // The new crop's centre, wherever it was in the frame, becomes the centre.
    const auto centre = to_frame(t, crop.x + crop.w / 2, crop.y + crop.h / 2);
    out.x_mm = centre.x;
    out.y_mm = centre.y;
    return out;
}
image_transform scaled_about(const image_transform& t, double factor, point_mm anchor)
{
    auto out = t;
    out.mm_per_pixel *= factor;
    out.x_mm = anchor.x + (t.x_mm - anchor.x) * factor;
    out.y_mm = anchor.y + (t.y_mm - anchor.y) * factor;
    return out;
}
image_transform fit_to_opening(const image_transform& t, const transform_options& o)
{
    const auto open = opening_mm(o);
    const auto b = bounds(t);
    const double factor = std::min(open.width() / b.width(), open.height() / b.height());
    auto out = t;
    out.mm_per_pixel *= factor;
    out.x_mm = (open.left + open.right) / 2;
    out.y_mm = (open.top + open.bottom) / 2;
    return out;
}
image_transform touch(const layer& l, const transform_options& o, touch_edge edge)
{
    auto t = effective_transform(l, o);
    // The subject's box within the crop, in source pixels; else the crop.
    auto box = detect_subject(cropped(current_pixels(l), t.crop));
    pixel_rect subject = t.crop;
    if (box.found)
        subject = {std::floor(t.crop.x) + box.left, std::floor(t.crop.y) + box.top, double(box.right - box.left), double(box.bottom - box.top)};
    // Bounds of the subject rectangle in the frame at the image's transform.
    const auto c = std::array{to_frame(t, subject.x, subject.y), to_frame(t, subject.x + subject.w, subject.y),
                              to_frame(t, subject.x + subject.w, subject.y + subject.h), to_frame(t, subject.x, subject.y + subject.h)};
    rect_mm r{c[0].x, c[0].y, c[0].x, c[0].y};
    for (const auto& p : c)
    {
        r.left = std::min(r.left, p.x);
        r.right = std::max(r.right, p.x);
        r.top = std::min(r.top, p.y);
        r.bottom = std::max(r.bottom, p.y);
    }
    const auto open = opening_mm(o);
    const double overlap = o.bridge_mm;
    if (edge == touch_bottom)
        t.y_mm += open.bottom + overlap - r.bottom;
    else if (edge == touch_top)
        t.y_mm += open.top - overlap - r.top;
    else if (edge == touch_left)
        t.x_mm += open.left - overlap - r.left;
    else if (edge == touch_right)
        t.x_mm += open.right + overlap - r.right;
    return t;
}
snap_result snap_to_opening(rect_mm box, rect_mm opening, double threshold_mm, unsigned edges)
{
    snap_result out;
    const auto axis = [&](double lo, double hi, double open_lo, double open_hi, unsigned lo_bit, unsigned hi_bit, double& delta, std::optional<double>& guide)
    {
        double best = threshold_mm;
        for (const auto [edge, bit] : {std::pair{lo, lo_bit}, std::pair{hi, hi_bit}})
        {
            if (!(edges & bit))
                continue;
            for (double target : {open_lo, open_hi})
                if (const double d = target - edge; std::abs(d) <= best)
                {
                    best = std::abs(d);
                    delta = d;
                    guide = target;
                }
        }
    };
    axis(box.left, box.right, opening.left, opening.right, touch_left, touch_right, out.dx, out.guide_x);
    axis(box.top, box.bottom, opening.top, opening.bottom, touch_top, touch_bottom, out.dy, out.guide_y);
    return out;
}
raster flatten(const std::vector<source_image>& sources, const transform_options& o)
{
    const auto g = dimensions(o);
    raster r{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h * 4, 255)};
    const double s = working_scale(o);
    // Layers stack bottom to top, each through its own transform. Sampling
    // runs over destination pixels only, so whatever lies outside the frame
    // is cropped by it.
    for (const auto& layer : sources)
    {
        if (!layer.visible || !layer.image.valid())
            continue;
        const auto& a = current_pixels(layer);
        const auto t = effective_transform(layer, o);
        const double angle = t.rotation_deg * pi / 180, cs = std::cos(angle), sn = std::sin(angle);
        // Source position = origin + x·ex + y·ey for working pixel centres.
        const double k = 1 / (t.mm_per_pixel * s);
        const double ccx = t.crop.x + t.crop.w / 2, ccy = t.crop.y + t.crop.h / 2;
        const double ex_u = cs * k, ex_v = -sn * k, ey_u = sn * k, ey_v = cs * k;
        const double cx = t.x_mm * s, cy = t.y_mm * s;
        const auto b = bounds(t);
        const int x0 = std::clamp(int(std::floor(b.left * s)) - 1, 0, g.w), x1 = std::clamp(int(std::ceil(b.right * s)) + 1, 0, g.w);
        const int y0 = std::clamp(int(std::floor(b.top * s)) - 1, 0, g.h), y1 = std::clamp(int(std::ceil(b.bottom * s)) + 1, 0, g.h);
        const double crop_r = t.crop.x + t.crop.w, crop_b = t.crop.y + t.crop.h;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const double dx = x + .5 - cx, dy = y + .5 - cy;
                const double u = ccx + dx * ex_u + dy * ey_u, v = ccy + dx * ex_v + dy * ey_v;
                if (u < t.crop.x || v < t.crop.y || u >= crop_r || v >= crop_b)
                    continue;
                const int sx = int(std::floor(u)), sy = int(std::floor(v));
                if (sx < 0 || sy < 0 || sx >= a.width || sy >= a.height)
                    continue;
                const size_t si = (size_t(sy) * a.width + sx) * 4;
                const size_t di = (size_t(y) * g.w + x) * 4;
                const int alpha = a.rgba[si + 3];
                for (int ch = 0; ch < 3; ++ch)
                    r.rgba[di + ch] = uint8_t((a.rgba[si + ch] * alpha + r.rgba[di + ch] * (255 - alpha) + 127) / 255);
            }
    }
    return r;
}
raster source_preview(const std::vector<source_image>& sources, const transform_options& o)
{
    auto r = flatten(sources, o);
    const auto g = dimensions(o);
    // A tinted card border makes the opening visible while allowing overlapped
    // subject pixels to show through faintly as a placement guide.
    const uint8_t card[] = {184, 178, 155};
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (border(x, y, g))
                for (int ch = 0; ch < 3; ++ch)
                {
                    auto& value = r.rgba[(size_t(y) * g.w + x) * 4 + ch];
                    value = uint8_t((2 * card[ch] + value) / 3);
                }
    return r;
}
namespace
{
sheet_result simplified(const std::vector<source_image>& sources, const transform_options& o);
}
std::vector<cut_layer> simplify(const std::vector<source_image>& sources, const transform_options& o)
{
    return simplified(sources, o).layers;
}
namespace
{
sheet_result simplified(const std::vector<source_image>& sources, const transform_options& o)
{
    if (std::ranges::none_of(sources, [](const auto& s) { return s.visible && s.image.valid(); }))
        throw std::runtime_error("Add at least one visible image.");
    auto r = flatten(sources, o);
    // The frame is card whatever's under it. Fill it with the artwork's edge
    // before smoothing, so blur doesn't fade a shape where it meets the frame
    // and cut it loose.
    const auto g = dimensions(o);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (border(x, y, g))
            {
                const int sx = std::clamp(x, g.left, g.w - g.right - 1), sy = std::clamp(y, g.top, g.h - g.bottom - 1);
                std::copy_n(r.rgba.begin() + (size_t(sy) * g.w + sx) * 4, 4, r.rgba.begin() + (size_t(y) * g.w + x) * 4);
            }
    const auto gray = luminance(r, o.smoothing);
    tone_map map{r.width, r.height, std::vector<uint8_t>(gray.size())};
    for (size_t i = 0; i < gray.size(); ++i)
        map.indices[i] = uint8_t(std::clamp(int(gray[i] * o.layers), 0, o.layers - 1));
    return build_sheets(map, o);
}
}  // namespace
std::vector<cut_layer> masks_from_images(const std::vector<source_image>& images, const transform_options& o)
{
    validate(o);
    if (images.empty() || images.size() > 32)
        throw std::runtime_error("Import between 1 and 32 layer images.");
    std::vector<cut_layer> cuts;
    for (const auto& image : images)
    {
        if (!image.image.valid())
            throw std::runtime_error("Invalid layer image.");
        const auto g = dimensions(o);
        const auto& input = image.image;
        const double ratio = double(input.width) / input.height;
        if (std::abs(ratio - o.width_mm / o.height_mm) > .02 * ratio)
            throw std::runtime_error("Layer image aspect ratio must match the project's overall dimensions.");
        // AI masks already describe the complete canvas, frame included.
        // Resample edge to edge; fitting inside the frame would shrink twice.
        raster r{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h * 4, 255)};
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
            {
                const size_t si = (size_t(y) * input.height / g.h * input.width + size_t(x) * input.width / g.w) * 4;
                const size_t di = (size_t(y) * g.w + x) * 4;
                const int alpha = input.rgba[si + 3];
                for (int ch = 0; ch < 3; ++ch)
                    r.rgba[di + ch] = uint8_t((input.rgba[si + ch] * alpha + 255 * (255 - alpha) + 127) / 255);
            }
        const auto gray = luminance(r, o.smoothing);
        cut_layer c{image.name, r.width, r.height, std::vector<uint8_t>(gray.size())};
        for (size_t i = 0; i < gray.size(); ++i)
            c.solid[i] = gray[i] < .5f;
        frame_and_settle(c, o);
        cuts.push_back(std::move(c));
    }
    return cuts;
}
namespace
{
struct perceptual_colour
{
    float l, a, b, y;
};
perceptual_colour illustration_colour(const raster& image, size_t pixel)
{
    static const auto linear = []
    {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i)
        {
            const double c = i / 255.;
            table[i] = float(c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4));
        }
        return table;
    }();
    const size_t p = pixel * 4;
    const int alpha = image.rgba[p + 3];
    const float r = linear[(image.rgba[p] * alpha + 255 * (255 - alpha) + 127) / 255];
    const float g = linear[(image.rgba[p + 1] * alpha + 255 * (255 - alpha) + 127) / 255];
    const float b = linear[(image.rgba[p + 2] * alpha + 255 * (255 - alpha) + 127) / 255];
    // Björn Ottosson's public-domain linear-sRGB → OKLab matrices:
    // https://bottosson.github.io/posts/oklab/
    const float ll = std::cbrt(.4122214708f * r + .5363325363f * g + .0514459929f * b);
    const float mm = std::cbrt(.2119034982f * r + .6806995451f * g + .1073969566f * b);
    const float ss = std::cbrt(.0883024619f * r + .2817188376f * g + .6299787005f * b);
    return {.2104542553f * ll + .7936177850f * mm - .0040720468f * ss, 1.9779984951f * ll - 2.4285922050f * mm + .4505937099f * ss,
            .0259040371f * ll + .7827717662f * mm - .8086757660f * ss, .2126f * r + .7152f * g + .0722f * b};
}
// Shadow-aware: hue and chroma weigh more than lightness, so a drop shadow
// or a darker band where planes overlap (rendered depth cues image models
// may draw) stays with the region of its hue instead of becoming a tone.
// Hook for later: give a darker boundary band to the far side's region.
double colour_distance(perceptual_colour a, perceptual_colour b)
{
    const double dl = a.l - b.l, da = a.a - b.a, db = a.b - b.b;
    return .5 * dl * dl + 2 * (da * da + db * db);
}
int nearest_tone(perceptual_colour colour, const std::vector<perceptual_colour>& centres)
{
    int nearest = 0;
    double distance = colour_distance(colour, centres.front());
    for (int i = 1; i < int(centres.size()); ++i)
        if (const double d = colour_distance(colour, centres[i]); d < distance)
        {
            distance = d;
            nearest = i;
        }
    return nearest;
}
// The tones a picture really has: colours (OKLab, merged within a just
// noticeable step for flat art) that cover at least 1% of it.
int count_tones(const raster& image)
{
    const size_t pixels = size_t(image.width) * image.height;
    const size_t step = std::max(size_t(1), (pixels + 32767) / 32768);
    std::map<std::array<int, 3>, size_t> bins;
    size_t samples = 0;
    for (size_t i = 0; i < pixels; i += step, ++samples)
    {
        const auto c = illustration_colour(image, i);
        ++bins[{int(std::lround(c.l * 40)), int(std::lround(c.a * 40)), int(std::lround(c.b * 40))}];
    }
    std::vector<std::pair<size_t, std::array<int, 3>>> ranked;
    for (const auto& [bin, n] : bins)
        ranked.emplace_back(n, bin);
    std::ranges::sort(ranked, std::greater{});
    std::vector<std::pair<std::array<int, 3>, size_t>> clusters;
    for (const auto& [n, bin] : ranked)
    {
        auto near = std::ranges::find_if(clusters,
                                         [&](const auto& c)
                                         {
                                             const int dl = c.first[0] - bin[0], da = c.first[1] - bin[1], db = c.first[2] - bin[2];
                                             return .5 * dl * dl + 2. * (da * da + db * db) <= 9;  // as colour_distance weighs them: shading is not a tone
                                         });
        if (near == clusters.end())
            clusters.push_back({bin, n});
        else
            near->second += n;
    }
    return int(std::ranges::count_if(clusters, [&](const auto& c) { return c.second * 100 >= samples; }));
}
std::vector<uint8_t> separate_tones(const raster& image, int count, const std::vector<std::array<uint8_t, 3>>& palette = {})
{
    const size_t pixels = size_t(image.width) * image.height;
    // Deterministic stratified samples keep k-means bounded for large inputs,
    // without repeatedly sampling the same columns of a wide image.
    const size_t step = std::max(size_t(1), (pixels + 32767) / 32768);
    std::vector<perceptual_colour> sample;
    for (size_t start = 0, bucket = 0; start < pixels; start += step, ++bucket)
    {
        const size_t offset = (uint32_t(bucket) * 2654435761u) % step;
        sample.push_back(illustration_colour(image, std::min(pixels - 1, start + offset)));
    }
    std::stable_sort(sample.begin(), sample.end(), [](auto a, auto b) { return a.y < b.y; });
    std::vector<perceptual_colour> centres;
    // A supplied palette (the plan's) is snapped to as it is.
    if (!palette.empty())
    {
        count = int(palette.size());
        for (const auto& c : palette)
        {
            const raster one{1, 1, {c[0], c[1], c[2], 255}};
            centres.push_back(illustration_colour(one, 0));
        }
    }
    const auto farthest = [&](const std::vector<perceptual_colour>& palette)
    {
        size_t best = 0;
        double worst = -1;
        for (size_t i = 0; i < sample.size(); ++i)
        {
            double distance = far_away;
            for (const auto& c : palette)
                distance = std::min(distance, colour_distance(sample[i], c));
            if (distance > worst)
            {
                worst = distance;
                best = i;
            }
        }
        return sample[best];
    };
    for (int i = int(centres.size()); i < count; ++i)
    {
        auto seed = sample[std::min(sample.size() - 1, (size_t(2 * i + 1) * sample.size()) / size_t(2 * count))];
        // Quantiles may land repeatedly in a dominant colour, especially with
        // slight noise. Spread near-duplicates to the least represented colour.
        if (std::ranges::any_of(centres, [&](auto c) { return colour_distance(seed, c) < .0004; }))
            seed = farthest(centres);
        centres.push_back(seed);
    }
    for (int iteration = 0; iteration < (palette.empty() ? 24 : 0); ++iteration)
    {
        struct sum
        {
            double l = 0, a = 0, b = 0, y = 0;
            size_t n = 0;
        };
        std::vector<sum> sums(count);
        for (const auto& p : sample)
        {
            auto& s = sums[nearest_tone(p, centres)];
            s.l += p.l;
            s.a += p.a;
            s.b += p.b;
            s.y += p.y;
            ++s.n;
        }
        double movement = 0;
        for (int i = 0; i < count; ++i)
        {
            const auto& s = sums[i];
            perceptual_colour next;
            if (s.n)
                next = {float(s.l / s.n), float(s.a / s.n), float(s.b / s.n), float(s.y / s.n)};
            else
                next = farthest(centres);
            movement = std::max(movement, colour_distance(next, centres[i]));
            centres[i] = next;
        }
        if (movement < 1e-10)
            break;
    }
    std::vector<uint8_t> indices(pixels), exact(pixels);
    std::vector<double> luminance_sum(count);
    std::vector<size_t> populations(count);
    for (size_t p = 0; p < pixels; ++p)
    {
        const auto colour = illustration_colour(image, p);
        const int tone = nearest_tone(colour, centres);
        indices[p] = uint8_t(tone);
        // Already its tone's colour: part of the flat picture, which cleanup
        // must not change (the visible picture is locked).
        exact[p] = colour_distance(colour, centres[tone]) < 1e-5;
        luminance_sum[tone] += colour.y;
        ++populations[tone];
    }
    std::vector<int> order(count), rank(count);
    std::iota(order.begin(), order.end(), 0);
    const auto brightness = [&](int i) { return populations[i] ? luminance_sum[i] / populations[i] : centres[i].y; };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return brightness(a) < brightness(b); });
    for (int i = 0; i < count; ++i)
        rank[order[i]] = i;
    for (auto& value : indices)
        value = uint8_t(rank[value]);
    // Two 3×3 mode passes remove isolated noise and one-pixel fringe tones.
    // Straight real boundaries keep their majority; sliver ties prefer the
    // darker neighbour so cleanup does not create a separate intermediate band.
    std::vector<uint8_t> cleaned(indices.size());
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
            {
                std::array<int, 12> votes{};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && xx < image.width && yy >= 0 && yy < image.height)
                            ++votes[indices[size_t(yy) * image.width + xx]];
                    }
                const size_t p = size_t(y) * image.width + x;
                const int current = indices[p];
                if (exact[p])
                {
                    cleaned[p] = uint8_t(current);
                    continue;
                }
                int best = current;
                for (int i = 0; i < count; ++i)
                    if (votes[i] > votes[best] || (votes[i] == votes[best] && votes[current] <= 3 && i < best))
                        best = i;
                cleaned[p] = uint8_t(votes[best] >= 3 ? best : current);
            }
        indices.swap(cleaned);
    }
    return indices;
}
}  // namespace
illustration_result layers_from_illustration(const raster& image, const transform_options& o, const std::vector<std::array<uint8_t, 3>>& palette,
                                             bool dissolve)
{
    const auto g = dimensions(o);
    if (!image.valid())
        throw std::runtime_error("Invalid flat illustration.");
    if (!palette.empty() && int(palette.size()) != o.layers)
        throw std::runtime_error("The palette has a colour per sheet.");
    const auto source = separate_tones(image, o.layers, palette);
    const double scale = std::max(double(g.w) / image.width, double(g.h) / image.height);
    const double visible_w = g.w / scale, visible_h = g.h / scale;
    const double left = (image.width - visible_w) / 2, top = (image.height - visible_h) / 2;
    tone_map map{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h)};
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
        {
            const int sx = std::clamp(int(std::floor(left + (x + .5) / scale)), 0, image.width - 1);
            const int sy = std::clamp(int(std::floor(top + (y + .5) / scale)), 0, image.height - 1);
            map.indices[size_t(y) * g.w + x] = source[size_t(sy) * image.width + sx];
        }
    const size_t dissolved = dissolve ? dissolve_outlines(map, o) : 0;
    auto built = build_sheets(map, o);
    illustration_result result;
    result.outline_pixels = dissolved;
    result.layers = std::move(built.layers);
    result.removed_per_layer = std::move(built.removed_per_layer);
    result.removed = std::move(built.removed);
    result.cropped_fraction = std::clamp(1 - visible_w * visible_h / (double(image.width) * image.height), 0., 1.);
    result.tones_found = count_tones(image);
    return result;
}
void apply_illustration(project& r, const raster& image)
{
    // An AI result keeps its floating pieces: they show as issues instead of
    // vanishing (the first real runs lost 26–84 of them silently).
    auto o = r.options;
    o.connect = false;
    auto imported = layers_from_illustration(image, o, {}, true);
    const size_t removed = std::accumulate(imported.removed_per_layer.begin(), imported.removed_per_layer.end(), size_t(0));
    r.activity = std::format("Imported an illustration: {} tones found, {} wanted.", imported.tones_found, r.options.layers);
    if (removed)
    {
        size_t remaining = 0;
        for (const auto& layer : imported.layers)
            remaining += disconnected_components(layer);
        if (remaining)
            r.activity += std::format(" {} floating pieces, marked red in Sheets.", remaining);
        std::string layers;
        for (size_t i = 0; i < imported.removed_per_layer.size(); ++i)
            if (imported.removed_per_layer[i])
            {
                if (!layers.empty())
                    layers += ", ";
                layers += std::format("{:02}", i + 1);
            }
        r.activity += std::format(" Removed {} specks from layers {}.", removed, layers);
    }
    else
    {
        size_t remaining = 0;
        for (const auto& layer : imported.layers)
            remaining += disconnected_components(layer);
        r.activity += remaining ? std::format(" {} floating pieces, marked red in Sheets.", remaining) : " Every piece reaches the frame.";
    }
    if (imported.cropped_fraction > .03)
        r.activity += std::format(" Cropped {:.0f}% to fit the frame.", imported.cropped_fraction * 100);
    r.cuts = std::move(imported.layers);
    r.selected_cut = 0;
    r.view = view_mode::assembled;
    r.stale = false;
    r.dirty = true;
}
std::array<uint8_t, 3> sheet_colour(int sheet, int count, bool dimmed)
{
    const float t = float(sheet) / std::max(1, count - 1);
    std::array<uint8_t, 3> c{uint8_t(38 + 189 * t), uint8_t(64 + 156 * t), uint8_t(69 + 123 * t)};
    // Dimmed sheets fade most of the way to the paper backdrop, keeping a
    // trace of their depth order.
    if (dimmed)
        for (auto& v : c)
            v = uint8_t((v + 4 * 238) / 5);
    return c;
}
raster render_cuts(const std::vector<cut_layer>& cuts, cut_view view)
{
    if (cuts.empty())
        return {};
    const auto& first = cuts.front();
    const int n = int(cuts.size());
    raster r{first.width, first.height, std::vector<uint8_t>(first.solid.size() * 4, 255)};
    const int highlight = view.single < 0 && view.highlight >= 0 && view.highlight < n ? view.highlight : -1;
    // Card that reaches no edge of the sheet cannot hold itself up.
    std::vector<std::vector<uint8_t>> floating(view.floating ? n : 0);
    for (int layer = 0; layer < int(floating.size()); ++layer)
    {
        const auto& c = cuts[layer];
        floating[layer].assign(c.solid.size(), 0);
        for (const auto& part : components(c))
            if (std::ranges::none_of(part, [&](int i)
                                     { return i % c.width == 0 || i / c.width == 0 || i % c.width == c.width - 1 || i / c.width == c.height - 1; }))
                for (int i : part)
                    floating[layer][i] = 1;
    }
    for (size_t i = 0; i < first.solid.size(); ++i)
    {
        // The frontmost shown sheet covers everything behind it.
        int top = -1;
        for (int layer = 0; layer < n; ++layer)
        {
            const auto& c = cuts[layer];
            if (view.single >= 0 ? layer != view.single : !c.visible)
                continue;
            if (c.solid[i])
            {
                top = layer;
                break;
            }
        }
        if (top < 0)
            continue;
        auto colour = sheet_colour(top, n, highlight >= 0 && top != highlight);
        if (!floating.empty() && floating[top][i])
        {
            colour = floating_colour;
            if (highlight >= 0 && top != highlight)
                for (auto& v : colour)
                    v = uint8_t((v + 4 * 238) / 5);
        }
        std::copy(colour.begin(), colour.end(), r.rgba.begin() + 4 * i);
    }
    if (highlight >= 0)
    {
        // The highlighted sheet's whole outline, including where sheets in
        // front of it hide it, so its shape reads within the stack.
        const auto& c = cuts[highlight];
        for (int y = 0; y < c.height; ++y)
            for (int x = 0; x < c.width; ++x)
            {
                const size_t i = size_t(y) * c.width + x;
                if (!c.solid[i])
                    continue;
                // Two pixels wide, inside the shape, so it reads at any zoom.
                bool edge = false;
                for (int d = 1; d <= 2 && !edge; ++d)
                    edge = (x >= d && !c.solid[i - d]) || (x + d < c.width && !c.solid[i + d]) || (y >= d && !c.solid[i - size_t(d) * c.width]) ||
                           (y + d < c.height && !c.solid[i + size_t(d) * c.width]);
                if (edge)
                    std::copy(highlight_outline.begin(), highlight_outline.end(), r.rgba.begin() + 4 * i);
            }
    }
    return r;
}
bool frame_only(const cut_layer& c, const transform_options& o)
{
    const auto g = dimensions(o);
    if (c.width != g.w || c.height != g.h)
        return false;
    for (int y = g.top; y < g.h - g.bottom; ++y)
        for (int x = g.left; x < g.w - g.right; ++x)
            if (c.solid[size_t(y) * g.w + x])
                return false;
    return true;
}
size_t disconnected_components(const cut_layer& c)
{
    size_t n = 0;
    for (const auto& part : components(c))
        if (std::ranges::none_of(part,
                                 [&](int i) { return i % c.width == 0 || i / c.width == 0 || i % c.width == c.width - 1 || i / c.width == c.height - 1; }))
            ++n;
    return n;
}
namespace
{
// Opening by a k × k square: erode (all of the window set), then dilate
// back. Outside the canvas counts as set, as the frame is card.
std::vector<uint8_t> opened_square(const std::vector<uint8_t>& mask, int w, int h, int k)
{
    const int a = k / 2, b = k - 1 - a;
    std::vector<int> run(size_t(std::max(w, h)) + 1);
    const auto pass = [&](const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool rows, bool erode)
    {
        const int lines = rows ? h : w, length = rows ? w : h;
        const int before = erode ? a : b, after = erode ? b : a;
        for (int line = 0; line < lines; ++line)
        {
            const auto at = [&](int i) { return rows ? size_t(line) * w + i : size_t(i) * w + line; };
            run[0] = 0;
            for (int i = 0; i < length; ++i)
                run[i + 1] = run[i] + (in[at(i)] ? 1 : 0);
            for (int i = 0; i < length; ++i)
            {
                const int lo = std::max(i - before, 0), hi = std::min(i + after, length - 1);
                const int count = run[hi + 1] - run[lo];
                out[at(i)] = erode ? count == hi - lo + 1 : count > 0;
            }
        }
    };
    std::vector<uint8_t> temp(mask.size()), eroded(mask.size()), opened(mask.size());
    pass(mask, temp, true, true);
    pass(temp, eroded, false, true);
    pass(eroded, temp, true, false);
    pass(temp, opened, false, false);
    return opened;
}
}  // namespace
size_t dissolve_outlines(tone_map& map, const transform_options& o)
{
    const auto g = dimensions(o);
    if (map.width != g.w || map.height != g.h)
        return 0;
    const int w = map.width, h = map.height;
    // Dark strokes: the darkest tone where a square 3 × Min. width wide
    // doesn't fit, inside the opening.
    const int k = std::max(3, int(std::lround(3 * o.bridge_mm * working_scale(o))));
    std::vector<uint8_t> dark(map.indices.size());
    for (size_t i = 0; i < dark.size(); ++i)
        dark[i] = map.indices[i] == 0;
    const auto wide = opened_square(dark, w, h, k);
    std::vector<uint8_t> stroke(dark.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            stroke[i] = dark[i] && !wide[i] && !border(x, y, g);
        }
    // Each stroke pixel takes the tone of the nearest pixel that isn't
    // stroke: a stroke between two fills splits down the middle; one along a
    // large dark shape joins it. A breadth-first flood from every non-stroke
    // pixel finds the nearest (4-connected).
    std::vector<int> queue;
    queue.reserve(dark.size());
    std::vector<uint8_t> tone = map.indices, done(dark.size());
    for (size_t i = 0; i < dark.size(); ++i)
        if (!stroke[i])
        {
            done[i] = 1;
            queue.push_back(int(i));
        }
    size_t changed = 0;
    for (size_t q = 0; q < queue.size(); ++q)
    {
        const int i = queue[q], x = i % w, y = i / w;
        for (int j : {x > 0 ? i - 1 : -1, x + 1 < w ? i + 1 : -1, y > 0 ? i - w : -1, y + 1 < h ? i + w : -1})
            if (j >= 0 && !done[j])
            {
                done[j] = 1;
                tone[j] = tone[i];
                changed += tone[j] != 0;
                queue.push_back(j);
            }
    }
    map.indices.swap(tone);
    return changed;
}
std::vector<piece_note> thin_features(const cut_layer& c, const transform_options& o, int sheet)
{
    const auto g = dimensions(o);
    if (c.width != g.w || c.height != g.h)
        return {};
    const int k = std::max(2, int(std::lround(o.bridge_mm * working_scale(o))));
    const int w = c.width, h = c.height;
    const auto opened = opened_square(c.solid, w, h, k);
    cut_layer thin{"", w, h, std::vector<uint8_t>(c.solid.size())};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            thin.solid[i] = c.solid[i] && !opened[i] && !border(x, y, g);
        }
    std::vector<piece_note> out;
    for (const auto& part : components(thin))
        if (part.size() >= size_t(k) * k)
            out.push_back(note(part, w, sheet));
    return out;
}
validation validate_sheets(const std::vector<cut_layer>& cuts, const transform_options& o, const std::vector<piece_note>& removed)
{
    validation v;
    const int k = std::max(2, int(std::lround(o.bridge_mm * working_scale(o))));
    for (const auto& r : removed)
        if (r.pixels >= size_t(k) * k)
            v.floating.push_back(r);
    for (int i = 0; i < int(cuts.size()); ++i)
    {
        const auto& c = cuts[size_t(i)];
        for (const auto& part : components(c))
            if (part.size() >= size_t(k) * k &&
                std::ranges::none_of(part, [&](int p) { return p % c.width == 0 || p / c.width == 0 || p % c.width == c.width - 1 || p / c.width == c.height - 1; }))
                v.floating.push_back(note(part, c.width, i));
        if (i + 1 < int(cuts.size()))  // the backing is all card
            for (auto& t : thin_features(c, o, i))
                v.thin.push_back(t);
    }
    return v;
}
namespace
{
// "sheets 1 and 2, mostly at the top left and the top"
std::string where(const std::vector<piece_note>& pieces, const transform_options& o)
{
    const auto g = dimensions(o);
    std::vector<int> sheets;
    std::map<std::string, int> places;
    static const char* names[3][3] = {{"the top left", "the top", "the top right"},
                                      {"the left", "the middle", "the right"},
                                      {"the bottom left", "the bottom", "the bottom right"}};
    for (const auto& p : pieces)
    {
        if (std::ranges::find(sheets, p.sheet) == sheets.end())
            sheets.push_back(p.sheet);
        const int col = std::clamp(int(p.x * 3 / g.w), 0, 2), row = std::clamp(int(p.y * 3 / g.h), 0, 2);
        ++places[names[row][col]];
    }
    std::ranges::sort(sheets);
    std::string s = sheets.size() == 1 ? "sheet " : "sheets ";
    for (size_t i = 0; i < sheets.size(); ++i)
        s += (i == 0 ? "" : i + 1 == sheets.size() ? " and " : ", ") + std::to_string(sheets[i] + 1);
    std::vector<std::pair<int, std::string>> ranked;
    for (const auto& [name, n] : places)
        ranked.emplace_back(-n, name);
    std::ranges::sort(ranked);
    if (ranked.size() > 2)
        ranked.resize(2);
    s += pieces.size() == 1 ? ", at " : ranked.size() == 1 ? ", all at " : ", mostly at ";
    for (size_t i = 0; i < ranked.size(); ++i)
        s += (i ? " and " : "") + ranked[i].second;
    return s;
}
}  // namespace
std::string describe_findings(const validation& v, const transform_options& o)
{
    std::string out;
    const auto mm = std::format("{:g} mm", o.bridge_mm);
    const bool one = v.floating.size() == 1;
    if (!v.floating.empty())
        out += std::format("{} {} of card would float, joined to nothing on {} sheet ({}): join {} to the frame or to same-colour shapes, or leave {} out.",
                           v.floating.size(), one ? "piece" : "pieces", one ? "its" : "their", where(v.floating, o), one ? "it" : "them",
                           one ? "it" : "them");
    if (!v.thin.empty())
        out += std::format("{}{} {} thinner than {} ({}), such as outlines: thicken them or leave them out.", out.empty() ? "" : " ", v.thin.size(),
                           v.thin.size() == 1 ? "line or detail is" : "lines or details are", mm, where(v.thin, o));
    if (v.tone_mismatch())
        out += std::format("{}The picture has {} distinct colours; it needs exactly {}.", out.empty() ? "" : " ", v.tones_found, v.tones_wanted);
    return out;
}
double validation::score(const transform_options& o) const
{
    const double px_mm = working_scale(o);
    double area = 0;
    for (const auto& f : floating)
        area += f.pixels / (px_mm * px_mm);
    return std::round(area) * 1000 + double(thin.size()) * 10 + std::max(0, tones_found - tones_wanted);
}
std::string measurements_json(const validation& v, const transform_options& o)
{
    const auto g = dimensions(o);
    const double s = working_scale(o);
    const auto b = borders(o);
    std::string out = std::format(
        "{{\"frame_mm\":[{:g},{:g}],\"opening_mm\":[{:g},{:g}],\"min_width_mm\":{:g},\"canvas_px\":[{},{}],\"tones\":{{\"found\":{},\"wanted\":{}}},"
        "\"score\":{:g},\"findings\":[",
        o.width_mm, o.height_mm, o.width_mm - b.left - b.right, o.height_mm - b.top - b.bottom, o.bridge_mm, g.w, g.h, v.tones_found, v.tones_wanted,
        v.score(o));
    int id = 0;
    const auto add = [&](const char* kind, const std::vector<piece_note>& pieces)
    {
        for (const auto& p : pieces)
        {
            if (id >= 200)  // enough to plan with; the count says the rest
                return;
            out += std::format("{}{{\"id\":\"m{}\",\"kind\":\"{}\",\"sheet\":{},\"at_mm\":[{:.1f},{:.1f}],\"area_mm2\":{:.1f}}}", id ? "," : "", id + 1, kind,
                               p.sheet + 1, p.x / s, p.y / s, p.pixels / (s * s));
            ++id;
        }
    };
    add("floating", v.floating);
    add("thin", v.thin);
    // Exact totals beside the capped, rounded findings, for ranking rounds.
    size_t floating_pixels = 0, thin_pixels = 0;
    for (const auto& p : v.floating)
        floating_pixels += p.pixels;
    for (const auto& p : v.thin)
        thin_pixels += p.pixels;
    out += std::format("],\"floating\":{},\"thin\":{},\"floating_pixels\":{},\"floating_area_mm2\":{},\"thin_pixels\":{}}}", v.floating.size(),
                       v.thin.size(), floating_pixels, double(floating_pixels) / (s * s), thin_pixels);
    return out;
}
sheet_result make_sheets(const std::vector<layer>& layers, const transform_options& o)
{
    if (std::ranges::none_of(layers, [](const auto& s) { return s.visible && s.image.valid(); }))
        throw std::runtime_error("Add at least one visible image.");
    // An AI result is flat colour already: snap to its palette.
    if (std::ranges::any_of(layers, [](const layer& l) { return l.visible && l.type == layer_type::ai_illustration && l.version != 0; }))
    {
        // Floating pieces stay, as issues, rather than vanishing.
        auto kept = o;
        kept.connect = false;
        // Snap to the colours the request gave the layers, one sheet each, so
        // planes the model drew in similar hues stay apart.
        std::vector<std::array<uint8_t, 3>> palette;
        for (const auto& l : layers)
            if (l.visible && l.type == layer_type::ai_illustration && l.version != 0)
                if (const auto* v = find_version(l, l.version); v && !v->palette.empty())
                    palette = parse_palette(v->palette);
        if (!palette.empty())
            kept.layers = int(palette.size());
        auto imported = layers_from_illustration(flatten(layers, kept), kept, palette, true);
        sheet_result out{std::move(imported.layers), std::move(imported.removed_per_layer), std::move(imported.removed)};
        out.tones_found = imported.tones_found;
        return out;
    }
    return simplified(layers, o);
}
std::string ai_notes(const project& p)
{
    std::vector<const layer*> noted;
    for (const auto& l : p.layers)
        if (l.visible && l.notes.find_first_not_of(" \t\r\n") != std::string::npos)
            noted.push_back(&l);
    if (noted.size() == 1)
        return noted.front()->notes;
    std::string out;
    for (auto it = noted.rbegin(); it != noted.rend(); ++it)  // front first
        out += (out.empty() ? "" : "\n") + (*it)->name + ": " + (*it)->notes;
    return out;
}
std::string ai_prompt(const project& p)
{
    return assemble_prompt(prompts(), p.options, reimagine_words(p)).text;
}
prompt_words reimagine_words(const project& p)
{
    const auto& o = p.options;
    prompt_words w;
    w.task = ai_task::reimagine;
    w.style = p.ai_style;
    w.notes = ai_notes(p);
    w.instructions = p.instructions;
    // The size the provider is asked for (account_image_endpoint's rule).
    w.width = o.width_mm > o.height_mm * 1.05 ? 1536 : 1024;
    w.height = o.height_mm > o.width_mm * 1.05 ? 1536 : 1024;
    // Where the composition already meets the frame, which it should keep.
    std::vector<std::string> edges;
    bool any = false;
    rect_mm u{};
    for (const auto& l : p.layers)
        if (l.visible && l.image.valid())
        {
            const auto b = bounds(effective_transform(l, o));
            u = any ? rect_mm{std::min(u.left, b.left), std::min(u.top, b.top), std::max(u.right, b.right), std::max(u.bottom, b.bottom)} : b;
            any = true;
        }
    if (any)
    {
        const auto open = opening_mm(o);
        if (u.bottom > open.bottom + .5)
            edges.push_back("bottom");
        if (u.left < open.left - .5)
            edges.push_back("left");
        if (u.right > open.right + .5)
            edges.push_back("right");
        if (u.top < open.top - .5)
            edges.push_back("top");
    }
    if (!edges.empty() && edges.size() < 4)
    {
        w.placement = ", where it meets the frame at the ";
        for (size_t i = 0; i < edges.size(); ++i)
            w.placement += (i == 0 ? "" : i + 1 == edges.size() ? " and " : ", ") + edges[i];
    }
    const auto references = std::ranges::count_if(p.layers, [](const layer& l) { return l.visible && l.image.valid(); });
    if (references)
        w.references = references == 1 ? "The second image is the original picture, for likeness." : "The other images are the original pictures, for likeness.";
    return w;
}
}  // namespace offcut
