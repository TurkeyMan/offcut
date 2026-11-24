#include "export.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include "transform.h"
#include "versions.h"

namespace offcut
{
namespace
{
void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    if (!out)
        throw std::runtime_error("Could not write " + utf8(path));
}
std::string numbered(size_t i)
{
    std::ostringstream s;
    s << std::setw(2) << std::setfill('0') << i + 1;
    return s.str();
}
std::filesystem::path fresh(const std::filesystem::path& parent, const char* prefix)
{
    for (int i = 1; i < 100000; ++i)
    {
        std::ostringstream name;
        name << prefix << '-';
        if (std::string_view(prefix) == "offcut-ai-request")
            name << std::setw(2) << std::setfill('0');
        name << i;
        auto p = parent / name.str();
        if (std::filesystem::create_directory(p))
            return p;
    }
    throw std::runtime_error("Cannot create export folder.");
}
void be32(std::vector<uint8_t>& b, uint32_t n)
{
    for (int i = 3; i >= 0; --i)
        b.push_back(uint8_t(n >> (8 * i)));
}
uint32_t crc(const uint8_t* p, size_t n)
{
    uint32_t c = 0xffffffff;
    for (size_t i = 0; i < n; ++i)
    {
        c ^= p[i];
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    be32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc(out.data() + start, data.size() + 4));
}
}  // namespace
std::string svg(const cut_layer& c, const transform_options& o)
{
    validate(o);
    if (c.width < 1 || c.height < 1 || c.solid.size() != size_t(c.width) * c.height)
        throw std::runtime_error("Invalid layer for SVG export.");
    // Clockwise edges around retained cells. Shared edges disappear. Trace
    // closed rings with right turns at diagonal contacts; holes use evenodd.
    const int stride = c.width + 1;
    std::vector<uint8_t> edges(size_t(stride) * (c.height + 1));
    auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < c.width && y < c.height && c.solid[y * c.width + x]; };
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x)
            if (at(x, y))
            {
                if (!at(x, y - 1))
                    edges[y * stride + x] |= 1;
                if (!at(x + 1, y))
                    edges[y * stride + x + 1] |= 2;
                if (!at(x, y + 1))
                    edges[(y + 1) * stride + x + 1] |= 4;
                if (!at(x - 1, y))
                    edges[(y + 1) * stride + x] |= 8;
            }
    const int delta[] = {1, stride, -1, -stride};
    std::ostringstream path;
    for (int start = 0; start < int(edges.size()); ++start)
        while (edges[start])
        {
            int d = 0;
            while (!(edges[start] & (1 << d)))
                ++d;
            const int first_direction = d;
            int pos = start;
            std::vector<std::pair<int, int>> ring;
            ring.emplace_back(pos % stride, pos / stride);
            do
            {
                edges[pos] &= uint8_t(~(1 << d));
                pos += delta[d];
                int next = -1;
                if (pos == start)
                    break;
                for (int turn : {1, 0, 3, 2})
                    if (edges[pos] & (1 << ((d + turn) % 4)))
                    {
                        next = (d + turn) % 4;
                        break;
                    }
                if (next < 0)
                    throw std::runtime_error("Open contour in layer.");
                if (next != d)
                    ring.emplace_back(pos % stride, pos / stride);
                d = next;
            } while (pos != start);
            (void)first_direction;
            path << 'M' << ring.front().first << ' ' << ring.front().second;
            for (size_t i = 1; i < ring.size(); ++i)
                path << 'L' << ring[i].first << ' ' << ring[i].second;
            path << 'Z';
        }
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << o.width_mm << "mm\" height=\"" << o.height_mm << "mm\" viewBox=\"0 0 " << c.width << ' '
        << c.height << "\">\n"
        << "<path fill=\"black\" fill-rule=\"evenodd\" d=\"" << path.str() << "\"/>\n</svg>\n";
    return out.str();
}
std::vector<uint8_t> encode_png(const raster& r)
{
    if (!r.valid())
        throw std::runtime_error("Invalid PNG image.");
    std::vector<uint8_t> raw;
    for (int y = 0; y < r.height; ++y)
    {
        raw.push_back(0);
        auto a = r.rgba.begin() + size_t(y) * r.width * 4;
        raw.insert(raw.end(), a, a + size_t(r.width) * 4);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t i = 0; i < raw.size();)
    {
        const size_t n = std::min(size_t(65535), raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        const uint16_t len = uint16_t(n), inv = uint16_t(~len);
        z.push_back(uint8_t(len));
        z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(inv));
        z.push_back(uint8_t(inv >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
        i += n;
    }
    uint32_t a = 1, b = 0;
    for (auto v : raw)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10}, ihdr;
    be32(ihdr, r.width);
    be32(ihdr, r.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}
void write_png(const raster& r, const std::filesystem::path& path)
{
    const auto png = encode_png(r);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write PNG.");
}
std::filesystem::path export_layers(const project& p, const std::filesystem::path& parent)
{
    if (p.cuts.empty())
        throw std::runtime_error("Generate or import cut layers first.");
    if (p.stale)
        throw std::runtime_error("The design has changed. Generate or import layers again before exporting.");
    const auto folder = fresh(parent, "offcut-layers");
    const auto frame = borders(p.options);
    for (size_t i = 0; i < p.cuts.size(); ++i)
    {
        write_text(folder / (numbered(i) + ".svg"), svg(p.cuts[i], p.options));
        raster mask{p.cuts[i].width, p.cuts[i].height, std::vector<uint8_t>(p.cuts[i].solid.size() * 4, 255)};
        for (size_t j = 0; j < p.cuts[i].solid.size(); ++j)
            if (p.cuts[i].solid[j])
                mask.rgba[j * 4] = mask.rgba[j * 4 + 1] = mask.rgba[j * 4 + 2] = 0;
        write_png(mask, folder / (numbered(i) + ".png"));
    }
    write_png(render_cuts(p.cuts), folder / "preview.png");
    std::ostringstream dimensions_text;
    dimensions_text << "Outer size: " << p.options.width_mm << " x " << p.options.height_mm << " mm.\n"
                    << "Frame (mm): top " << frame.top << ", bottom " << frame.bottom << ", left " << frame.left << ", right " << frame.right << ".\n"
                    << "Spacer thickness / layer gap: " << p.options.layer_gap_mm
                    << " mm. Hidden overlap allows a 45-degree viewing angle, with extra hidden support where needed.\n";
    write_text(
        folder / "ASSEMBLY.txt",
        dimensions_text.str() +
            "Offcut — experimental cut layers\n01.svg is the front; assemble in ascending order towards the back.\nBlack is retained cardstock. "
            "White/transparent areas are removed.\nAll SVG files share the same physical dimensions, including their border.\nPNG files are reference "
            "masks, not cutting files.\nLayer numbers are filenames only. Choose your own cardstock colours.\nInspect every layer in Design Space and make "
            "a small test cut before committing materials.\nThe preview is flat artwork, not a simulation of transmitted light or material strength.\n");
    return folder;
}
std::filesystem::path export_ai_request(const project& p, const std::filesystem::path& parent)
{
    if (p.layers.empty())
        throw std::runtime_error("Add a source image first.");
    validate(p.options);
    const auto folder = fresh(parent, "offcut-ai-request");
    write_text(folder / "PROMPT.txt", ai_prompt(p));
    write_png(flatten(p.layers, p.options), folder / "composition.png");
    for (size_t i = 0; i < p.layers.size(); ++i)
        if (p.layers[i].visible)
            write_png(cropped(current_pixels(p.layers[i]), effective_transform(p.layers[i], p.options).crop), folder / ("reference-" + numbered(i) + ".png"));
    write_text(
        folder / "README.txt",
        "Attach composition.png and the relevant reference images in your chosen AI app, then paste PROMPT.txt.\nRequest ONE flat-colour illustration with "
        "exactly the requested number of opaque tones, darkest at the border.\n"
        "Features sharing a tone share a sheet of card. Arrange them to touch each other or the border through the same or darker tones: a bird on a branch, "
        "lettering on the ground, a cloud meeting the top edge.\n"
        "Ask the model to move, resize, overlap or add connecting elements instead of leaving anything floating. Prefer contact areas at least Min. width "
        "wide, not point contacts or thin fake bridges.\n"
        "Return to Offcut and choose AI handoff > Import flat illustration… . Offcut separates and cleans the tones, cover-crops if necessary, then builds "
        "aligned sheets locally.\n"
        "The layer gap controls hidden overlap beneath front sheets. Floating-piece removal and crop amounts are reported after import; inspect the result "
        "before exporting SVGs.\n"
        "Import layer images… remains available for separate, already aligned black-on-white masks, numbered front to back.\n"
        "Manual export does not upload anything or require an account connection. Generate with AI sends these images only after the explicit Send review; "
        "its response.png is imported automatically, with model/endpoint/time/usage in response.json and no API key in this folder.\n");
    return folder;
}
std::filesystem::path write_ai_request(const std::filesystem::path& parent, const std::string& prompt,
                                       const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files)
{
    const auto folder = fresh(parent, "offcut-ai-request");
    write_text(folder / "PROMPT.txt", prompt);
    for (const auto& [name, bytes] : files)
    {
        std::ofstream out(folder / from_utf8(name), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Could not write " + name);
    }
    write_text(folder / "README.txt",
               "What Offcut sent to the AI provider: PROMPT.txt and the images beside it, exactly as uploaded. The reply is response.png, .jpg or "
               ".webp, with the model, time and usage in response.json. No API key is stored here.\n");
    return folder;
}
}  // namespace offcut
