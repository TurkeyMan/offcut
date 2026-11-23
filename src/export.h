#pragma once
#include "project.h"
namespace offcut
{
std::string svg(const cut_layer&, const transform_options&);
void write_png(const raster&, const std::filesystem::path&);
// Each export creates a new subfolder, so earlier exports are never overwritten.
std::filesystem::path export_layers(const project&, const std::filesystem::path& parent);
std::filesystem::path export_ai_request(const project&, const std::filesystem::path& parent);
}  // namespace offcut
