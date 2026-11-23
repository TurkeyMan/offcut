#include "image_io.h"
#include <rapport/sys.h>
#include <rapport/widgets.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
namespace offcut
{
source_image read_image(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    if (size > 64 * 1024 * 1024)
        throw std::runtime_error("Image exceeds 64 MB: " + utf8(path.filename()));
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    if (!in)
        throw std::runtime_error("Cannot read " + utf8(path.filename()));
    auto decoded = rap::sys::decode_image(bytes);
    if (!decoded)
        throw std::runtime_error("Unsupported or invalid image: " + utf8(path.filename()));
    const auto& d = *decoded;
    const double scale = std::min(1., 1024. / std::max(d.width, d.height));
    raster r{std::max(1, int(std::round(d.width * scale))), std::max(1, int(std::round(d.height * scale))), {}};
    r.rgba.resize(size_t(r.width) * r.height * 4);
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x)
        {
            const size_t si = (size_t(std::min(d.height - 1, int(y / scale))) * d.width + std::min(d.width - 1, int(x / scale))) * 4;
            const size_t di = (size_t(y) * r.width + x) * 4;
            if (d.format == rap::pixel_format::rgba8)
                std::copy_n(d.pixels.data() + si, 4, r.rgba.data() + di);
            else
            {
                const int a = d.pixels[si + 3];
                r.rgba[di + 3] = uint8_t(a);
                for (int c = 0; c < 3; ++c)
                    r.rgba[di + c] = a ? uint8_t(std::min(255, (int(d.pixels[si + 2 - c]) * 255 + a / 2) / a)) : 0;
            }
        }
    return {utf8(path.filename()), std::move(r)};
}
}  // namespace offcut
