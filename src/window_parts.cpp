#include "window_parts.h"
#include <algorithm>
#include <cmath>
#include <format>
#include "history.h"
#include "versions.h"
namespace offcut
{
using namespace rap;
using namespace rap::literals;
namespace
{
// Glyphs Rapport doesn't have, drawn on a 16-unit grid in the text colour
// as IfritView draws its own, so they follow light and dark themes.
const paint ink{theme_color::window_text};
icon drawn(std::function<void(painter&, float)> f)
{
    return icon::drawn([f = std::move(f)](painter& p, size_f s) { f(p, s.w / 16); });
}
// A picture: a frame with a hill and a sun.
void picture(painter& p, float u, float x, float y, float w, float h)
{
    p.rect({x * u, y * u, w * u, h * u}, 1.5f * u, std::nullopt, ink);
    p.polygon({{(x + 1.5f) * u, (y + h - 1.5f) * u}, {(x + w * .4f) * u, (y + h * .45f) * u}, {(x + w * .65f) * u, (y + h - 1.5f) * u}}, ink, std::nullopt);
    p.circle({(x + w * .72f) * u, (y + h * .32f) * u}, 1.2f * u, ink, std::nullopt);
}
}  // namespace
icon new_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            picture(p, u, 1, 4.5f, 10, 9);
            p.line({13 * u, 1 * u}, {13 * u, 7 * u}, ink, 1.4f * u);
            p.line({10 * u, 4 * u}, {16 * u, 4 * u}, ink, 1.4f * u);
        });
}
icon picture_icon()
{
    return drawn([](painter& p, float u) { picture(p, u, 1.5f, 2.5f, 13, 11); });
}
// Stacked cards, offset like a shadow box seen from the side.
icon layers_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            const float w = 1.3f * u;
            p.polygon({{8 * u, 1.5f * u}, {15 * u, 5 * u}, {8 * u, 8.5f * u}, {1 * u, 5 * u}}, std::nullopt, ink, w);
            p.line({1 * u, 8 * u}, {8 * u, 11.5f * u}, ink, w, line_cap::round);
            p.line({8 * u, 11.5f * u}, {15 * u, 8 * u}, ink, w, line_cap::round);
            p.line({1 * u, 11 * u}, {8 * u, 14.5f * u}, ink, w, line_cap::round);
            p.line({8 * u, 14.5f * u}, {15 * u, 11 * u}, ink, w, line_cap::round);
        });
}
// One card on its own.
icon layer_icon()
{
    return drawn([](painter& p, float u) { p.polygon({{8 * u, 4 * u}, {15 * u, 7.5f * u}, {8 * u, 11 * u}, {1 * u, 7.5f * u}}, std::nullopt, ink, 1.3f * u); });
}
// Auto: a spark over the stack.
icon generate_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            const float w = 1.3f * u;
            p.polygon({{7 * u, 6 * u}, {13 * u, 9 * u}, {7 * u, 12 * u}, {1 * u, 9 * u}}, std::nullopt, ink, w);
            p.line({1 * u, 12 * u}, {7 * u, 15 * u}, ink, w, line_cap::round);
            p.line({7 * u, 15 * u}, {13 * u, 12 * u}, ink, w, line_cap::round);
            p.polygon({{13 * u, 0.5f * u},
                       {13.8f * u, 3.2f * u},
                       {16 * u, 4 * u},
                       {13.8f * u, 4.8f * u},
                       {13 * u, 7.5f * u},
                       {12.2f * u, 4.8f * u},
                       {10 * u, 4 * u},
                       {12.2f * u, 3.2f * u}},
                      ink, std::nullopt);
        });
}
// Export: an arrow up out of a tray.
icon export_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            const float w = 1.4f * u;
            p.line({2 * u, 10 * u}, {2 * u, 14 * u}, ink, w, line_cap::round);
            p.line({2 * u, 14 * u}, {14 * u, 14 * u}, ink, w, line_cap::round);
            p.line({14 * u, 14 * u}, {14 * u, 10 * u}, ink, w, line_cap::round);
            p.line({8 * u, 11 * u}, {8 * u, 2.5f * u}, ink, w, line_cap::round);
            p.line({4.5f * u, 6 * u}, {8 * u, 2.5f * u}, ink, w, line_cap::round);
            p.line({11.5f * u, 6 * u}, {8 * u, 2.5f * u}, ink, w, line_cap::round);
        });
}
// AI: two sparkles.
icon ai_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            const auto star = [&](float x, float y, float r)
            {
                const float k = r * .28f;
                p.polygon({{x * u, (y - r) * u},
                           {(x + k) * u, (y - k) * u},
                           {(x + r) * u, y * u},
                           {(x + k) * u, (y + k) * u},
                           {x * u, (y + r) * u},
                           {(x - k) * u, (y + k) * u},
                           {(x - r) * u, y * u},
                           {(x - k) * u, (y - k) * u}},
                          ink, std::nullopt);
            };
            star(6.5f, 9.5f, 6);
            star(13, 3.5f, 3);
        });
}
icon orientation_icon(bool landscape)
{
    return drawn([landscape](painter& p, float u)
                 { p.rect(landscape ? rect_f{u, 4 * u, 14 * u, 8 * u} : rect_f{4 * u, u, 8 * u, 14 * u}, u, std::nullopt, ink); });
}
// Move: a cross of four arrows.
icon move_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            const float w = 1.3f * u;
            p.line({8 * u, 1.5f * u}, {8 * u, 14.5f * u}, ink, w);
            p.line({1.5f * u, 8 * u}, {14.5f * u, 8 * u}, ink, w);
            p.polygon({{8 * u, .5f * u}, {10.5f * u, 3.5f * u}, {5.5f * u, 3.5f * u}}, ink, std::nullopt);
            p.polygon({{8 * u, 15.5f * u}, {10.5f * u, 12.5f * u}, {5.5f * u, 12.5f * u}}, ink, std::nullopt);
            p.polygon({{.5f * u, 8 * u}, {3.5f * u, 5.5f * u}, {3.5f * u, 10.5f * u}}, ink, std::nullopt);
            p.polygon({{15.5f * u, 8 * u}, {12.5f * u, 5.5f * u}, {12.5f * u, 10.5f * u}}, ink, std::nullopt);
        });
}
// Crop: two overlapping right angles.
icon crop_icon()
{
    return drawn(
        [](painter& p, float u)
        {
            const float w = 1.5f * u;
            p.line({4 * u, 1 * u}, {4 * u, 12 * u}, ink, w);
            p.line({4 * u, 12 * u}, {15 * u, 12 * u}, ink, w);
            p.line({1 * u, 4 * u}, {12 * u, 4 * u}, ink, w);
            p.line({12 * u, 4 * u}, {12 * u, 15 * u}, ink, w);
        });
}
// Undo and redo: a curved arrow.
icon undo_icon(bool redo)
{
    return drawn(
        [redo](painter& p, float u)
        {
            const float w = 1.5f * u;
            const auto x = [&](float v) { return (redo ? 16 - v : v) * u; };
            constexpr float pi = 3.14159265f;
            if (redo)
                p.arc({x(8.5f), 9.5f * u}, 5 * u, -pi / 2, pi * .75f, ink, w, line_cap::round);
            else
                p.arc({x(8.5f), 9.5f * u}, 5 * u, pi * .25f, pi * 1.5f, ink, w, line_cap::round);
            p.polygon({{x(8.5f), 1.5f * u}, {x(8.5f), 7.5f * u}, {x(4.5f), 4.5f * u}}, ink, std::nullopt);
        });
}

// -- the right panel's lists --------------------------------------------------

namespace
{
void paint_thumbnail(painter& p, const cell_info& cell, const rap::image* image, bool checker)
{
    const rect_f box{4, 3, cell.size.w - 8, cell.size.h - 6};
    if (!image)
        return;
    const auto dst = fit_inside(box, float(image->width()), float(image->height()));
    if (checker)
    {
        // Transparent pixels read as transparent, as image editors show them.
        p.rect(dst, 0, color{255, 255, 255}, std::nullopt);
        p.push_clip(dst);
        for (float y = dst.y; y < dst.y + dst.h; y += 4)
            for (float x = dst.x + (int((y - dst.y) / 4) % 2) * 4; x < dst.x + dst.w; x += 8)
                p.rect({x, y, 4, 4}, 0, color{214, 214, 214}, std::nullopt);
        p.pop_clip();
    }
    p.image(*image, {0, 0, float(image->width()), float(image->height())}, dst);
    p.rect(dst, 0, std::nullopt, paint{theme_color::border});
}
}  // namespace
class editor_window::layer_model : public list_model
{
public:
    explicit layer_model(editor_window& w) : w_(w) {}
    std::shared_ptr<project> shown;
    std::vector<row_key> keys;
    using list_model::notify_changed;
    using list_model::notify_inserted;
    using list_model::notify_removed;
    using list_model::notify_reset;
    // Rows are front first; layers are stored bottom first.
    int index(size_t row) const { return int(keys.size()) - 1 - int(row); }
    const layer* at(size_t row) const
    {
        const int i = index(row);
        return shown && row < keys.size() && i >= 0 && i < int(shown->layers.size()) ? &shown->layers[i] : nullptr;
    }
    static row_key key(const project& p, const layer& l)
    {
        return {std::to_string(l.id), std::format("{}\x1f{}\x1f{}", l.name, l.visible, detail(p, l))};
    }
    static std::string detail(const project& p, const layer& l)
    {
        const auto t = effective_transform(l, p.options);
        // An AI result waiting for an answer keeps its flag here (⚑).
        return std::format("{}{:.0f} × {:.0f} mm", to_review(l) ? "⚑ to review · " : "", width_mm(t), height_mm(t));
    }
    size_t row_count() const override { return keys.size(); }
    std::string text(size_t row, size_t column) const override
    {
        const auto* l = at(row);
        if (!l)
            return {};
        if (column == 2)
            return l->name;
        if (column == 3)
            return l->visible ? detail(*shown, *l) : "hidden";
        return {};
    }
    text_style style(size_t row, size_t column) const override
    {
        const auto* l = at(row);
        text_style s;
        if (l && (!l->visible || column == 3))
            s.fg = theme_color::placeholder_text;
        return s;
    }
    rap::icon icon(size_t row, size_t column) const override { return column == 2 && at(row) ? picture_icon() : rap::icon{}; }
    void paint(size_t row, size_t column, painter& p, const cell_info& cell) const override
    {
        if (const auto* l = at(row); l && column == 1)
            paint_thumbnail(p, cell, w_.thumbnail(*l), true);
    }
    check_state checked(size_t row) const override
    {
        const auto* l = at(row);
        return l && l->visible ? check_state::checked : check_state::unchecked;
    }
    void toggle_checked(size_t row) override
    {
        auto p = w_.current();
        if (!p || p != shown || p->busy || !at(row))
            return;
        auto& l = p->layers[index(row)];
        l.visible = !l.visible;
        w_.edited(l.visible ? "Show image" : "Hide image");
    }

private:
    editor_window& w_;
};
class editor_window::sheet_model : public list_model
{
public:
    explicit sheet_model(editor_window& w) : w_(w) {}
    std::shared_ptr<project> shown;
    std::vector<row_key> keys;
    using list_model::notify_changed;
    using list_model::notify_inserted;
    using list_model::notify_removed;
    using list_model::notify_reset;
    static std::string name(const project& p, size_t i)
    {
        const auto n = p.cuts.size();
        return std::format("Sheet {}{}", i + 1, i == 0 ? " · front" : i + 1 == n ? " · backing" : "");
    }
    static std::string detail(const project& p, const std::vector<size_t>& floating, size_t i)
    {
        if (i + 1 < p.cuts.size() && frame_only(p.cuts[i], p.options))
            return "frame only";
        if (i < floating.size() && floating[i])
            return std::format("{} floating", floating[i]);
        return {};
    }
    const cut_layer* at(size_t row) const { return shown && row < keys.size() && row < shown->cuts.size() ? &shown->cuts[row] : nullptr; }
    size_t row_count() const override { return keys.size(); }
    std::string text(size_t row, size_t column) const override
    {
        if (!at(row))
            return {};
        if (column == 2)
            return name(*shown, row);
        if (column == 3)
            return detail(*shown, w_.floating_, row);
        return {};
    }
    text_style style(size_t row, size_t column) const override
    {
        text_style s;
        if (column == 3 && at(row))
            s.fg = theme_color::negative;
        else if (const auto* c = at(row); c && !c->visible)
            s.fg = theme_color::placeholder_text;
        return s;
    }
    void paint(size_t row, size_t column, painter& p, const cell_info& cell) const override
    {
        if (column == 1 && at(row))
            paint_thumbnail(p, cell, w_.sheet_thumbnail(row), false);
    }
    check_state checked(size_t row) const override
    {
        const auto* c = at(row);
        return c && c->visible ? check_state::checked : check_state::unchecked;
    }
    void toggle_checked(size_t row) override
    {
        auto p = w_.current();
        if (!p || p != shown || p->busy || !at(row))
            return;
        auto& c = p->cuts[row];
        c.visible = !c.visible;
        // Preview visibility: every sheet is still exported.
        w_.edited(c.visible ? "Show sheet" : "Hide sheet", {}, false);
    }

private:
    editor_window& w_;
};
// Here, where the list models are complete types.
editor_window::editor_window(application& a, project_tab first) : app_(a)
{
    layer_rows_ = make_layer_model(*this);
    sheet_rows_ = make_sheet_model(*this);
    build();
    insert(std::move(first));
}
editor_window::~editor_window() = default;
std::unique_ptr<editor_window::layer_model> make_layer_model(editor_window& w)
{
    return std::make_unique<editor_window::layer_model>(w);
}
std::unique_ptr<editor_window::sheet_model> make_sheet_model(editor_window& w)
{
    return std::make_unique<editor_window::sheet_model>(w);
}
rap::list_model* layer_model_ptr(editor_window::layer_model& m)
{
    return &m;
}
rap::list_model* sheet_model_ptr(editor_window::sheet_model& m)
{
    return &m;
}
std::vector<column> layer_columns()
{
    return {{.title = "Shown", .width = 3_ch}, {.title = "Preview", .width = 54_px, .painted = true}, {.title = "Name", .stretch = true}, {.title = "Size", .width = 13_ch}};
}
std::vector<column> sheet_columns()
{
    return {{.title = "Shown", .width = 3_ch}, {.title = "Preview", .width = 54_px, .painted = true}, {.title = "Sheet", .stretch = true}, {.title = "Issues", .width = 11_ch}};
}

// -- keeping the lists in place ------------------------------------------------

namespace
{
// Applies new rows without losing the user's place: changed rows redraw in
// place; added or removed rows keep the row the user was looking at where it
// was in the view.
template <class Model>
void apply_rows(Model& model, rap::list* list, const std::shared_ptr<project>& p, std::vector<row_key> keys)
{
    if (model.shown != p)
    {
        model.shown = p;
        model.keys = std::move(keys);
        model.notify_reset();
        return;
    }
    const auto plan = plan_rows(model.keys, keys);
    if (plan.same_rows)
    {
        model.keys = std::move(keys);
        for (size_t row : plan.changed)
            model.notify_changed(row, 1);
        return;
    }
    // An anchor: the selected row if it's in view, else the first one in view.
    std::optional<std::string> anchor;
    float offset = 0;
    const auto sel = list->selection();
    if (!sel.empty() && sel.front() < model.keys.size())
        if (const auto at = list->row_offset(sel.front()))
        {
            anchor = model.keys[sel.front()].id;
            offset = *at;
        }
    for (size_t row = 0; !anchor && row < model.keys.size(); ++row)
        if (const auto at = list->row_offset(row); at && *at >= 0)
        {
            anchor = model.keys[row].id;
            offset = *at;
        }
    model.keys = std::move(keys);
    if (plan.removed)
        model.notify_removed(plan.first, plan.removed);
    if (plan.inserted)
        model.notify_inserted(plan.first, plan.inserted);
    for (size_t row : plan.changed)
        model.notify_changed(row, 1);
    if (anchor)
        for (size_t row = 0; row < model.keys.size(); ++row)
            if (model.keys[row].id == *anchor)
            {
                list->set_row_offset(row, offset);
                break;
            }
}
void select_row(rap::list* list, std::optional<size_t> row, bool scroll)
{
    const auto sel = list->selection();
    if (row ? sel.size() == 1 && sel.front() == *row : sel.empty())
        return;
    list->select(row ? std::vector<size_t>{*row} : std::vector<size_t>{});
    if (row)
    {
        list->set_current(*row);
        if (scroll)
            list->scroll_to(*row, scroll_align::nearest);
    }
}
}  // namespace
void editor_window::sync_lists()
{
    auto p = current();
    if (!p)
        return;
    syncing_ = true;
    // Sheets: thumbnails and floating pieces follow the sheets' contents.
    uint64_t key = 1469598103934665603ull ^ p->cuts.size();
    for (const auto& c : p->cuts)
    {
        key = (key ^ uint64_t(c.width) << 20 ^ uint64_t(c.height)) * 1099511628211ull;
        for (size_t i = 0; i < c.solid.size(); i += 97)
            key = (key ^ c.solid[i] ^ (i << 1)) * 1099511628211ull;
    }
    if (key != sheet_thumbs_key_ || sheet_thumbs_.size() != p->cuts.size())
    {
        sheet_thumbs_key_ = key;
        sheet_thumbs_.clear();
        floating_.clear();
        for (size_t i = 0; i < p->cuts.size(); ++i)
        {
            const auto& c = p->cuts[i];
            floating_.push_back(disconnected_components(c));
            const int scale = std::max(1, std::max(c.width, c.height) / 64);
            raster small{std::max(1, c.width / scale), std::max(1, c.height / scale), {}};
            small.rgba.assign(size_t(small.width) * small.height * 4, 255);
            const auto colour = sheet_colour(int(i), int(p->cuts.size()));
            for (int y = 0; y < small.height; ++y)
                for (int x = 0; x < small.width; ++x)
                    if (c.solid[size_t(y * scale) * c.width + x * scale])
                        std::copy(colour.begin(), colour.end(), small.rgba.begin() + (size_t(y) * small.width + x) * 4);
            sheet_thumbs_.push_back(app_.ui.create_image(small.width, small.height, pixel_format::rgba8, small.rgba.data(), size_t(small.width) * 4));
        }
    }
    std::vector<row_key> layer_keys, sheet_keys;
    for (size_t i = p->layers.size(); i-- > 0;)
        layer_keys.push_back(layer_model::key(*p, p->layers[i]));
    for (size_t i = 0; i < p->cuts.size(); ++i)
        sheet_keys.push_back(
            {sheet_model::name(*p, i), std::format("{}\x1f{}\x1f{}", p->cuts[i].visible, sheet_model::detail(*p, floating_, i), sheet_thumbs_key_)});
    const bool new_project = layer_rows_->shown != p;
    apply_rows(*layer_rows_, layers_list_, p, std::move(layer_keys));
    apply_rows(*sheet_rows_, sheets_list_, p, std::move(sheet_keys));
    const int n = int(p->layers.size());
    select_row(layers_list_, p->selected_layer >= 0 && p->selected_layer < n ? std::optional<size_t>(size_t(n - 1 - p->selected_layer)) : std::nullopt,
               true);
    select_row(sheets_list_,
               p->selected_cut >= 0 && p->selected_cut < int(p->cuts.size()) ? std::optional<size_t>(size_t(p->selected_cut)) : std::nullopt, true);
    (void)new_project;
    // Thumbnails of layers that are gone, or whose pixels moved (a job's
    // result replaces the project): rebuilt as they're painted.
    size_t total = 0;
    for (const auto& t : tabs_)
        total += t.document->layers.size();
    if (thumbs_.size() > 2 * total + 8)
        thumbs_.clear();
    syncing_ = false;
}
const rap::image* editor_window::thumbnail(const layer& l)
{
    const auto& image = current_pixels(l);
    if (!image.valid())
        return nullptr;
    // Ids are per project, so different tabs may reuse one: key the pixels too.
    const auto key = std::format("{}:{}x{}:{}", l.id, image.width, image.height, static_cast<const void*>(image.rgba.data()));
    auto& slot = thumbs_[key];
    if (!slot)
    {
        const double scale = std::min(1., 96. / std::max(image.width, image.height));
        raster small{std::max(1, int(image.width * scale)), std::max(1, int(image.height * scale)), {}};
        small.rgba.resize(size_t(small.width) * small.height * 4);
        for (int y = 0; y < small.height; ++y)
            for (int x = 0; x < small.width; ++x)
            {
                const int sx = std::min(image.width - 1, int(x / scale)), sy = std::min(image.height - 1, int(y / scale));
                std::copy_n(image.rgba.begin() + (size_t(sy) * image.width + sx) * 4, 4, small.rgba.begin() + (size_t(y) * small.width + x) * 4);
            }
        slot = app_.ui.create_image(small.width, small.height, pixel_format::rgba8, small.rgba.data(), size_t(small.width) * 4);
    }
    return slot.get();
}
const rap::image* editor_window::sheet_thumbnail(size_t sheet)
{
    return sheet < sheet_thumbs_.size() ? sheet_thumbs_[sheet].get() : nullptr;
}
}  // namespace offcut
