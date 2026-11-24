#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include "ai_provider.h"
#include "ai_workflow.h"
#include "director.h"
#include "local_ops.h"
// The Auto loop (docs/ux-design.md section 8, "The loop"): Measure → Plan →
// Compile → Draw → Check → Critique → Fix, repeating until a stop rule, then
// the best-scoring round is presented. The director and the illustrator are
// function objects, so the control flow is tested without a network.
namespace offcut
{
enum class auto_until
{
    good,            // Auto until good
    ask_each_round,  // stop after each round with Continue / Stop
    plan_only,       // stop after the plan and show it
};
std::string_view auto_until_name(auto_until);
// What a run does.
enum class auto_mode
{
    full,     // plan, draw, check, critique, fix
    arrange,  // Arrange for me: the plan's local part on the picture as it is, no drawing
};
struct auto_settings
{
    int rounds = 3;  // drawings, 1–5
    auto_until until = auto_until::good;
    std::string instructions;  // Your instructions: this run only
};
enum class auto_stop
{
    none,            // still running (or not started)
    passed,          // the director passed it and no errors remain
    rounds,          // max rounds reached
    no_improvement,  // the score did not improve for two rounds
    cancelled,
    failed,     // a provider failed after something was made; the best so far is kept
    plan_only,  // Plan only: the plan is ready
    asking,     // Ask after each round: waiting for Continue or Stop
    arranged,   // Arrange for me finished
    no_drawing,  // the verdict needs a drawing and no illustrator is configured
};
std::string_view auto_stop_text(auto_stop);
struct auto_progress
{
    std::string step;  // "Planning…", "Drawing 1 of 3…", "Checking…", "Reviewing…", "Fixing locally…"
    int round = 0, rounds = 0;
    std::string plan;  // the plan so far (Show plan during the run)
    // A copy of the project when a version has just landed, for the
    // Versions strip; null otherwise.
    std::shared_ptr<const project> snapshot;
};
// Seams for the roles. A missing director makes run_auto refuse (the
// caller falls back); a missing illustrator limits it to arranging.
struct auto_roles
{
    using director_call = std::function<director_reply(const director_input&, const std::filesystem::path& parent, const std::atomic_bool* cancel)>;
    director_call plan, critique;
    // The illustrator: the upload is fully prepared (account chosen).
    std::function<ai_result(const prepared_ai_upload&, const std::filesystem::path& parent, const std::atomic_bool* cancel)> draw;
    std::optional<ai_account> illustrator;  // what choose_ai_account is given
    std::function<local_ops_result(const local_ops_input&)> local_ops;  // empty: apply_local_ops
    std::function<std::string(const std::string& plan, const std::string& patch)> plan_patch;  // empty: apply_plan_patch
    std::function<double(const std::string& measurements)> score;  // empty: auto_round_score
    std::function<void(const auto_progress&)> progress;
};
// One run's state: kept between rounds so Ask after each round (Continue) and
// a review sent from the review bar resume where it stopped.
struct auto_run
{
    enum class step
    {
        measure_plan,  // Measure and Plan
        arrange,       // route "local": the plan's local part, no drawing
        draw,          // compile from the plan and draw from the composition
        refine,        // draw again from the last version with an instruction
        critique,      // judge the last checked version
        done,
    };
    std::string id;  // the run id the versions carry
    auto_settings settings;
    auto_mode mode = auto_mode::full;
    std::filesystem::path folder;  // requests and replies of the run
    step next = step::measure_plan;
    std::string plan;  // the plan JSON, as validated
    ai_plan compiled;
    uint32_t item = 0;  // the AI illustration item's layer id; 0 until the first result
    uint32_t last = 0;  // the latest checked version
    int drawings = 0;   // drawings made in this run (rounds)
    int round_fixes = 0;  // local fixes in the current round
    std::string instruction, marks;  // the next refinement: the verdict's instruction, its marks in words
    std::string review;              // the user's review for the next critique (chips, Write a review…)
    std::string left_for_you;  // ops Offcut cannot do by itself (extract, extend_to_frame), in words
    std::string sheets_note;         // "Sheets 7 → 4: …" or the proposal
    double round_start_best = std::numeric_limits<double>::infinity();
    int stale_rounds = 0;
    auto_stop stop = auto_stop::none;
    std::string message;  // why it failed, or what it says at the end
    uint32_t best = 0;    // the version presented
    // Director usage not yet given to a version (the plan before the first drawing).
    item_version::role_usage pending_director;
};
// Runs the loop on `p` from run.next until a stop rule (run.stop set). The
// project holds every round's versions in the item's thread; the best is
// current and pending review, unless asking (then the round's latest).
// Throws only when nothing was made yet; later failures stop with
// auto_stop::failed and keep what was made.
void run_auto(project& p, auto_run& run, const auto_roles& roles, const std::atomic_bool* cancel = nullptr);
// After Ask after each round: Stop presents the best round.
void finish_auto(project& p, auto_run& run);
// A run rebuilt from a version, for a review sent from the review bar: one
// more critique and, if needed, one more drawing.
auto_run resume_auto_from(const project& p, uint32_t item, uint32_t version, const std::string& review);

// Pure parts, for tests and the UI.
// The plan's sheet count when it is lower than the project's and the plan
// says why (in its summary or risks): the reason. Empty: not confident.
std::string sheet_reduction_reason(const std::string& plan_json, int project_sheets);
// Errors in a version's measurements: unsupported card (floating pieces).
int auto_errors(const std::string& measurements_json);
// The best version of a run in an item: lowest score; ties go to a passed
// verdict, then to the later round.
uint32_t best_auto_version(const layer&, const std::string& run);
// The verdict's marks (m ids) in words from the measurements: "the floating
// piece on sheet 2 at 40, 80 mm (38 mm²)".
std::string describe_marks(const std::string& measurements_json, const std::vector<std::string>& marks);
// The plan's route ("generate", "planes", "edit", "local"); empty if unreadable.
std::string plan_route(const std::string& plan_json);
// Ceiling for the consent dialog: "Up to 3 drawings, 7 reasoning calls, about 2–6 min."
std::string auto_ceiling(const auto_settings&, bool can_draw);
// Plan JSON → the plan dialog's text for Copy.
std::string plan_text(const std::string& plan_json);
// The overlay with numbered marks (m ids) that the director sees.
raster auto_overlay(const std::vector<cut_layer>& cuts, const validation&);
// The picture the sheets show front to back, each in its palette colour: the
// version as the import snapped it, at the frame's working size (what
// apply_local_ops takes).
raster snapped_picture(const std::vector<cut_layer>& cuts, const std::vector<std::array<uint8_t, 3>>& palette);
// What Auto does with the providers configured: the loop (a director and an
// illustrator), plan and arrange (a director only), one drawing from a
// template (an illustrator only, today's Reimagine), or the local pipeline.
enum class auto_route
{
    local,
    template_draw,
    plan_and_arrange,
    loop,
};
auto_route auto_route_for(const ai_accounts&);
// Which entries take each role.
const ai_account* drawing_account(const ai_accounts&);
const ai_account* planning_account(const ai_accounts&);
director_endpoint director_endpoint_for(const ai_account&);
}  // namespace offcut
