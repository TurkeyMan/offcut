#pragma once
#include <optional>
#include "project.h"
// An item's version thread (docs/ux-design.md sections 2 and 5): the
// original as version 0, then AI results, each made from a parent version, so
// the thread can branch. One is current, and the whole pipeline uses it.
namespace offcut
{
// The pixels the item shows: its current version's, else its own image.
const raster& current_pixels(const layer&);
item_version* find_version(layer&, uint32_t id);
const item_version* find_version(const layer&, uint32_t id);
uint32_t next_version_id(const layer&);
// Adds `v` after the others with the next id, as a result arriving: it
// becomes current, pending review, and remembers what was current before.
// Its pixels are fitted to the item's (cover-cropped). Returns its id.
uint32_t add_version(layer&, item_version v);
// Makes a version current; false if the item has no such version.
bool set_current_version(layer&, uint32_t id);
// Removes a result from the thread: its children move to its parent, and if
// it was current, the version current before it arrived is again (else its
// parent). False if there is no such version.
bool discard_version(layer&, uint32_t id);
// The drift guard: more issues than the version it was made from.
bool drifted(const layer&, const item_version&);
bool to_review(const layer&);  // a result waits for an answer (⚑)
// Where the Versions strip draws each version: version 0 at row 0, column 0;
// a result continues its parent's row when it follows its parent or a
// sibling there (Try again), else it starts a branch row below, one column
// right of its parent.
struct version_cell
{
    uint32_t id = 0;
    int row = 0, column = 0;
};
std::vector<version_cell> version_layout(const layer&);
// What the rounds before `id` asked for, oldest first, for "already done":
// each ancestor's instructions and the review that led on from it.
std::vector<std::string> thread_requests(const layer&, uint32_t id);
// Cover-crops and resamples (area average) to exactly w × h.
raster fit_raster(const raster&, int w, int h);
// Keeps an instruction at the front of the project's recent ten.
void remember_instruction(project&, const std::string&);
}  // namespace offcut
