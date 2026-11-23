#include "project.h"
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
std::string utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
std::filesystem::path from_utf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
void validate(const transform_options& o)
{
    if (!std::isfinite(o.width_mm) || !std::isfinite(o.height_mm) || !std::isfinite(o.border_mm) || !std::isfinite(o.bridge_mm) || o.width_mm < 30 ||
        o.width_mm > 1000 || o.height_mm < 30 || o.height_mm > 1000 || o.border_mm < 2 || 2 * o.border_mm >= std::min(o.width_mm, o.height_mm) ||
        o.bridge_mm < .5 || o.bridge_mm > 10 || o.layers < 2 || o.layers > 12 || o.smoothing < 0 || o.smoothing > 6)
        throw std::runtime_error("Use 30–1000 mm dimensions, a border smaller than half the design, and 2–12 layers.");
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
void string(std::ostream& out, const std::string& s)
{
    bytes(out, {s.begin(), s.end()});
}
std::string string(std::istream& in)
{
    const auto b = bytes(in, 65536);
    return {b.begin(), b.end()};
}
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
    out.write("OFFCUT01", 8);
    string(out, p.name);
    string(out, p.instructions);
    const auto& o = p.options;
    for (double v : {o.width_mm, o.height_mm, o.border_mm, o.bridge_mm})
        number(out, uint32_t(std::lround(v * 100)));
    number(out, o.layers);
    number(out, o.smoothing);
    number(out, o.connect);
    number(out, p.stale);
    number(out, uint32_t(p.sources.size()));
    for (const auto& s : p.sources)
    {
        string(out, s.name);
        number(out, s.visible);
        number(out, s.image.width);
        number(out, s.image.height);
        bytes(out, s.image.rgba);
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
project load_project(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    char magic[8]{};
    in.read(magic, 8);
    if (std::string(magic, 8) != "OFFCUT01")
        throw std::runtime_error("This is not an Offcut project.");
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
    validate(o);
    const auto count = number(in);
    if (count > 32)
        throw std::runtime_error("Too many source images.");
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        source_image s;
        s.name = string(in);
        s.visible = number(in) != 0;
        s.image.width = int(number(in));
        s.image.height = int(number(in));
        s.image.rgba = bytes(in, 2048 * 2048 * 4);
        total += s.image.rgba.size();
        if (!s.image.valid() || total > 128 * 1024 * 1024)
            throw std::runtime_error("Invalid image in project.");
        p.sources.push_back(std::move(s));
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
    if (!p.cuts.empty())
        p.view = view_mode::assembled;
    return p;
}
}  // namespace offcut
