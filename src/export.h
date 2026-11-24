#pragma once
#include "project.h"
namespace offcut
{
std::string svg(const cut_layer&, const transform_options&);
void write_png(const raster&, const std::filesystem::path&);
std::vector<uint8_t> encode_png(const raster&);
// Each export creates a new subfolder, so earlier exports are never overwritten.
std::filesystem::path export_layers(const project&, const std::filesystem::path& parent);
std::filesystem::path export_ai_request(const project&, const std::filesystem::path& parent);
// A new offcut-ai-request-NN folder holding the prompt and the named files
// exactly as they are uploaded.
std::filesystem::path write_ai_request(const std::filesystem::path& parent, const std::string& prompt,
                                       const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);
}  // namespace offcut
