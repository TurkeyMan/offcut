#pragma once
#include <array>
#include <optional>
#include "project.h"
namespace offcut
{
struct frame_geometry
{
    int w, h, left, top, right, bottom;
    int opening_width() const { return w - left - right; }
    int opening_height() const { return h - top - bottom; }
};
frame_geometry dimensions(const transform_options&);
bool border(int x, int y, frame_geometry);
struct subject_box
{
    int left = 0, top = 0, right = 0, bottom = 0;
    bool found = false, isolated = false;
};
subject_box detect_subject(const raster&);
transform_options new_project_options(const raster&);
// -- image transforms ---------------------------------------------------------
// Frame coordinates are millimetres from the frame's outer top-left, y down.
struct point_mm
{
    double x = 0, y = 0;
};
struct rect_mm
{
    double left = 0, top = 0, right = 0, bottom = 0;
    double width() const { return right - left; }
    double height() const { return bottom - top; }
};
rect_mm opening_mm(const transform_options&);
// Working-canvas pixels per millimetre (masks and previews).
double working_scale(const transform_options&);
pixel_rect full_crop(const raster&);
raster cropped(const raster&, const pixel_rect&);
// Where a placement mode puts this image (inside its crop) in the frame:
// what a newly added image starts with, and what Placement re-applies.
// Rotation is reset; the crop is kept (an unset crop is the whole image).
image_transform place_image(const layer&, const transform_options&, placement_mode, unsigned touch_edges);
// The layer's own transform, or for an unplaced one the project placement.
image_transform effective_transform(const layer&, const transform_options&);
point_mm to_frame(const image_transform&, double u, double v);  // source pixel → frame mm
point_mm to_source(const image_transform&, point_mm);           // frame mm → source pixel
// The crop rectangle's corners in the frame: top-left, top-right,
// bottom-right, bottom-left of the unrotated image.
std::array<point_mm, 4> corners(const image_transform&);
rect_mm bounds(const image_transform&);  // axis-aligned, rotation included
double width_mm(const image_transform&);
double height_mm(const image_transform&);
bool contains(const image_transform&, point_mm);
// The topmost visible layer under a frame point, or -1.
int hit_test(const std::vector<layer>&, const transform_options&, point_mm);
// A new crop that leaves the remaining pixels where they are in the frame.
image_transform with_crop(const image_transform&, pixel_rect);
// Uniform scale about a frame point, which stays put.
image_transform scaled_about(const image_transform&, double factor, point_mm anchor);
// Fits the (rotated) image inside the opening, centred.
image_transform fit_to_opening(const image_transform&, const transform_options&);
// Moves the image, at its current scale and rotation, so its subject (or,
// without one, its edge) overlaps that opening edge by Min. width.
image_transform touch(const layer&, const transform_options&, touch_edge);
// Edge snapping: moves `box` so whichever of its edges (`edges`, touch_edge
// bits) is nearest an opening edge within `threshold_mm` lands on it.
struct snap_result
{
    double dx = 0, dy = 0;
    std::optional<double> guide_x, guide_y;  // the opening edge snapped to, mm
};
snap_result snap_to_opening(rect_mm box, rect_mm opening, double threshold_mm, unsigned edges = 15);
raster source_preview(const std::vector<source_image>&, const transform_options&);
raster flatten(const std::vector<source_image>&, const transform_options&);
std::vector<cut_layer> simplify(const std::vector<source_image>&, const transform_options&);
std::vector<cut_layer> masks_from_images(const std::vector<source_image>&, const transform_options&);
struct tone_map
{
    int width = 0, height = 0;
    std::vector<uint8_t> indices;  // darkest/front = 0
};
// A piece of card worth telling the user (and a model) about: which sheet,
// where its centre is in working-canvas pixels, and how big it is.
struct piece_note
{
    int sheet = 0;
    double x = 0, y = 0;
    size_t pixels = 0;
};
struct sheet_result
{
    std::vector<cut_layer> layers;
    std::vector<size_t> removed_per_layer;
    std::vector<piece_note> removed;  // the floating pieces removed, specks included
    int tones_found = 0;              // an AI result's distinct tones (1% or more); 0 not measured
};
// A common working-canvas tone map becomes depth-limited sheets with hidden
// connectivity support. Neither route asks an image model to produce SVG.
sheet_result build_sheets(const tone_map&, const transform_options&);
// Pixels where the sheets, stacked front to back, don't show the tone map's
// tone (its frame counted as the front tone): 0 when the visible picture is
// kept exactly. Debug builds check build_sheets with it.
size_t composite_mismatch(const std::vector<cut_layer>&, const tone_map&);
struct illustration_result : sheet_result
{
    double cropped_fraction = 0;  // fraction of input area lost to cover-cropping
    size_t outline_pixels = 0;    // dissolved outline pixels
};
// Snaps to `palette` (one colour per sheet, e.g. a plan's) when given, else
// finds the sheet count's tones; keeps floating pieces unless o.connect.
// With `dissolve`, outlines are dissolved first (dissolve_outlines).
illustration_result layers_from_illustration(const raster&, const transform_options&, const std::vector<std::array<uint8_t, 3>>& palette = {},
                                             bool dissolve = false);
// AI import: dark strokes (the darkest tone) narrower than about 3 × Min.
// width are given to the fills on either side, split down the middle, so an
// outline never becomes a lacy sheet of its own. Returns pixels reassigned.
size_t dissolve_outlines(tone_map&, const transform_options&);
void apply_illustration(project&, const raster&);  // shared manual/remote import and diagnostics
// How the sheets are shown: Assembled stacks the ticked sheets, front over
// back; `highlight` dims the others and outlines that sheet (shown even when
// unticked). `single` shows only that sheet, ticked or not.
struct cut_view
{
    int single = -1;
    int highlight = -1;
    bool floating = false;  // draw pieces that reach no edge in floating_colour
};
raster render_cuts(const std::vector<cut_layer>&, cut_view = {});
// The colour each sheet is drawn in; dimmed is the highlight's backdrop.
std::array<uint8_t, 3> sheet_colour(int sheet, int count, bool dimmed = false);
constexpr std::array<uint8_t, 3> highlight_outline{255, 122, 26};
constexpr std::array<uint8_t, 3> floating_colour{214, 48, 49};
size_t disconnected_components(const cut_layer&);
// Nothing but the frame: everything inside floated and was removed.
bool frame_only(const cut_layer&, const transform_options&);
// Validation findings for AI results (docs/ux-design.md sections 6 and 8):
// card that floats (removed or left) and features narrower than Min. width
// (what opening by a Min. width square removes). Specks below Min. width
// squared are not counted.
struct validation
{
    std::vector<piece_note> floating, thin;
    int tones_found = 0, tones_wanted = 0;  // an AI result's colours against the sheets (0: not measured)
    bool tone_mismatch() const { return tones_found && tones_wanted && tones_found != tones_wanted; }
    int count() const { return int(floating.size() + thin.size()) + (tone_mismatch() ? 1 : 0); }
    // Lower is better (docs/ux-design.md section 8): unsupported area first,
    // then thin features, then tone surplus.
    double score(const transform_options&) const;
};
// Offcut's measurements for a version record (and, in phase 2, the
// director), JSON with an id per finding ("m1", …).
std::string measurements_json(const validation&, const transform_options&);
std::vector<piece_note> thin_features(const cut_layer&, const transform_options&, int sheet);
validation validate_sheets(const std::vector<cut_layer>&, const transform_options&, const std::vector<piece_note>& removed);
// The findings in words, for the user and for every refinement: empty when
// nothing was found.
std::string describe_findings(const validation&, const transform_options&);
// The sheets for the composition: by tone separation (palette snap) when an
// AI illustration item shows an AI result, else by luminance bands (Auto).
sheet_result make_sheets(const std::vector<layer>&, const transform_options&);
// Reimagine's prompt for the project as it is: the manual route's Copy
// prompt and Export request, and what the Send dialog starts from.
std::string ai_prompt(const project&);
struct prompt_words;
// What Reimagine's prompt says about this project (Style, Notes, last
// instructions, where the composition meets the frame, references).
prompt_words reimagine_words(const project&);
// The Notes for AI of the visible items, one per line when several.
std::string ai_notes(const project&);
}  // namespace offcut
