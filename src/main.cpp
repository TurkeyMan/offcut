#include <cstdio>
#include <cstdlib>
#include "app.h"
#include "director_cli.h"
#include "settings.h"
#include "smoke.h"
#include "window.h"
int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "offcut: %s\n", ui.error().c_str());
        return 1;
    }
    if (const int result = offcut::director_command(argc, argv); result >= 0)
        return result;
    offcut::application app(**ui);
    offcut::apply_saved_appearance(**ui);
    // offcut --ai-live <entry|default> <image> <out> [instructions]: one real
    // provider request (uploads the image; billed to that entry's key).
    if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--ai-live")
    {
        try
        {
            auto run = offcut::ai_live_run(app, argv[2], offcut::from_utf8(argv[3]), offcut::from_utf8(argv[4]), argc == 6 ? argv[5] : "");
            return (*ui)->run();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--ai-smoke-test")
    {
        try
        {
            auto smoke = offcut::ai_smoke_test(app, argv[2], offcut::from_utf8(argv[3]), offcut::from_utf8(argv[4]));
            return (*ui)->run();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    // offcut --scene arrange|sheets out.png [images...]: a screenshot of that state.
    if (argc >= 4 && std::string_view(argv[1]) == "--scene")
    {
        try
        {
            std::vector<std::filesystem::path> images;
            for (int i = 4; i < argc; ++i)
                images.push_back(offcut::from_utf8(argv[i]));
            auto scene = offcut::scene_snapshot(app, argv[2], std::move(images), offcut::from_utf8(argv[3]));
            return (*ui)->run();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--smoke-test")
    {
        try
        {
            auto smoke = offcut::smoke_test(app, offcut::from_utf8(argv[2]), offcut::from_utf8(argv[3]));
            return (*ui)->run();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    auto& window = app.open();
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i)
        paths.emplace_back(argv[i]);
    if (!paths.empty())
        window.load_paths(std::move(paths), true);
    rap::subscription snapshot;
    // OFFCUT_SETTINGS_PAGE=general|ai-providers opens Settings at that
    // page; with OFFCUT_SNAPSHOT, that page is captured instead of the main
    // window, after its checks have had a few seconds.
    const std::string_view page = std::getenv("OFFCUT_SETTINGS_PAGE") ? std::getenv("OFFCUT_SETTINGS_PAGE") : "";
    const auto settings_page = page == "ai-providers" ? offcut::settings_page::ai_providers : offcut::settings_page::general;
    rap::window* settings = nullptr;
    int ticks = 0;
    if (!page.empty() && !std::getenv("OFFCUT_SNAPSHOT"))
        (*ui)->post([&] { offcut::show_settings(**ui, &window.native(), settings_page); });
    if (const char* path = std::getenv("OFFCUT_SNAPSHOT"))
        snapshot = (*ui)->timer(500, true,
                                [&, path = std::string(path)]
                                {
                                    if (window.busy())
                                        return;
                                    window.native().show();
                                    if (!page.empty() && !settings)
                                    {
                                        settings = offcut::show_settings(**ui, &window.native(), settings_page);
                                        return;
                                    }
                                    if (settings && ++ticks < 6)
                                        return;
                                    (*ui)->save_snapshot(settings ? *settings : window.native(), path);
                                    (*ui)->quit();
                                });
    return (*ui)->run();
}
