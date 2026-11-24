#include "guidance.h"
#include <algorithm>
#include <format>
#include "transform.h"
namespace offcut
{
guidance_text guidance_for(const project& p, const std::vector<size_t>& floating)
{
    if (p.busy)
        return {p.activity, guidance_level::info, guidance_action::none};
    if (p.layers.empty() && p.cuts.empty())
        return {"Add an image to start: drop one here, or choose Add images (Ctrl+I).", guidance_level::info, guidance_action::add_images};
    if (p.cuts.empty())
    {
        if (std::ranges::none_of(p.layers, [](const layer& l) { return l.visible; }))
            return {"Every image is hidden: tick one in Artwork to use it.", guidance_level::problem, guidance_action::none};
        return {"Arrange your images, then Auto makes the sheets (Ctrl+G).", guidance_level::info, guidance_action::auto_};
    }
    if (p.stale)
        return {"Design changed: make the sheets again before exporting.", guidance_level::problem, guidance_action::auto_};
    // A sheet left with only its frame had nothing reaching it.
    for (size_t i = 0; i + 1 < p.cuts.size(); ++i)
        if (frame_only(p.cuts[i], p.options))
            return {std::format("Sheet {} has nothing reaching the frame: Touch bottom on the subject, then Auto again.", i + 1), guidance_level::problem,
                    guidance_action::none};
    for (size_t i = 0; i < floating.size() && i < p.cuts.size(); ++i)
        if (floating[i])
            return {std::format("Sheet {} has {} floating {}: {} no edge of the sheet.", i + 1, floating[i], floating[i] == 1 ? "piece" : "pieces",
                                floating[i] == 1 ? "it reaches" : "they reach"),
                    guidance_level::problem, p.step == work_step::sheets ? guidance_action::none : guidance_action::show_sheets};
    return {"Ready to export.", guidance_level::done, guidance_action::export_};
}
}  // namespace offcut
