#pragma once
#include "project.h"
namespace offcut
{
raster flatten(const std::vector<source_image>&, const transform_options&);
std::vector<cut_layer> simplify(const std::vector<source_image>&, const transform_options&);
std::vector<cut_layer> masks_from_images(const std::vector<source_image>&, const transform_options&);
raster render_cuts(const std::vector<cut_layer>&, int selected = -1);
size_t disconnected_components(const cut_layer&);
std::string ai_prompt(const project&);
}  // namespace offcut
