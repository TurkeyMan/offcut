#pragma once
// The AI director (docs/ux-design.md section 8): a vision chat model that
// plans and critiques, never draws. Interface shared by the mechanical
// implementation (director.cpp) and the UI. Keep these signatures stable.
#include <atomic>
#include <string>
#include <vector>
#include "ai_client.h"
#include "prompts.h"
namespace offcut
{
// Which entry, model and route a director call uses.
struct director_endpoint
{
    std::string provider_id;  // "chatgpt" (Sign in with ChatGPT), "openai", "anthropic", …
    std::string base_url;
    std::string model;       // the entry's reasoning model
    std::string credential;  // the entry's credential target (API key or ChatGPT session)
};
struct director_input
{
    // PNGs, at most 1024 px. Plan: composition, references, marked overlay.
    // Critique: drawing as returned, marked overlay, original/references.
    std::vector<ai_image> images;
    std::string measurements;                // Offcut's measurements JSON (validation)
    std::string findings;                    // the same in words
    std::string style, notes, instructions;  // the user's three levels
    int sheets = 4;
    double frame_width_mm = 0, frame_height_mm = 0, min_width_mm = 0;
    std::string plan;  // critique: the plan the drawing was made from (JSON); plan: empty
    // Appended for source compatibility. Callers with a project pass borders(options).
    frame_borders border_widths_mm{};
    double layer_gap_mm = 5;
};
struct director_reply
{
    std::string json;         // validated against the plan or verdict schema
    std::string explanation;  // one sentence for the review bar
    ai_plan plan;             // plan calls: what compiling the image prompt reads
    double seconds = 0;
    unsigned long long input_tokens = 0, output_tokens = 0;
    std::string request;  // folder holding the request (prompt, images) and reply
};
// Plan: analyse the source and measurements, return the plan (schema in
// docs/ux-design.md, "The plan"). Critique: judge a drawing against its plan
// and the findings, return the verdict (pass | fix_local | refine | replan).
// Both validate the reply locally, allow one repair turn, and throw a
// user-readable error otherwise. Requests and replies are saved under
// `parent` (never tokens or keys).
director_reply director_plan(const director_endpoint&, const director_input&, const std::string& parent, const std::atomic_bool* cancel);
director_reply director_critique(const director_endpoint&, const director_input&, const std::string& parent, const std::atomic_bool* cancel);
}  // namespace offcut
