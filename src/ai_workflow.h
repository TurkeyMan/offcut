#pragma once
#include <filesystem>
#include "ai_client.h"
#include "ai_provider.h"
#include "project.h"
#include "prompts.h"
#include "transform.h"
// The AI loop's requests (docs/ux-design.md section 8): what is uploaded,
// the prompt assembled for it, the request itself, and the result as a new
// version of an item. The stateless fallback thread: each refinement attaches
// the version it refines and the original, and summarises earlier rounds.
// (The Responses API thread and Mark come later; see `thread` below.)
namespace offcut
{
struct prepared_ai_upload
{
    ai_task task = ai_task::reimagine;
    transform_options options;
    prompt_words words;          // what the prompt is assembled from; the Send dialog edits words.instructions
    std::string prompt;          // exactly what is sent
    std::string prompt_files;    // the prompt files' versions, for the version record
    std::string palette;         // the colours the prompt gave the layers (palette_text)
    std::string resend;          // Try again: the version's own prompt, sent as it was
    uint32_t item = 0;           // the item (layer id) a refinement adds to; 0: Reimagine makes an AI illustration
    uint32_t parent = 0;         // the version refined
    std::vector<ai_image> images;  // exactly what is uploaded: the input first
    raster composition;          // Reimagine: version 0 of the new item
    int width = 0, height = 0;   // the input image's size
    double frame_width_mm = 0, frame_height_mm = 0;
    // Set once a provider is chosen (choose_ai_account).
    ai_endpoint endpoint;
    std::string account_id, account_name;
    // The run (one consent) and this request's round in it.
    std::string run;
    uint32_t round = 1;
    // Seams for phase 2: the director's plan (JSON; assembly then compiles
    // from it), and the preferred thread (Responses API,
    // previous_response_id, markup). Unused by the stateless template route.
    std::string plan;
    std::string thread;
    // Auto's refinement: the verdict's marks, in words, added to its instruction.
    std::string refine_marks;
};
// Reimagine as shadowbox: the composition and the visible items' pictures,
// with the project's Style, the items' Notes and its last instructions.
prepared_ai_upload prepare_ai_upload(const project&);
// A follow-up on version `version` of the item `item` (a layer id): Refine and
// Fix with AI send that version with the original as a second reference and
// earlier rounds summarised, plus the version's validation findings; Try
// again resends the version's own prompt with the images it was made from.
prepared_ai_upload prepare_refinement(const project&, uint32_t item, uint32_t version, ai_task, const std::string& instructions);
// Assembles the prompt again (Your instructions changed, or the provider).
// With a plan, the prompt is compiled from it (its planes, palette,
// directions and avoid); a refinement's instruction comes first.
void assemble(prepared_ai_upload&);
void set_ai_instructions(prepared_ai_upload&, const std::string&);
// The provider entry: endpoint, output size and, for refinements where the
// model takes it, input_fidelity high.
void choose_ai_account(prepared_ai_upload&, const ai_account&);
struct ai_result
{
    raster image;
    double seconds = 0;
    std::string usage = "null";
    std::filesystem::path folder;  // the request and response
};
// Called only after UI consent (or the explicitly loopback-only smoke test).
// Writes the request (prompt and images as uploaded) to a new folder under
// `parent`, sends it, and saves and decodes the reply. No key is written.
ai_result request_ai_image(const prepared_ai_upload&, const std::string& key, const std::filesystem::path& parent, const std::atomic_bool* cancel = nullptr);
// Adds the result as the next version of its item (Reimagine: a new "AI
// illustration" item over the hidden sources), makes it current, pending
// review, rebuilds the sheets and records the validation findings on it.
// Returns the item's index.
int apply_ai_result(project&, const prepared_ai_upload&, const ai_result&);
// Both, as one job (the --ai-live run and the mock smoke test).
std::filesystem::path run_ai_roundtrip(project&, const prepared_ai_upload&, const std::string& key, const std::filesystem::path& parent,
                                       const std::atomic_bool* cancel = nullptr);
// Rebuilds the sheets for the items' current versions and returns the
// findings (the pipeline following a version change).
validation rebuild_sheets(project&);
}  // namespace offcut
