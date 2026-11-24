#pragma once
#include <string>
#include <vector>
#include "project.h"
// The guidance bar's sentence (docs/ux-design.md section 10).
namespace offcut
{
enum class guidance_level
{
    info,
    problem,
    done,
};
enum class guidance_action
{
    none,
    add_images,
    auto_,
    show_sheets,
    export_,
};
struct guidance_text
{
    std::string text;
    guidance_level level = guidance_level::info;
    guidance_action action = guidance_action::none;
};
// What is next or wrong, most important first: a running job, a problem with
// the sheets, the next missing step, else ready to export. `floating` holds
// each sheet's floating pieces.
guidance_text guidance_for(const project&, const std::vector<size_t>& floating);
}  // namespace offcut
