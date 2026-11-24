#include "settings.h"
#include <rapport/sys.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include "ai_providers_page.h"
#include "project.h"
namespace offcut
{
namespace
{
constexpr rap::color_scheme appearance_choices[] = {rap::color_scheme::system, rap::color_scheme::light, rap::color_scheme::dark};
constexpr const char* appearance_names[] = {"system", "light", "dark"};

rap::color_scheme saved_appearance()
{
    std::ifstream in(settings_directory() / "appearance.txt");
    std::string name;
    std::getline(in, name);
    for (int i = 0; i < 3; ++i)
        if (name == appearance_names[i])
            return appearance_choices[i];
    return rap::color_scheme::system;
}
void write_line(const std::filesystem::path& file, const std::string& line)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    out << line << '\n';
    out.close();
    if (!out)
        throw std::runtime_error("Could not save " + file.filename().string() + ".");
}

struct state
{
    rap::ui* ui = nullptr;
    std::unique_ptr<rap::window> window;
    rap::tabs* pages = nullptr;
    // Bound when their page is first shown; null until then, and an
    // unvisited page saves nothing.
    rap::drop_down* appearance = nullptr;
};
std::weak_ptr<state> open_settings;

rap::node general_page(std::shared_ptr<state> st)
{
    using namespace rap::layout;
    const auto current = int(std::ranges::find(appearance_choices, saved_appearance()) - std::begin(appearance_choices));
    return vbox{
        group{"Appearance", form{
                                {"Appearance", drop_down{{"Follow system", "Light", "Dark"}, current}
                                                   .bind(st->appearance)
                                                   .on_changed(
                                                       [weak = std::weak_ptr(st)](int i)
                                                       {
                                                           auto s = weak.lock();
                                                           if (s && s->ui && i >= 0 && i < 3)
                                                               s->ui->set_color_scheme(appearance_choices[i]);
                                                       })},
                            }
                                .align_group("settings")},
        spacer{},
    };
}

}  // namespace

std::filesystem::path settings_directory()
{
    if (const char* dir = std::getenv("OFFCUT_CONFIG_DIR"); dir && *dir)
        return from_utf8(dir);
    return rap::sys::config_dir("offcut");
}
void apply_saved_appearance(rap::ui& ui)
{
    if (const auto scheme = saved_appearance(); scheme != rap::color_scheme::system)
        ui.set_color_scheme(scheme);
}
ai_accounts configured_ai_accounts()
{
    auto c = read_ai_accounts(settings_directory(), [](const std::string& target) { return ai_key_saved(target); });
    // A key Offcut stored whose entry has gone from the list (an older build,
    // or another window's save) comes back as an entry: keys can't be shown
    // again by the provider, so they are never left orphaned. Local and
    // custom servers aren't recovered: only the list knew their URL.
    // OFFCUT_NO_KEY_RECOVERY=1 skips this, so a snapshot with a seeded
    // OFFCUT_CONFIG_DIR never adopts (or checks) the real saved keys.
    bool recovered = false;
    const char* no_recovery = std::getenv("OFFCUT_NO_KEY_RECOVERY");
    const bool recover = !(no_recovery && *no_recovery && std::string_view(no_recovery) != "0");
    for (const auto& target : recover ? saved_ai_key_targets("Offcut/AI/") : std::vector<std::string>{})
    {
        if (std::ranges::any_of(c.entries, [&](const ai_account& a) { return a.credential_target == target; }))
            continue;
        const auto id = target.substr(std::string_view("Offcut/AI/").size());
        const auto provider_id = id.substr(0, id.find('-'));
        const auto p = std::ranges::find(ai_providers(), provider_id, &ai_provider::id);
        if (p == ai_providers().end() || p->typed_url || id.empty() || c.find(id))
            continue;
        ai_account a;
        a.id = id;
        a.provider = p->id;
        a.name = new_ai_account_name(c, *p, p->name + " (recovered)");
        a.base_url = p->base_url;
        a.model = p->preferred_model;
        a.credential_target = target;
        c.entries.push_back(std::move(a));
        recovered = true;
    }
    if (recovered)
    {
        if (!c.find(c.default_id))
            c.default_id = c.entries.front().id;
        try
        {
            save_ai_accounts(c);
        }
        catch (const std::exception&)
        {
        }
    }
    return c;
}
void save_ai_accounts(const ai_accounts& accounts)
{
    write_ai_accounts(accounts, settings_directory());
}

rap::window* show_settings(rap::ui& ui, rap::window* parent, settings_page first)
{
    if (auto open = open_settings.lock(); open && open->window)
    {
        open->pages->set_current(int(first));
        open->window->show();
        return open->window.get();
    }
    using namespace rap::layout;
    using namespace rap::literals;
    auto st = std::make_shared<state>();
    st->ui = &ui;
    std::weak_ptr<state> weak = st;
    auto build = [](const std::weak_ptr<state>& w, rap::node (*page)(std::shared_ptr<state>))
    {
        auto s = w.lock();
        return s ? page(s) : rap::node{};
    };
    // Lazy pages don't size the dialog, so it has a size of its own.
    st->window = ui.create(dialog{"Settings — Offcut",
                                  {vbox{
                                      tabs{
                                          // Weak: the window that keeps these is the state's own.
                                          page{"General", lazy{[weak, build] { return build(weak, general_page); }}},
                                          page{"AI providers", lazy{[weak, &ui]
                                                                    {
                                                                        if (!weak.lock())
                                                                            return rap::node{};
                                                                        return ai_providers_page(ui,
                                                                                                 [weak]
                                                                                                 {
                                                                                                     auto s = weak.lock();
                                                                                                     return s ? s->window.get() : nullptr;
                                                                                                 });
                                                                    }}},
                                      }
                                          .sidebar()
                                          .bind(st->pages)
                                          .stretch(),
                                      buttons{ok, cancel},
                                  }}}
                               .size(900_px, 640_px),
                           parent);
    st->window->on_finished(
        [st, &ui](rap::dialog_role role) mutable
        {
            auto keep = std::move(st);
            open_settings.reset();
            // Appearance was shown live: kept on OK, back as saved otherwise.
            if (keep->appearance && role != rap::dialog_role::ok)
                ui.set_color_scheme(saved_appearance());
            if (role == rap::dialog_role::ok)
                try
                {
                    if (keep->appearance)
                        write_line(settings_directory() / "appearance.txt", appearance_names[std::clamp(keep->appearance->current(), 0, 2)]);
                }
                catch (const std::exception& e)
                {
                    ui.message(nullptr, {.icon = rap::message_spec::icon::error, .title = "Settings", .text = e.what()});
                }
            ui.post([keep] {});  // destroy outside the window's own callback
        });
    st->pages->set_current(int(first));
    open_settings = st;
    st->window->show();
    return st->window.get();
}
}  // namespace offcut
