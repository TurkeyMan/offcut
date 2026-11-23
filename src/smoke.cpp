#include "smoke.h"
#include <fstream>
#include <stdexcept>
#include "export.h"
#include "image_io.h"
#include "transform.h"
#include "window.h"
namespace offcut
{
namespace
{
void require(bool condition, const char* text)
{
    if (!condition)
        throw std::runtime_error(text);
}
}  // namespace
rap::subscription smoke_test(application& app, const std::filesystem::path& image, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    auto p = std::make_shared<project>();
    p->name = "Native smoke test";
    p->zoom = 1.2f;
    p->pan_x = 3;
    auto& original = app.open(p);
    const auto id = original.take("1");
    require(id && id->document == p, "Initial tab state missing");
    original.insert(*id);
    original.insert({app.next_id(), std::make_shared<project>()});
    app.task(p, "Importing and generating…",
             [image](project& result)
             {
                 result.sources = {read_image(image)};
                 result.cuts = simplify(result.sources, result.options);
                 result.view = view_mode::assembled;
                 result.activity = "Generated smoke-test layers.";
             });
    auto* detached = app.detach(original, id->id);
    require(detached != nullptr && original.count() == 1, "Detach failed");
    auto* destination = app.find(detached);
    require(destination && destination->count() == 1, "Detached project window missing");
    auto stage = std::make_shared<int>(0);
    return app.ui.timer(300, true,
                        [&app, &original, p, id = *id, output, detached, stage]
                        {
                            try
                            {
                                if (p->busy)
                                    return;
                                if (*stage == 0)
                                {
                                    require(p->sources.size() == 1 && p->cuts.size() == 7, "Image job failed or result was lost during detach");
                                    require(p->zoom == 1.2f && p->pan_x == 3, "View state lost during generation");
                                    require(app.receive(original, {.id = id.id, .index = 0, .source = detached}), "Cross-window tab receive failed");
                                    require(original.count() == 2, "Transferred tab missing");
                                    auto moved = original.take(id.id);
                                    require(moved && moved->document == p, "Transfer changed document identity");
                                    original.insert(*moved);
                                    save_project(*p, output / "smoke.offcut");
                                    const auto loaded = load_project(output / "smoke.offcut");
                                    require(loaded.sources.front().image.rgba == p->sources.front().image.rgba, "Embedded source changed on reopen");
                                    const auto layers = export_layers(loaded, output);
                                    auto first = read_image(layers / "01.png");
                                    require(first.image.width == loaded.cuts.front().width && first.image.height == loaded.cuts.front().height,
                                            "PNG export/decode roundtrip failed");
                                    const auto imported = masks_from_images({first}, loaded.options);
                                    require(disconnected_components(imported.front()) == 0, "Returned mask has detached cardstock");
                                    export_ai_request(loaded, output);
                // A test runner may start us hidden. Show again after the first
                // ShowWindow call consumed the launcher's hidden-window flag.
                original.native().show();
                *stage = 1;
                                }
                                else
                                {
                                    require(app.ui.save_snapshot(original.native(), utf8(output / "workspace.png")), "Native snapshot failed");
                const auto snapshot = read_image(output / "workspace.png");
                require(std::ranges::any_of(snapshot.image.rgba, [](uint8_t value) { return value > 0 && value < 255; }),
                        "Native snapshot was blank");
                                    std::ofstream(output / "result.txt")
                                        << "PASS: image decoding, queued transform, detach during job, cross-window receive, preserved document identity/view, "
                                           "portable save/reopen, numbered SVG/PNG exports, PNG decoding, mask reimport and AI request export.\n";
                                    app.ui.quit();
                                }
                            }
                            catch (const std::exception& e)
                            {
                                std::ofstream(output / "result.txt") << "FAIL: " << e.what() << '\n';
                                app.ui.quit(1);
                            }
                        });
}
}  // namespace offcut
