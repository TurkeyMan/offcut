#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "project.h"
namespace offcut
{
// Snapshot undo for a project's document state: frame and cut options, name
// and instructions, layers and their transforms, and the generated sheets.
// Selection is restored with a state but never makes a step of its own.
// Layer pixels are immutable once added (operations make new layers), so
// snapshots share them by layer id instead of copying; sheets are shared
// between snapshots while their contents are unchanged.
class history
{
public:
    using clock = std::chrono::steady_clock;
    explicit history(size_t limit = 100) : limit_(limit) {}
    // Forgets everything: `p` is the only, saved state (a new or opened project).
    void reset(const project& p);
    // Records the document as it is now after an edit called `label`. An
    // unchanged document records nothing. With a merge key, an edit that
    // follows one with the same key within `merge_window` replaces it, so a
    // drag or typing a number is one step. Returns true when a step was added
    // or replaced.
    bool commit(const project& p, const std::string& label, const std::string& merge = {}, clock::time_point now = clock::now());
    bool can_undo() const { return index_ > 0; }
    bool can_redo() const { return index_ + 1 < states_.size(); }
    std::string undo_label() const;  // what Undo would undo, empty when nothing
    std::string redo_label() const;
    // Restores the previous/next state into `p`, keeping its runtime state
    // (path, view, zoom, job). Sets `p.dirty` from the saved state.
    bool undo(project& p);
    bool redo(project& p);
    void mark_saved() { saved_ = index_; }
    bool at_saved() const { return saved_ == index_; }
    size_t size() const { return states_.size(); }
    // Breaks merging, so the next edit is a step of its own (a drag ended).
    void seal() { merge_.clear(); }
    static constexpr auto merge_window = std::chrono::milliseconds(1500);

private:
    struct layer_state
    {
        layer meta;  // without pixels, its versions' too
        std::shared_ptr<const raster> pixels;
        std::vector<std::shared_ptr<const raster>> version_pixels;  // one per meta.versions
    };
    struct state
    {
        std::string label, name, instructions, ai_style;
        transform_options options;
        std::vector<layer_state> layers;
        std::shared_ptr<const std::vector<cut_layer>> cuts;
        bool stale = false;
        int selected_layer = -1, selected_cut = 0;
    };
    state capture(const project&) const;
    static bool same_document(const state&, const state&);
    void restore(const state&, project&) const;
    std::vector<state> states_;
    size_t index_ = 0, saved_ = 0, limit_;
    std::string merge_;
    clock::time_point merged_at_{};
};
// What a list must do to go from rows `before` to rows `after`, named by
// stable ids: when the ids are the same, only the changed rows are redrawn;
// otherwise the common start and end stay and the middle is removed and
// inserted, so the list keeps its scroll position and selection.
struct row_plan
{
    bool same_rows = false;
    size_t first = 0;  // first row that differs
    size_t removed = 0, inserted = 0;
    std::vector<size_t> changed;  // rows (in `after`) whose content changed but not their id
};
struct row_key
{
    std::string id, content;
    bool operator==(const row_key&) const = default;
};
row_plan plan_rows(const std::vector<row_key>& before, const std::vector<row_key>& after);
}  // namespace offcut
