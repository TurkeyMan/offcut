#include "settings.h"
#include <rapport/sys.h>
#include <algorithm>
#include <fstream>
#include "project.h"
namespace offcut
{
namespace
{
const std::vector<std::string> providers{"ChatGPT", "Claude", "Grok"};
std::filesystem::path config()
{
    return rap::sys::config_dir("offcut") / "ai-provider.txt";
}
struct dialog_state
{
    std::unique_ptr<rap::window> window;
    rap::drop_down* provider = nullptr;
};
}  // namespace
std::string preferred_provider()
{
    std::ifstream in(config());
    std::string p;
    std::getline(in, p);
    for (auto& v : providers)
        if (p == v)
            return p;
    return providers.front();
}
void show_settings(rap::ui& ui, rap::window* parent)
{
    namespace L = rap::layout;
    using namespace rap;
    auto state = std::make_shared<dialog_state>();
    std::weak_ptr<dialog_state> weak = state;
    int index = 0;
    for (int i = 0; i < int(providers.size()); ++i)
        if (providers[i] == preferred_provider())
            index = i;
    state->window = ui.create(
        L::dialog{
            "Settings — Offcut",
            {L::vbox{
                L::group{"Appearance", L::form{{"Colour scheme", L::drop_down{{"System", "Light", "Dark"}, 0}.on_changed(
                                                                     [&ui](int i) { ui.set_color_scheme(rap::color_scheme(i)); })}}},
                L::group{
                    "AI assistance",
                    L::vbox{
                        L::form{{"Preferred app", L::drop_down{providers, index}.bind(state->provider)}},
                        L::label{"Connection: manual handoff"},
                        L::label{"Export an AI request from the workspace. Open your chosen app, attach the images and paste the prompt. Import its separate "
                                 "layer images back into Offcut."}
                            .wrap(),
                        L::button{"Open selected AI app"}.on_click(
                            [weak]
                            {
                                if (auto s = weak.lock())
                                {
                                    const char* urls[]{"https://chatgpt.com/", "https://claude.ai/", "https://grok.com/"};
                                    rap::sys::open_url(urls[std::clamp(s->provider->current(), 0, 2)]);
                                }
                            }),
                        L::label{"Direct provider connections are not implemented yet. Offcut does not request passwords or upload images in this version."}
                            .wrap(),
                    }},
                L::spacer{},
                L::buttons{L::ok, L::cancel},
            }}}
            .size(650_px, 440_px),
        parent);
    state->window->on_finished(
        [state, &ui](rap::dialog_role role) mutable
        {
            auto keep = std::move(state);
            if (role == rap::dialog_role::ok)
            {
                std::error_code ec;
                std::filesystem::create_directories(config().parent_path(), ec);
                std::ofstream out(config());
                out << providers[std::clamp(keep->provider->current(), 0, 2)];
                if (!out)
                    ui.message(nullptr, {.icon = rap::message_spec::icon::error, .title = "Settings", .text = "Could not save the preferred AI app."});
            }
            ui.post([keep] {});
        });
    state->window->show();
}
}  // namespace offcut
