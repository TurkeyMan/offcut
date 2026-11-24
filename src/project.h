#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace offcut
{
struct raster
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool valid() const;
};
// A rectangle in source pixels.
struct pixel_rect
{
    double x = 0, y = 0, w = 0, h = 0;
    bool operator==(const pixel_rect&) const = default;
};
// Where an Image layer sits in the frame. The crop rectangle (source pixels)
// is centred at (x_mm, y_mm) in frame coordinates (0,0 at the frame's outer
// top-left, y down), scaled by mm_per_pixel and rotated clockwise about that
// centre. An unplaced transform (mm_per_pixel 0) takes the project's
// placement when drawn: images built in code before they are arranged.
struct image_transform
{
    double x_mm = 0, y_mm = 0;
    double mm_per_pixel = 0;
    double rotation_deg = 0;
    pixel_rect crop;
    bool placed() const { return mm_per_pixel > 0; }
    bool operator==(const image_transform&) const = default;
};
// The document's typed layers. Phase 1 creates and edits Image layers only;
// the other kinds are reserved so files and the layers panel can grow into
// them (Colour regions → Masks are derived non-destructively from an Image).
enum class layer_type : uint32_t
{
    image = 0,
    colour_regions = 1,
    mask = 2,
    text = 3,
    sketch = 4,
    scenery = 5,           // stock shapes: Ground, Hill, Banner, Inner frame
    ai_illustration = 6,   // made by Reimagine; version 0 is its composition
};
// How the user answered an AI result (docs/ux-design.md section 8).
enum class review_state : uint32_t
{
    none = 0,      // the original, or not an AI result
    pending = 1,   // arrived, not answered yet: "to review" (⚑)
    used = 2,
    discarded = 3,
    answered = 4,  // answered with a follow-up (a chip, a review, Try again)
};
// One version in an item's thread. Version 0 is the original (the layer's
// own `image`); AI results follow, each made from a parent version, so
// threads can branch. Each keeps what was asked for and how it was reviewed
// (docs/ux-design.md section 8): never a key.
struct item_version
{
    uint32_t id = 0;      // 1, 2, …; 0 is the original
    uint32_t parent = 0;  // the version this one was made from
    raster image;         // the same pixel size as the layer's `image`
    std::string prompt;   // the full prompt sent for it
    std::string review;   // the user's answer, in words ("support looks added on")
    review_state state = review_state::none;
    uint32_t previous = 0;     // the version that was current when it arrived (Discard returns there)
    std::string task;          // "Reimagine", "Refine", "Try again", "Fix with AI"
    std::string instructions;  // the three user texts as sent
    std::string style, notes;
    std::string prompt_files;  // "brief.txt@1a2b3c4d refine.txt@…"
    std::string account, provider, model;  // the Settings entry's id and name, and the model
    std::string created;                   // local time, ISO 8601
    double seconds = 0;                    // the whole round
    std::string usage;     // the image provider's usage JSON
    int issues = -1;       // validation after it arrived: floating + too thin; -1 unknown
    std::string findings;  // the same in words, added to every refinement
    std::string request;   // the folder holding the request and response
    // The run it belongs to (one consent: Reimagine, or a Refine from the
    // Send dialog) and its round there; review-bar follow-ups continue a run.
    std::string run;
    uint32_t round = 1;
    double score = -1;          // lower is better: unsupported area, then thin features, then tone surplus; -1 unknown
    std::string measurements;   // Offcut's measurements, JSON
    std::string plan, verdict;  // the director's (phase 2), JSON; empty on the template route
    std::string ops;            // local ops applied, JSON
    std::string explanation;    // one sentence for the review bar (the verdict's "explain")
    std::string chips;          // the review chips chosen, by id ("support.added-on detail.thin-lines")
    // The colours the request gave the layers, front to back ("#1e5b2e
    // #e8b83a …"): the import snaps to them, one sheet each. Empty: Offcut
    // finds the tones.
    std::string palette;
    struct role_usage
    {
        double seconds = 0;
        uint64_t input_tokens = 0, output_tokens = 0;
        bool operator==(const role_usage&) const = default;
    };
    role_usage illustrator, director;
};
std::string_view layer_type_name(layer_type);
struct layer
{
    std::string name;
    raster image;  // Image: straight-alpha RGBA source pixels, immutable once added
    bool visible = true;
    layer_type type = layer_type::image;
    uint32_t id = 0;            // unique in its project; 0 until the project assigns one
    uint32_t derived_from = 0;  // the layer this one was made from (non-destructive operations), 0 for none
    image_transform transform;
    // The version thread after the original (empty until AI results arrive),
    // and which version the item uses: 0 is `image`.
    std::vector<item_version> versions;
    uint32_t version = 0;
    std::string notes;  // Notes for AI about this item
};
// Earlier code called Image layers "sources"; the name remains for them.
using source_image = layer;
struct cut_layer
{
    std::string name;
    int width = 0, height = 0;
    // 1 is retained cardstock, 0 is removed. Every layer uses the same canvas.
    std::vector<uint8_t> solid;
    bool visible = true;
};
enum class placement_mode
{
    fit,
    fill,
    touch_edges
};
enum touch_edge : unsigned
{
    touch_bottom = 1,
    touch_top = 2,
    touch_left = 4,
    touch_right = 8
};
struct transform_options
{
    double width_mm = 200, height_mm = 200, border_mm = 10, bridge_mm = 2;
    int layers = 7, smoothing = 2;
    bool connect = true;
    // border_mm remains the linked-width field and OFFCUT01 compatibility value.
    bool uniform_border = true;
    double border_top_mm = 10, border_bottom_mm = 10, border_left_mm = 10, border_right_mm = 10;
    placement_mode placement = placement_mode::fit;
    unsigned touch_edges = touch_bottom;
    int size_preset = 9;  // Custom; dimension edits deliberately keep it Custom.
    bool landscape = false;
    double layer_gap_mm = 5;
};
struct frame_borders
{
    double top, bottom, left, right;
};
frame_borders borders(const transform_options&);
struct frame_preset
{
    std::string_view name;
    double width_mm, height_mm;
};
std::span<const frame_preset> frame_sizes();
void set_frame_size(transform_options&, int preset, bool landscape);
enum class view_mode
{
    original,
    assembled,
    layer
};
// The stages of the stage switch, in the order the work goes. Each is a
// view of the same document; nothing is locked.
enum class work_step
{
    arrange,
    colours,
    sheets,
};
class history;
struct project
{
    std::string name = "Untitled";
    std::filesystem::path path;
    // Image (and later other) layers, bottom of the stack first; the layers
    // panel lists them front first. Sheets are the generated `cuts`.
    std::vector<layer> layers;
    std::vector<cut_layer> cuts;
    transform_options options;
    // Your instructions for the next request (the Send dialog's last), and
    // the ten most recent, newest first: the Send dialog's combo.
    std::string instructions;
    std::vector<std::string> recent_instructions;
    std::string ai_style;  // the document's AI Style, sent with every request
    // Auto with AI (docs/ux-design.md section 8): Rounds and Until from
    // Options, and the consent given with "Don't ask again for this project
    // and these providers" (the providers' ids; empty: ask).
    int auto_rounds = 3;
    int auto_until = 0;  // auto_until: 0 until good, 1 ask after each round, 2 plan only
    std::string auto_consent;
    view_mode view = view_mode::original;
    int selected_layer = -1, selected_cut = 0;  // -1: nothing selected
    work_step step = work_step::arrange;
    float zoom = 1, pan_x = 0, pan_y = 0;
    bool dirty = false, busy = false, stale = false;
    std::string activity;
    // Runtime job state only: never written to a project file.
    bool ai_running = false;
    std::shared_ptr<std::atomic_bool> cancel;
    // Undo history, shared by the copies a background job works on; null
    // until the window starts one (history.h).
    std::shared_ptr<offcut::history> undo;
    // Gives each layer without an id the next free one.
    void assign_layer_ids();
    int find_layer(uint32_t id) const;
};
std::string utf8(const std::filesystem::path&);
std::filesystem::path from_utf8(const std::string&);
void validate(const transform_options&);
void save_project(const project&, const std::filesystem::path&);
project load_project(const std::filesystem::path&);
}  // namespace offcut
