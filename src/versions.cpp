#include "versions.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace offcut
{
const raster& current_pixels(const layer& l)
{
    if (l.version)
        if (const auto* v = find_version(l, l.version); v && v->image.valid() && v->image.width == l.image.width && v->image.height == l.image.height)
            return v->image;
    return l.image;
}
item_version* find_version(layer& l, uint32_t id)
{
    for (auto& v : l.versions)
        if (v.id == id)
            return &v;
    return nullptr;
}
const item_version* find_version(const layer& l, uint32_t id)
{
    return find_version(const_cast<layer&>(l), id);
}
uint32_t next_version_id(const layer& l)
{
    uint32_t next = 1;
    for (const auto& v : l.versions)
        next = std::max(next, v.id + 1);
    return next;
}
uint32_t add_version(layer& l, item_version v)
{
    if (!v.image.valid())
        throw std::runtime_error("The AI result has no image.");
    if (v.parent && !find_version(l, v.parent))
        v.parent = 0;
    if (v.image.width != l.image.width || v.image.height != l.image.height)
        v.image = fit_raster(v.image, l.image.width, l.image.height);
    v.id = next_version_id(l);
    v.previous = l.version;
    if (v.state == review_state::none)
        v.state = review_state::pending;
    l.versions.push_back(std::move(v));
    l.version = l.versions.back().id;
    return l.version;
}
bool set_current_version(layer& l, uint32_t id)
{
    if (id && !find_version(l, id))
        return false;
    l.version = id;
    return true;
}
bool discard_version(layer& l, uint32_t id)
{
    const auto it = std::ranges::find(l.versions, id, &item_version::id);
    if (!id || it == l.versions.end())
        return false;
    const uint32_t parent = it->parent, previous = it->previous;
    l.versions.erase(it);
    for (auto& v : l.versions)
    {
        if (v.parent == id)
            v.parent = parent;
        if (v.previous == id)
            v.previous = parent;
    }
    if (l.version == id)
        l.version = previous && find_version(l, previous) ? previous : parent && find_version(l, parent) ? parent : 0;
    return true;
}
bool drifted(const layer& l, const item_version& v)
{
    const auto* parent = v.parent ? find_version(l, v.parent) : nullptr;
    return parent && parent->issues >= 0 && v.issues > parent->issues;
}
bool to_review(const layer& l)
{
    return std::ranges::any_of(l.versions, [](const item_version& v) { return v.state == review_state::pending; });
}
std::vector<version_cell> version_layout(const layer& l)
{
    std::vector<version_cell> cells{{0, 0, 0}};
    struct row_end
    {
        uint32_t id;
        int column;
    };
    std::vector<row_end> rows{{0, 0}};
    const auto cell_of = [&](uint32_t id) -> const version_cell*
    {
        for (const auto& c : cells)
            if (c.id == id)
                return &c;
        return nullptr;
    };
    const auto parent_of = [&](uint32_t id) -> uint32_t
    {
        const auto* v = find_version(l, id);
        return v ? v->parent : 0;
    };
    for (const auto& v : l.versions)
    {
        const auto* p = cell_of(v.parent);
        if (!p)
            p = &cells.front();
        const int row = p->row, column = p->column;
        auto& end = rows[size_t(row)];
        if (end.id == v.parent || (end.id != 0 && parent_of(end.id) == v.parent))
        {
            end = {v.id, end.column + 1};
            cells.push_back({v.id, row, end.column});
        }
        else
        {
            rows.push_back({v.id, column + 1});
            cells.push_back({v.id, int(rows.size()) - 1, column + 1});
        }
    }
    return cells;
}
std::vector<std::string> thread_requests(const layer& l, uint32_t id)
{
    std::vector<const item_version*> chain;
    for (const auto* v = find_version(l, id); v && chain.size() < 64; v = v->parent ? find_version(l, v->parent) : nullptr)
        chain.push_back(v);
    std::ranges::reverse(chain);
    std::vector<std::string> out;
    for (const auto* v : chain)
    {
        std::string s = v->task.empty() ? std::string("Reimagine") : v->task;
        if (s == "Reimagine")
            s = "reimagined as a shadowbox";
        else if (s == "Try again")
            continue;  // the same request again: nothing new was asked
        else
        {
            std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
        }
        if (!v->instructions.empty())
            s += ": “" + v->instructions + "”";
        out.push_back(std::move(s));
    }
    return out;
}
raster fit_raster(const raster& src, int w, int h)
{
    if (!src.valid() || w <= 0 || h <= 0)
        throw std::runtime_error("Cannot resample an empty image.");
    raster out{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    const double scale = std::max(double(w) / src.width, double(h) / src.height);
    const double left = (src.width - w / scale) / 2, top = (src.height - h / scale) / 2;
    for (int y = 0; y < h; ++y)
    {
        const double sy0 = top + y / scale, sy1 = top + (y + 1) / scale;
        const int y0 = std::clamp(int(std::floor(sy0)), 0, src.height - 1), y1 = std::clamp(int(std::ceil(sy1)), y0 + 1, src.height);
        for (int x = 0; x < w; ++x)
        {
            const double sx0 = left + x / scale, sx1 = left + (x + 1) / scale;
            const int x0 = std::clamp(int(std::floor(sx0)), 0, src.width - 1), x1 = std::clamp(int(std::ceil(sx1)), x0 + 1, src.width);
            uint32_t sum[4] = {};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += src.rgba[(size_t(yy) * src.width + xx) * 4 + c];
            const uint32_t n = uint32_t((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c)
                out.rgba[(size_t(y) * w + x) * 4 + c] = uint8_t((sum[c] + n / 2) / n);
        }
    }
    return out;
}
void remember_instruction(project& p, const std::string& text)
{
    auto s = text;
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    p.instructions = s;
    if (s.empty())
        return;
    std::erase(p.recent_instructions, s);
    p.recent_instructions.insert(p.recent_instructions.begin(), s);
    if (p.recent_instructions.size() > 10)
        p.recent_instructions.resize(10);
}
}  // namespace offcut
