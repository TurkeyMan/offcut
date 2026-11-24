#include "history.h"
#include <algorithm>
#include <cstring>
namespace offcut
{
namespace
{
bool same_pixels(const raster& a, const raster& b)
{
    return a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size() &&
           (a.rgba.empty() || std::memcmp(a.rgba.data(), b.rgba.data(), a.rgba.size()) == 0);
}
bool same_cuts(const std::vector<cut_layer>& a, const std::vector<cut_layer>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].visible != b[i].visible || a[i].width != b[i].width || a[i].height != b[i].height ||
            a[i].solid.size() != b[i].solid.size() || (!a[i].solid.empty() && std::memcmp(a[i].solid.data(), b[i].solid.data(), a[i].solid.size()) != 0))
            return false;
    return true;
}
// Versions are immutable once they arrive, apart from their review; their
// pixels are compared by the caller.
bool same_versions(const std::vector<item_version>& a, const std::vector<item_version>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto &x = a[i], &y = b[i];
        if (x.id != y.id || x.parent != y.parent || x.state != y.state || x.prompt != y.prompt || x.review != y.review || x.previous != y.previous ||
            x.task != y.task || x.instructions != y.instructions || x.style != y.style || x.notes != y.notes || x.prompt_files != y.prompt_files ||
            x.account != y.account || x.provider != y.provider || x.model != y.model || x.created != y.created || x.seconds != y.seconds ||
            x.usage != y.usage || x.issues != y.issues || x.findings != y.findings || x.request != y.request || x.image.width != y.image.width ||
            x.image.height != y.image.height || x.run != y.run || x.round != y.round || x.score != y.score || x.measurements != y.measurements ||
            x.plan != y.plan || x.verdict != y.verdict || x.ops != y.ops || x.explanation != y.explanation || x.chips != y.chips || x.palette != y.palette ||
            !(x.illustrator == y.illustrator) || !(x.director == y.director))
            return false;
    }
    return true;
}
bool same_options(const transform_options& a, const transform_options& b)
{
    return a.width_mm == b.width_mm && a.height_mm == b.height_mm && a.border_mm == b.border_mm && a.bridge_mm == b.bridge_mm && a.layers == b.layers &&
           a.smoothing == b.smoothing && a.connect == b.connect && a.uniform_border == b.uniform_border && a.border_top_mm == b.border_top_mm &&
           a.border_bottom_mm == b.border_bottom_mm && a.border_left_mm == b.border_left_mm && a.border_right_mm == b.border_right_mm &&
           a.placement == b.placement && a.touch_edges == b.touch_edges && a.size_preset == b.size_preset && a.landscape == b.landscape &&
           a.layer_gap_mm == b.layer_gap_mm;
}
}  // namespace
history::state history::capture(const project& p) const
{
    state s;
    s.name = p.name;
    s.instructions = p.instructions;
    s.ai_style = p.ai_style;
    s.options = p.options;
    s.stale = p.stale;
    s.selected_layer = p.selected_layer;
    s.selected_cut = p.selected_cut;
    const state* previous = states_.empty() ? nullptr : &states_[index_];
    for (const auto& l : p.layers)
    {
        layer_state ls;
        ls.meta = l;
        ls.meta.image = {l.image.width, l.image.height, {}};
        // Share the pixels with any snapshot that already holds this layer.
        if (l.id)
            for (const auto& st : states_)
            {
                for (const auto& old : st.layers)
                    if (old.meta.id == l.id && same_pixels(*old.pixels, l.image))
                    {
                        ls.pixels = old.pixels;
                        break;
                    }
                if (ls.pixels)
                    break;
            }
        if (!ls.pixels)
            ls.pixels = std::make_shared<const raster>(l.image);
        // Versions' pixels, shared the same way by layer and version id.
        for (auto& v : ls.meta.versions)
        {
            std::shared_ptr<const raster> shared;
            if (l.id)
                for (const auto& st : states_)
                {
                    for (const auto& old : st.layers)
                        if (old.meta.id == l.id)
                            for (size_t k = 0; k < old.meta.versions.size() && k < old.version_pixels.size(); ++k)
                                if (old.meta.versions[k].id == v.id && same_pixels(*old.version_pixels[k], v.image))
                                {
                                    shared = old.version_pixels[k];
                                    break;
                                }
                    if (shared)
                        break;
                }
            ls.version_pixels.push_back(shared ? shared : std::make_shared<const raster>(v.image));
            v.image = {v.image.width, v.image.height, {}};
        }
        s.layers.push_back(std::move(ls));
    }
    if (previous && same_cuts(*previous->cuts, p.cuts))
        s.cuts = previous->cuts;
    else
        s.cuts = std::make_shared<const std::vector<cut_layer>>(p.cuts);
    return s;
}
bool history::same_document(const state& a, const state& b)
{
    if (a.name != b.name || a.instructions != b.instructions || a.ai_style != b.ai_style || !same_options(a.options, b.options) || a.stale != b.stale ||
        a.layers.size() != b.layers.size() || (a.cuts != b.cuts && !same_cuts(*a.cuts, *b.cuts)))
        return false;
    for (size_t i = 0; i < a.layers.size(); ++i)
    {
        const auto &x = a.layers[i].meta, &y = b.layers[i].meta;
        if (x.name != y.name || x.visible != y.visible || x.type != y.type || x.id != y.id || x.derived_from != y.derived_from ||
            !(x.transform == y.transform) || x.notes != y.notes || x.version != y.version || !same_versions(x.versions, y.versions) || (a.layers[i].pixels != b.layers[i].pixels && !same_pixels(*a.layers[i].pixels, *b.layers[i].pixels)))
            return false;
        const auto &u = a.layers[i].version_pixels, &w = b.layers[i].version_pixels;
        for (size_t k = 0; k < u.size() && k < w.size(); ++k)
            if (u[k] != w[k] && !same_pixels(*u[k], *w[k]))
                return false;
    }
    return true;
}
void history::restore(const state& s, project& p) const
{
    p.name = s.name;
    p.instructions = s.instructions;
    p.ai_style = s.ai_style;
    p.options = s.options;
    p.stale = s.stale;
    p.layers.clear();
    for (const auto& ls : s.layers)
    {
        auto l = ls.meta;
        l.image = *ls.pixels;
        for (size_t k = 0; k < l.versions.size() && k < ls.version_pixels.size(); ++k)
            l.versions[k].image = *ls.version_pixels[k];
        p.layers.push_back(std::move(l));
    }
    p.cuts = *s.cuts;
    p.selected_layer = std::clamp(s.selected_layer, -1, int(p.layers.size()) - 1);
    p.selected_cut = std::clamp(s.selected_cut, 0, std::max(0, int(p.cuts.size()) - 1));
    p.dirty = !at_saved();
}
void history::reset(const project& p)
{
    states_.clear();
    index_ = saved_ = 0;
    merge_.clear();
    states_.push_back(capture(p));
    states_.back().label = "Open";
}
bool history::commit(const project& p, const std::string& label, const std::string& merge, clock::time_point now)
{
    if (states_.empty())
    {
        reset(p);
        return false;
    }
    auto next = capture(p);
    next.label = label;
    if (same_document(next, states_[index_]))
    {
        // Nothing to undo; remember where the selection went.
        states_[index_].selected_layer = next.selected_layer;
        states_[index_].selected_cut = next.selected_cut;
        return false;
    }
    const bool merging = !merge.empty() && merge == merge_ && index_ > 0 && index_ + 1 == states_.size() && now - merged_at_ <= merge_window &&
                         saved_ != index_;
    if (merging)
        states_[index_] = std::move(next);
    else
    {
        states_.erase(states_.begin() + std::ptrdiff_t(index_ + 1), states_.end());
        if (saved_ > index_)
            saved_ = size_t(-1);  // the saved state was undone and is gone
        states_.push_back(std::move(next));
        ++index_;
        if (states_.size() > limit_)
        {
            states_.erase(states_.begin());
            --index_;
            saved_ = saved_ == 0 || saved_ == size_t(-1) ? size_t(-1) : saved_ - 1;
        }
    }
    merge_ = merge;
    merged_at_ = now;
    return true;
}
std::string history::undo_label() const
{
    return can_undo() ? states_[index_].label : std::string();
}
std::string history::redo_label() const
{
    return can_redo() ? states_[index_ + 1].label : std::string();
}
bool history::undo(project& p)
{
    if (!can_undo())
        return false;
    --index_;
    merge_.clear();
    restore(states_[index_], p);
    return true;
}
bool history::redo(project& p)
{
    if (!can_redo())
        return false;
    ++index_;
    merge_.clear();
    restore(states_[index_], p);
    return true;
}
row_plan plan_rows(const std::vector<row_key>& before, const std::vector<row_key>& after)
{
    row_plan plan;
    size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix].id == after[prefix].id)
        ++prefix;
    if (prefix == before.size() && prefix == after.size())
    {
        plan.same_rows = true;
        plan.first = before.size();
        for (size_t i = 0; i < after.size(); ++i)
            if (before[i].content != after[i].content)
                plan.changed.push_back(i);
        return plan;
    }
    size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - 1 - suffix].id == after[after.size() - 1 - suffix].id)
        ++suffix;
    plan.first = prefix;
    plan.removed = before.size() - prefix - suffix;
    plan.inserted = after.size() - prefix - suffix;
    for (size_t i = 0; i < prefix; ++i)
        if (before[i].content != after[i].content)
            plan.changed.push_back(i);
    for (size_t i = 0; i < suffix; ++i)
        if (before[before.size() - 1 - i].content != after[after.size() - 1 - i].content)
            plan.changed.push_back(after.size() - 1 - i);
    std::ranges::sort(plan.changed);
    return plan;
}
}  // namespace offcut
