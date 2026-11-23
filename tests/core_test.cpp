#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "export.h"
#include "transform.h"
using namespace offcut;
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
int main()
{
    const auto folder = std::filesystem::temp_directory_path() / "offcut-core-tests";
    try
    {
        std::filesystem::create_directories(folder);
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
            require(disconnected_components(c) == 0, "Unattached cardstock after repair");
        options.connect = false;
        auto disconnected = simplify({{"island", white}}, options);
        require(disconnected_components(disconnected.front()) > 0, "Unconnected mode unexpectedly repaired artwork");
        options.connect = true;
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
        p.sources = {{"cat.png", white}};
        p.cuts = cuts;
        p.options = options;
        p.instructions = "Keep the ears";
        const auto path = folder / "roundtrip.offcut";
        save_project(p, path);
        p.name = "Saved twice";
        save_project(p, path);
        auto loaded = load_project(path);
        require(loaded.name == p.name && loaded.instructions == p.instructions && loaded.sources.front().image.rgba == white.rgba &&
                    loaded.cuts.front().solid == cuts.front().solid,
                "Portable project lost content");
        {
            std::ofstream bad(folder / "bad.offcut");
            bad << "OFFCUT01";
        }
        rejects([&] { load_project(folder / "bad.offcut"); }, "Truncated project accepted");
        auto invalid = options;
        invalid.border_mm = 110;
        rejects([&] { flatten(p.sources, invalid); }, "Impossible frame accepted");
        rejects([&] { simplify({}, options); }, "Empty source accepted");
        const auto export_dir = export_layers(p, folder);
        require(std::filesystem::exists(export_dir / "01.svg") && std::filesystem::exists(export_dir / "05.svg"), "Missing numbered exports");
        p.stale = true;
        rejects([&] { export_layers(p, folder); }, "Stale layers exported");
        auto ai = export_ai_request(p, folder);
        require(std::filesystem::exists(ai / "PROMPT.txt") && std::filesystem::exists(ai / "composition.png"), "AI handoff missing assets");
        std::cout << "Passed: 512 SVG topology cases, bridges, alpha composition, persistence, invalid inputs, exports and AI handoff.\n";
        std::cout << "Artifacts: " << utf8(folder) << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
