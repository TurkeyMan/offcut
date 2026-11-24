#pragma once
#include "app.h"
namespace offcut
{
rap::subscription smoke_test(application&, const std::filesystem::path& image, const std::filesystem::path& output);
rap::subscription ai_smoke_test(application&, const std::string& endpoint, const std::filesystem::path& image, const std::filesystem::path& output);
// One real request through a configured provider entry ("default" or an id),
// reading its key from Credential Manager inside Offcut. Writes the request,
// response, sheets, export and a workspace snapshot to `output`.
rap::subscription ai_live_run(application&, const std::string& account, const std::filesystem::path& image, const std::filesystem::path& output,
                              const std::string& instructions);
// Puts a window into a state for a screenshot (arrange: two images arranged
// with the pointer; sheets: the stack with one sheet unticked and another
// highlighted), saves it to `out` and quits.
rap::subscription scene_snapshot(application&, const std::string& scene, std::vector<std::filesystem::path> images, const std::filesystem::path& out);
}  // namespace offcut
