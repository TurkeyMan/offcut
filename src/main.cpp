#include <cstdio>
#include <cstdlib>
#include "app.h"
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
    offcut::application app(**ui);
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
    if (const char* path = std::getenv("OFFCUT_SNAPSHOT"))
        snapshot = (*ui)->timer(1800, false,
                                [&, path = std::string(path)]
                                {
                                    (*ui)->save_snapshot(path);
                                    (*ui)->quit();
                                });
    return (*ui)->run();
}
