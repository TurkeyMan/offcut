#pragma once
#include "director.h"
namespace offcut
{
// Pure validation/compilation used by both the network calls and unit tests.
ai_plan validate_director_plan(std::string_view json, int maximum_sheets);
std::string validate_director_verdict(std::string_view json);
std::string director_plan_schema();
std::string director_verdict_schema();
std::string build_director_prompt(bool critique, const director_input&);
std::map<std::string, std::string> director_prompt_values(const director_input&);
std::vector<ai_image> director_image_order(bool critique, ai_image primary, std::vector<ai_image> references, std::optional<ai_image> overlay = {});
}  // namespace offcut
