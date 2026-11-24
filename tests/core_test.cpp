#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
#include <format>
#include <map>
#include "ai_client.h"
#include "ai_provider.h"
#include "auto_loop.h"
#include "director_helpers.h"
#include "export.h"
#include "guidance.h"
#include "history.h"
#include "prompts.h"
#include "transform.h"
#include "versions.h"
using namespace offcut;
namespace offcut
{
void test_director_protocol();
}
void require(bool condition, const char* text)
{
    if (!condition)
        throw std::runtime_error(text);
}
template <class F>
void rejects(F f, const char* text)
{
    bool threw = false;
    try
    {
        f();
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    require(threw, text);
}
raster image(int w, int h, uint8_t value)
{
    return {w, h, std::vector<uint8_t>(size_t(w) * h * 4, value)};
}
// Read exported SVG rings independently and check their even-odd coverage at
// cell centres. Exhaustive small masks include holes and diagonal contacts.
void check_svg(const cut_layer& mask)
{
    const auto xml = svg(mask, {});
    const size_t begin = xml.find(" d=\"") + 4, end = xml.find('"', begin);
    std::istringstream in(xml.substr(begin, end - begin));
    std::vector<std::vector<std::pair<double, double>>> rings;
    char command;
    while (in >> command)
    {
        if (command == 'M')
        {
            double x, y;
            in >> x >> y;
            rings.push_back({{x, y}});
        }
        else if (command == 'L')
        {
            double x, y;
            in >> x >> y;
            rings.back().emplace_back(x, y);
        }
        else
            require(command == 'Z', "Unexpected SVG command");
    }
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
        {
            bool inside = false;
            const double px = x + .5, py = y + .5;
            for (const auto& r : rings)
                for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
                {
                    const auto [ax, ay] = r[i];
                    const auto [bx, by] = r[j];
                    if ((ay > py) != (by > py) && px < (bx - ax) * (py - ay) / (by - ay) + ax)
                        inside = !inside;
                }
            require(inside == bool(mask.solid[y * mask.width + x]), "SVG changed a cut region");
        }
    require(xml.find("width=\"200mm\"") != std::string::npos, "SVG dimensions lost");
    require(xml.find("<image") == std::string::npos, "Raster embedded in SVG");
}
using rgb = std::array<uint8_t, 3>;
const std::array<rgb, 5> illustration_palette{{{15, 25, 45}, {115, 35, 55}, {30, 130, 110}, {225, 175, 55}, {245, 235, 210}}};
void paint_pixel(raster& image, int x, int y, rgb colour)
{
    const auto p = (size_t(y) * image.width + x) * 4;
    for (int ch = 0; ch < 3; ++ch)
        image.rgba[p + ch] = colour[ch];
    image.rgba[p + 3] = 255;
}
int visible_depth(const std::vector<cut_layer>& layers, int x, int y)
{
    for (int i = 0; i < int(layers.size()); ++i)
        if (layers[i].solid[size_t(y) * layers[i].width + x])
            return i;
    return -1;
}
std::filesystem::path fresh_sample_folder(const std::filesystem::path& parent)
{
    for (int i = 1; i < 10000; ++i)
    {
        const auto folder = parent / ("offcut-illustration-samples-" + std::to_string(i));
        if (std::filesystem::create_directory(folder))
            return folder;
    }
    throw std::runtime_error("Could not create sample folder.");
}
void test_illustration(const std::filesystem::path& samples)
{
    transform_options o;
    o.layers = 5;
    o.smoothing = 0;
    auto synthetic = image(160, 160, 255);
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 160; ++x)
        {
            const int edge = std::min({x, y, 159 - x, 159 - y});
            const int tone = std::min(4, edge / 8);
            rgb colour = illustration_palette[tone];
            // Anti-aliased single-pixel transitions, plus bounded JPEG-like noise.
            if (edge % 8 == 7 && tone < 4)
                for (int ch = 0; ch < 3; ++ch)
                    colour[ch] = uint8_t((colour[ch] + illustration_palette[tone + 1][ch]) / 2);
            const int noise = int((uint32_t(y * 160 + x) * 1103515245u + 12345u) % 5) - 2;
            for (auto& c : colour)
                c = uint8_t(std::clamp(int(c) + noise, 0, 255));
            if (x >= 76 && x < 84 && y >= 72)
                colour = illustration_palette[0];
            paint_pixel(synthetic, x, y, colour);
        }
    // Transparent black must composite to the light background, not front card.
    const size_t transparent = (size_t(50) * 160 + 50) * 4;
    for (int ch = 0; ch < 4; ++ch)
        synthetic.rgba[transparent + ch] = 0;
    const auto imported = layers_from_illustration(synthetic, o);
    require(imported.layers.size() == 5, "Illustration did not produce exactly N sheets");
    require(imported.cropped_fraction == 0, "Square illustration was cropped");
    require(std::ranges::all_of(imported.layers.back().solid, [](auto v) { return v == 1; }), "Illustration backing is not solid");
    for (const auto& layer : imported.layers)
        require(disconnected_components(layer) == 0, "Illustration left detached card");
    require(std::accumulate(imported.removed_per_layer.begin(), imported.removed_per_layer.end(), size_t(0)) == 0,
            "Supported synthetic illustration lost pieces");
    require(visible_depth(imported.layers, 80 * 4 + 2, 120 * 4 + 2) == 0, "Darkest illustration tone is not the front");
    require(visible_depth(imported.layers, 50 * 4 + 2, 50 * 4 + 2) == 4, "Transparent black was not composited over white");
    for (int boundary : {8, 16, 24, 32})
    {
        const int lo = boundary / 8 - 1, hi = lo + 1;
        for (int x = boundary - 1; x <= boundary; ++x)
        {
            const int tone = visible_depth(imported.layers, x * 4 + 2, 80 * 4 + 2);
            require(tone == lo || tone == hi, "Anti-aliased boundary created an unrelated tone sliver");
        }
        require(visible_depth(imported.layers, (boundary - 2) * 4 + 2, 80 * 4 + 2) == lo &&
                    visible_depth(imported.layers, (boundary + 2) * 4 + 2, 80 * 4 + 2) == hi,
                "Mode cleanup damaged a genuine tone region");
    }
    const auto repeat = layers_from_illustration(synthetic, o);
    for (size_t i = 0; i < imported.layers.size(); ++i)
        require(imported.layers[i].solid == repeat.layers[i].solid, "Tone separation is not deterministic");
    write_png(synthetic, samples / "synthetic-5-tone.png");
    project sample;
    sample.name = "Five-tone noisy illustration";
    sample.options = o;
    sample.cuts = imported.layers;
    export_layers(sample, samples);
    // Hue order in the image is deliberately unrelated to luminance order.
    auto hues = image(160, 100, 255);
    const int shuffled[] = {3, 0, 4, 1, 2};
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 160; ++x)
            paint_pixel(hues, x, y, illustration_palette[shuffled[x / 32]]);
    auto wide = o;
    wide.width_mm = 320;
    wide.height_mm = 200;
    const auto hue_layers = layers_from_illustration(hues, wide);
    for (int i = 0; i < 5; ++i)
        require(visible_depth(hue_layers.layers, (i * 32 + 16) * 4 + 2, 50 * 4 + 2) == shuffled[i],
                "Tone order follows hue/input position instead of luminance");
    auto crop = image(200, 100, 255);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x)
        {
            const uint8_t gray = x < 50 || x >= 150 ? 100 : x < 100 ? 0 : 255;
            paint_pixel(crop, x, y, {gray, gray, gray});
        }
    auto three = o;
    three.layers = 3;
    const auto cropped = layers_from_illustration(crop, three);
    require(std::abs(cropped.cropped_fraction - .5) < 1e-12, "Cover crop fraction is incorrect");
    require(visible_depth(cropped.layers, 160, 320) == 0 && visible_depth(cropped.layers, 480, 320) == 2,
            "Illustration was stretched instead of centre cover-cropped");
    auto floating = image(128, 128, 255);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            uint8_t gray = 255;
            if (std::min({x, y, 127 - x, 127 - y}) < 8 || (x >= 48 && x < 64 && y >= 48 && y < 64))
                gray = 0;
            else if (x >= 96 && y >= 24 && y < 104)
                gray = 100;
            paint_pixel(floating, x, y, {gray, gray, gray});
        }
    const auto settled = layers_from_illustration(floating, three);
    require(settled.removed_per_layer.front() == 1, "Floating darker island was not counted");
    require(settled.layers.front().solid[280 * 640 + 280] == 0, "Floating darker island survived");
    project prompt_project;
    prompt_project.options = o;
    const auto prompt = ai_prompt(prompt_project);
    require(prompt.contains("layered paper shadowbox") && prompt.contains(std::format("exactly these {} colours", o.layers)) &&
                prompt.contains("no outlines or contour lines") && prompt.contains("Nothing floating"),
            "AI prompt does not ask for one N-colour shadowbox design");
    {
        // Each plane gets its own colour, listed beside it and in the output
        // spec, and the record round-trips.
        const auto pal = default_palette(o.layers);
        require(int(pal.size()) == o.layers && pal.front() != pal.back(), "Default palette has the wrong size");
        for (size_t i = 0; i < pal.size(); ++i)
            for (size_t j = i + 1; j < pal.size(); ++j)
                require(std::abs(int(pal[i][0]) - pal[j][0]) + std::abs(int(pal[i][1]) - pal[j][1]) + std::abs(int(pal[i][2]) - pal[j][2]) > 90,
                        "Default palette colours are too close to separate");
        const auto text = palette_text(pal);
        require(parse_palette(text) == pal && parse_palette("#12345").empty() && parse_palette("#gg0000").empty(), "Palette text does not round-trip");
        require(prompt.contains("(front, colour " + text.substr(0, 7) + ")") && prompt.contains(text.substr(text.size() - 7)),
                "Prompt does not give each layer its colour");
    }
    rejects([&] { layers_from_illustration({}, o); }, "Invalid illustration accepted");
}
void test_depth_overlap(const std::filesystem::path& samples)
{
    transform_options o;
    o.layers = 3;
    o.smoothing = 0;
    o.layer_gap_mm = 5;
    const auto g = dimensions(o);
    tone_map map{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 2)};
    for (int y = 200; y < g.h; ++y)
        for (int x = 390; x < g.w; ++x)
            map.indices[size_t(y) * g.w + x] = 1;
    for (int y = 100; y < g.h; ++y)
        for (int x = 250; x < 390; ++x)
            map.indices[size_t(y) * g.w + x] = 0;
    const auto built = build_sheets(map, o);
    const auto& mountain = built.layers[1];
    require(mountain.solid[300 * g.w + 389] && mountain.solid[300 * g.w + 374], "Mountain lacks its r-band beneath the tree");
    require(!mountain.solid[300 * g.w + 373] && !mountain.solid[300 * g.w + 250], "Mountain leaks under the tree's sky-side edge or past r");
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
        {
            const size_t p = size_t(y) * g.w + x;
            require(bool(built.layers.front().solid[p]) == (map.indices[p] == 0 || border(x, y, g)), "Front sheet differs from darkest tone plus frame");
            if (!border(x, y, g) && map.indices[p] > 1)
                require(!mountain.solid[p], "Hidden overlap added visible card");
        }
    require(std::ranges::all_of(built.layers.back().solid, [](auto v) { return v == 1; }), "Depth-aware backing is not solid");
    tone_map diagonal{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 0)};
    for (int y = 50; y < 100; ++y)
        for (int x = 50; x < 100; ++x)
            diagonal.indices[size_t(y) * g.w + x] = 1;
    for (int y = 70; y < 80; ++y)
        for (int x = 0; x < 60; ++x)
            diagonal.indices[size_t(y) * g.w + x] = 1;
    const auto euclidean = build_sheets(diagonal, o);
    require(euclidean.layers[1].solid[110 * g.w + 110] && !euclidean.layers[1].solid[114 * g.w + 114], "Hidden reach is not Euclidean distance");
    auto four = o;
    four.layers = 4;
    auto scaled = map;
    for (auto& tone : scaled.indices)
        if (tone == 2)
            tone = 3;
        else if (tone == 1)
            tone = 2;
    scaled.indices[350 * g.w + 370] = 1;
    const auto depth_scaled = build_sheets(scaled, four);
    require(depth_scaled.layers[2].solid[300 * g.w + 370] && !depth_scaled.layers[2].solid[350 * g.w + 370], "Hidden reach is not scaled by depth difference");
    tone_map support{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 2)};
    for (int y = 300; y < g.h; ++y)
        for (int x = 100; x < 120; ++x)
            support.indices[size_t(y) * g.w + x] = 0;
    for (int y = 240; y < 300; ++y)
        for (int x = 80; x < 140; ++x)
            support.indices[size_t(y) * g.w + x] = 1;
    auto no_gap = o;
    no_gap.layer_gap_mm = 0;
    const auto restored = build_sheets(support, no_gap);
    require(restored.layers[1].solid[500 * g.w + 110] && !restored.layers[1].solid[500 * g.w + 120] && disconnected_components(restored.layers[1]) == 0,
            "Hidden-only cumulative support was not restored safely");
    auto invalid = map;
    invalid.indices[100] = 3;
    rejects([&] { build_sheets(invalid, o); }, "Out-of-range tone accepted");
    // A landscape-style pine and mountain, with sky beyond the pine's far edge.
    tone_map landscape{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 2)};
    const std::array<rgb, 3> palette{{{30, 65, 50}, {110, 150, 115}, {200, 225, 240}}};
    auto picture = image(g.w, g.h, 255);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
        {
            int tone = 2;
            if (x >= 340 && y > 380 - (x - 340) * .55)
                tone = 1;
            const bool crown = (y >= 90 && y < 280 && std::abs(x - 295) < (y - 90) * .48) || (y >= 190 && y < 430 && std::abs(x - 295) < (y - 190) * .48);
            if (crown || (x >= 280 && x < 310 && y >= 360) || y >= g.h - g.bottom - 16 || border(x, y, g))
                tone = 0;
            landscape.indices[size_t(y) * g.w + x] = uint8_t(tone);
            paint_pixel(picture, x, y, palette[tone]);
        }
    const auto folder = samples / "landscape";
    std::filesystem::create_directory(folder);
    write_png(picture, folder / "illustration.png");
    project p;
    p.options = o;
    p.cuts = build_sheets(landscape, o).layers;
    export_layers(p, folder);
    const auto imported = layers_from_illustration(picture, o);
    for (const auto& layer : imported.layers)
        require(disconnected_components(layer) == 0, "Imported landscape has detached card");
    require(visible_depth(imported.layers, 295, 500) == 0 && visible_depth(imported.layers, 550, 450) == 1 && visible_depth(imported.layers, 150, 150) == 2,
            "Imported landscape lost tree/mountain/sky ordering");
}
// The OFFCUT02 writer as it was before per-image transforms, kept here as a
// fixture: placement applied to the composite and images had no transform.
void write_offcut02(const project& p, const std::filesystem::path& path, bool gap)
{
    std::ofstream out(path, std::ios::binary);
    const auto n = [&](uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.put(char(v >> (8 * i)));
    };
    const auto b = [&](const std::vector<uint8_t>& bytes)
    {
        n(uint32_t(bytes.size()));
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    };
    const auto s = [&](const std::string& text) { b({text.begin(), text.end()}); };
    out.write("OFFCUT02", 8);
    s(p.name);
    s(p.instructions);
    const auto& o = p.options;
    for (double v : {o.width_mm, o.height_mm, o.border_mm, o.bridge_mm})
        n(uint32_t(std::lround(v * 100)));
    n(o.layers);
    n(o.smoothing);
    n(o.connect);
    n(p.stale);
    for (double v : {o.border_top_mm, o.border_bottom_mm, o.border_left_mm, o.border_right_mm})
        n(uint32_t(std::lround(v * 100)));
    n(o.uniform_border);
    n(uint32_t(o.placement));
    n(o.touch_edges);
    n(o.size_preset);
    n(o.landscape);
    n(uint32_t(p.layers.size()));
    for (const auto& l : p.layers)
    {
        s(l.name);
        n(l.visible);
        n(l.image.width);
        n(l.image.height);
        b(l.image.rgba);
    }
    n(uint32_t(p.cuts.size()));
    for (const auto& c : p.cuts)
    {
        s(c.name);
        n(c.visible);
        n(c.width);
        n(c.height);
        b(c.solid);
    }
    if (gap)
        n(uint32_t(std::lround(o.layer_gap_mm * 100)));
}
bool close_to(double a, double b, double e = 1e-9)
{
    return std::abs(a - b) <= e;
}
// Pixels that differ between two composites.
size_t differing(const raster& a, const raster& b)
{
    size_t n = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        if (a.rgba[i] != b.rgba[i] || a.rgba[i + 1] != b.rgba[i + 1] || a.rgba[i + 2] != b.rgba[i + 2])
            ++n;
    return n;
}
// A subject on transparent padding: opaque black square at [20, 80).
raster padded_subject()
{
    auto subject = image(100, 100, 0);
    for (int y = 20; y < 80; ++y)
        for (int x = 20; x < 80; ++x)
            subject.rgba[(y * 100 + x) * 4 + 3] = 255;
    return subject;
}
void test_layer_editor(const std::filesystem::path& folder)
{
    transform_options o;
    o.smoothing = 0;
    // Transform maths: frame ↔ source, corners and bounds with rotation.
    image_transform t;
    t.x_mm = 100;
    t.y_mm = 80;
    t.mm_per_pixel = .5;
    t.crop = {10, 20, 40, 20};
    t.rotation_deg = 0;
    require(close_to(to_frame(t, 30, 30).x, 100) && close_to(to_frame(t, 30, 30).y, 80), "Crop centre is not the transform position");
    require(close_to(to_frame(t, 10, 20).x, 90) && close_to(to_frame(t, 10, 20).y, 75), "Unrotated corner wrong");
    require(close_to(width_mm(t), 20) && close_to(height_mm(t), 10), "Width/height in mm wrong");
    t.rotation_deg = 90;
    const auto tl = corners(t)[0];
    require(close_to(tl.x, 105) && close_to(tl.y, 70), "Clockwise rotation of the top-left corner wrong");
    const auto box = bounds(t);
    require(close_to(box.left, 95) && close_to(box.right, 105) && close_to(box.top, 70) && close_to(box.bottom, 90), "Rotated bounds wrong");
    t.rotation_deg = 33;
    for (const auto& [u, v] : {std::pair{10., 20.}, std::pair{47.5, 21.25}, std::pair{30., 39.}})
    {
        const auto back = to_source(t, to_frame(t, u, v));
        require(close_to(back.x, u) && close_to(back.y, v), "Frame/source round trip failed");
    }
    // Hit testing: inside the rotated rectangle only.
    require(contains(t, {100, 80}) && !contains(t, {box.left - 30, 80}), "Hit test on a rotated image failed");
    t.rotation_deg = 0;
    require(contains(t, {90.1, 75.1}) && !contains(t, {89.9, 75.1}) && !contains(t, {100, 85.1}), "Hit test at the crop edge failed");
    // Recropping keeps pixels where they were.
    {
        auto r = t;
        r.rotation_deg = 27;
        const auto before = to_frame(r, 25, 28);
        const auto recropped = with_crop(r, {20, 25, 10, 8});
        const auto after = to_frame(recropped, 25, 28);
        require(close_to(before.x, after.x) && close_to(before.y, after.y) && recropped.crop == pixel_rect{20, 25, 10, 8}, "Crop moved the remaining pixels");
        const auto scaled = scaled_about(r, 2, corners(r)[2]);
        require(close_to(corners(scaled)[2].x, corners(r)[2].x) && close_to(corners(scaled)[2].y, corners(r)[2].y) && close_to(width_mm(scaled), 40),
                "Scaling moved its anchor corner");
    }
    // Layers: topmost visible wins; hidden layers aren't hit.
    {
        std::vector<layer> stack{{"bottom", image(40, 20, 0)}, {"top", image(40, 20, 0)}};
        stack[0].transform = t;
        stack[1].transform = t;
        stack[1].transform.x_mm += 15;
        require(hit_test(stack, o, {95, 80}) == 0 && hit_test(stack, o, {108, 80}) == 1 && hit_test(stack, o, {300, 300}) == -1, "Hit test order wrong");
        stack[1].visible = false;
        require(hit_test(stack, o, {108, 80}) == 0, "Hidden layer was hit");
    }
    // Snapping to the opening within the threshold, nearest edge first.
    {
        const rect_mm opening{10, 10, 190, 190};
        auto snap = snap_to_opening({12, 50, 62, 100}, opening, 3);
        require(close_to(snap.dx, -2) && snap.guide_x && close_to(*snap.guide_x, 10) && snap.dy == 0 && !snap.guide_y, "Left edge did not snap");
        snap = snap_to_opening({50, 50, 188.5, 191}, opening, 3);
        require(close_to(snap.dx, 1.5) && close_to(snap.dy, -1) && close_to(*snap.guide_y, 190), "Right/bottom edges did not snap");
        snap = snap_to_opening({14, 50, 60, 100}, opening, 3);
        require(snap.dx == 0 && !snap.guide_x, "Snapped beyond the threshold");
        snap = snap_to_opening({12, 50, 62, 100}, opening, 3, touch_right);
        require(snap.dx == 0, "A masked edge snapped");
    }
    // Placement sets the initial transform; drawing through the transform
    // looks exactly as the old composite placement did.
    {
        auto placed = o;
        for (auto mode : {placement_mode::fit, placement_mode::fill, placement_mode::touch_edges})
            for (unsigned edges : {unsigned(touch_bottom), unsigned(touch_left | touch_right), unsigned(touch_top)})
            {
                placed.placement = mode;
                placed.touch_edges = edges;
                layer l{"subject", padded_subject()};
                const auto legacy = flatten({l}, placed);  // unplaced: the project placement
                l.transform = place_image(l, placed, mode, edges);
                require(l.transform.placed() && l.transform.crop == full_crop(l.image), "Placement did not set a full-crop transform");
                require(differing(flatten({l}, placed), legacy) == 0, "Transformed placement differs from the composite placement");
            }
        // A crop shows only its pixels; the rest of the image is gone.
        auto halves = image(100, 50, 255);
        for (int y = 0; y < 50; ++y)
            for (int x = 0; x < 50; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    halves.rgba[(y * 100 + x) * 4 + ch] = 0;
        layer l{"halves", halves};
        l.transform = place_image(l, o, placement_mode::fit, 0);
        l.transform = with_crop(l.transform, {50, 0, 50, 50});  // the white half
        const auto composite = flatten({l}, o);
        require(std::ranges::all_of(composite.rgba, [](uint8_t v) { return v == 255; }), "Cropped-away pixels were drawn");
        // Rotation: a left-dark image turned 90° clockwise is dark on top.
        l.transform = {};
        l.transform = place_image(l, o, placement_mode::fit, 0);
        l.transform.rotation_deg = 90;
        const auto g = dimensions(o);
        const auto turned = flatten({l}, o);
        const double s = working_scale(o);
        const int cx = int(l.transform.x_mm * s), cy = int(l.transform.y_mm * s), reach = int(width_mm(l.transform) * s / 4);
        require(turned.rgba[(size_t(cy - reach) * g.w + cx) * 4] == 0 && turned.rgba[(size_t(cy + reach) * g.w + cx) * 4] == 255,
                "Rotated image drawn the wrong way round");
        // Fit and touch.
        layer subject{"subject", padded_subject()};
        subject.transform = place_image(subject, o, placement_mode::fit, 0);
        subject.transform.rotation_deg = 45;
        const auto fitted = fit_to_opening(subject.transform, o);
        const auto fb = bounds(fitted), open = opening_mm(o);
        require(fb.left >= open.left - 1e-9 && fb.right <= open.right + 1e-9 && fb.top >= open.top - 1e-9 && fb.bottom <= open.bottom + 1e-9 &&
                    (close_to(fb.width(), open.width(), 1e-6) || close_to(fb.height(), open.height(), 1e-6)),
                "Fit to frame does not fit the rotated image");
        subject.transform.rotation_deg = 0;
        subject.transform = touch(subject, o, touch_bottom);
        const auto bottom = to_frame(subject.transform, 50, 80).y;
        require(close_to(bottom, open.bottom + o.bridge_mm, 1e-6), "Touch bottom did not sink the subject by Min. width");
        subject.transform = touch(subject, o, touch_left);
        require(close_to(to_frame(subject.transform, 20, 50).x, open.left - o.bridge_mm, 1e-6), "Touch left failed");
    }
    // OFFCUT03 round trip: types, ids, derivation and exact transforms.
    {
        project p;
        p.name = "Layers";
        p.options = o;
        p.layers = {{"cat.png", padded_subject()}, {"title", image(30, 10, 128)}};
        p.layers[0].transform = place_image(p.layers[0], o, placement_mode::fit, 0);
        p.layers[0].transform.rotation_deg = -12.5;
        p.layers[0].transform.x_mm = -3.25;
        p.layers[1].transform = with_crop(place_image(p.layers[1], o, placement_mode::fill, 0), {2, 1, 20, 7.5});
        p.layers[1].derived_from = 1;
        p.layers[1].visible = false;
        p.layers[1].type = layer_type::ai_illustration;
        p.ai_style = "Paper-cut, flat, bold shapes";
        p.layers[0].notes = "Keep the ears";
        p.layers[0].versions = {{1, 0, image(12, 8, 40), "Reimagine as a shadowbox", "support looks added on", review_state::discarded},
                                {2, 1, image(12, 8, 90), "Recompose instead", "", review_state::used}};
        p.layers[0].version = 2;
        p.assign_layer_ids();
        require(p.layers[0].id == 1 && p.layers[1].id == 2 && p.find_layer(2) == 1, "Layer ids not assigned");
        save_project(p, folder / "layers03.offcut");
        {
            std::ifstream in(folder / "layers03.offcut", std::ios::binary);
            char magic[8];
            in.read(magic, 8);
            require(std::string(magic, 8) == "OFFCUT03", "Not saved as OFFCUT03");
        }
        const auto back = load_project(folder / "layers03.offcut");
        require(back.layers.size() == 2 && back.layers[0].transform == p.layers[0].transform && back.layers[1].transform == p.layers[1].transform &&
                    back.layers[1].derived_from == 1 && back.layers[1].id == 2 && !back.layers[1].visible && back.layers[0].type == layer_type::image &&
                    back.layers[0].image.rgba == p.layers[0].image.rgba && back.layers[1].type == layer_type::ai_illustration,
                "OFFCUT03 did not round-trip layers and transforms");
        require(back.ai_style == p.ai_style && back.layers[0].notes == "Keep the ears" && back.layers[0].version == 2 && back.layers[0].versions.size() == 2 &&
                    back.layers[0].versions[1].parent == 1 && back.layers[0].versions[0].review == "support looks added on" &&
                    back.layers[0].versions[0].state == review_state::discarded && back.layers[0].versions[1].image.rgba == image(12, 8, 90).rgba &&
                    back.layers[1].versions.empty(),
                "OFFCUT03 did not round-trip version threads, Notes for AI and the AI Style");
        require(differing(flatten(back.layers, back.options), flatten(p.layers, p.options)) == 0, "OFFCUT03 composite changed on reopen");
        // OFFCUT02: the composite placement becomes each image's transform.
        project old = p;
        old.options.placement = placement_mode::touch_edges;
        old.options.touch_edges = touch_bottom;
        for (auto& l : old.layers)
        {
            l.transform = {};
            l.versions.clear();
            l.version = 0;
        }
        write_offcut02(old, folder / "placed02.offcut", true);
        const auto converted = load_project(folder / "placed02.offcut");
        require(converted.layers.size() == 2 && std::ranges::all_of(converted.layers, [](const layer& l) { return l.transform.placed() && l.id; }),
                "OFFCUT02 images were not given transforms and ids");
        require(differing(flatten(converted.layers, converted.options), flatten(old.layers, old.options)) == 0, "OFFCUT02 looks different after loading");
        // OFFCUT01 with an image: uniform borders, Fit.
        std::ofstream legacy(folder / "image01.offcut", std::ios::binary);
        const auto n = [&](uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                legacy.put(char(v >> (8 * i)));
        };
        legacy.write("OFFCUT01", 8);
        n(0);
        n(0);
        for (auto v : {20000, 15000, 1200, 200, 7, 2, 1, 0, 1})
            n(v);
        const auto wide = image(60, 20, 30);
        n(3);
        legacy.write("img", 3);
        n(1);
        n(60);
        n(20);
        n(uint32_t(wide.rgba.size()));
        legacy.write(reinterpret_cast<const char*>(wide.rgba.data()), std::streamsize(wide.rgba.size()));
        n(0);
        legacy.close();
        const auto one = load_project(folder / "image01.offcut");
        auto fit = one.options;
        require(fit.placement == placement_mode::fit && one.layers.size() == 1, "OFFCUT01 image project failed to load");
        layer unplaced{"img", wide};
        require(differing(flatten(one.layers, fit), flatten({unplaced}, fit)) == 0 && close_to(bounds(one.layers[0].transform).left, 12, .5),
                "OFFCUT01 image was not fitted as before");
    }
    // Undo history: steps, merging, redo branch, saved state, sharing.
    {
        project p;
        p.layers = {{"a", image(8, 8, 10)}};
        p.layers[0].transform = place_image(p.layers[0], p.options, placement_mode::fit, 0);
        p.assign_layer_ids();
        history h(4);
        h.reset(p);
        require(!h.can_undo() && !h.can_redo() && h.at_saved(), "Fresh history not empty and saved");
        p.selected_layer = 0;
        require(!h.commit(p, "Select"), "Selection alone made an undo step");
        const auto start = history::clock::now();
        const auto x0 = p.layers[0].transform.x_mm;
        p.layers[0].transform.x_mm = x0 + 1;
        require(h.commit(p, "Move", "move", start), "Move not recorded");
        p.layers[0].transform.x_mm = x0 + 2;
        h.commit(p, "Move", "move", start + std::chrono::milliseconds(100));
        require(h.size() == 2 && h.undo_label() == "Move", "Consecutive moves were not merged");
        p.layers[0].transform.x_mm = x0 + 3;
        h.commit(p, "Move", "move", start + std::chrono::seconds(5));
        require(h.size() == 3, "Moves far apart in time were merged");
        p.layers[0].visible = false;
        h.commit(p, "Hide image");
        require(h.undo(p) && p.layers[0].visible && p.layers[0].transform.x_mm == x0 + 3 && p.dirty, "Undo did not restore visibility");
        require(h.undo(p) && p.layers[0].transform.x_mm == x0 + 2, "Undo did not restore the merged move");
        require(h.redo(p) && p.layers[0].transform.x_mm == x0 + 3 && h.redo_label() == "Hide image", "Redo failed");
        h.mark_saved();
        require(h.undo(p) && p.dirty && h.redo(p) && !p.dirty, "Dirty state does not follow the saved state");
        h.undo(p);
        p.options.layers = 5;
        h.commit(p, "Sheets");
        require(!h.can_redo() && !h.at_saved(), "A new edit did not drop the redo branch");
        require(h.undo(p) && p.dirty, "The dropped saved state counted as saved");
        // Layers added and removed; pixels come back intact.
        p.layers.push_back({"b", image(4, 4, 200)});
        p.assign_layer_ids();
        h.commit(p, "Add image");
        p.layers.erase(p.layers.begin());
        h.commit(p, "Remove image");
        require(h.undo(p) && p.layers.size() == 2 && p.layers[0].image.rgba == image(8, 8, 10).rgba && p.layers[1].id == 2, "Undo of remove lost pixels");
        // The limit drops the oldest steps.
        for (int i = 0; i < 6; ++i)
        {
            p.name = "n" + std::to_string(i);
            h.commit(p, "Rename");
        }
        require(h.size() == 4, "History exceeds its limit");
        int undone = 0;
        while (h.undo(p))
            ++undone;
        require(undone == 3 && p.name == "n2", "Oldest steps not dropped in order");
        // Notes for AI and the AI Style are document edits too.
        p.layers[0].notes = "fur";
        h.commit(p, "Notes");
        p.ai_style = "woodcut";
        h.commit(p, "Style");
        require(h.undo(p) && p.ai_style.empty() && p.layers[0].notes == "fur" && h.undo(p) && p.layers[0].notes.empty(), "Notes/Style not undone");
        // Sheets are part of the state.
        p.cuts = {{"Layer 1", 2, 2, {1, 1, 1, 1}}};
        h.commit(p, "Generate");
        p.cuts.clear();
        h.commit(p, "Clear");
        require(h.undo(p) && p.cuts.size() == 1, "Undo did not restore sheets");
    }
    // Sheets: stacked front over back, unticked ones skipped, highlight.
    {
        const auto sheet = [](std::vector<uint8_t> solid) { return cut_layer{"s", 3, 1, std::move(solid)}; };
        std::vector<cut_layer> cuts{sheet({1, 0, 0}), sheet({1, 1, 0}), sheet({1, 1, 1})};
        const auto px = [](const raster& r, int x) { return std::array<uint8_t, 3>{r.rgba[x * 4], r.rgba[x * 4 + 1], r.rgba[x * 4 + 2]}; };
        auto r = render_cuts(cuts);
        require(px(r, 0) == sheet_colour(0, 3) && px(r, 1) == sheet_colour(1, 3) && px(r, 2) == sheet_colour(2, 3), "Assembled stacking wrong");
        cuts[1].visible = false;
        r = render_cuts(cuts);
        require(px(r, 1) == sheet_colour(2, 3), "Unticked sheet still drawn in the stack");
        cuts[1].visible = true;
        r = render_cuts(cuts, {.highlight = 1});
        require(px(r, 2) == sheet_colour(2, 3, true), "Other sheets not dimmed around the highlight");
        require(px(r, 1) == highlight_outline, "Highlighted sheet's edge not outlined");
        // A wider sheet: its interior keeps its own colour.
        std::vector<cut_layer> wide{{"front", 5, 3, std::vector<uint8_t>(15, 0)}, {"back", 5, 3, std::vector<uint8_t>(15, 1)}};
        for (int y = 0; y < 3; ++y)
            wide[1].solid[y * 5 + 4] = 0;
        r = render_cuts(wide, {.highlight = 1});
        require(px(r, 1) == sheet_colour(1, 2) && px(r, 3) == highlight_outline && px(r, 4)[0] == 255, "Highlighted sheet interior not shown in full");
        r = render_cuts(cuts, {.single = 1, .highlight = 0});
        require(px(r, 0) == sheet_colour(1, 3) && px(r, 2)[0] == 255, "Single sheet view wrong");
        cuts[0].visible = false;
        r = render_cuts(cuts, {.highlight = 0});
        require(px(r, 0) == highlight_outline && px(r, 1) == sheet_colour(1, 3, true), "Unticked highlighted sheet should show only its outline");
        // Floating pieces in red, where they show.
        std::vector<cut_layer> island{{"front", 5, 5, std::vector<uint8_t>(25, 0)}, {"back", 5, 5, std::vector<uint8_t>(25, 1)}};
        island[0].solid[12] = 1;
        r = render_cuts(island, {.floating = true});
        require(px(r, 12) == floating_colour && px(r, 0) == sheet_colour(1, 2), "Floating piece not marked");
        island[0].solid[2] = island[0].solid[7] = 1;  // now joined to the top edge
        r = render_cuts(island, {.floating = true});
        require(px(r, 12) == sheet_colour(0, 2), "Supported piece marked as floating");
    }
    // Row planning for lists that keep their place.
    {
        const std::vector<row_key> a{{"1", "x"}, {"2", "y"}, {"3", "z"}};
        auto plan = plan_rows(a, {{"1", "x"}, {"2", "Y"}, {"3", "z"}});
        require(plan.same_rows && plan.changed == std::vector<size_t>{1}, "A check change rebuilt the rows");
        plan = plan_rows(a, a);
        require(plan.same_rows && plan.changed.empty(), "Unchanged rows reported changes");
        plan = plan_rows(a, {{"1", "x"}, {"9", "n"}, {"2", "y"}, {"3", "z"}});
        require(!plan.same_rows && plan.first == 1 && plan.removed == 0 && plan.inserted == 1, "Insertion planned wrongly");
        plan = plan_rows(a, {{"1", "x"}, {"3", "Z"}});
        require(plan.first == 1 && plan.removed == 1 && plan.inserted == 0 && plan.changed == std::vector<size_t>{1}, "Removal planned wrongly");
        plan = plan_rows(a, {{"2", "y"}, {"1", "x"}, {"3", "z"}});
        require(plan.first == 0 && plan.removed == 2 && plan.inserted == 2, "Reorder planned wrongly");
        plan = plan_rows({}, a);
        require(plan.first == 0 && plan.inserted == 3 && plan.removed == 0, "Fill planned wrongly");
    }
    // The guidance bar: most important first.
    {
        project p;
        auto g = guidance_for(p, {});
        require(g.text.starts_with("Add an image") && g.action == guidance_action::add_images, "Empty project guidance wrong");
        p.layers = {{"a", padded_subject()}};
        p.layers[0].visible = false;
        require(guidance_for(p, {}).level == guidance_level::problem, "All-hidden images not flagged");
        p.layers[0].visible = true;
        g = guidance_for(p, {});
        require(g.text.starts_with("Arrange your images") && g.action == guidance_action::auto_, "Next step after adding wrong");
        p.options.smoothing = 0;
        p.options.layers = 2;
        p.layers[0].transform = place_image(p.layers[0], p.options, placement_mode::touch_edges, touch_bottom);
        p.cuts = simplify(p.layers, p.options);
        g = guidance_for(p, {0, 0});
        require(g.text == "Ready to export." && g.level == guidance_level::done, "Ready state wrong");
        g = guidance_for(p, {0, 3});
        require(g.text == "Sheet 2 has 3 floating pieces: they reach no edge of the sheet." && g.action == guidance_action::show_sheets,
                "Floating pieces not reported");
        p.stale = true;
        require(guidance_for(p, {0, 3}).text.starts_with("Design changed"), "Stale sheets not reported first");
        p.busy = true;
        p.activity = "Making sheets…";
        require(guidance_for(p, {}).text == p.activity, "A running job is not shown first");
    }
    std::cout << "Passed: transform maths (compose, rotate, crop, hit test, snapping, fit, touch), placement as initial transforms, OFFCUT03 round trip, "
                 "OFFCUT01/02 conversion, undo history, sheet stacking, highlight and floating pieces, row planning, guidance.\n";
}
// The AI loop (docs/ux-design.md section 8): prompt files and assembly,
// placeholders, chips, version threads, records, validation and import.
void test_ai_loop(const std::filesystem::path& folder)
{
    transform_options o;
    o.layers = 4;
    o.width_mm = 203.2;
    o.height_mm = 254;
    // Placeholders: values fill in; a line with an empty required one goes;
    // {name?} may be empty; unknown names stay as written.
    {
        const std::map<std::string, std::string> v{{"a", "one"}, {"empty", ""}};
        require(fill_prompt("x {a} y\nskip {empty}\nkeep{empty?}.\n{unknown}", v) == "x one y\nkeep.\n{unknown}", "Placeholder filling failed");
        require(strip_prompt_comments("# note\n\nline 1\n# more\nline 2\n\n") == "line 1\nline 2", "Prompt comments not stripped");
    }
    // Every file has a compiled-in copy; a missing folder falls back to them.
    const auto builtin = load_prompt_library({});
    for (const auto& name : prompt_file_names())
        require(!builtin.text(name).empty() && !builtin_prompt_text(name).empty(), "A prompt file has no compiled-in copy");
    std::filesystem::create_directories(folder / "prompts-partial");
    {
        std::ofstream(folder / "prompts-partial" / "output.txt") << "# edited\nOutput: {size}, edited.\n";
    }
    const auto partial = load_prompt_library(folder / "prompts-partial");
    require(partial.text("output.txt") == "Output: {size}, edited." && partial.text("brief.txt") == builtin.text("brief.txt") &&
                std::ranges::find(partial.fallbacks, "brief.txt") != partial.fallbacks.end(),
            "Missing prompt files did not fall back to the built-in copies");
    require(partial.version("output.txt") != builtin.version("output.txt") && builtin.version("brief.txt").starts_with("brief.txt@"),
            "Prompt file versions don't follow their text");
    // The brief stays whole, for a designer; outlines are forbidden plainly.
    require(builtin.text("brief.txt").contains("no outlines or contour lines") && builtin.text("brief.txt").contains("subject reference only"),
            "The brief lost the outline or reference rule");
    // Template assembly: depth directions first, the user's words, the output spec last.
    prompt_words w;
    w.task = ai_task::reimagine;
    w.width = 1024;
    w.height = 1536;
    w.style = "bold folk-art shapes";
    w.notes = "Taco, a tortoise";
    w.instructions = "plain sky";
    const auto a = assemble_prompt(builtin, o, w);
    const auto at = [&](const char* s) { return a.text.find(s); };
    require(at("layered paper shadowbox, seen straight on") < at("Layers, near to far:") && at("Layers, near to far:") < at("Style: bold folk-art shapes") &&
                at("Style:") < at("About this subject: Taco, a tortoise") && at("About this subject:") < at("Instructions for this request: plain sky") &&
                at("Instructions for this request:") < at("Output: the whole frame, 1024 × 1536 pixels") &&
                at("no outlines or contour lines") != std::string::npos && a.text.contains("exactly these 4 colours") &&
                a.text.contains("- 1 (front, colour #1e4d2b): the frame border") && a.text.contains("- 4 (back, colour #cfe6f2): plain sky") &&
                a.text.contains("subject reference only"),
            "Template prompt order or content wrong");
    require(!a.text.contains('{') && !a.text.contains("# ") && !a.text.contains("brief"), "Template prompt has unfilled placeholders or the brief");
    // Min. width in output pixels: 2 mm at 1536 / 254 px per mm ≈ 12.
    require(a.text.contains("nothing thinner than 12 pixels"), "Min. width in pixels wrong");
    require(a.files.contains("reimagine.txt@") && a.files.contains("preferences.txt@") && a.files.contains("output.txt@"), "Prompt file versions not recorded");
    {
        prompt_words quiet = w;
        quiet.style = quiet.notes = quiet.instructions = "";
        const auto q = assemble_prompt(builtin, o, quiet);
        require(!q.text.contains("Style:") && !q.text.contains("wishes") && !q.files.contains("preferences"), "Empty user text still introduced");
        prompt_words refine;
        refine.task = ai_task::refine;
        refine.done = "reimagined as a shadowbox";
        const auto r = assemble_prompt(builtin, o, refine);
        require(r.text.contains("Already done in earlier rounds, keep it: reimagined as a shadowbox") && !r.text.contains("Also fix what Offcut found"),
                "Refine template lines wrong");
    }
    require(default_planes(2).size() == 2 && default_planes(4).size() == 4 && default_planes(7).size() == 7 && default_planes(12).size() == 12 &&
                default_planes(7).back().contains("sky"),
            "Default depth planes don't match the sheet count");
    // A plan replaces the template: its planes and directions, then the spec.
    {
        ai_plan plan;
        plan.planes = {"tall grass", "the tortoise", "a low hill", "plain cream sky"};
        plan.scene_directions = {"The tortoise stands on a grassy bank that runs off both sides."};
        plan.avoid = {"a sun"};
        plan.palette = {{{20, 40, 30}}, {{90, 140, 60}}, {{160, 190, 90}}, {{240, 230, 200}}};
        const auto pa = assemble_prompt(builtin, o, w, &plan);
        require(pa.text.find("- 2: the tortoise") < pa.text.find("grassy bank") && pa.text.contains("Avoid: a sun.") && pa.text.contains("#14281e") &&
                    pa.text.find("grassy bank") < pa.text.find("Output:") && !pa.text.contains("Style:") && pa.files.ends_with("plan"),
                "Plan-compiled prompt wrong");
    }
    // The project's prompt (Copy prompt, Export request) is the same assembly.
    {
        project p;
        p.options = o;
        p.ai_style = "woodcut";
        p.instructions = "a hill running off both sides";
        p.layers = {{"taco", image(40, 30, 90)}};
        p.layers[0].notes = "keep the shell spots";
        p.layers[0].transform = place_image(p.layers[0], o, placement_mode::touch_edges, touch_bottom);
        const auto text = ai_prompt(p);
        require(text.contains("Style: woodcut") && text.contains("About this subject: keep the shell spots") &&
                    text.contains("Instructions for this request: a hill running off both sides") && text.contains("meets the frame at the bottom") &&
                    text == assemble_prompt(prompts(), o, reimagine_words(p)).text,
                "Project prompt not assembled from Style, Notes and instructions");
    }
    // Chips: ids, follow-up text with placeholders, recorded words.
    {
        const auto chips = parse_review_chips(builtin.text("review-chips.txt"));
        require(chips.size() == 14 && std::ranges::all_of(chips, [](const review_chip& c) { return c.id.contains('.'); }), "Review chips not parsed");
        const auto use = std::ranges::find(chips, "support.use", &review_chip::id);
        require(use != chips.end() && use->asks() && use->menu == "Support", "Use… chip missing");
        const std::vector<chip_choice> chosen{{"support.added-on", ""}, {"support.use", "reeds"}, {"detail.thin-lines", ""}};
        const auto text = chip_instruction(chips, chosen, o);
        require(text.starts_with("The supports look added on.") && text.contains("Use reeds to hold up") && text.contains("at least 12 pixels wide") &&
                    !text.contains('{'),
                "Chip instruction wrong");
        require(chip_review(chips, chosen) == "support looks added on; support use reeds; detail lines too thin", "Chip review words wrong");
        require(chip_ids(chosen) == "support.added-on support.use=reeds detail.thin-lines", "Chip ids wrong");
        require(chip_instruction(chips, {{"no.such", ""}}, o).empty(), "Unknown chip sent text");
    }
    // Version threads: add, branch, set current, discard, drift.
    {
        layer l{"AI illustration", image(16, 20, 200)};
        l.type = layer_type::ai_illustration;
        item_version v1;
        v1.image = image(32, 40, 10);  // resampled to the item's size
        v1.issues = 3;
        v1.instructions = "plain sky";
        v1.task = "Reimagine";
        v1.run = "r1";
        require(add_version(l, v1) == 1 && l.version == 1 && l.versions[0].image.width == 16 && l.versions[0].state == review_state::pending &&
                    current_pixels(l).rgba == l.versions[0].image.rgba && to_review(l),
                "Adding a version failed");
        item_version v2;
        v2.parent = 1;
        v2.image = image(16, 20, 50);
        v2.issues = 5;  // more than its parent: drift
        v2.task = "Refine";
        v2.instructions = "simpler fur";
        require(add_version(l, v2) == 2 && l.versions[1].previous == 1 && drifted(l, l.versions[1]) && !drifted(l, l.versions[0]), "Drift guard wrong");
        item_version v3;  // Try again of 1: a sibling of 2, on the same row
        v3.parent = 0;
        v3.image = image(16, 20, 60);
        v3.task = "Try again";
        add_version(l, v3);
        item_version v4;  // refining 2 after a sibling: a branch row
        v4.parent = 2;
        v4.image = image(16, 20, 70);
        v4.task = "Refine";
        v4.instructions = "bird on the branch";
        add_version(l, v4);
        const auto cells = version_layout(l);
        const auto cell = [&](uint32_t id) { return *std::ranges::find(cells, id, &version_cell::id); };
        require(cell(0).row == 0 && cell(0).column == 0 && cell(1).row == 0 && cell(1).column == 1 && cell(2).row == 0 && cell(2).column == 2 &&
                    cell(3).row == 1 && cell(4).row == 0 && cell(4).column == 3,
                "Version layout wrong");
        item_version v5;  // refining 1 again: its row's end is 4, not 1's child → branch
        v5.parent = 1;
        v5.image = image(16, 20, 80);
        add_version(l, v5);
        require(cell(0).row == 0 && version_layout(l).back().row == 2 && version_layout(l).back().column == 2, "Branch row wrong");
        const auto done = thread_requests(l, 4);
        require(done.size() == 3 && done[0] == "reimagined as a shadowbox: “plain sky”" && done[1] == "refine: “simpler fur”", "Thread summary wrong");
        require(set_current_version(l, 2) && l.version == 2 && current_pixels(l).rgba == image(16, 20, 50).rgba && !set_current_version(l, 99),
                "Setting the current version failed");
        require(set_current_version(l, 4) && discard_version(l, 4) && l.version == 3 && !find_version(l, 4),
                "Discard did not return to the version current before");  // 4 arrived while 3 was current
        require(discard_version(l, 1) && find_version(l, 2)->parent == 0 && find_version(l, 5)->parent == 0 && !discard_version(l, 1),
                "Discard did not move children to the parent");
        require(set_current_version(l, 0) && current_pixels(l).rgba == l.image.rgba, "Version 0 is not the original");
        project p;
        remember_instruction(p, "  plain sky ");
        for (int i = 0; i < 12; ++i)
            remember_instruction(p, "instruction " + std::to_string(i));
        remember_instruction(p, "instruction 3");
        require(p.recent_instructions.size() == 10 && p.recent_instructions.front() == "instruction 3" && p.instructions == "instruction 3" &&
                    std::ranges::count(p.recent_instructions, "instruction 3") == 1,
                "Recent instructions wrong");
    }
    // OFFCUT03 round trip of version records and review state.
    {
        project p;
        p.options = o;
        p.layers = {{"AI illustration", image(16, 20, 200)}};
        p.layers[0].type = layer_type::ai_illustration;
        p.layers[0].transform = place_image(p.layers[0], o, placement_mode::fit, 0);
        item_version v{1, 0, image(16, 20, 30), "the prompt", "support looks added on", review_state::answered};
        v.task = "Reimagine";
        v.instructions = "plain sky";
        v.style = "woodcut";
        v.notes = "Taco";
        v.prompt_files = "reimagine.txt@01234567";
        v.account = "pollinations";
        v.provider = "Pollinations";
        v.model = "microsoft/mai-image-2.7";
        v.created = "2026-11-24T14:00:00";
        v.seconds = 41.5;
        v.usage = R"({"input_tokens":10})";
        v.issues = 7;
        v.findings = "7 pieces would float.";
        v.request = "C:/requests/offcut-ai-request-01";
        v.run = "run-1";
        v.round = 2;
        v.score = 12030;
        v.measurements = R"({"floating":7})";
        v.plan = R"({"plan_version":1})";
        v.verdict = R"({"verdict":"refine"})";
        v.ops = "[]";
        v.explanation = "Sun cut as a hole.";
        v.chips = "support.added-on support.use=reeds";
        v.illustrator = {41.5, 10, 20};
        v.director = {3.25, 1000, 200};
        add_version(p.layers[0], v);
        p.layers[0].versions[0].state = review_state::answered;
        p.recent_instructions = {"plain sky", "a hill"};
        p.instructions = "plain sky";
        p.assign_layer_ids();
        save_project(p, folder / "versions03.offcut");
        const auto back = load_project(folder / "versions03.offcut");
        const auto& b = back.layers[0].versions.at(0);
        require(b.state == review_state::answered && b.review == "support looks added on" && b.task == "Reimagine" && b.instructions == "plain sky" &&
                    b.style == "woodcut" && b.notes == "Taco" && b.prompt_files == v.prompt_files && b.account == "pollinations" && b.model == v.model &&
                    b.created == v.created && b.seconds == 41.5 && b.usage == v.usage && b.issues == 7 && b.findings == v.findings && b.request == v.request &&
                    b.run == "run-1" && b.round == 2 && b.score == 12030 && b.measurements == v.measurements && b.plan == v.plan && b.verdict == v.verdict &&
                    b.ops == "[]" && b.explanation == v.explanation && b.chips == v.chips && b.illustrator == v.illustrator && b.director == v.director &&
                    b.image.rgba == image(16, 20, 30).rgba,
                "OFFCUT03 did not round-trip a version record");
        require(back.recent_instructions == p.recent_instructions && back.layers[0].version == 1 && to_review(back.layers[0]) == false,
                "OFFCUT03 did not round-trip recent instructions or the current version");
        // Undo keeps version records and their pixels.
        history h;
        auto q = back;
        h.reset(q);
        q.layers[0].versions[0].state = review_state::used;
        q.layers[0].versions[0].review = "used";
        require(h.commit(q, "Use AI result") && h.undo(q) && q.layers[0].versions[0].state == review_state::answered &&
                    q.layers[0].versions[0].image.rgba == image(16, 20, 30).rgba && q.layers[0].versions[0].director == v.director,
                "Undo lost a version's review or pixels");
    }
    // Validation: thin features and floating pieces, in words and as measurements.
    {
        transform_options t;
        t.layers = 3;
        t.bridge_mm = 2;
        const auto g = dimensions(t);
        cut_layer c{"", g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h)};
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
            {
                const bool thick = x >= 100 && x < 200 && y >= 100 && y < 200;   // a block
                const bool line = x >= 300 && x < 302 && y >= 50 && y < g.h;     // a 2 px line to the bottom edge
                const bool island = x >= 150 && x < 180 && y >= 300 && y < 330;  // floats
                c.solid[size_t(y) * g.w + x] = border(x, y, g) || thick || line || island;
            }
        const auto thin = thin_features(c, t, 0);
        require(thin.size() == 1 && std::abs(thin[0].x - 300.5) < 1, "Thin line not found, or the block called thin");
        auto v = validate_sheets({c, cut_layer{"", g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 1)}}, t, {});
        require(v.floating.size() == 2 && v.thin.size() == 1, "Floating pieces not found");  // the block and the island
        v.tones_found = 9;
        v.tones_wanted = 3;
        const auto words = describe_findings(v, t);
        require(words.contains("2 pieces of card would float") && words.contains("1 line or detail is thinner than 2 mm") &&
                    words.contains("9 distinct colours; it needs exactly 3") && v.count() == 4,
                "Findings wrong in words");
        const auto json = measurements_json(v, t);
        require(json.contains("\"id\":\"m1\"") && json.contains("\"kind\":\"thin\"") && json.contains("\"found\":9") && v.score(t) > 1000,
                "Measurements or score wrong");
    }
    // Import: outlines dissolve into the fills either side; a palette is snapped to.
    {
        transform_options t;
        t.layers = 3;
        const auto g = dimensions(t);
        tone_map map{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 2)};
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
            {
                auto& v = map.indices[size_t(y) * g.w + x];
                if (x >= g.w / 2)
                    v = 1;
                if (x >= g.w / 2 - 2 && x < g.w / 2 + 2 && y > 100 && y < 400)
                    v = 0;  // a 4 px outline between the two fills
                if (x >= 100 && x < 200 && y >= 450 && y < 520)
                    v = 0;  // a large dark shape stays
            }
        const auto before_big = map.indices[size_t(480) * g.w + 150];
        const auto changed = dissolve_outlines(map, t);
        require(changed > 0 && map.indices[size_t(200) * g.w + g.w / 2 - 2] == 2 && map.indices[size_t(200) * g.w + g.w / 2 + 1] == 1 &&
                    map.indices[size_t(480) * g.w + 150] == before_big,
                "Outline was not split between its fills, or a dark shape dissolved");
        raster picture{90, 120, std::vector<uint8_t>(size_t(90) * 120 * 4, 255)};
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 90; ++x)
            {
                const uint8_t c = y < 40 ? 30 : y < 80 ? 130 : 220;
                for (int ch = 0; ch < 3; ++ch)
                    picture.rgba[(size_t(y) * 90 + x) * 4 + ch] = c;
            }
        const auto snapped = layers_from_illustration(picture, t, {{{0, 0, 0}}, {{128, 128, 128}}, {{255, 255, 255}}});
        require(snapped.tones_found == 3 && snapped.layers.size() == 3, "Palette snap or tone count failed");
        rejects([&] { layers_from_illustration(picture, t, {{{0, 0, 0}}}); }, "A palette without a colour per sheet accepted");
    }
    // The visible picture is locked: the sheets stacked front to back show
    // the approved flat picture exactly. A scene with a floating island, a
    // thin stalk, a far shape behind a near one and a hole through to the back.
    {
        transform_options t;
        t.layers = 4;
        t.connect = false;
        const auto g = dimensions(t);
        tone_map map{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h, 3)};
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
            {
                auto& v = map.indices[size_t(y) * g.w + x];
                if (y > g.h * 2 / 3)
                    v = 1;  // ground
                if (x > 150 && x < 350 && y > 200 && y < g.h * 2 / 3 + 20)
                    v = 2;  // a far hill, partly behind the ground
                if (x > 220 && x < 280 && y > 300 && y < 460)
                    v = 0;  // the subject, standing in the ground
                if (x > 240 && x < 260 && y > 340 && y < 360)
                    v = 3;  // a hole through to the backing
                if (x > 100 && x < 130 && y > 100 && y < 130)
                    v = 1;  // floats in the sky
                if (x >= 400 && x < 402 && y > 150)
                    v = 0;  // a thin stalk
            }
        auto expected = map;
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
                if (border(x, y, g))
                    expected.indices[size_t(y) * g.w + x] = 0;
        const auto kept = build_sheets(map, t);
        require(composite_mismatch(kept.layers, expected) == 0, "Auto's sheets changed the visible picture (floating pieces kept)");
        // With "Remove floating pieces" on, the island is gone from view: by
        // design, and reported (it shows only as the removed piece).
        t.connect = true;
        const auto removed = build_sheets(map, t);
        size_t removed_pixels = 0;
        for (const auto& r : removed.removed)
            removed_pixels += r.pixels;
        const auto lost = composite_mismatch(removed.layers, expected);
        require(lost > 0 && lost <= removed_pixels, "Removing floating pieces changed more than the pieces");
        std::cout << "Visible-picture lock: Auto with Remove floating pieces on hides " << lost << " px (the removed pieces); off: exact.\n";
        // AI import: a flat picture in the palette's colours, snapped to it.
        t.connect = false;
        const std::vector<std::array<uint8_t, 3>> palette{{{20, 30, 25}}, {{70, 120, 60}}, {{150, 170, 110}}, {{235, 228, 205}}};
        raster picture{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h * 4, 255)};
        for (size_t p = 0; p < expected.indices.size(); ++p)
            std::copy_n(palette[expected.indices[p]].begin(), 3, picture.rgba.begin() + p * 4);
        for (bool dissolve : {false, true})
        {
            const auto imported = layers_from_illustration(picture, t, palette, dissolve);
            const auto mismatch = composite_mismatch(imported.layers, expected);
            if (dissolve)
                std::cout << "Visible-picture lock: AI import with outlines dissolved changes " << mismatch
                          << " px (the thin dark stalk, given to its neighbours by design).\n";
            else
            {
                if (mismatch)
                    std::cout << "AI import mismatch: " << mismatch << " px\n";
                require(mismatch == 0, "AI import's sheets changed the visible picture");
            }
        }
    }
}
// Providers: Pollinations' paid-only models, free defaults, the paid-balance
// hint; reasoning models and two defaults in ai-accounts.txt.
void test_ai_providers(const std::filesystem::path& folder)
{
    const auto json =
        R"([{"name":"microsoft/mai-image-2.7","input_modalities":["text","image"],"paid_only":false},{"name":"gptimage","input_modalities":["image"],"paid_only":true},)"
        R"({"name":"flux.2-flex","input_modalities":["image"]},{"name":"flux.1-kontext-pro","input_modalities":["image"],"paid_only":false},)"
        R"({"name":"seedream-pro","input_modalities":["image","text"],"paid_only":true},{"name":"text-paid","input_modalities":["text"],"paid_only":true}])";
    const auto models = filter_ai_models(ai_model_list::pollinations_images, json);
    const auto paid = paid_ai_models(ai_model_list::pollinations_images, json);
    require(models.size() == 5 && paid == std::vector<std::string>{"gptimage", "seedream-pro"}, "Pollinations paid_only filtering failed");
    require(paid_ai_models(ai_model_list::openai_images, R"({"data":[{"id":"gpt-image-2.6-sunburst","paid_only":true}]})").empty(),
            "Paid models outside Pollinations");
    const auto& poll = find_ai_provider("pollinations");
    const auto without = [&](std::vector<std::string> list, std::initializer_list<const char*> gone)
    {
        for (const char* g : gone)
            std::erase(list, g);
        return list;
    };
    require(default_ai_model(poll, models, {}, paid) == "microsoft/mai-image-2.7" &&
                default_ai_model(poll, without(models, {"microsoft/mai-image-2.7"}), {}, paid) == "flux.2-flex" &&
                default_ai_model(poll, without(models, {"microsoft/mai-image-2.7", "flux.2-flex"}), {}, paid) == "flux.1-kontext-pro" &&
                default_ai_model(poll, {"gptimage", "zzz-free"}, {}, {"gptimage"}) == "zzz-free" &&
                default_ai_model(poll, {"gptimage"}, {}, {"gptimage"}) == "gptimage" && default_ai_model(poll, models, "gptimage", paid) == "gptimage",
            "Pollinations free-model defaults wrong");
    ai_account a{"pollinations", "pollinations", "Pollinations", poll.base_url, "microsoft/mai-image-2.7", "Offcut/AI/pollinations", models};
    a.paid_models = paid;
    require(ai_model_label(a, "gptimage") == "gptimage (paid)" && ai_model_label(a, "flux.2-flex") == "flux.2-flex", "Paid model label wrong");
    const auto hint = ai_failure_detail(ai_http_error(402, "This model requires a paid balance; your Quest balance can't be used."));
    require(hint.contains("Pick a free model") && hint.contains("paid balance"), "Paid-balance failure has no hint");
    require(!ai_failure_detail(ai_http_error(401, "Invalid key")).contains("free model"), "Paid hint on an unrelated failure");
    ai_endpoint e;
    e.provider_id = "openai";
    e.model = "gpt-image-2.6-sunburst";
    require(supports_input_fidelity(e), "input_fidelity not offered to GPT Image");
    e.model = "gpt-image-1-mini";
    require(!supports_input_fidelity(e), "input_fidelity offered to a mini model");
    e.provider_id = "pollinations";
    e.model = "microsoft/mai-image-2.7";
    require(!supports_input_fidelity(e), "input_fidelity offered to Pollinations");
    // Roles: a reasoning model per entry, "Plans and reviews with" and "Draws with".
    ai_accounts c;
    c.default_id = "pollinations";
    c.plans_id = "openai";
    c.entries = {{"openai", "openai", "OpenAI", "https://api.openai.com/v1", "", "Offcut/AI/openai", {}}, a};
    c.entries[0].reasoning_model = "gpt-5.5";
    write_ai_accounts(c, folder / "accounts-roles");
    const auto r = read_ai_accounts(folder / "accounts-roles", [](const std::string&) { return false; });
    require(r.plans_account() && r.plans_account()->reasoning_model == "gpt-5.5" && r.default_account()->id == "pollinations" && r.entries[0].model.empty() &&
                r.entries[1].reasoning_model.empty() && r.entries[1].models == models,
            "Reasoning models or the two defaults did not persist");
    std::filesystem::create_directories(folder / "accounts-v1");
    {
        std::ofstream(folder / "accounts-v1" / "ai-accounts.txt") << "OFFCUT_AI_ACCOUNTS_1 \"openai\"\n\"openai\" \"openai\" \"OpenAI\" "
                                                                     "\"https://api.openai.com/v1\" \"gpt-image-2.6-sunburst\" \"Offcut/AI/openai\" 0\n";
    }
    const auto v1 = read_ai_accounts(folder / "accounts-v1", [](const std::string&) { return false; });
    require(v1.entries.size() == 1 && v1.entries[0].model == "gpt-image-2.6-sunburst" && v1.entries[0].reasoning_model.empty() && !v1.plans_account(),
            "Version 1 provider settings no longer load");
}
// -- Auto loop (docs/ux-design.md section 8, "The loop") ---------------------
// The director and the illustrator are fakes: scripted plans, verdicts,
// drawings and scores, never a network.
namespace auto_fixture
{
const char* colours[] = {"#203040", "#c08040", "#60a050", "#e0f0f0", "#802020", "#f0d020", "#4060c0", "#a0a0a0"};
std::string plan_json(int sheets, const std::string& risk = "Keep the joins wide.", const std::string& route = "generate")
{
    std::string s = "{\"plan_version\":1,\"route\":\"" + route +
                    "\",\"summary\":\"A tortoise on a bank under a plain sky.\",\"style\":{\"notes\":\"flat\"},\"sheets\":[";
    for (int i = 0; i < sheets; ++i)
        s += std::format("{}{{\"id\":\"s{}\",\"depth\":{},\"card\":\"{}\",\"role\":\"{}\",\"holds\":\"plane {}\"}}", i ? "," : "", i + 1, i + 1, colours[i],
                         i == 0 ? "front" : i + 1 == sheets ? "backing" : "mid", i + 1);
    s += "],\"features\":[{\"id\":\"subject\",\"label\":\"Tortoise\",\"source\":\"reference\",\"keep\":\"keep\",\"simplify_to\":null,"
         "\"arrangement\":{\"kind\":\"sheet\",\"sheet\":\"s1\",\"through\":[],\"merge_with\":null},"
         "\"support\":{\"by\":\"frame\",\"with\":\"bottom\",\"contact_mm\":10},\"extends_behind\":[],\"hidden_support\":false,\"evidence\":[],"
         "\"attention\":null}],"
         "\"scene_directions\":[\"The tortoise stands on a bank that runs off both sides.\"],\"avoid\":[\"outlines\"],\"local\":{\"sheet_count\":" +
         std::to_string(sheets) + ",\"palette\":[";
    for (int i = 0; i < sheets; ++i)
        s += std::format("{}{{\"colour\":\"{}\",\"sheet\":\"s{}\"}}", i ? "," : "", colours[i], i + 1);
    s += "],\"ops\":[]},\"splits\":[],\"risks\":[" + ai_json_quote(risk) + "]}";
    return s;
}
std::string verdict(const std::string& kind)
{
    if (kind == "pass")
        return R"({"verdict":"pass","explain":"It reads well.","local_ops":[],"refine":null,"plan_patch":[]})";
    if (kind == "fix_local")
        return R"({"verdict":"fix_local","explain":"Two specks go.","local_ops":[{"op":"remove","target":"m1","sheet":null,"colour":null,"count":null,"width_mm":null}],"refine":null,"plan_patch":[]})";
    if (kind == "replan")
        return R"({"verdict":"replan","explain":"The planes swap.","local_ops":[],"refine":null,"plan_patch":[{"op":"replace","path":"/summary","value":"Swapped."}]})";
    return R"({"verdict":"refine","explain":"The bank needs redrawing.","local_ops":[],"refine":{"mode":"edit","instruction":"The bank runs off both sides.","marks":["m1"]},"plan_patch":[]})";
}
// Bands of the request's palette, front at the bottom: every band reaches the frame.
raster drawing(const prepared_ai_upload& u, int shift = 0)
{
    auto palette = parse_palette(u.palette);
    if (palette.empty())
        palette = default_palette(u.options.layers);
    raster r{256, 320, {}};
    r.rgba.resize(size_t(r.width) * r.height * 4);
    const int n = int(palette.size());
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x)
        {
            const int band = std::clamp(n - 1 - (y + shift) * n / r.height, 0, n - 1);
            auto* px = &r.rgba[(size_t(y) * r.width + x) * 4];
            px[0] = palette[size_t(band)][0], px[1] = palette[size_t(band)][1], px[2] = palette[size_t(band)][2], px[3] = 255;
        }
    return r;
}
struct fake
{
    int project_sheets = 4, plan_sheets = 4;
    std::string risk = "Keep the joins wide.", route = "generate";
    std::vector<std::string> verdicts;  // in order
    std::vector<double> scores = {0};   // per check, in order; the last repeats
    int plans = 0, critiques = 0, draws = 0, checks = 0, fixes = 0;
    std::vector<std::string> prompts, critique_instructions;
    std::vector<size_t> critique_images;
    std::shared_ptr<std::atomic_bool> cancel_on_draw;  // set during this draw
    int cancel_at_draw = 0;
    auto_roles roles(bool with_illustrator = true)
    {
        auto_roles r;
        r.plan = [this](const director_input& in, const std::filesystem::path&, const std::atomic_bool*)
        {
            ++plans;
            require(in.sheets == project_sheets && !in.images.empty() && !in.measurements.empty(), "Plan input incomplete");
            director_reply reply;
            reply.json = plan_json(plan_sheets, risk, route);
            reply.plan = validate_director_plan(reply.json, in.sheets);
            reply.explanation = reply.plan.summary;
            reply.seconds = 2;
            reply.input_tokens = 100;
            reply.output_tokens = 50;
            return reply;
        };
        r.critique = [this](const director_input& in, const std::filesystem::path&, const std::atomic_bool*)
        {
            require(!in.plan.empty() && !in.measurements.empty(), "Critique input incomplete");
            critique_images.push_back(in.images.size());
            critique_instructions.push_back(in.instructions);
            director_reply reply;
            reply.json = verdict(critiques < int(verdicts.size()) ? verdicts[size_t(critiques)] : "pass");
            reply.explanation = ai_json_string(reply.json, {"explain"}).value_or("");
            reply.seconds = 1;
            reply.input_tokens = 10;
            reply.output_tokens = 5;
            ++critiques;
            return reply;
        };
        if (with_illustrator)
        {
            r.illustrator = ai_account{"fake", "openai", "Fake", "http://127.0.0.1:9/v1", "gpt-image-2.6-flare", {}, {}, {}};
            r.draw = [this](const prepared_ai_upload& u, const std::filesystem::path&, const std::atomic_bool*)
            {
                ++draws;
                if (cancel_on_draw && draws == cancel_at_draw)
                {
                    cancel_on_draw->store(true);
                    throw ai_cancelled();
                }
                prompts.push_back(u.prompt);
                ai_result result;
                result.image = drawing(u, draws);
                result.seconds = 3;
                result.usage = R"({"input_tokens":7,"output_tokens":9})";
                return result;
            };
        }
        r.local_ops = [this](const local_ops_input& in)
        {
            ++fixes;
            local_ops_result out;
            out.image = in.image;
            out.image.rgba[0] ^= 1;
            out.changed_pixels = 1;
            out.applied = {"Removed the speck m1"};
            return out;
        };
        r.plan_patch = [](const std::string& plan, const std::string& patch)
        {
            require(patch.contains("/summary"), "Plan patch not passed on");
            auto out = plan;
            const std::string was = "A tortoise on a bank under a plain sky.";
            return out.replace(out.find(was), was.size(), "Swapped.");
        };
        r.score = [this](const std::string&)
        {
            const auto i = std::min(size_t(checks++), scores.size() - 1);
            return scores[i];
        };
        return r;
    }
};
project make_project(int sheets)
{
    project p;
    raster source{200, 250, {}};
    source.rgba.resize(size_t(source.width) * source.height * 4);
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < source.width; ++x)
        {
            auto* px = &source.rgba[(size_t(y) * source.width + x) * 4];
            px[0] = uint8_t(x), px[1] = uint8_t(y), px[2] = 128, px[3] = 255;
        }
    layer l;
    l.name = "Taco";
    l.image = source;
    p.layers = {l};
    p.options = new_project_options(source);
    p.options.layers = sheets;
    p.layers[0].transform = place_image(p.layers[0], p.options, p.options.placement, p.options.touch_edges);
    p.assign_layer_ids();
    p.selected_layer = 0;
    return p;
}
auto_run new_run(const std::filesystem::path& folder, int rounds = 3, auto_until until = auto_until::good)
{
    auto_run run;
    run.id = "run-test";
    run.folder = folder;
    run.settings.rounds = rounds;
    run.settings.until = until;
    return run;
}
const layer& item(const project& p)
{
    for (const auto& l : p.layers)
        if (l.type == layer_type::ai_illustration)
            return l;
    throw std::runtime_error("No AI illustration item");
}
}  // namespace auto_fixture
void test_auto_loop(const std::filesystem::path& folder)
{
    using namespace auto_fixture;
    const auto dir = folder / "auto-loop";
    std::filesystem::create_directories(dir);
    // The smoke's sequence: plan → draw → fix_local → refine → pass.
    {
        fake f;
        f.verdicts = {"fix_local", "refine", "pass"};
        f.scores = {50, 40, 10};
        auto p = make_project(4);
        auto run = new_run(dir);
        run_auto(p, run, f.roles());
        const auto& l = item(p);
        require(run.stop == auto_stop::passed && f.plans == 1 && f.draws == 2 && f.critiques == 3 && f.fixes == 1, "Auto loop sequence wrong");
        require(l.versions.size() == 3 && l.versions[1].task == "Local fix" && l.versions[1].parent == 1 && l.versions[1].round == 1 &&
                    l.versions[2].parent == 2 && l.versions[2].round == 2 && l.versions[2].task == "Refine",
                "Auto thread: local fix in the round, refinement from it");
        require(std::ranges::all_of(l.versions, [](const item_version& v) { return v.run == "run-test" && !v.plan.empty() && !v.palette.empty(); }),
                "Auto versions lack run, plan or palette");
        require(!l.versions[0].verdict.empty() && l.versions[1].ops.contains("Removed the speck m1") && l.versions[2].explanation == "It reads well.",
                "Verdicts, ops or explanation not recorded");
        require(l.versions[0].director.input_tokens == 110 && l.versions[0].director.seconds == 3 && l.versions[0].illustrator.input_tokens == 7 &&
                    l.versions[0].illustrator.seconds == 3,
                "Per-role usage not recorded on the round's version");
        require(f.prompts.size() == 2 && f.prompts[0].contains("The tortoise stands on a bank") && f.prompts[0].contains("exactly these 4 colours") &&
                    f.prompts[1].starts_with("Change the first attached image") && f.prompts[1].contains("The bank runs off both sides."),
                "Prompts not compiled from the plan, or the refinement missing");
        require(f.critique_images.front() >= 2, "Critique images missing");
        require(l.version == 3 && l.versions[2].state == review_state::pending && l.versions[0].state == review_state::answered && run.best == 3,
                "The best round is not presented for review");
    }
    // Stop at max rounds and present the best-scoring round, not the last.
    {
        fake f;
        f.verdicts = {"refine", "refine", "refine"};
        f.scores = {30, 10, 20};
        auto p = make_project(4);
        auto run = new_run(dir);
        run_auto(p, run, f.roles());
        const auto& l = item(p);
        require(run.stop == auto_stop::rounds && f.draws == 3 && l.versions.size() == 3, "Max rounds did not stop the loop");
        require(run.best == 2 && l.version == 2 && l.versions[1].state == review_state::pending && l.versions[2].state == review_state::answered,
                "Auto presented the last round instead of the best");
        require(best_auto_version(l, "run-test") == 2, "Best version choice wrong");
    }
    // No improvement for two rounds stops early.
    {
        fake f;
        f.verdicts = {"refine", "refine", "refine", "refine", "refine"};
        f.scores = {10, 20, 30, 5};
        auto p = make_project(4);
        auto run = new_run(dir, 5);
        run_auto(p, run, f.roles());
        require(run.stop == auto_stop::no_improvement && f.draws == 3 && run.best == 1, "No-improvement stop wrong");
    }
    require(auto_errors(R"({"floating":3,"thin":1})") == 3 && auto_errors(R"({"floating":0})") == 0, "Error count from measurements wrong");
    // Sheet count: reduced when the plan says why, proposed otherwise.
    {
        require(!sheet_reduction_reason(plan_json(4, "The four-colour limit trades tomato and meat for larger shapes."), 7).empty() &&
                    sheet_reduction_reason(plan_json(4, "Keep the joins wide."), 7).empty() &&
                    sheet_reduction_reason(plan_json(4, "The four-colour limit trades tomato for larger shapes."), 4).empty(),
                "Sheet reduction confidence wrong");
        fake f;
        f.project_sheets = 7;
        f.risk = "Fewer planes read better: the four-colour limit merges the filling.";
        f.verdicts = {"pass"};
        auto p = make_project(7);
        auto run = new_run(dir);
        run_auto(p, run, f.roles());
        require(p.options.layers == 4 && run.sheets_note.starts_with("Sheets 7 → 4") && item(p).versions[0].explanation.contains("Sheets 7 → 4") &&
                    p.cuts.size() == 4,
                "Confident sheet reduction not applied or not said");
        fake g;
        g.project_sheets = 7;
        g.verdicts = {"pass"};
        auto q = make_project(7);
        auto run2 = new_run(dir);
        run_auto(q, run2, g.roles());
        require(q.options.layers == 7 && run2.sheets_note.contains("set Sheets to 4"), "Unexplained reduction was applied instead of proposed");
    }
    // Plan only stops after the plan; nothing is drawn. Re-run from plan draws from it.
    {
        fake f;
        auto p = make_project(4);
        auto run = new_run(dir, 3, auto_until::plan_only);
        run_auto(p, run, f.roles());
        require(run.stop == auto_stop::plan_only && f.draws == 0 && !run.plan.empty() && p.layers.size() == 1, "Plan only drew or lost the plan");
        run.settings.until = auto_until::good;
        run.next = auto_run::step::draw;
        f.verdicts = {"pass"};
        run_auto(p, run, f.roles());
        require(run.stop == auto_stop::passed && f.plans == 1 && f.draws == 1, "Re-run from plan re-planned or did not draw");
    }
    // Ask after each round: stops with the round shown, Continue resumes, Stop presents the best.
    {
        fake f;
        f.verdicts = {"refine", "refine", "refine"};
        f.scores = {10, 30};
        auto p = make_project(4);
        auto run = new_run(dir, 3, auto_until::ask_each_round);
        run_auto(p, run, f.roles());
        require(run.stop == auto_stop::asking && f.draws == 1 && item(p).version == 1, "Ask after each round did not stop after round 1");
        run_auto(p, run, f.roles());  // Continue
        require(run.stop == auto_stop::asking && f.draws == 2 && item(p).version == 2, "Continue did not run one more round");
        finish_auto(p, run);  // Stop
        require(item(p).version == 1 && run.best == 1, "Stop did not present the best round");
    }
    // Replan: the patch applies, then a fresh drawing from the composition.
    {
        fake f;
        f.verdicts = {"replan", "pass"};
        f.scores = {20, 10};
        auto p = make_project(4);
        auto run = new_run(dir);
        run_auto(p, run, f.roles());
        const auto& l = item(p);
        require(run.stop == auto_stop::passed && run.plan.contains("Swapped.") && l.versions.size() == 2 && l.versions[1].parent == 0 &&
                    l.versions[1].task == "Replan" && l.versions[1].plan.contains("Swapped."),
                "Replan did not patch the plan and draw again");
    }
    // Cancel mid-run keeps what was made and presents the best.
    {
        fake f;
        f.verdicts = {"refine", "refine"};
        f.cancel_on_draw = std::make_shared<std::atomic_bool>(false);
        f.cancel_at_draw = 2;
        auto p = make_project(4);
        auto run = new_run(dir);
        run_auto(p, run, f.roles(), f.cancel_on_draw.get());
        require(run.stop == auto_stop::cancelled && item(p).versions.size() == 1 && item(p).version == 1, "Cancel lost the round made");
    }
    // A review resumes the loop for one round, with the review as critique input.
    {
        fake f;
        f.verdicts = {"pass", "refine", "pass"};
        f.scores = {10, 5};
        auto p = make_project(4);
        auto run = new_run(dir);
        run_auto(p, run, f.roles());
        auto again = resume_auto_from(p, item(p).id, 1, "Support looks added on");
        again.folder = dir;
        run_auto(p, again, f.roles());
        require(f.critique_instructions[1].starts_with("Support looks added on") && f.draws == 2 && again.stop == auto_stop::passed &&
                    item(p).versions.size() == 2 && again.best == 2,
                "A review did not resume one round through the director");
    }
    // Route local and Arrange for me: no drawing.
    {
        fake f;
        f.route = "local";
        auto p = make_project(4);
        auto run = new_run(dir);
        run.mode = auto_mode::arrange;
        run_auto(p, run, f.roles(false));
        require(run.stop == auto_stop::arranged && f.draws == 0 && f.critiques == 0 && item(p).versions.size() == 1 &&
                    item(p).versions[0].task == "Arrange for me" && parse_palette(item(p).versions[0].palette).size() == 4 && p.cuts.size() == 4,
                "Arrange for me did not apply the plan's palette without drawing");
    }
    // A refine with no illustrator stops and says so.
    {
        fake f;
        f.route = "local";
        f.verdicts = {"refine"};
        auto p = make_project(4);
        auto run = new_run(dir);
        run_auto(p, run, f.roles(false));
        require(run.stop == auto_stop::no_drawing && f.critiques == 1 && item(p).version == 1, "No-illustrator stop wrong");
    }
    // Without a director Auto falls back: the window routes by providers, the loop refuses.
    {
        ai_accounts none;
        require(auto_route_for(none) == auto_route::local, "No provider: Auto must stay local");
        ai_accounts draws;
        draws.entries = {ai_account{"d", "openai", "OpenAI", "https://api.openai.com/v1", "gpt-image-2.6-flare", {}, {}, {}}};
        draws.default_id = "d";
        require(auto_route_for(draws) == auto_route::template_draw, "Illustrator only: Auto must draw once from a template");
        auto both = draws;
        both.entries.push_back(ai_account{"c", "openai", "Planner", "https://api.openai.com/v1", {}, {}, {}, {}, "gpt-5"});
        both.plans_id = "c";
        require(auto_route_for(both) == auto_route::loop, "Director and illustrator: Auto must loop");
        ai_accounts plans;
        plans.entries = {both.entries[1]};
        plans.plans_id = "c";
        require(auto_route_for(plans) == auto_route::plan_and_arrange, "Director only: Auto must plan and arrange");
        fake f;
        auto roles = f.roles();
        roles.plan = nullptr;
        auto p = make_project(4);
        auto run = new_run(dir);
        rejects([&] { run_auto(p, run, roles); }, "The loop ran without a director");
    }
    // Wording helpers.
    require(auto_ceiling({}, true) == "Up to 3 drawings, 7 reasoning calls, about 2–6 min.", "Consent ceiling wrong");
    require(describe_marks(
                R"({"frame_mm":[200,250],"findings":[{"id":"m1","kind":"floating","sheet":1,"at_mm":[20,230],"area_mm2":4},{"id":"m2","kind":"floating","sheet":1,"at_mm":[25,235],"area_mm2":3},{"id":"m3","kind":"thin","sheet":2,"at_mm":[100,125],"area_mm2":1}]})",
                {"m1", "m2", "m3"}) == "2 small loose pieces near the lower left; a thin part near the centre",
            "Marks in words wrong");
    require(plan_text(plan_json(4)).contains("- s1 (front, #203040): plane 1") && plan_text(plan_json(4)).contains("Tortoise: keep, sheet s1, held by frame"),
            "Plan text wrong");
}

int main(int argc, char** argv)
{
    const auto folder = std::filesystem::temp_directory_path() / "offcut-core-tests";
    try
    {
        test_director_protocol();
        {
            const auto u = split_ai_url("http://127.0.0.1:8123/v1");
            require(u.host == "127.0.0.1" && u.port == 8123 && !u.secure && u.path == "/v1", "URL split failed");
            require(split_ai_url("https://example.com/v1").port == 443, "HTTPS default port failed");
            rejects([] { split_ai_url("https://user:secret@example.com/v1"); }, "URL credentials accepted");
            rejects([] { split_ai_url("file:///test"); }, "Non-HTTP URL accepted");
            rejects([] { split_ai_url("http://localhost/\r\ninjected"); }, "Header injection accepted");
            require(ai_json_string(R"({ "data" : [ { "b64_json" : "YWJj" } ], "error":{"message":"bad\n\"key\" \u263a \ud83d\ude00"}})",
                                   {"data", "0", "b64_json"}) == "YWJj",
                    "Nested JSON extraction failed");
            require(ai_json_string(R"({"error":{"message":"bad\n\"key\""}})", {"error", "message"}) == "bad\n\"key\"", "JSON escapes lost");
            require(!ai_json_string("{}", {"missing"}), "Missing JSON field found");
            rejects([] { ai_json_string(R"({"x":"\ud800"})", {"x"}); }, "Unpaired surrogate accepted");
            const auto bytes = decode_ai_base64(" YW\nJjZA==\r\n");
            require(std::string(bytes.begin(), bytes.end()) == "abcd", "Base64 decode failed");
            rejects([] { decode_ai_base64("a==="); }, "Bad padding accepted");
            rejects([] { decode_ai_base64("YR=="); }, "Invalid base64 tail accepted");
            rejects([] { decode_ai_base64("@@@@"); }, "Invalid alphabet accepted");
            const ai_image png{"source.png", encode_png(image(2, 2, 255))};
            ai_endpoint endpoint;
            endpoint.model = "gpt-image-2.6-sunburst";
            auto single = make_ai_multipart(endpoint, "a prompt", {png}, "TestBoundary");
            std::string body(single.body.begin(), single.body.end());
            require(body.contains("name=\"image\";") && !body.contains("name=\"quality\"") && body.ends_with("--TestBoundary--\r\n"),
                    "Single multipart fields wrong");
            require(std::search(single.body.begin(), single.body.end(), png.png.begin(), png.png.end()) != single.body.end(), "Multipart PNG bytes changed");
            endpoint.quality = "high";
            auto multiple = make_ai_multipart(endpoint, "prompt", {png, png}, "TestBoundary");
            body.assign(multiple.body.begin(), multiple.body.end());
            require(body.contains("name=\"image[]\";") && body.contains("name=\"quality\"\r\n\r\nhigh"), "Multiple multipart fields wrong");
            rejects([&] { make_ai_multipart(endpoint, "--TestBoundary", {png}, "TestBoundary"); }, "Multipart collision accepted");
            require(redact_ai_secret("secret secret", "secret") == "[redacted] [redacted]", "Secret redaction failed");
            std::atomic_bool cancel{true};
            bool cancelled = false;
            try
            {
                edit_ai_image(endpoint, "prompt", {png}, "", &cancel);
            }
            catch (const ai_cancelled&)
            {
                cancelled = true;
            }
            require(cancelled, "Pre-cancelled request attempted network");
        }
        std::filesystem::create_directories(folder);
        const auto samples = argc > 1 ? from_utf8(argv[1]) : fresh_sample_folder(folder);
        if (argc > 1 && !std::filesystem::create_directory(samples))
            throw std::runtime_error("Sample output folder must be fresh.");
        test_depth_overlap(samples);
        {
            require(ai_providers().size() == 5 && find_ai_provider("openai").key_page == "https://platform.openai.com/api-keys" &&
                        find_ai_provider("pollinations").key_page == "https://enter.pollinations.ai/keys",
                    "Provider registry key pages changed");
            rejects([] { find_ai_provider("unknown"); }, "Unknown provider accepted");
            // Model filtering: GPT Image only (multi-image edits), newest first.
            const auto openai = filter_ai_models(
                ai_model_list::openai_images,
                R"({"data":[{"id":"text-model"},{"id":"gpt-image-1"},{"id":"dall-e-3"},{"id":"dall-e-2"},{"id":"gpt-image-2.5-sunburst-2026-09-08"},{"id":"gpt-image-2.5-sunburst"},{"id":"gpt-image-2.6-flare"},{"id":"gpt-image-1.5"},{"id":"gpt-image-2.6-flare"},{"id":"gpt-image-1-mini"}]})");
            require(openai.size() == 6 && openai.front() == "gpt-image-2.6-flare" && openai[1] == "gpt-image-2.5-sunburst" &&
                        std::ranges::none_of(openai, [](const auto& m) { return m.starts_with("dall-e"); }) && openai[4] == "gpt-image-1" &&
                        openai[5] == "gpt-image-1-mini",
                    "OpenAI model filtering/newest preference failed");
            require(default_ai_model(find_ai_provider("openai"), openai) == "gpt-image-2.6-flare" &&
                        default_ai_model(find_ai_provider("openai"), openai, "gpt-image-1") == "gpt-image-1" &&
                        default_ai_model(find_ai_provider("openai"), openai, "gone") == "gpt-image-2.6-flare",
                    "OpenAI default model choice failed");
            const auto poll = filter_ai_models(
                ai_model_list::pollinations_images,
                R"([{"name":"text-only","input_modalities":["text"]},{"name":"flux","input_modalities":["text"]},{"name":"a-image","input_modalities":["image","text"]},{"name":"google/gemini-3.1-flash-image","input_modalities":["text","image"]},{"name":"no-modalities"}])");
            require(poll == std::vector<std::string>{"a-image", "google/gemini-3.1-flash-image"}, "Pollinations image-input filtering failed");
            require(default_ai_model(find_ai_provider("pollinations"), poll) == "a-image", "Pollinations default model failed");
            require(filter_ai_models(ai_model_list::compatible, R"({"data":[{"id":"my-local-model"}]})").front() == "my-local-model",
                    "Compatible model filtering failed");
            const auto compatible =
                filter_ai_models(ai_model_list::compatible, R"({"data":[{"id":"dall-e-2"},{"id":"gpt-4o"},{"id":"gpt-image-1"},{"id":"gpt-image-1.5"}]})");
            require(default_ai_model(find_ai_provider("custom"), compatible) == "gpt-image-1.5" && compatible.size() == 4,
                    "Compatible server's GPT Image preference failed");
            require(default_ai_model(find_ai_provider("local"), {}, "typed-model") == "typed-model" && default_ai_model(find_ai_provider("local"), {}).empty(),
                    "Compatible free-text model failed");
            rejects([] { filter_ai_models(ai_model_list::openai_images, "{}"); }, "Malformed model list accepted");
            // Keys.
            require(validate_provider_key(find_ai_provider("pollinations"), " \r\nsk_fixture\t ") == "sk_fixture", "Key trim failed");
            rejects([] { validate_provider_key(find_ai_provider("pollinations"), "pk_fixture"); }, "Publishable key accepted");
            rejects([] { validate_provider_key(find_ai_provider("openai"), "sk-fixture\ninside"); }, "Internal key whitespace accepted");
            rejects([] { validate_provider_key(find_ai_provider("openai"), " \t"); }, "Empty OpenAI key accepted");
            require(validate_provider_key(find_ai_provider("custom"), "").empty() && validate_provider_key(find_ai_provider("local"), "ignored").empty(),
                    "Keyless provider validation failed");
            // Status and error mapping, as the Status column and Add show them.
            require(ai_failure_summary(ai_http_error(401, "Incorrect API key provided: sk-ab***cd.")) == "Key rejected (401)" &&
                        ai_failure_detail(ai_http_error(401, "Incorrect API key provided: sk-ab***cd.")) ==
                            "Key rejected (401): Incorrect API key provided: sk-ab***cd.",
                    "401 mapping failed");
            require(ai_failure_detail(ai_http_error(403, "Your organization must be verified to use the model `gpt-image-1`.")) ==
                        "Access denied (403): Your organization must be verified to use the model `gpt-image-1`.",
                    "403 mapping failed");
            require(ai_failure_summary(ai_http_error(429, "x")) == "Rate limit or quota reached (429)" &&
                        ai_failure_summary(ai_http_error(502, "x")) == "Provider error (502)" &&
                        ai_failure_detail(ai_http_error(404, "Provider returned HTTP 404.")) == "Not found (404): check the server URL.",
                    "HTTP status mapping failed");
            require(ai_failure_summary(ai_network_error(12029, "Sending AI request failed (WinHTTP 12029).")) == "Can't connect to the server" &&
                        ai_failure_summary(ai_network_error(12007, "")) == "Server not found" &&
                        ai_failure_summary(ai_network_error(12175, "")) == "Secure connection failed" &&
                        ai_failure_summary(std::runtime_error("No key in Windows Credential Manager")) == "No key in Windows Credential Manager",
                    "Network/missing key mapping failed");
            require(redact_ai_secret(ai_failure_detail(ai_http_error(400, "bad key sk-secret")), "sk-secret") == "Refused (HTTP 400): bad key [redacted]",
                    "Failure detail not redacted");
            // Keyless entries never read a credential; a keyed one without
            // its credential says so.
            const ai_account keyless{"local", "local", "Local", "http://127.0.0.1:9/v1", "m", "", {}};
            require(load_account_key(keyless).empty(), "Keyless entry read a key");
            const ai_account missing{"t", "openai", "OpenAI", "https://api.openai.com/v1", "gpt-image-1", "Offcut/AI/offcut-test-never-saved", {}};
            rejects([&] { load_account_key(missing); }, "Missing credential not reported");
#ifdef _WIN32
            {
                // A throwaway target, removed again: never a real one.
                const std::string target = "Offcut/AI/offcut-core-test-" + std::to_string(GetCurrentProcessId());
                store_ai_key("sk-throwaway-fixture", target);
                const bool saved = ai_key_saved(target);
                const auto loaded = load_ai_key(target);
                clear_ai_key(target);
                require(saved && loaded == "sk-throwaway-fixture" && !ai_key_saved(target) && load_ai_key(target).empty(),
                        "Credential Manager store/load/clear failed");
            }
#endif
            // Persistence of the list: non-secret fields only.
            const auto none = [](const std::string&) { return false; };
            const auto config_dir = samples / "provider-settings";
            std::filesystem::create_directory(config_dir);
            require(read_ai_accounts(config_dir, none).entries.empty(), "Fresh settings not empty");
            ai_accounts list;
            ai_account work{new_ai_account_id(list, "openai"),
                            "openai",
                            new_ai_account_name(list, find_ai_provider("openai"), ""),
                            "https://api.openai.com/v1",
                            "gpt-image-1.5",
                            "",
                            openai};
            work.credential_target = ai_credential_target(work.id);
            list.entries.push_back(work);
            ai_account home{new_ai_account_id(list, "openai"),
                            "openai",
                            new_ai_account_name(list, find_ai_provider("openai"), "  Home \"quoted\"  "),
                            "https://api.openai.com/v1",
                            "gpt-image-2.6-flare",
                            "",
                            {}};
            home.credential_target = ai_credential_target(home.id);
            list.entries.push_back(home);
            ai_account local{
                new_ai_account_id(list, "local"), "local", new_ai_account_name(list, find_ai_provider("local"), ""), "http://127.0.0.1:8188/v1", "", "", {}};
            list.entries.push_back(local);
            list.default_id = home.id;
            require(work.id == "openai" && home.id == "openai-2" && work.name == "OpenAI" && home.name == "Home \"quoted\"" &&
                        work.credential_target == "Offcut/AI/openai" && new_ai_account_name(list, find_ai_provider("openai"), "") == "OpenAI 2",
                    "Entry ids/names/targets wrong");
            write_ai_accounts(list, config_dir);
            const auto restored = read_ai_accounts(config_dir, none);
            require(restored.entries.size() == 3 && restored.default_account()->id == "openai-2" && restored.entries[1].name == "Home \"quoted\"" &&
                        restored.entries[0].models == openai && restored.entries[0].credential_target == "Offcut/AI/openai" &&
                        restored.entries[2].model.empty() && restored.entries[2].credential_target.empty(),
                    "AI provider list persistence failed");
            {
                std::ifstream in(config_dir / "ai-accounts.txt");
                const std::string text((std::istreambuf_iterator<char>(in)), {});
                require(!text.contains("sk-"), "Settings file contains key-like text");
            }
            ai_account bad = local;
            bad.credential_target = "Offcut/AI/local";  // a keyless provider never has a key
            rejects([&] { write_ai_accounts({"local", {}, {bad}}, samples / "invalid-provider-settings"); }, "Keyless entry with key target accepted");
            const auto bad_config = samples / "truncated-provider-settings";
            std::filesystem::create_directory(bad_config);
            {
                std::ofstream out(bad_config / "ai-accounts.txt");
                out << "OFFCUT_AI_ACCOUNTS_1 \"openai\"\n\"openai\" \"openai\" \"OpenAI\" \"https://api.openai.com/v1\"";
            }
            rejects([&] { read_ai_accounts(bad_config, none); }, "Truncated provider record accepted");
            // Migration 1: the original single endpoint plus a saved Offcut/OpenAI key.
            const auto v0 = samples / "migrate-endpoint";
            std::filesystem::create_directory(v0);
            {
                std::ofstream legacy(v0 / "ai-endpoint.txt");
                legacy << "\"https://api.openai.com/v1\"\n\"gpt-image-1.5\"\n\"1024x1536\"\n\"high\"\n";
            }
            std::vector<std::string> probed;
            const auto openai_saved = [&](const std::string& t)
            {
                probed.push_back(t);
                return t == "Offcut/OpenAI";
            };
            auto m0 = read_ai_accounts(v0, openai_saved);
            require(m0.entries.size() == 1 && m0.default_id == "openai" && m0.entries[0].provider == "openai" && m0.entries[0].model == "gpt-image-1.5" &&
                        m0.entries[0].credential_target == "Offcut/OpenAI",
                    "Legacy endpoint migration lost data");
            // A loopback endpoint was a local server.
            {
                std::ofstream legacy(v0 / "ai-endpoint.txt");
                legacy << "\"http://127.0.0.1:9988/v1\"\n\"local-model\"\n\"auto\"\n\"\"\n";
            }
            m0 = read_ai_accounts(v0, none);
            require(m0.entries.size() == 1 && m0.entries[0].provider == "local" && m0.entries[0].credential_target.empty() &&
                        m0.entries[0].base_url == "http://127.0.0.1:9988/v1",
                    "Legacy local endpoint migration failed");
            // Migration 2: no old settings, only a saved Offcut/OpenAI key.
            const auto key_only = samples / "migrate-key-only";
            std::filesystem::create_directory(key_only);
            m0 = read_ai_accounts(key_only, openai_saved);
            require(m0.entries.size() == 1 && m0.entries[0].credential_target == "Offcut/OpenAI" && m0.entries[0].model.empty(), "Key-only migration failed");
            // Migration 3: the per-provider profiles.
            const auto v1 = samples / "migrate-profiles";
            std::filesystem::create_directory(v1);
            {
                std::ofstream out(v1 / "ai-providers.txt");
                out << "OFFCUT_AI_PROVIDERS_1 \"pollinations\"\n"
                    << "\"openai\" \"https://api.openai.com/v1\" \"gpt-image-1\" \"auto\" \"auto\" 0 0\n"
                    << "\"pollinations\" \"https://gen.pollinations.ai/v1\" \"google/gemini-3.1-flash-image\" \"auto\" \"auto\" 1 2\n"
                    << "\"google/gemini-3.1-flash-image\"\n\"a-image\"\n"
                    << "\"local\" \"http://127.0.0.1:8188/v1\" \"local-image\" \"auto\" \"auto\" 0 0\n"
                    << "\"custom\" \"https://images.example.com/v1\" \"edit-model\" \"auto\" \"\" 1 0\n";
            }
            const auto m1 = read_ai_accounts(v1, openai_saved);
            require(m1.entries.size() == 3 && m1.default_id == "pollinations" && m1.find("openai") && m1.find("openai")->credential_target == "Offcut/OpenAI" &&
                        m1.find("pollinations")->credential_target == "Offcut/Pollinations" && m1.find("pollinations")->models.size() == 2 &&
                        !m1.find("local") && m1.find("custom") && m1.find("custom")->credential_target.empty(),
                    "Per-provider profile migration failed");
            write_ai_accounts(m1, v1);
            require(read_ai_accounts(v1, none).entries.size() == 3 && std::filesystem::exists(v1 / "ai-providers.txt"),
                    "Migrated list not kept, or old settings removed");
            // The request: size follows the frame; OpenAI gets quality auto.
            const ai_account gpt{"o", "openai", "OpenAI", "https://api.openai.com/v1", "gpt-image-1.5", "", {}};
            require(account_image_endpoint(gpt, 200, 250).size == "1024x1536" && account_image_endpoint(gpt, 250, 200).size == "1536x1024" &&
                        account_image_endpoint(gpt, 200, 200).size == "1024x1024" && account_image_endpoint(gpt, 200, 200).quality == "auto",
                    "Frame-matching OpenAI size failed");
            ai_account dalle = gpt;
            dalle.model = "dall-e-2";
            rejects([&] { account_image_endpoint(dalle, 200, 200); }, "Non-GPT Image OpenAI model accepted");
            rejects([&] { account_image_endpoint(local, 200, 200); }, "Entry without a model accepted");
            ai_account typed = local;
            typed.model = "typed";
            require(account_image_endpoint(typed, 200, 200).quality.empty(), "Compatible server sent OpenAI quality");
            const ai_image png{"test.png", encode_png(image(2, 2, 255))};
            const auto m = make_ai_multipart(account_image_endpoint(gpt, 200, 250), "prompt", {png, png, png}, "ProviderTest");
            const std::string body(m.body.begin(), m.body.end());
            const auto count = [&](std::string_view needle)
            {
                size_t n = 0;
                for (size_t at = body.find(needle); at != std::string::npos; at = body.find(needle, at + 1))
                    ++n;
                return n;
            };
            require(count("name=\"image[]\";") == 3 && body.contains("name=\"output_format\"\r\n\r\npng") && body.contains("name=\"quality\"\r\n\r\nauto") &&
                        body.contains("name=\"size\"\r\n\r\n1024x1536") && body.contains("name=\"model\"\r\n\r\ngpt-image-1.5") &&
                        !body.contains("response_format"),
                    "GPT Image edit request fields wrong");
            std::cout << "Passed: provider registry, model filtering/defaults, key validation, status/error mapping, credential store, "
                         "list persistence, migration and GPT Image edit fields.\n";
        }
        test_layer_editor(folder);
        test_illustration(samples);
        test_ai_loop(folder);
        test_auto_loop(folder);
        test_ai_providers(folder);
        std::cout << "Illustration and landscape samples: " << utf8(samples) << '\n';
        for (unsigned bits = 0; bits < 512; ++bits)
        {
            cut_layer c{"mask", 3, 3, std::vector<uint8_t>(9)};
            for (int i = 0; i < 9; ++i)
                c.solid[i] = (bits >> i) & 1;
            check_svg(c);
        }
        auto white = image(64, 64, 255);
        for (int y = 24; y < 40; ++y)
            for (int x = 24; x < 40; ++x)
                for (int c = 0; c < 3; ++c)
                    white.rgba[(y * 64 + x) * 4 + c] = 0;
        transform_options options;
        options.layers = 5;
        options.smoothing = 1;
        auto cuts = simplify({{"island", white}}, options);
        require(cuts.size() == 5, "Wrong layer count");
        for (const auto& c : cuts)
            require(disconnected_components(c) == 0, "Floating cardstock left after removal");
        require(frame_only(cuts.front(), options), "A floating square should be removed, not bridged");
        {
            // A post standing on the bottom edge reaches the frame and stays.
            auto post = image(64, 64, 255);
            for (int y = 16; y < 64; ++y)
                for (int x = 28; x < 36; ++x)
                    for (int c = 0; c < 3; ++c)
                        post.rgba[(y * 64 + x) * 4 + c] = 0;
            const auto standing = simplify({{"post", post}}, options);
            require(!frame_only(standing.front(), options), "A shape touching the frame was removed");
            require(disconnected_components(standing.front()) == 0, "A standing shape floated");
        }
        options.connect = false;
        auto disconnected = simplify({{"island", white}}, options);
        require(disconnected_components(disconnected.front()) > 0, "Keeping floating pieces unexpectedly removed them");
        options.connect = true;
        {
            auto subject = image(100, 100, 0);
            for (int y = 20; y < 80; ++y)
                for (int x = 20; x < 80; ++x)
                    subject.rgba[(y * 100 + x) * 4 + 3] = 255;
            auto placed = new_project_options(subject);
            require(placed.size_preset == 2 && placed.width_mm == 203.2 && placed.height_mm == 254, "New frame defaults are not 8x10 portrait");
            require(placed.placement == placement_mode::touch_edges && placed.touch_edges == touch_bottom, "Transparent subject did not default to Bottom");
            placed.smoothing = 0;
            const auto g = dimensions(placed);
            const auto composition = flatten({{"subject", subject}}, placed);
            // Transparent padding must not limit the scale. Bottom-only contact
            // makes this square subject fill the portrait opening's width, with
            // a clean two-pixel gap at each unselected side.
            int subject_left = g.w, subject_right = -1;
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                    if (composition.rgba[(size_t(y) * g.w + x) * 4] < 128)
                    {
                        subject_left = std::min(subject_left, x);
                        subject_right = std::max(subject_right, x);
                    }
            require(subject_left - g.left == 2 && g.w - g.right - 1 - subject_right == 2,
                    "Padded Bottom-only subject does not fill the limiting width with clean side gaps");
            require(subject_right - subject_left + 1 >= g.opening_width() - 4, "Touch edges scale still includes transparent image padding");
            const int centre_x = g.left + g.opening_width() / 2, opening_bottom = g.h - g.bottom;
            int last = -1;
            for (int y = 0; y < g.h; ++y)
                if (composition.rgba[(size_t(y) * g.w + centre_x) * 4] < 128)
                    last = y;
            const double overlap = placed.bridge_mm * 640 / std::max(placed.width_mm, placed.height_mm);
            require(std::abs((last + 1 - opening_bottom) - overlap) <= 1, "Subject base does not overlap the bottom frame by Min. width");
            auto tall = image(100, 200, 0);
            for (int y = 20; y < 180; ++y)
                for (int x = 40; x < 60; ++x)
                    tall.rgba[(y * 100 + x) * 4 + 3] = 255;
            const auto tall_composition = flatten({{"tall subject", tall}}, placed);
            int first_tall = g.h, last_tall = -1;
            for (int y = 0; y < g.h; ++y)
                if (tall_composition.rgba[(size_t(y) * g.w + centre_x) * 4] < 128)
                {
                    first_tall = std::min(first_tall, y);
                    last_tall = y;
                }
            require(first_tall - g.top == 2 && std::abs(last_tall + 1 - opening_bottom - overlap) <= 1,
                    "Height-limited Bottom contact does not fit its subject with a clean top gap");
            const auto standing = simplify({{"subject", subject}}, placed);
            require(!frame_only(standing.front(), placed) && disconnected_components(standing.front()) == 0, "Transparent subject vanished or floated");
            placed.smoothing = 2;
            require(!frame_only(simplify({{"subject", subject}}, placed).front(), placed), "Smoothing detached a placed subject");
            placed.touch_edges = touch_left | touch_right;
            const auto horizontal = flatten({{"subject", subject}}, placed);
            int min_x = g.w, min_y = g.h, max_x = -1, max_y = -1;
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                    if (horizontal.rgba[(size_t(y) * g.w + x) * 4] < 128)
                    {
                        min_x = std::min(min_x, x);
                        max_x = std::max(max_x, x);
                        min_y = std::min(min_y, y);
                        max_y = std::max(max_y, y);
                    }
            require(std::abs((max_x - min_x) - (max_y - min_y)) <= 1, "Opposing edge placement stretched the square subject");
            require(min_x < g.left && max_x >= g.w - g.right, "Left/right placement did not reach both sides");
            // Opposing edges determine scale. Constraining both axes uses one
            // cover scale, keeping the square subject square even in a portrait.
            placed.touch_edges = touch_left | touch_right | touch_top | touch_bottom;
            const auto covered = flatten({{"subject", subject}}, placed);
            require(covered.rgba[(size_t(g.top) * g.w + g.left) * 4] == 0 && covered.rgba[(size_t(g.h - g.bottom - 1) * g.w + g.w - g.right - 1) * 4] == 0,
                    "Opposing constraints did not cover the opening");
            placed.touch_edges = touch_top;
            const auto top = simplify({{"subject", subject}}, placed);
            require(!frame_only(top.front(), placed) && disconnected_components(top.front()) == 0, "Top contact failed");
            auto opaque = image(100, 100, 255);
            for (int y = 20; y < 80; ++y)
                for (int x = 20; x < 80; ++x)
                    for (int ch = 0; ch < 3; ++ch)
                        opaque.rgba[(y * 100 + x) * 4 + ch] = 0;
            require(new_project_options(opaque).placement == placement_mode::touch_edges, "Plain white background did not default to Touch edges");
            for (size_t i = 0; i < opaque.rgba.size(); i += 4)
                if (opaque.rgba[i] == 255)
                {
                    opaque.rgba[i] = 80;
                    opaque.rgba[i + 1] = 120;
                    opaque.rgba[i + 2] = 160;
                }
            const auto box = detect_subject(opaque);
            require(box.isolated && box.left == 20 && box.right == 80, "Plain coloured background detection failed");
            auto wide = image(100, 50, 0);
            for (size_t i = 3; i < wide.rgba.size(); i += 4)
                wide.rgba[i] = 255;
            auto fit = options;
            fit.placement = placement_mode::fit;
            const auto fit_image = flatten({{"wide", wide}}, fit);
            fit.placement = placement_mode::fill;
            const auto fill_image = flatten({{"wide", wide}}, fit);
            const auto square = dimensions(fit);
            const size_t upper = (size_t(square.top) * square.w + square.w / 2) * 4;
            require(fit_image.rgba[upper] == 255 && fill_image.rgba[upper] == 0, "Fit and Fill do not contain/crop as expected");
            require(new_project_options(wide).landscape && new_project_options(wide).width_mm == 254, "Landscape image did not choose landscape orientation");
        }
        {
            auto sided = options;
            sided.uniform_border = false;
            sided.border_top_mm = 5;
            sided.border_bottom_mm = 15;
            sided.border_left_mm = 10;
            sided.border_right_mm = 20;
            const auto g = dimensions(sided);
            require(g.top == 16 && g.bottom == 48 && g.left == 32 && g.right == 64 && g.opening_width() == 544 && g.opening_height() == 576,
                    "Per-side frame geometry incorrect");
            const auto masks = simplify({{"blank", image(32, 32, 255)}}, sided);
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                    require(bool(masks.front().solid[y * g.w + x]) == border(x, y, g), "Per-side frame mask incorrect");
            const auto preview = source_preview({{"blank", image(32, 32, 255)}}, sided);
            require(preview.rgba[0] < 255 && preview.rgba[(size_t(g.top) * g.w + g.left) * 4] == 255, "Source preview does not distinguish frame and opening");
            auto impossible = sided;
            impossible.border_right_mm = 195;
            rejects([&] { flatten({}, impossible); }, "Overlapping side borders accepted");
            set_frame_size(sided, 7, true);
            require(sided.width_mm == 297 && sided.height_mm == 210 && sided.landscape, "A4 landscape preset incorrect");
            set_frame_size(sided, 9, false);
            require(sided.width_mm == 210 && sided.height_mm == 297 && sided.size_preset == 9, "Custom orientation incorrect");
        }
        auto fullmask = image(640, 640, 255);
        for (int y = 200; y < 400; ++y)
            for (int x = 200; x < 400; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    fullmask.rgba[(y * 640 + x) * 4 + ch] = 0;
        auto import_options = options;
        import_options.smoothing = 0;
        import_options.connect = false;
        const auto imported = masks_from_images({{"01.png", fullmask}}, import_options);
        require(imported.front().solid[200 * 640 + 200] && !imported.front().solid[199 * 640 + 200],
                "Imported layer was fitted inside its frame a second time");
        rejects([&] { masks_from_images({{"wrong aspect", image(100, 50, 255)}}, options); }, "Mismatched AI canvas accepted");
        require(std::ranges::all_of(cuts.back().solid, [](auto v) { return v == 1; }), "Backing must be solid");
        auto red = image(4, 4, 255);
        for (size_t i = 0; i < red.rgba.size(); i += 4)
        {
            red.rgba[i + 1] = red.rgba[i + 2] = 0;
        }
        auto blue = red;
        for (size_t i = 0; i < blue.rgba.size(); i += 4)
        {
            blue.rgba[i] = 0;
            blue.rgba[i + 2] = 255;
            blue.rgba[i + 3] = 128;
        }
        auto flattened = flatten({{"red", red}, {"blue", blue}}, options);
        const size_t centre = (size_t(flattened.height / 2) * flattened.width + flattened.width / 2) * 4;
        require(std::abs(int(flattened.rgba[centre]) - 127) <= 1 && flattened.rgba[centre + 2] == 128, "Source alpha composition failed");
        project p;
        p.name = "Jay ♥";
        p.layers = {{"cat.png", white}};
        p.cuts = cuts;
        p.options = options;
        p.instructions = "Keep the ears";
        const auto path = folder / "roundtrip.offcut";
        save_project(p, path);
        p.name = "Saved twice";
        save_project(p, path);
        auto loaded = load_project(path);
        require(loaded.name == p.name && loaded.instructions == p.instructions && loaded.layers.front().image.rgba == white.rgba &&
                    loaded.cuts.front().solid == cuts.front().solid,
                "Portable project lost content");
        {
            p.options.uniform_border = false;
            p.options.border_top_mm = 5;
            p.options.border_bottom_mm = 15;
            p.options.border_left_mm = 12;
            p.options.border_right_mm = 20;
            p.options.placement = placement_mode::touch_edges;
            p.options.touch_edges = touch_bottom | touch_left;
            p.options.size_preset = 2;
            p.options.landscape = true;
            p.options.layer_gap_mm = 7.5;
            p.stale = true;
            const auto modern = folder / "sided.offcut";
            save_project(p, modern);
            const auto again = load_project(modern);
            require(!again.options.uniform_border && again.options.border_top_mm == 5 && again.options.border_bottom_mm == 15 &&
                        again.options.border_left_mm == 12 && again.options.border_right_mm == 20 && again.options.placement == placement_mode::touch_edges &&
                        again.options.touch_edges == (touch_bottom | touch_left) && again.options.size_preset == 2 && again.options.landscape &&
                        again.options.layer_gap_mm == 7.5,
                    "New project settings did not round-trip");
            // The previous unreleased OFFCUT02 layout ended before the optional
            // gap trailer.
            write_offcut02(p, folder / "previous02.offcut", false);
            require(load_project(folder / "previous02.offcut").options.layer_gap_mm == 5, "OFFCUT02 without a gap did not default to 5 mm");
            write_offcut02(p, folder / "gap02.offcut", true);
            require(load_project(folder / "gap02.offcut").options.layer_gap_mm == 7.5, "OFFCUT02 gap trailer lost");
            // A literal old-format fixture, independent of the current writer.
            std::ofstream legacy(folder / "legacy.offcut", std::ios::binary);
            const auto n = [&](uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                    legacy.put(char(v >> (8 * i)));
            };
            legacy.write("OFFCUT01", 8);
            n(0);
            n(0);
            for (auto v : {20000, 15000, 1200, 200, 7, 2, 1, 0, 0, 0})
                n(v);
            legacy.close();
            const auto old = load_project(folder / "legacy.offcut");
            const auto b = borders(old.options);
            require(old.options.uniform_border && b.top == 12 && b.bottom == 12 && b.left == 12 && b.right == 12 &&
                        old.options.placement == placement_mode::fit && old.options.layer_gap_mm == 5,
                    "OFFCUT01 did not load with uniform borders/Fit");
            p.options = options;
            p.stale = false;
        }
        {
            std::ofstream bad(folder / "bad.offcut");
            bad << "OFFCUT01";
        }
        rejects([&] { load_project(folder / "bad.offcut"); }, "Truncated project accepted");
        auto invalid = options;
        invalid.border_mm = 110;
        rejects([&] { flatten(p.layers, invalid); }, "Impossible frame accepted");
        rejects([&] { simplify({}, options); }, "Empty source accepted");
        const auto export_dir = export_layers(p, folder);
        require(std::filesystem::exists(export_dir / "01.svg") && std::filesystem::exists(export_dir / "05.svg"), "Missing numbered exports");
        p.stale = true;
        rejects([&] { export_layers(p, folder); }, "Stale layers exported");
        auto ai = export_ai_request(p, folder);
        require(std::filesystem::exists(ai / "PROMPT.txt") && std::filesystem::exists(ai / "composition.png"), "AI handoff missing assets");
        std::cout << "Passed: 512 SVG topology cases; subject detection and contact/cover placement; asymmetric borders and preview; presets; OFFCUT01/02 "
                     "compatibility; floating-piece removal; alpha composition; persistence; invalid inputs; exports and AI handoff.\n";
        std::cout << "Artifacts: " << utf8(folder) << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
