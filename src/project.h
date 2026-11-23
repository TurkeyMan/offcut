#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace offcut
{
struct raster
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool valid() const;
};
struct source_image
{
    std::string name;
    raster image;
    bool visible = true;
};
struct cut_layer
{
    std::string name;
    int width = 0, height = 0;
    // 1 is retained cardstock, 0 is removed. Every layer uses the same canvas.
    std::vector<uint8_t> solid;
    bool visible = true;
};
struct transform_options
{
    double width_mm = 200, height_mm = 200, border_mm = 10, bridge_mm = 2;
    int layers = 7, smoothing = 2;
    bool connect = true;
};
enum class view_mode
{
    original,
    assembled,
    layer
};
struct project
{
    std::string name = "Untitled";
    std::filesystem::path path;
    std::vector<source_image> sources;
    std::vector<cut_layer> cuts;
    transform_options options;
    std::string instructions;
    view_mode view = view_mode::original;
    int selected_source = 0, selected_cut = 0;
    float zoom = 1, pan_x = 0, pan_y = 0;
    bool dirty = false, busy = false, stale = false;
    std::string activity;
};
std::string utf8(const std::filesystem::path&);
std::filesystem::path from_utf8(const std::string&);
void validate(const transform_options&);
void save_project(const project&, const std::filesystem::path&);
project load_project(const std::filesystem::path&);
}  // namespace offcut
