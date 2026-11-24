#include "ai_providers_page.h"
#include <rapport/sys.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include "ai_provider.h"
#include "chatgpt_auth.h"
#include "settings.h"
namespace offcut
{
namespace
{
// Sign in with ChatGPT (docs/ux-design.md section 8): an OAuth session in
// place of a key, which plans and reviews but can't draw. Looked up by id at
// run time, so the page works whatever the registry holds.
constexpr const char* chatgpt_id = "chatgpt";

const ai_provider* lookup_provider(const std::string& id)
{
    for (const auto& p : ai_providers())
        if (p.id == id)
            return &p;
    return nullptr;
}
// A browser sign-in in place of a key.
bool signs_in(const std::string& provider)
{
    const auto* p = lookup_provider(provider);
    return p && p->auth == ai_auth::oauth_session;
}
// Providers that plan and review only: no image model.
bool plan_only(const std::string& provider)
{
    const auto* p = lookup_provider(provider);
    return p && !p->can_draw;
}
bool draws(const ai_account& a) { return !plan_only(a.provider); }
bool plans(const ai_account& a) { return !a.reasoning_model.empty(); }
std::string can_text(const ai_account& a)
{
    if (!draws(a))
        return "Plan · review";
    return plans(a) ? "Draw · plan · review" : "Draw";
}

// Whether an entry's key (or sign-in) works, as asked when the page opens.
struct entry_status
{
    enum class check
    {
        checking,
        connected,
        failed
    } state = check::checking;
    std::string why, detail;
};

rap::icon status_icon(entry_status::check s)
{
    return rap::icon::drawn(
        [s](rap::painter& p, rap::size_f size)
        {
            const float d = std::min(size.w, size.h) - 4, x = (size.w - d) / 2, y = (size.h - d) / 2;
            if (s == entry_status::check::connected)
            {
                const rap::color green{46, 160, 67};
                p.line({x, y + d * .55f}, {x + d * .38f, y + d * .9f}, green, 2);
                p.line({x + d * .38f, y + d * .9f}, {x + d, y + d * .12f}, green, 2);
            }
            else if (s == entry_status::check::failed)
            {
                const rap::color red{207, 34, 46};
                p.line({x + d * .1f, y + d * .1f}, {x + d * .9f, y + d * .9f}, red, 2);
                p.line({x + d * .9f, y + d * .1f}, {x + d * .1f, y + d * .9f}, red, 2);
            }
            else
                p.circle({size.w / 2, size.h / 2}, d / 4, rap::paint{rap::theme_color::mid}, std::nullopt);
        });
}

// Columns: Name, Provider, Can, Model, Status.
constexpr size_t status_column = 4;

class provider_rows : public rap::list_model
{
public:
    using rap::list_model::notify_changed;
    using rap::list_model::notify_reset;
    ai_accounts accounts;
    std::vector<entry_status> status;  // alongside accounts.entries
    size_t row_count() const override { return accounts.entries.size(); }
    rap::icon icon(size_t row, size_t column) const override
    {
        if (column != status_column || row >= status.size())
            return {};
        return status_icon(status[row].state);
    }
    std::string text(size_t row, size_t column) const override
    {
        if (row >= accounts.entries.size())
            return "";
        const ai_account& a = accounts.entries[row];
        switch (column)
        {
            case 0:
                return a.name;
            case 1:
                return find_ai_provider(a.provider).name;
            case 2:
                return can_text(a);
            case 3:
                // The drawing model; an entry that can't draw shows what it plans with.
                if (!draws(a))
                    return a.reasoning_model.empty() ? "Choose a reasoning model" : a.reasoning_model;
                return a.model.empty() ? "Type a model name" : a.model;
            default:
                if (row >= status.size())
                    return "";
                switch (status[row].state)
                {
                    case entry_status::check::connected:
                        return signs_in(a.provider) ? "Signed in" : "Connected";
                    case entry_status::check::failed:
                        return status[row].why;
                    default:
                        return "Checking…";
                }
        }
    }
    std::string tooltip(size_t row, size_t column, rap::point_f) const override
    {
        if (row >= accounts.entries.size())
            return {};
        const ai_account& a = accounts.entries[row];
        if (column == status_column && row < status.size())
            return status[row].detail;
        if (column == 1)
            return a.base_url;
        if (column == 2 || column == 3)
        {
            std::string t = draws(a) ? "Draws with " + (a.model.empty() ? std::string("no model yet") : a.model) : std::string("Can't draw");
            t += plans(a) ? "; plans and reviews with " + a.reasoning_model : draws(a) ? "; no reasoning model, so it doesn't plan" : "";
            return t + ".";
        }
        return {};
    }
};

struct providers_state : std::enable_shared_from_this<providers_state>
{
    rap::ui& ui;
    std::function<rap::window*()> parent;
    provider_rows rows;
    // The Add drop-down's providers, in its order: the registry's, with Sign
    // in with ChatGPT straight after OpenAI.
    std::vector<const ai_provider*> kinds;
    rap::list* list = nullptr;
    rap::drop_down* model = nullptr;
    rap::combo *typed_model = nullptr, *reasoning = nullptr;
    rap::button *duplicate = nullptr, *remove = nullptr;
    rap::entry* entry_name = nullptr;  // the selected entry's name, edited in place
    rap::drop_down *draws_with = nullptr, *plans_with = nullptr;
    std::vector<std::string> draw_ids, plan_ids;  // the two defaults' items; plan_ids[0] is "none"
    rap::label* roles_note = nullptr;
    rap::drop_down* kind = nullptr;
    rap::stack* credential = nullptr;  // "key" or "chatgpt"
    rap::entry *url = nullptr, *key = nullptr, *name = nullptr;
    rap::label *key_hint = nullptr, *key_link = nullptr, *status = nullptr, *verify_link = nullptr;
    rap::button *add = nullptr, *stop = nullptr, *sign_in = nullptr, *sign_in_stop = nullptr;
    // The Add or sign-in under way, told to stop when the page goes; and the checks.
    std::shared_ptr<std::atomic_bool> adding;
    std::shared_ptr<std::atomic_bool> checks = std::make_shared<std::atomic_bool>(false);

    providers_state(rap::ui& u, std::function<rap::window*()> p) : ui(u), parent(std::move(p))
    {
        for (const auto& k : ai_providers())
            if (k.id != chatgpt_id)
            {
                kinds.push_back(&k);
                if (k.id == "openai")
                    if (const auto* c = lookup_provider(chatgpt_id))
                        kinds.push_back(c);
            }
        if (const auto* c = lookup_provider(chatgpt_id); c && std::ranges::find(kinds, c) == kinds.end())
            kinds.push_back(c);
    }
    ~providers_state()
    {
        checks->store(true);
        if (adding)
            adding->store(true);
    }

    const ai_provider& chosen() const { return *kinds[size_t(std::clamp(kind->current(), 0, int(kinds.size()) - 1))]; }
    std::optional<size_t> selected() const
    {
        const auto sel = list->selection();
        if (sel.empty() || sel.front() >= rows.accounts.entries.size())
            return std::nullopt;
        return sel.front();
    }

    // One line, cut with "…" if long (the whole text is its tooltip), so a
    // message never changes the group's height.
    void set_status(const std::string& text)
    {
        status->set_text(text);
        status->set_tooltip(text);
    }
    void show_verify(bool on) { verify_link->set_text(on ? "Open OpenAI's organization settings…" : " "); }

    void complain(const std::string& text, const std::string& detail = {})
    {
        ui.message(parent(), {.icon = rap::message_spec::icon::error, .title = "AI providers", .text = text, .detail = detail}, [](rap::dialog_role) {});
    }
    // Entries this page removed, so a merge doesn't bring them back.
    std::vector<std::string> removed_ids;

    // Another Offcut window or instance may have saved since this page
    // loaded: entries it added are kept, not overwritten by this page's list.
    void merge_from_disk()
    {
        ai_accounts disk;
        try
        {
            disk = configured_ai_accounts();
        }
        catch (const std::exception&)
        {
            return;
        }
        std::vector<ai_account> added;
        for (auto& a : disk.entries)
            if (!rows.accounts.find(a.id) && std::ranges::find(removed_ids, a.id) == removed_ids.end())
                added.push_back(a);
        if (added.empty())
            return;
        const auto first = rows.accounts.entries.size();
        for (auto& a : added)
        {
            rows.accounts.entries.push_back(a);
            rows.status.push_back({});
        }
        rows.notify_reset();
        check(std::vector<ai_account>(rows.accounts.entries.begin() + std::ptrdiff_t(first), rows.accounts.entries.end()));
    }

    // The two defaults kept pointing at entries that can do the job: "Draws
    // with" at one that draws, "Plans and reviews with" at one with a
    // reasoning model (or none).
    void fix_defaults()
    {
        auto& c = rows.accounts;
        if (const auto* d = c.find(c.default_id); !d || !draws(*d))
        {
            const auto it = std::ranges::find_if(c.entries, draws);
            c.default_id = it == c.entries.end() ? std::string() : it->id;
        }
        if (!c.plans_id.empty() && !c.find(c.plans_id))
            c.plans_id.clear();
        if (c.plans_id.empty())
            if (const auto it = std::ranges::find_if(c.entries, plans); it != c.entries.end())
                c.plans_id = it->id;
    }

    bool save()
    {
        merge_from_disk();
        fix_defaults();
        try
        {
            save_ai_accounts(rows.accounts);
            return true;
        }
        catch (const std::exception& e)
        {
            complain("Offcut couldn't save the AI provider list.", e.what());
            return false;
        }
    }

    // The buttons and fields as the choices and what's under way allow.
    void update()
    {
        const bool busy = adding != nullptr;
        const auto row = selected();
        remove->set_enabled(row.has_value());
        duplicate->set_enabled(row.has_value());
        entry_name->set_enabled(row.has_value());
        const auto& p = chosen();
        const bool session = signs_in(p.id);
        // Every row stays, whatever the provider, so the group keeps its
        // height and the list above doesn't jump: rows that don't apply are
        // read-only, disabled or blank instead of hidden, and the key and
        // sign-in rows are pages of one stack, sized to the larger.
        credential->show(session ? "chatgpt" : "key");
        url->set_read_only(!p.typed_url);
        key->set_placeholder(p.auth == ai_auth::none ? "Not needed" : "");
        key_hint->set_text(p.key_hint);
        key_link->set_text(p.key_page.empty() ? std::string(" ") : "Make a key on " + p.name + "…");
        kind->set_enabled(!busy);
        url->set_enabled(!busy);
        key->set_enabled(!busy && p.auth != ai_auth::none);
        name->set_enabled(!busy);
        sign_in->set_enabled(!busy);
        sign_in_stop->set_visible(busy && session);
        add->set_enabled(!busy && !session && (p.auth != ai_auth::api_key || !trim_ai_key(key->text()).empty()) &&
                         (!p.typed_url || !trim_ai_key(url->text()).empty()));
        add->set_tooltip(session ? "Signing in with ChatGPT adds it" : "");
        stop->set_visible(busy && !session);
    }

    // The two defaults' choices, and what Auto will do with them.
    void show_roles()
    {
        const auto& c = rows.accounts;
        std::vector<std::string> items;
        draw_ids.clear();
        int current = -1;
        for (const auto& a : c.entries)
            if (draws(a))
            {
                if (a.id == c.default_id)
                    current = int(draw_ids.size());
                draw_ids.push_back(a.id);
                items.push_back(a.name);
            }
        if (draw_ids.empty())
            items = {"Nothing: add a provider that draws"};
        draws_with->set_items(items);
        draws_with->set_current(draw_ids.empty() ? 0 : std::max(current, 0));
        draws_with->set_enabled(draw_ids.size() > 1 || (draw_ids.size() == 1 && current < 0));

        items = {"Nothing: Auto uses template prompts"};
        plan_ids = {""};
        current = 0;
        for (const auto& a : c.entries)
            if (plans(a))
            {
                if (a.id == c.plans_id)
                    current = int(plan_ids.size());
                plan_ids.push_back(a.id);
                items.push_back(a.name + " · " + a.reasoning_model);
            }
        plans_with->set_items(items);
        plans_with->set_current(current);
        plans_with->set_enabled(plan_ids.size() > 1);

        const ai_account* drawer = draw_ids.empty() ? nullptr : c.find(draw_ids[size_t(std::max(0, draws_with->current()))]);
        const ai_account* planner = c.plans_account();
        std::string note;
        if (!drawer && !planner)
            note = c.entries.empty() ? "With no provider, Auto arranges the picture locally." : "Choose a model for an entry to draw or plan with.";
        else if (!planner)
            note = "Auto will draw once from a template, without planning: give an entry a reasoning model to plan and review.";
        else if (!drawer)
            note = "Auto can plan and arrange, but not draw: add a provider that draws.";
        else
            note = "Auto plans and reviews with " + planner->name + ", and draws with " + drawer->name + ".";
        roles_note->set_text(note);
        roles_note->set_tooltip(note);
    }

    // The model choices for the selected entry: the drawing model from its
    // listed models (typed for a compatible server; none for an entry that
    // can't draw), and the reasoning model, typed or chosen from the list.
    void show_model()
    {
        const auto row = selected();
        const ai_account* a = row ? &rows.accounts.entries[*row] : nullptr;
        const bool typed = a && draws(*a) && find_ai_provider(a->provider).model_list == ai_model_list::compatible;
        model->set_visible(!typed);
        typed_model->set_visible(typed);
        model->set_enabled(a && draws(*a));
        reasoning->set_enabled(a != nullptr);
        entry_name->set_text(a ? a->name : std::string());
        if (!a)
        {
            model->set_items({});
            reasoning->set_items({});
            reasoning->set_text("");
            return;
        }
        // An entry that signs in lists its reasoning models; others are typed.
        reasoning->set_items(draws(*a) ? std::vector<std::string>{} : a->models);
        reasoning->set_text(a->reasoning_model);
        reasoning->set_tooltip(draws(*a) ? "The vision chat model this entry plans and reviews with, as the provider names it. Empty: it only draws."
                                         : "The model this entry plans and reviews with.");
        if (!draws(*a))
        {
            model->set_items({"Can't draw: choose another entry to draw with"});
            model->set_current(0);
            return;
        }
        if (typed)
        {
            typed_model->set_items(a->models);
            typed_model->set_text(a->model);
            return;
        }
        auto items = a->models;
        if (!a->model.empty() && std::ranges::find(items, a->model) == items.end())
            items.insert(items.begin(), a->model);
        const int current = int(std::ranges::find(items, a->model) - items.begin());
        // Models that need a paid balance say so; the choice is by position.
        for (auto& item : items)
            item = ai_model_label(*a, item);
        model->set_items(items);
        model->set_current(current);
    }

    void reload(const std::string& select = {})
    {
        rows.notify_reset();
        for (size_t i = 0; i < rows.accounts.entries.size(); ++i)
            if (rows.accounts.entries[i].id == select)
            {
                list->select({i});
                list->set_current(i);
            }
        if (!rows.accounts.entries.empty() && !selected())
        {
            list->select({0});
            list->set_current(0);
        }
        show_model();
        show_roles();
        update();
    }

    // Each entry tried on its provider (GET models; a ChatGPT entry first
    // refreshes its sign-in), on a worker: the Status column says whether it
    // works, and why not. Never uploads images.
    void check(std::vector<ai_account> entries)
    {
        std::thread(
            [weak = weak_from_this(), entries = std::move(entries), flag = checks, &ui = ui]
            {
                for (const ai_account& a : entries)
                {
                    if (flag->load())
                        return;
                    entry_status st{.state = entry_status::check::connected};
                    std::vector<std::string> models, paid;
                    std::string key;
                    // A sign-in that's gone needs no request to say so.
                    const bool signed_out = signs_in(a.provider) && !chatgpt_signed_in(a.credential_target);
                    if (signed_out)
                        st = {entry_status::check::failed, "Not signed in",
                              "The ChatGPT sign-in is no longer in Windows Credential Manager. Remove this entry and sign in with ChatGPT again."};
                    else
                    try
                    {
                        const auto& p = find_ai_provider(a.provider);
                        if (signs_in(a.provider))
                            key = chatgpt_access_token(a.credential_target, flag.get());
                        else
                            key = load_account_key(a);
                        models = list_ai_models(p, a.base_url, key, flag.get(), &paid);
                    }
                    catch (const ai_cancelled&)
                    {
                        return;
                    }
                    catch (const std::exception& e)
                    {
                        st = {entry_status::check::failed, redact_ai_secret(ai_failure_summary(e), key), redact_ai_secret(ai_failure_detail(e), key)};
                    }
                    ui.post(
                        [weak, id = a.id, st, models = std::move(models), paid = std::move(paid)]
                        {
                            auto s = weak.lock();
                            if (!s || !s->list)
                                return;
                            for (size_t i = 0; i < s->rows.accounts.entries.size() && i < s->rows.status.size(); ++i)
                            {
                                auto& b = s->rows.accounts.entries[i];
                                if (b.id != id)
                                    continue;
                                s->rows.status[i] = st;
                                if (st.state == entry_status::check::connected && paid != b.paid_models)
                                {
                                    b.paid_models = paid;  // only for this session's list: not saved
                                    if (s->selected() == i)
                                        s->show_model();
                                }
                                // Keep the cached list current; the chosen models stay.
                                const bool empty_choice = draws(b) ? b.model.empty() : b.reasoning_model.empty();
                                if (st.state == entry_status::check::connected && !models.empty() && (models != b.models || empty_choice))
                                {
                                    b.models = models;
                                    // A recovered entry: choose as Add would.
                                    if (draws(b) && b.model.empty())
                                        b.model = default_ai_model(find_ai_provider(b.provider), models, {}, paid);
                                    else if (!draws(b) && b.reasoning_model.empty())
                                        b.reasoning_model = default_ai_model(find_ai_provider(b.provider), models);
                                    s->save();
                                    if (s->selected() == i)
                                        s->show_model();
                                    s->show_roles();
                                }
                                s->rows.notify_changed(i, 1);
                            }
                        });
                }
            })
            .detach();
    }

    void add_chosen()
    {
        if (adding || signs_in(chosen().id))
            return;
        const ai_provider p = chosen();
        std::string base_url = p.typed_url ? trim_ai_key(url->text()) : p.base_url, typed_key;
        show_verify(false);
        try
        {
            split_ai_url(base_url);
            typed_key = validate_provider_key(p, key->text());
        }
        catch (const std::exception& e)
        {
            set_status(e.what());
            return;
        }
        const auto host = split_ai_url(base_url).host;
        set_status("Checking with " + host + "… This reads the model list; no images are uploaded.");
        adding = std::make_shared<std::atomic_bool>(false);
        update();
        struct result
        {
            std::vector<std::string> models, paid;
            std::string error;
            bool verification = false;  // the note looks like OpenAI's organization verification
        };
        std::thread(
            [weak = weak_from_this(), flag = adding, p, base_url, typed_key, &ui = ui]
            {
                result r;
                try
                {
                    r.models = list_ai_models(p, base_url, typed_key, flag.get(), &r.paid);
                }
                catch (const ai_cancelled&)
                {
                    return;
                }
                catch (const std::exception& e)
                {
                    r.error = redact_ai_secret(ai_failure_detail(e), typed_key);
                    const auto* h = dynamic_cast<const ai_http_error*>(&e);
                    r.verification = p.id == "openai" && ((h && h->status == 403) || std::string_view(e.what()).find("verif") != std::string_view::npos);
                }
                ui.post(
                    [weak, flag, p, base_url, typed_key, r = std::move(r)]
                    {
                        auto s = weak.lock();
                        if (!s || s->adding != flag)
                            return;  // gone, or stopped
                        s->adding.reset();
                        s->added(p, base_url, typed_key, r.models, r.paid, r.error, r.verification);
                        s->update();
                    });
            })
            .detach();
    }

    void added(const ai_provider& p, const std::string& base_url, const std::string& typed_key, const std::vector<std::string>& models,
               const std::vector<std::string>& paid, const std::string& error, bool verification)
    {
        if (!error.empty())
        {
            set_status(error);
            show_verify(verification);
            return;
        }
        ai_account a;
        a.id = new_ai_account_id(rows.accounts, p.id);
        a.provider = p.id;
        a.name = new_ai_account_name(rows.accounts, p, name->text());
        a.base_url = base_url;
        a.models = models;
        a.paid_models = paid;
        if (plan_only(p.id))
            a.reasoning_model = default_ai_model(p, models, {}, paid);
        else
            a.model = default_ai_model(p, models, {}, paid);  // a free model first
        if (!typed_key.empty())
        {
            a.credential_target = ai_credential_target(a.id);
            try
            {
                store_ai_key(typed_key, a.credential_target);
            }
            catch (const std::exception& e)
            {
                set_status(std::string("Windows Credential Manager couldn't keep the key: ") + e.what());
                return;
            }
        }
        rows.accounts.entries.push_back(a);
        rows.status.push_back({.state = entry_status::check::connected});
        if (!save())
        {
            if (!a.credential_target.empty())
                clear_ai_key(a.credential_target);
            rows.accounts.entries.pop_back();
            rows.status.pop_back();
            reload();
            return;
        }
        key->set_text("");
        name->set_text("");
        const auto& m = plan_only(p.id) ? a.reasoning_model : a.model;
        set_status(m.empty() ? "Added " + a.name + ". The server lists no models: type the model name above." : "Added " + a.name + ", using " + m + ".");
        reload(a.id);
    }

    // Sign in with ChatGPT: the browser sign-in on a worker, as Tortilla's
    // "Sign in with the browser" does; Stop cancels it. Success adds the
    // entry with its listed models for the reasoning model.
    void sign_in_chosen()
    {
        if (adding || !signs_in(chosen().id))
            return;
        const ai_provider p = chosen();
        const std::string id = new_ai_account_id(rows.accounts, p.id), target = ai_credential_target(id);
        show_verify(false);
        set_status("Waiting for the browser… approve Offcut there.");
        adding = std::make_shared<std::atomic_bool>(false);
        update();
        struct result
        {
            bool signed_in = false;
            std::string label, error, list_error;
            std::vector<std::string> models;
        };
        const auto sign_out_quietly = [](const std::string& t)
        {
            std::thread(
                [t]
                {
                    try
                    {
                        chatgpt_sign_out(t);
                    }
                    catch (const std::exception&)
                    {
                    }
                })
                .detach();
        };
        std::thread(
            [weak = weak_from_this(), flag = adding, p, id, target, sign_out_quietly, &ui = ui]
            {
                result r;
                try
                {
                    r.label = chatgpt_sign_in(target, flag.get()).label;
                    r.signed_in = true;
                }
                catch (const ai_cancelled&)
                {
                    return;
                }
                catch (const std::exception& e)
                {
                    r.error = e.what();
                }
                if (r.signed_in)
                {
                    std::string token;
                    try
                    {
                        token = chatgpt_access_token(target, flag.get());
                        r.models = list_ai_models(p, p.base_url, token, flag.get());
                    }
                    catch (const ai_cancelled&)
                    {
                    }
                    catch (const std::exception& e)
                    {
                        r.list_error = redact_ai_secret(ai_failure_summary(e), token);
                    }
                    if (flag->load())
                    {
                        // Stopped after the browser approved: nothing is kept.
                        sign_out_quietly(target);
                        return;
                    }
                }
                ui.post(
                    [weak, flag, p, id, target, sign_out_quietly, r = std::move(r)]
                    {
                        auto s = weak.lock();
                        if (!s || s->adding != flag)
                        {
                            if (r.signed_in)
                                sign_out_quietly(target);
                            return;  // gone, or stopped
                        }
                        s->adding.reset();
                        s->signed_in(p, id, target, r.signed_in, r.label, r.error, r.list_error, r.models);
                        s->update();
                    });
            })
            .detach();
    }

    void signed_in(const ai_provider& p, const std::string& id, const std::string& target, bool ok, const std::string& label, const std::string& error,
                   const std::string& list_error, const std::vector<std::string>& models)
    {
        if (!ok)
        {
            set_status(error.empty() ? std::string("The sign-in didn't finish.") : error);
            return;
        }
        ai_account a;
        a.id = rows.accounts.find(id) ? new_ai_account_id(rows.accounts, p.id) : id;
        a.provider = p.id;
        auto typed = trim_ai_key(name->text());
        std::erase_if(typed, [](char c) { return static_cast<unsigned char>(c) < 32; });
        a.name = !typed.empty() ? typed : label.empty() ? std::string("ChatGPT") : "ChatGPT (" + label + ")";
        a.base_url = p.base_url;
        a.models = models;
        a.reasoning_model = models.empty() ? p.preferred_model : default_ai_model(p, models);
        a.credential_target = target;
        rows.accounts.entries.push_back(a);
        if (list_error.empty())
            rows.status.push_back({.state = entry_status::check::connected});
        else
            rows.status.push_back({entry_status::check::failed, list_error, list_error + "."});
        if (!save())
        {
            try
            {
                chatgpt_sign_out(target);
            }
            catch (const std::exception&)
            {
            }
            rows.accounts.entries.pop_back();
            rows.status.pop_back();
            reload();
            return;
        }
        name->set_text("");
        std::string text = "Signed in as " + (label.empty() ? a.name : label) + ".";
        if (!list_error.empty())
            text += " Offcut couldn't list its models (" + list_error + "): type the reasoning model above.";
        else if (!a.reasoning_model.empty())
            text += " It plans and reviews with " + a.reasoning_model + ".";
        if (std::ranges::none_of(rows.accounts.entries, draws))
            text += " Add another provider to draw with.";
        set_status(text);
        reload(a.id);
    }

    void remove_selected()
    {
        const auto row = selected();
        if (!row)
            return;
        const ai_account a = rows.accounts.entries[*row];
        const auto& p = find_ai_provider(a.provider);
        const bool shared = std::ranges::count(rows.accounts.entries, a.credential_target, &ai_account::credential_target) > 1;
        std::string detail;
        if (a.credential_target.empty())
            detail = "It uses no key, so nothing else is stored for it.";
        else if (signs_in(a.provider))
            detail = shared ? "Offcut stays signed in to ChatGPT: other entries use the same sign-in."
                            : "Offcut signs out: it ends this sign-in at OpenAI and deletes it from Windows Credential Manager. Your ChatGPT account and "
                              "plan aren't affected.";
        else
            detail = shared ? "Its key stays in Windows Credential Manager: other entries use it too."
                            : "Offcut deletes its key from Windows Credential Manager. The key itself stays valid until you revoke it " +
                                  (p.key_page.empty() ? std::string("on the server.") : "on " + p.name + "'s site.");
        ui.message(parent(),
                   {.icon = rap::message_spec::icon::question,
                    .title = "AI providers",
                    .text = "Remove “" + a.name + "”?",
                    .detail = detail,
                    .buttons = {rap::dialog_role::yes, rap::dialog_role::no}},
                   [weak = weak_from_this(), a](rap::dialog_role role)
                   {
                       auto s = weak.lock();
                       if (!s || role != rap::dialog_role::yes)
                           return;
                       // Keys and sign-ins only Offcut put there; never one another entry still uses.
                       const bool shared = std::ranges::count(s->rows.accounts.entries, a.credential_target, &ai_account::credential_target) > 1;
                       if (!a.credential_target.empty() && !shared)
                       {
                           if (signs_in(a.provider))
                               // Revoking talks to OpenAI: on a worker.
                               std::thread(
                                   [weak, target = a.credential_target, &ui = s->ui]
                                   {
                                       std::string error;
                                       try
                                       {
                                           chatgpt_sign_out(target);
                                       }
                                       catch (const std::exception& e)
                                       {
                                           error = e.what();
                                       }
                                       if (!error.empty())
                                           ui.post(
                                               [weak, error]
                                               {
                                                   if (auto s = weak.lock())
                                                       s->complain("Offcut couldn't sign out of ChatGPT.", error);
                                               });
                                   })
                                   .detach();
                           else
                               try
                               {
                                   clear_ai_key(a.credential_target);
                               }
                               catch (const std::exception& e)
                               {
                                   s->complain("Windows Credential Manager couldn't remove the key.", e.what());
                               }
                       }
                       s->removed_ids.push_back(a.id);
                       for (size_t i = 0; i < s->rows.accounts.entries.size(); ++i)
                           if (s->rows.accounts.entries[i].id == a.id)
                           {
                               s->rows.accounts.entries.erase(s->rows.accounts.entries.begin() + std::ptrdiff_t(i));
                               if (i < s->rows.status.size())
                                   s->rows.status.erase(s->rows.status.begin() + std::ptrdiff_t(i));
                               break;
                           }
                       s->save();  // the defaults move to another capable entry
                       s->set_status("Removed " + a.name + ".");
                       s->reload();
                   });
    }

    void set_draws_with(const std::string& id)
    {
        const auto* a = rows.accounts.find(id);
        if (!a || !draws(*a) || rows.accounts.default_id == id)
            return;
        rows.accounts.default_id = id;
        save();
        show_roles();
    }
    void set_plans_with(const std::string& id)
    {
        const auto* a = id.empty() ? nullptr : rows.accounts.find(id);
        if ((!id.empty() && (!a || !plans(*a))) || rows.accounts.plans_id == id)
            return;
        rows.accounts.plans_id = id;
        // "Nothing" is a choice: kept as written, not filled in again.
        try
        {
            merge_from_disk();
            save_ai_accounts(rows.accounts);
        }
        catch (const std::exception& e)
        {
            complain("Offcut couldn't save the AI provider list.", e.what());
        }
        show_roles();
    }

    // Names are how the list and Generate with AI tell entries apart, so an
    // empty one isn't kept: the field just shows what was typed until then.
    void set_name(std::string n)
    {
        const auto row = selected();
        n = trim_ai_key(std::move(n));
        std::erase_if(n, [](char c) { return static_cast<unsigned char>(c) < 32; });
        if (!row || n.empty() || n == rows.accounts.entries[*row].name)
            return;
        rows.accounts.entries[*row].name = std::move(n);
        save();
        rows.notify_changed(*row, 1);
        show_roles();
    }

    void rename_selected()
    {
        if (selected())
            entry_name->focus();
    }

    // Another entry for the same provider and key, to keep a second model at
    // hand: it shares the original's credential, which Remove keeps while
    // any entry still uses it.
    void duplicate_selected()
    {
        const auto row = selected();
        if (!row)
            return;
        ai_account copy = rows.accounts.entries[*row];
        copy.id = new_ai_account_id(rows.accounts, copy.provider);
        const auto taken = [&](const std::string& n) { return std::ranges::any_of(rows.accounts.entries, [&](const auto& a) { return a.name == n; }); };
        std::string copy_name = copy.name + " (copy)";
        for (int n = 2; taken(copy_name); ++n)
            copy_name = copy.name + " (copy " + std::to_string(n) + ")";
        copy.name = copy_name;
        rows.accounts.entries.insert(rows.accounts.entries.begin() + std::ptrdiff_t(*row + 1), copy);
        const auto checked = *row < rows.status.size() ? rows.status[*row] : decltype(rows.status)::value_type{};
        rows.status.insert(rows.status.begin() + std::ptrdiff_t(std::min(*row + 1, rows.status.size())), checked);
        save();
        set_status("Added " + copy.name + ": rename it and choose its model.");
        reload(copy.id);
        entry_name->focus();
    }

    rap::menu row_menu(std::optional<size_t> row)
    {
        if (!row || *row >= rows.accounts.entries.size())
            return {};
        list->select({*row});
        list->set_current(*row);
        show_model();
        update();
        const ai_account& a = rows.accounts.entries[*row];
        return {{.label = "Re_name", .action = [this] { rename_selected(); }},
                {.label = "_Duplicate", .action = [this] { duplicate_selected(); }},
                rap::menu_item::separator(),
                {.label = "Dra_w with this", .action = [this, id = a.id] { set_draws_with(id); }, .enabled = draws(a) && a.id != rows.accounts.default_id},
                {.label = "_Plan and review with this", .action = [this, id = a.id] { set_plans_with(id); }, .enabled = plans(a) && a.id != rows.accounts.plans_id},
                rap::menu_item::separator(),
                {.label = "_Remove…", .action = [this] { remove_selected(); }}};
    }

    void set_model(const std::string& m)
    {
        const auto row = selected();
        if (!row || !draws(rows.accounts.entries[*row]) || std::ranges::any_of(m, [](char c) { return static_cast<unsigned char>(c) < 32; }))
            return;
        rows.accounts.entries[*row].model = m;
        save();
        rows.notify_changed(*row, 1);
    }

    void set_reasoning_model(const std::string& m)
    {
        const auto row = selected();
        if (!row || std::ranges::any_of(m, [](char c) { return static_cast<unsigned char>(c) < 32; }) || m == rows.accounts.entries[*row].reasoning_model)
            return;
        rows.accounts.entries[*row].reasoning_model = m;
        save();  // the first entry given one becomes "Plans and reviews with"
        rows.notify_changed(*row, 1);
        show_roles();
    }
};

}  // namespace

rap::node ai_providers_page(rap::ui& ui, std::function<rap::window*()> parent)
{
    using namespace rap::layout;
    using namespace rap::literals;
    auto st = std::make_shared<providers_state>(ui, std::move(parent));
    std::string unreadable;
    try
    {
        st->rows.accounts = configured_ai_accounts();
    }
    catch (const std::exception& e)
    {
        unreadable = std::string("Offcut couldn't read its AI provider list (") + e.what() + "). Adding a provider starts a new one.";
    }
    st->rows.status.assign(st->rows.accounts.entries.size(), {});
    st->check(st->rows.accounts.entries);
    // The widgets' callbacks keep the state, which goes with the window; the
    // state keeps no widget alive, so there's no cycle.
    auto with = [st](auto f) { return [st, f] { f(*st); }; };
    std::vector<std::string> kinds;
    for (const auto* p : st->kinds)
        kinds.push_back(p->name);
    const auto& first = *st->kinds.front();
    rap::node page = vbox{
        group{"Configured",
              vbox{
                  list{std::vector<rap::column>{{.title = "Name", .width = 20_ch},
                                              {.title = "Provider", .width = 22_ch},
                                              {.title = "Can", .width = 17_ch},
                                              {.title = "Model", .width = 20_ch},
                                              {.title = "Status", .width = 18_ch, .stretch = true}},
                       &st->rows}
                      .bind(st->list)
                      .selection(rap::selection_mode::single)
                      .on_selection_changed(with(
                          [](providers_state& s)
                          {
                              s.show_model();
                              s.update();
                          }))
                      .on_activate([weak = std::weak_ptr(st)](size_t)
                                   {
                                       if (auto s = weak.lock())
                                           s->rename_selected();
                                   })
                      .on_context_menu(
                          [weak = std::weak_ptr(st)](std::optional<size_t> row)
                          {
                              auto s = weak.lock();
                              return s ? s->row_menu(row) : rap::menu{};
                          })
                      .tooltip("Double-click an entry to rename it; right-click for more.")
                      .stretch(),
                  form{{"Name", hbox{entry{""}
                                         .bind(st->entry_name)
                                         .enabled(false)
                                         .placeholder("How the list and Generate with AI show it")
                                         .on_changed(
                                             [weak = std::weak_ptr(st)](const std::string& text)
                                             {
                                                 if (auto s = weak.lock())
                                                     s->set_name(text);
                                             })
                                         .stretch(),
                                     button{"D_uplicate"}
                                         .bind(st->duplicate)
                                         .enabled(false)
                                         .tooltip("Another entry with the same key or sign-in, to keep a second model at hand")
                                         .on_click(with([](providers_state& s) { s.duplicate_selected(); })),
                                     button{"_Remove"}.bind(st->remove).enabled(false).on_click(with([](providers_state& s) { s.remove_selected(); }))}},
                       {"Model", hbox{drop_down{}
                                          .bind(st->model)
                                          .enabled(false)
                                          .tooltip("The image model this entry draws with")
                                          .on_changed(
                                              [weak = std::weak_ptr(st)](int i)
                                              {
                                                  auto s = weak.lock();
                                                  const auto row = s ? s->selected() : std::nullopt;
                                                  if (!row)
                                                      return;
                                                  auto items = s->rows.accounts.entries[*row].models;
                                                  const auto& m = s->rows.accounts.entries[*row].model;
                                                  if (!m.empty() && std::ranges::find(items, m) == items.end())
                                                      items.insert(items.begin(), m);
                                                  if (i >= 0 && i < int(items.size()))
                                                      s->set_model(items[size_t(i)]);
                                              })
                                          .stretch(),
                                      combo{{}, ""}
                                          .bind(st->typed_model)
                                          .visible(false)
                                          .tooltip("The image model this entry draws with, as the server names it")
                                          .on_changed(
                                              [weak = std::weak_ptr(st)](const std::string& text)
                                              {
                                                  if (auto s = weak.lock())
                                                      s->set_model(trim_ai_key(text));
                                              })
                                          .stretch()}},
                       {"Reasoning model", combo{{}, ""}
                                               .bind(st->reasoning)
                                               .enabled(false)
                                               .on_changed(
                                                   [weak = std::weak_ptr(st)](const std::string& text)
                                                   {
                                                       if (auto s = weak.lock())
                                                           s->set_reasoning_model(trim_ai_key(text));
                                                   })},
                       {"Draws with", hbox{drop_down{}
                                               .bind(st->draws_with)
                                               .enabled(false)
                                               .tooltip("The entry whose image model Auto and Generate with AI draw with")
                                               .on_changed(
                                                   [weak = std::weak_ptr(st)](int i)
                                                   {
                                                       if (auto s = weak.lock(); s && i >= 0 && size_t(i) < s->draw_ids.size())
                                                           s->set_draws_with(s->draw_ids[size_t(i)]);
                                                   })
                                               .stretch(),
                                           label{"Plans and reviews with"},
                                           drop_down{}
                                               .bind(st->plans_with)
                                               .enabled(false)
                                               .tooltip("The entry whose reasoning model plans each picture and reviews each drawing")
                                               .on_changed(
                                                   [weak = std::weak_ptr(st)](int i)
                                                   {
                                                       if (auto s = weak.lock(); s && i >= 0 && size_t(i) < s->plan_ids.size())
                                                           s->set_plans_with(s->plan_ids[size_t(i)]);
                                                   })
                                               .stretch()}},
                       {"", label{" "}.bind(st->roles_note).ellipsize()}}
                      .align_group("ai-providers"),
              }}
            .stretch(),
        group{"Add provider",
              vbox{
                  form{
                      {"Provider", drop_down{kinds, 0}.bind(st->kind).on_changed(
                                       [weak = std::weak_ptr(st)](int)
                                       {
                                           auto s = weak.lock();
                                           if (!s)
                                               return;
                                           // A URL that's one of the usual ones follows the provider.
                                           const auto typed = trim_ai_key(s->url->text());
                                           if (typed.empty() || std::ranges::any_of(ai_providers(), [&](const auto& p) { return p.base_url == typed; }))
                                               s->url->set_text(s->chosen().base_url);
                                           s->show_verify(false);
                                           s->update();
                                       })},
                  }
                      .align_group("ai-providers"),
                  form{{"Server URL", entry{first.base_url}
                                          .bind(st->url)
                                          .placeholder("http://127.0.0.1:8188/v1")
                                          .on_changed(
                                              [weak = std::weak_ptr(st)](const std::string&)
                                              {
                                                  if (auto s = weak.lock(); s && s->add)
                                                      s->update();
                                              })}}
                      .align_group("ai-providers"),
                  // A key, or for Sign in with ChatGPT the browser sign-in:
                  // two pages of one stack, so the group's height never changes.
                  stack{{"key",
                         form{
                             {"API key", entry{}
                                             .bind(st->key)
                                             .password()
                                             .on_changed(
                                                 [weak = std::weak_ptr(st)](const std::string&)
                                                 {
                                                     if (auto s = weak.lock(); s && s->add)
                                                         s->update();
                                                 })
                                             .on_activate(with([](providers_state& s) { s.add_chosen(); }))},
                             {"", label{first.key_hint}.bind(st->key_hint)},
                             {"", label{"Make a key on " + first.name + "…"}.bind(st->key_link).link(
                                      [weak = std::weak_ptr(st)]
                                      {
                                          if (auto s = weak.lock(); s && !s->chosen().key_page.empty())
                                              rap::sys::open_url(s->chosen().key_page);
                                      })},
                         }
                             .align_group("ai-providers")},
                        {"chatgpt",
                         form{
                             {"Account", hbox{button{"_Sign in with ChatGPT…"}
                                                  .bind(st->sign_in)
                                                  .tooltip("Opens your browser to approve Offcut; no password is typed here")
                                                  .on_click(with([](providers_state& s) { s.sign_in_chosen(); })),
                                              button{"Stop"}.bind(st->sign_in_stop).visible(false).on_click(with(
                                                  [](providers_state& s)
                                                  {
                                                      if (s.adding)
                                                          s.adding->store(true);
                                                      s.adding.reset();
                                                      s.set_status("Sign-in stopped.");
                                                      s.update();
                                                  })),
                                              spacer{}}},
                             {"", label{"Uses your ChatGPT Plus or Pro plan for planning and reviewing.\nIt can't draw: choose another provider to draw with."}},
                             {"", label{" "}},
                         }
                             .align_group("ai-providers")}}
                      .bind(st->credential),
                  form{
                      {"Name", entry{}.bind(st->name).placeholder("Optional: how the list and Generate with AI show it")},
                      {"", hbox{button{"_Add"}.bind(st->add).enabled(false).on_click(with([](providers_state& s) { s.add_chosen(); })),
                                button{"Stop"}.bind(st->stop).visible(false).on_click(with(
                                    [](providers_state& s)
                                    {
                                        if (s.adding)
                                            s.adding->store(true);
                                        s.adding.reset();
                                        s.set_status("");
                                        s.update();
                                    })),
                                spacer{}}},
                  }
                      .align_group("ai-providers"),
                  label{unreadable}.bind(st->status).ellipsize().selectable(),
                  label{" "}
                      .bind(st->verify_link)
                      .link([] { rap::sys::open_url("https://platform.openai.com/settings/organization/general"); }),
              }},
        label{"Offcut keeps keys and sign-ins in Windows Credential Manager, and uses them only to talk to that provider. Images are uploaded only "
              "when you choose Generate with AI and confirm."}
            .wrap(),
    };
    // Once its widgets are made: the drawing default selected.
    ui.post(
        [weak = std::weak_ptr(st)]
        {
            if (auto s = weak.lock(); s && s->list)
                s->reload(s->rows.accounts.default_id);
        });
    return page;
}
}  // namespace offcut
