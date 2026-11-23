#include "transform.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace offcut
{
namespace
{
struct geometry
{
    int w, h, bx, by;
};
geometry dimensions(const transform_options& o)
{
    validate(o);
    const double scale = 640. / std::max(o.width_mm, o.height_mm);
    return {std::max(20, int(std::lround(o.width_mm * scale))), std::max(20, int(std::lround(o.height_mm * scale))),
            std::max(1, int(std::ceil(o.border_mm * scale))), std::max(1, int(std::ceil(o.border_mm * scale)))};
}
bool border(int x, int y, geometry g)
{
    return x < g.bx || y < g.by || x >= g.w - g.bx || y >= g.h - g.by;
}
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
void frame_and_connect(cut_layer& c, const transform_options& o)
{
    const auto g = dimensions(o);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (border(x, y, g))
                c.solid[y * g.w + x] = 1;
    const int bridge = std::max(1, int(std::ceil(o.bridge_mm * g.w / o.width_mm)));
    for (const auto& component : components(c))
    {
        if (std::ranges::any_of(component, [&](int i) { return border(i % g.w, i / g.w, g); }))
            continue;
        // Suppress specks before connecting them; never discard a larger feature.
        if (component.size() < size_t((o.smoothing + 1) * (o.smoothing + 1)))
        {
            for (int i : component)
                c.solid[i] = 0;
            continue;
        }
        if (!o.connect)
            continue;
        int best = component.front(), distance = g.w + g.h, side = 0;
        for (int i : component)
        {
            const int x = i % g.w, y = i / g.w;
            const int d[] = {x - g.bx + 1, y - g.by + 1, g.w - g.bx - x, g.h - g.by - y};
            for (int s = 0; s < 4; ++s)
                if (d[s] < distance)
                {
                    best = i;
                    distance = d[s];
                    side = s;
                }
        }
        int x = best % g.w, y = best / g.w;
        const int dx[] = {-1, 0, 1, 0}, dy[] = {0, -1, 0, 1};
        for (int step = 0; step <= distance; ++step, x += dx[side], y += dy[side])
            for (int a = -(bridge / 2); a < bridge - bridge / 2; ++a)
            {
                const int px = x + (side % 2 ? a : 0), py = y + (side % 2 ? 0 : a);
                if (px >= 0 && px < g.w && py >= 0 && py < g.h)
                    c.solid[py * g.w + px] = 1;
            }
    }
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
}  // namespace
raster flatten(const std::vector<source_image>& sources, const transform_options& o)
{
    const auto g = dimensions(o);
    raster r{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h * 4, 255)};
    // Sources stack bottom to top, each contained inside the shared frame.
    for (const auto& s : sources)
    {
        if (!s.visible || !s.image.valid())
            continue;
        const auto& a = s.image;
        const double k = std::min(double(g.w - 2 * g.bx) / a.width, double(g.h - 2 * g.by) / a.height);
        const int w = std::max(1, int(a.width * k)), h = std::max(1, int(a.height * k));
        const int ox = (g.w - w) / 2, oy = (g.h - h) / 2;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t si = (size_t(std::min(a.height - 1, int(y / k))) * a.width + std::min(a.width - 1, int(x / k))) * 4;
                const size_t di = (size_t(y + oy) * g.w + x + ox) * 4;
                const int alpha = a.rgba[si + 3];
                for (int ch = 0; ch < 3; ++ch)
                    r.rgba[di + ch] = uint8_t((a.rgba[si + ch] * alpha + r.rgba[di + ch] * (255 - alpha) + 127) / 255);
            }
    }
    return r;
}
std::vector<cut_layer> simplify(const std::vector<source_image>& sources, const transform_options& o)
{
    if (std::ranges::none_of(sources, [](const auto& s) { return s.visible && s.image.valid(); }))
        throw std::runtime_error("Add at least one visible source image.");
    const auto r = flatten(sources, o);
    const auto gray = luminance(r, o.smoothing);
    std::vector<cut_layer> cuts;
    for (int layer = 0; layer < o.layers; ++layer)
    {
        cut_layer c{"Layer " + std::to_string(layer + 1), r.width, r.height, std::vector<uint8_t>(gray.size())};
        for (size_t i = 0; i < gray.size(); ++i)
            c.solid[i] = layer == o.layers - 1 || gray[i] < float(layer + 1) / o.layers;
        frame_and_connect(c, o);
        cuts.push_back(std::move(c));
    }
    return cuts;
}
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
        frame_and_connect(c, o);
        cuts.push_back(std::move(c));
    }
    return cuts;
}
raster render_cuts(const std::vector<cut_layer>& cuts, int selected)
{
    if (cuts.empty())
        return {};
    const auto& first = cuts.front();
    raster r{first.width, first.height, std::vector<uint8_t>(first.solid.size() * 4, 255)};
    for (int layer = int(cuts.size()) - 1; layer >= 0; --layer)
    {
        const auto& c = cuts[layer];
        if (selected >= 0 ? selected != layer : !c.visible)
            continue;
        for (size_t i = 0; i < c.solid.size(); ++i)
            if (c.solid[i])
            {
                const float t = float(layer) / std::max(1, int(cuts.size()) - 1);
                r.rgba[4 * i] = uint8_t(38 + 189 * t);
                r.rgba[4 * i + 1] = uint8_t(64 + 156 * t);
                r.rgba[4 * i + 2] = uint8_t(69 + 123 * t);
            }
    }
    return r;
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
std::string ai_prompt(const project& p)
{
    std::ostringstream s;
    s << "Create a manufacturable layered papercut shadow-box design from the attached reference images.\n"
      << "Preserve the subject's recognisable silhouette and defining features. Simplify texture and clutter.\n"
      << "Deliver exactly " << p.options.layers << " separate PNG masks, numbered 01 through " << p.options.layers << ".\n"
      << "Each canvas must have the SAME dimensions and alignment; aspect ratio " << p.options.width_mm << ':' << p.options.height_mm << ".\n"
      << "Physical size: " << p.options.width_mm << " x " << p.options.height_mm << " mm, INCLUDING a continuous " << p.options.border_mm
      << " mm outer frame on every layer.\n"
      << "BLACK means retained cardstock, WHITE means removed material. Use opaque, flat black and white: no gradients, shadows, perspective, mockups, labels "
         "or contact sheets.\n"
      << "Layer 01 is the front. Subsequent layers sit behind it. Plan occlusion so their visible regions form the intended illustration. The final layer is a "
         "solid backing.\n"
      << "Every retained region must connect to the outer frame; no floating islands. Keep bridges and narrow retained features at least "
      << p.options.bridge_mm << " mm wide.\n"
      << "Use smooth, simple boundaries and avoid tiny holes. Keep the whole composition inside the frame.\n"
      << "Do not place layer numbers or captions inside the artwork; number filenames only.\n"
      << "If separate image files cannot be produced, explain the limitation instead of returning a montage.\n\n"
      << "Additional creative instructions:\n"
      << p.instructions << "\n";
    return s.str();
}
}  // namespace offcut
