#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "project.h"
// Prompt assembly (docs/ux-design.md section 8). Every prompt is built from
// the files in data/prompts, so wording is tuned without rebuilding: the
// shadowbox brief (read whole by the director, phase 2), one template per
// touchpoint, the illustrator's three-line rules and output spec, and the
// review chips' follow-ups.
// Offcut fills in {placeholders} from the project; a line holding a
// placeholder with no value is left out unless it is written {name?}. A copy
// of each file is compiled in, so a missing file is never fatal.
namespace offcut
{
enum class ai_task
{
    reimagine,
    refine,
    try_again,  // resends a version's own prompt; no file of its own
    fix_support,
    describe_scene,
    describe_element,
};
std::string_view ai_task_name(ai_task);  // "Reimagine", "Refine", …
// The touchpoint's file: "reimagine.txt"; refine's for Try again.
std::string_view ai_task_file(ai_task);
struct prompt_library
{
    std::map<std::string, std::string> files;  // by name, comments removed
    std::filesystem::path folder;              // where they were read; empty: all built in
    std::vector<std::string> fallbacks;        // files that were missing or unreadable there
    const std::string& text(const std::string& name) const;
    // "brief.txt@1a2b3c4d": the name and a hash of the text sent.
    std::string version(const std::string& name) const;
};
const std::vector<std::string>& prompt_file_names();
std::string_view builtin_prompt_text(std::string_view name);  // generated from data/prompts
std::string strip_prompt_comments(std::string_view);
// Reads each file from `folder` (none: all built in); a missing or
// unreadable one falls back to its compiled-in copy.
prompt_library load_prompt_library(const std::filesystem::path& folder);
// Where the prompt files are: OFFCUT_PROMPTS_DIR, else the source tree's
// data/prompts in a development build, else data/prompts beside the exe.
// Empty when none exists.
std::filesystem::path find_prompt_folder();
const prompt_library& prompts();  // loaded once, from find_prompt_folder()

std::string fill_prompt(std::string_view text, const std::map<std::string, std::string>& values);

// What a request says beyond Offcut's own task: the three levels of user
// text, and what Offcut adds for the touchpoint.
struct prompt_words
{
    ai_task task = ai_task::reimagine;
    std::string style;         // Shadowbox ▸ AI style: every request in the project
    std::string notes;         // the items' Notes for AI
    std::string instructions;  // Your instructions: this request only
    std::string done;          // what earlier rounds asked for (refine)
    std::string issues;        // validation findings (refine, fix)
    std::string placement, references, description, area, background;
    std::vector<std::string> planes;  // depth planes near to far; empty: default_planes(sheets)
    std::vector<std::array<uint8_t, 3>> palette;  // one colour per plane, front to back; empty: default_palette(sheets)
    int width = 0, height = 0;  // the image size asked for; 0: the frame's proportions
};
// The director's plan (phase 2; docs/ux-design.md section 8, "The plan"):
// kept as its JSON, with what compiling an image prompt reads from it.
struct ai_plan
{
    std::string json;  // opaque until the director lands
    std::string summary;
    std::vector<std::string> planes;  // what lives on each depth plane, near to far
    std::vector<std::string> scene_directions, avoid;
    std::vector<std::array<uint8_t, 3>> palette;  // front to back; empty: Offcut finds the tones
};
// "Depth is the picture": the template route's depth planes for a sheet
// count, near to far (frame and foreground, subject, scenery, sky), and how
// a plane list is written into a prompt.
std::vector<std::string> default_planes(int sheets);
std::string list_planes(const std::vector<std::string>&, const std::vector<std::array<uint8_t, 3>>& palette = {});
// The template route's colours, one per plane, front to back: distinct in
// hue and lightness so each separates cleanly, darkest at the front (it's
// also the border) and a pale sky last. The import snaps to them and maps
// each straight to its sheet, so depth never depends on guessing tones.
std::vector<std::array<uint8_t, 3>> default_palette(int sheets);
std::string palette_text(const std::vector<std::array<uint8_t, 3>>&);  // "#1e4d2b #c8742a …"
std::vector<std::array<uint8_t, 3>> parse_palette(std::string_view);      // the reverse; empty when malformed
std::map<std::string, std::string> prompt_values(const transform_options&, const prompt_words&);
struct assembled_prompt
{
    std::string text;
    std::string files;  // the versions of the files used, space separated
    std::string palette;  // the colours given to the planes (palette_text), for the version record
};
// The image prompt, from a plan or (none) from the touchpoint's template:
// concrete depth and scene directions first (a layered paper shadowbox seen
// straight on, its planes near to far; the plan's, else the template's with
// project values), then the user's words (template route only: "Style:",
// "About this subject:", "Instructions for this request:", each only when
// written; a director folds them into its directions), then the three-line
// output spec last.
assembled_prompt assemble_prompt(const prompt_library&, const transform_options&, const prompt_words&, const ai_plan* = nullptr);

// Review chips (review-chips.txt): id | menu | label | follow-up.
struct review_chip
{
    std::string id, menu, label, text;
    bool asks() const { return label.ends_with("…"); }  // the user names an element
};
std::vector<review_chip> parse_review_chips(std::string_view text);
struct chip_choice
{
    std::string id;  // review-chips.txt's id: what versions record
    std::string element;  // for a chip that asks
    bool operator==(const chip_choice&) const = default;
};
// The pending instruction for chosen chips, in the order chosen.
std::string chip_instruction(const std::vector<review_chip>&, const std::vector<chip_choice>&, const transform_options&);
// The answer in words, as recorded on the version: "support looks added on; use reeds".
std::string chip_review(const std::vector<review_chip>&, const std::vector<chip_choice>&);
// The chosen chips as recorded: "support.added-on support.use=reeds".
std::string chip_ids(const std::vector<chip_choice>&);
}  // namespace offcut
