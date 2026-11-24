#pragma once
// The director's local operations (docs/ux-design.md section 8, verdict
// "fix_local"), applied by Offcut to a flat illustration without any AI call.
// Interface shared by the mechanical implementation (local_ops.cpp) and the
// Auto loop (UI side). Keep these signatures stable.
//
// Ops change the approved picture itself (the visible-picture lock): the
// result is a new flat raster in the same palette, which then becomes a new
// version of the item; sheets are rebuilt from it as usual.
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "project.h"
namespace offcut
{
struct local_ops_input
{
    raster image;                                  // the version's pixels (flat, snapped to `palette`)
    std::vector<std::array<uint8_t, 3>> palette;   // one colour per sheet, front to back
    transform_options options;                     // frame, borders, sheets, Min. width, layer gap
    std::string measurements;                      // Offcut's measurements JSON the ops' ids refer to (m1, m2, …)
    std::string ops;                               // the verdict's local_ops JSON array
};
struct local_ops_result
{
    raster image;                  // the edited picture; equal to the input when nothing applied
    std::vector<std::string> applied;  // one line per op applied, in words ("Pushed back the floating piece m3 (38 mm²) to sheet 3")
    std::vector<std::string> skipped;  // ops not applied, with why ("extract needs a UI step", "m9 not found")
    size_t changed_pixels = 0;
};
// Applies the ops in order. Never throws for a single bad op: it is skipped
// with a reason. Throws only for malformed input (not JSON, wrong sizes).
local_ops_result apply_local_ops(const local_ops_input&);

// Applies a verdict's plan_patch (JSON Patch: add/remove/replace/move/copy/
// test, values as strings) to a plan JSON; the result must still validate
// as a plan. Throws a user-readable error otherwise.
std::string apply_plan_patch(const std::string& plan_json, const std::string& patch_json);

// Lower is better: unsupported area first, then thin features, then tone
// surplus (the same order validation reports). For choosing the best round.
double auto_round_score(const std::string& measurements_json);
}  // namespace offcut
