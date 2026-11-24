// The editor's AI loop (docs/ux-design.md section 8): Send to AI (consent),
// the review bar, the Versions strip and the stateless refinements. The
// director (plans and critiques, phase 2) slots in where marked.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include "ai_json.h"
#include "ai_workflow.h"
#include "auto_loop.h"
#include "director_helpers.h"
#include "history.h"
#include "settings.h"
#include "versions.h"
#include "window.h"
namespace offcut
{
namespace L = rap::layout;
using namespace rap;
namespace
{
// Thumbnails fit inside a box: what the Send dialog shows of each upload.
raster contain(const raster& r, int box)
{
    if (!r.valid())
        return {};
    const double scale = std::min(1., double(box) / std::max(r.width, r.height));
    raster out{std::max(1, int(r.width * scale)), std::max(1, int(r.height * scale)), {}};
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
        {
            const int sx = std::min(r.width - 1, int(x / scale)), sy = std::min(r.height - 1, int(y / scale));
            std::copy_n(r.rgba.begin() + (size_t(sy) * r.width + sx) * 4, 4, out.rgba.begin() + (size_t(y) * out.width + x) * 4);
        }
    return out;
}
std::string host_of(const std::string& url)
{
    try
    {
        return split_ai_url(url).host;
    }
    catch (const std::exception&)
    {
        return url;
    }
}
rap::extent px(float v)
{
    rap::extent e;
    e.value = v;
    e.unit = decltype(e.unit)::px;
    return e;
}
std::string seconds_text(double s)
{
    return s >= 90 ? std::format("{} min {:02}", int(s) / 60, int(s) % 60) : std::format("{:.0f} s", s);
}
const std::vector<review_chip>& chips()
{
    static const auto list = parse_review_chips(prompts().text("review-chips.txt"));
    return list;
}
}  // namespace

namespace
{
std::string new_auto_id()
{
    static std::atomic_uint serial{0};
    return std::format("auto-{:x}-{}", std::chrono::system_clock::now().time_since_epoch().count() / 10000000, serial++);
}
std::string consent_key(const ai_account& planner, const std::optional<ai_account>& drawer, bool arrange)
{
    return std::format("plans={};draws={};{}", planner.id, drawer ? drawer->id : std::string(), arrange ? "arrange" : "auto");
}
std::string minutes(double s)
{
    return s >= 60 ? std::format("{} min {:02}", int(s) / 60, int(s) % 60) : std::format("{:.0f} s", s);
}
// The run's versions: rounds drawn, and the time and tokens each role took.
struct run_totals
{
    int rounds = 0, versions = 0;
    double seconds = 0;
    uint64_t tokens = 0;
};
run_totals totals(const layer& l, const std::string& run)
{
    run_totals t;
    for (const auto& v : l.versions)
        if (v.run == run)
        {
            ++t.versions;
            t.rounds = std::max(t.rounds, int(v.round));
            t.seconds += v.director.seconds + v.illustrator.seconds;
            t.tokens += v.director.input_tokens + v.director.output_tokens + v.illustrator.input_tokens + v.illustrator.output_tokens;
        }
    return t;
}
std::string auto_summary(const project& r, const auto_run& run)
{
    if (run.stop == auto_stop::plan_only)
        return "Auto · plan ready: " + (run.compiled.summary.empty() ? std::string("Show plan to read it.") : run.compiled.summary);
    const int i = run.item ? r.find_layer(run.item) : -1;
    if (i < 0)
        return "Auto · " + std::string(auto_stop_text(run.stop)) + (run.message.empty() ? "" : ": " + run.message);
    const auto& l = r.layers[size_t(i)];
    const auto t = totals(l, run.id);
    const auto* v = find_version(l, l.version);
    const int issues = v ? std::max(0, v->issues) : 0;
    std::string out = run.stop == auto_stop::asking ? std::format("Auto · round {} of {} done", run.drawings, run.settings.rounds)
                                                    : std::format("Auto · best of {}", t.rounds);
    out += std::format(" · {} · {}", issues == 0 ? std::string("no issues") : std::format("{} {}", issues, issues == 1 ? "issue" : "issues"),
                       minutes(t.seconds));
    if (run.stop != auto_stop::asking && run.stop != auto_stop::passed)
        out += " · " + std::string(auto_stop_text(run.stop));
    if (!run.message.empty() && (run.stop == auto_stop::failed || run.stop == auto_stop::no_drawing))
        out += ": " + run.message;
    return out;
}
}  // namespace

// What one project's Auto run needs between rounds.
struct editor_window::auto_state
{
    std::shared_ptr<auto_run> run;
    ai_account planner;
    std::optional<ai_account> drawer;
    std::filesystem::path parent;
    auto_settings settings;
    bool arrange = false;
    std::string preset_plan;  // Re-run from plan: draw from this, no re-analysis
    // UI copies, updated by progress (the run itself belongs to the worker while it goes).
    std::string plan, step;
    std::chrono::steady_clock::time_point started{};
    bool running = false;
};

// What the Send dialog holds while it is open.
struct editor_window::send_state
{
    std::unique_ptr<rap::window> window;
    std::shared_ptr<project> doc;
    std::shared_ptr<prepared_ai_upload> upload;
    std::filesystem::path parent;
    std::vector<ai_account> entries;  // the default first
    std::vector<std::unique_ptr<rap::image>> thumbs;
    std::vector<raster> previews;
    rap::drop_down* provider = nullptr;
    rap::label *draws = nullptr, *sizes = nullptr;
    rap::combo* instructions = nullptr;
    rap::text_view* prompt = nullptr;
    rap::widget* prompt_box = nullptr;
    bool sent = false;
    const ai_account& chosen() const { return entries[size_t(std::clamp(provider->current(), 0, int(entries.size()) - 1))]; }
};

ai_accounts editor_window::ai_accounts_now() const
{
    return test_accounts_ ? *test_accounts_ : configured_ai_accounts();
}
std::string editor_window::ai_key(const ai_account& a) const
{
    return test_key_ ? test_key_(a) : load_account_key(a);
}
void editor_window::set_ai_test_hooks(ai_accounts accounts, std::function<std::string(const ai_account&)> key, std::filesystem::path folder)
{
    test_accounts_ = std::move(accounts);
    test_key_ = std::move(key);
    test_folder_ = std::move(folder);
}
// Where AI requests and responses are kept (what was sent, what came back,
// each run's plan): beside a saved project, otherwise in Offcut's own folder.
// Never asked for: a folder prompt in the middle of starting an AI action
// reads as a request to save something.
std::optional<std::filesystem::path> editor_window::ai_folder(const project& p) const
{
    if (!test_folder_.empty())
        return test_folder_;
    if (!p.path.empty())
        return p.path.parent_path();
    return settings_directory() / "AI requests";
}
void editor_window::with_ai_folder(std::shared_ptr<project> p, std::function<void(std::filesystem::path)> then)
{
    const auto folder = *ai_folder(*p);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec)
    {
        app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "AI requests",
                                        .text = "Offcut couldn't create the folder for AI requests:\n" + utf8(folder) + "\n" + ec.message()});
        return;
    }
    // After the menu or button that asked has finished its click.
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.post(
        [this, weak, p, folder, then = std::move(then)]
        {
            if (!weak.expired() && !p->busy)
                then(folder);
        });
}
bool editor_window::offer_settings()
{
    try
    {
        if (!ai_accounts_now().entries.empty())
            return false;
    }
    catch (const std::exception& e)
    {
        app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "AI providers", .text = e.what()});
        return true;
    }
    // Nothing to send to yet: Send to AI's body says so, with Open Settings.
    struct prompt
    {
        std::unique_ptr<rap::window> window;
    };
    auto dialog = std::make_shared<prompt>();
    auto weak = std::weak_ptr<void>(alive_);
    dialog->window = app_.ui.create(
        L::dialog{"Send to AI — Offcut",
                  {L::vbox{
                      L::label{"No AI provider yet"},
                      L::label{"Add one in Settings → AI providers: OpenAI, Pollinations, or an OpenAI-compatible server. Nothing is uploaded until you "
                               "confirm what is sent."}
                          .wrap(),
                      L::buttons{L::dialog_button{dialog_role::ok, "Open Settings"}, L::cancel},
                  }}}
            .size(520_px, {}),
        window_.get());
    dialog->window->on_finished(
        [this, weak, dialog](dialog_role role) mutable
        {
            auto keep = std::move(dialog);
            if (role == dialog_role::ok && !weak.expired())
                show_settings(app_.ui, window_.get(), settings_page::ai_providers);
            app_.ui.post([keep] {});
        });
    dialog->window->show();
    return true;
}
const layer* editor_window::selected_item() const
{
    auto p = current();
    return p && p->selected_layer >= 0 && p->selected_layer < int(p->layers.size()) ? &p->layers[size_t(p->selected_layer)] : nullptr;
}

// -- Send to AI ----------------------------------------------------------------

void editor_window::reimagine()
{
    auto p = current();
    if (!p || p->busy || p->layers.empty() || offer_settings())
        return;
    with_ai_folder(p,
                   [this, p](std::filesystem::path parent)
                   {
                       auto upload = std::make_shared<prepared_ai_upload>();
                       auto weak = std::weak_ptr<void>(alive_);
                       app_.task(
                           p, "Preparing AI upload…", [upload](project& r) { *upload = prepare_ai_upload(r); },
                           [this, weak, p, upload, parent](bool success)
                           {
                               if (success && !weak.expired())
                                   open_send_dialog(p, upload, parent);
                           });
                   });
}
void editor_window::refine_dialog(int index, uint32_t version, ai_task task)
{
    auto p = current();
    if (!p || p->busy || index < 0 || index >= int(p->layers.size()) || offer_settings())
        return;
    const auto id = p->layers[size_t(index)].id;
    with_ai_folder(p,
                   [this, p, id, version, task](std::filesystem::path parent)
                   {
                       try
                       {
                           auto upload = std::make_shared<prepared_ai_upload>(prepare_refinement(*p, id, version, task, p->instructions));
                           open_send_dialog(p, upload, parent);
                       }
                       catch (const std::exception& e)
                       {
                           flash(e.what());
                       }
                   });
}
void editor_window::open_send_dialog(std::shared_ptr<project> p, std::shared_ptr<prepared_ai_upload> upload, std::filesystem::path parent)
{
    ai_accounts accounts;
    try
    {
        accounts = ai_accounts_now();
    }
    catch (const std::exception& e)
    {
        app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "AI providers", .text = e.what()});
        return;
    }
    if (accounts.entries.empty())
    {
        offer_settings();
        return;
    }
    if (send_ && send_->window)
        send_->window->close();
    auto s = std::make_shared<send_state>();
    s->doc = p;
    s->upload = upload;
    s->parent = parent;
    // The default "Draws with" entry first.
    if (const auto* d = accounts.default_account())
        s->entries.push_back(*d);
    for (const auto& a : accounts.entries)
        if (s->entries.empty() || a.id != s->entries.front().id)
            s->entries.push_back(a);
    std::vector<std::string> names;
    for (const auto& a : s->entries)
        names.push_back(a.name + (a.model.empty() ? "" : " · " + a.model));
    // Exactly what is uploaded, as thumbnails, decoded from what is sent.
    size_t bytes = 0;
    for (const auto& image : upload->images)
        bytes += image.png.size();
    // The pixels behind each upload, in order.
    if (upload->task == ai_task::reimagine && !upload->item)
    {
        s->previews.push_back(contain(upload->composition, 84));
        for (const auto& source : p->layers)
            if (source.visible)
                s->previews.push_back(contain(cropped(current_pixels(source), effective_transform(source, p->options).crop), 84));
    }
    else if (const int i = p->find_layer(upload->item); i >= 0)
    {
        // Refine sends the version refined; Try again the one its sibling came
        // from: both are upload->parent. The original follows as a reference.
        const auto& l = p->layers[size_t(i)];
        const auto* v = upload->parent ? find_version(l, upload->parent) : nullptr;
        s->previews.push_back(contain(v ? v->image : l.image, 84));
        if (upload->parent)
            s->previews.push_back(contain(l.image, 84));
    }
    const bool follow = upload->task != ai_task::reimagine;
    const auto weak_state = std::weak_ptr<send_state>(s);
    const auto describe = [weak_state]
    {
        auto st = weak_state.lock();
        if (!st)
            return;
        try
        {
            const auto& a = st->chosen();
            choose_ai_account(*st->upload, a);
            const auto& e = st->upload->endpoint;
            st->draws->set_text(std::format("{} · {} · {}", host_of(e.base_url), e.model, e.size));
        }
        catch (const std::exception& e)
        {
            st->draws->set_text(e.what());
        }
        if (st->prompt)
            st->prompt->set_text(st->upload->prompt);
    };
    std::string what = upload->task == ai_task::reimagine ? "Reimagine as shadowbox"
                       : upload->task == ai_task::fix_support ? std::format("Fix with AI: version {}", upload->parent)
                                                              : std::format("{}: version {}", ai_task_name(upload->task), upload->parent);
    auto recent = p->recent_instructions;
    s->window = app_.ui.create(
        L::dialog{"Send to AI — Offcut",
                  {L::vbox{
                      L::label{what},
                      L::form{
                          {"Draws", L::drop_down{names, 0}.bind(s->provider).on_changed([describe](int) { describe(); }).stretch()},
                          {"Sends to", L::label{}.bind(s->draws).wrap().selectable()},
                          {"Sees", L::label{"your pictures (below) and the prompt"}},
                          {"Plans and reviews", L::label{"No reasoning model: Offcut sends a template prompt, and you review each drawing."}.wrap()},
                      }
                          .align_group("send"),
                      L::hbox{
                          L::canvas{}
                              .focusable(false)
                              .width(px(float(std::max<size_t>(1, s->previews.size()) * 92)))
                              .height(92_px)
                              .on_paint(
                                  [weak_state](painter& painter, size_f)
                                  {
                                      auto st = weak_state.lock();
                                      if (!st)
                                          return;
                                      float x = 0;
                                      for (const auto& t : st->thumbs)
                                      {
                                          const auto dst = fit_inside({x + 2, 2, 84, 84}, float(t->width()), float(t->height()));
                                          painter.image(*t, {0, 0, float(t->width()), float(t->height())}, dst);
                                          painter.rect(dst, 0, std::nullopt, rap::paint{theme_color::border});
                                          x += 92;
                                      }
                                  }),
                          L::label{std::format("{} {}, {} KB\nexactly what is uploaded", upload->images.size(), upload->images.size() == 1 ? "image" : "images",
                                               (bytes + 1023) / 1024)}
                              .bind(s->sizes),
                      },
                      L::form{
                          {"Your instructions", L::combo{recent, upload->words.instructions}
                                                    .bind(s->instructions)
                                                    .on_changed(
                                                        [weak_state](const std::string& text)
                                                        {
                                                            if (auto st = weak_state.lock())
                                                            {
                                                                set_ai_instructions(*st->upload, text);
                                                                if (st->prompt)
                                                                    st->prompt->set_text(st->upload->prompt);
                                                            }
                                                        })
                                                    .tooltip("This request only; e.g. “plain sky, a hill running off both sides”. The last ten are in the list.")},
                          {"Until", L::hbox{
                                        L::drop_down{{"Review each drawing"}, 0}
                                            .enabled(false)
                                            .tooltip("Auto until good and Plan only need a reasoning model (Settings → AI providers)"),
                                        L::label{"Rounds"},
                                        L::spin{1, 5, 1}.enabled(false).tooltip("More rounds need a reasoning model to judge each drawing"),
                                    }},
                      }
                          .align_group("send"),
                      L::check{"▸ Full prompt", false}.on_toggled(
                          [weak_state](bool on)
                          {
                              if (auto st = weak_state.lock())
                                  st->prompt_box->set_visible(on);
                          }),
                      L::vbox{
                          L::text_view{upload->prompt}.read_only().height(12_lines).bind(s->prompt),
                          L::hbox{L::label{"The exact text sent. Put changes in Your instructions."}.stretch(),
                                  L::button{"Copy"}.on_click(
                                      [this, weak_state]
                                      {
                                          if (auto st = weak_state.lock())
                                              app_.ui.set_clipboard(st->upload->prompt);
                                      })},
                      }
                          .handle(s->prompt_box)
                          .visible(false),
                      L::label{follow ? "Nothing is sent until Send. One drawing from the version above; about a minute."
                                      : "Nothing is sent until Send. One drawing; about a minute."},
                      L::buttons{L::dialog_button{dialog_role::ok, "Send"}, L::cancel},
                  }}}
            .size(640_px, {}),
        window_.get());
    for (const auto& preview : s->previews)
        if (preview.valid())
            s->thumbs.push_back(app_.ui.create_image(preview.width, preview.height, pixel_format::rgba8, preview.rgba.data(), size_t(preview.width) * 4));
    describe();
    send_ = s;
    auto weak = std::weak_ptr<void>(alive_);
    s->window->on_finished(
        [this, weak, weak_state](dialog_role role)
        {
            auto st = weak_state.lock();
            if (!st || weak.expired())
                return;
            if (role == dialog_role::ok && !st->sent)
            {
                st->sent = true;
                const auto text = st->instructions->text();
                set_ai_instructions(*st->upload, text);
                try
                {
                    const auto account = st->chosen();
                    choose_ai_account(*st->upload, account);
                    remember_instruction(*st->doc, text);
                    start_ai(st->doc, st->upload, account, st->parent, false, {});
                }
                catch (const std::exception& e)
                {
                    app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "Send to AI", .text = e.what()});
                }
            }
            else if (!st->sent)
                flash("Nothing was sent.");
            if (send_ == st)
                app_.ui.post([this, weak, st] {
                    if (!weak.expired() && send_ == st)
                        send_.reset();
                });
        });
    s->window->show();
}
rap::window* editor_window::send_dialog_window() const
{
    return send_ ? send_->window.get() : nullptr;
}
bool editor_window::press_send(const std::string& instructions)
{
    auto s = send_;
    if (!s || !s->window || s->sent)
        return false;
    if (!instructions.empty())
    {
        s->instructions->set_text(instructions);
        set_ai_instructions(*s->upload, instructions);
    }
    s->sent = true;
    const auto text = s->instructions->text();
    set_ai_instructions(*s->upload, text);
    try
    {
        const auto account = s->chosen();
        choose_ai_account(*s->upload, account);
        remember_instruction(*s->doc, text);
        start_ai(s->doc, s->upload, account, s->parent, false, {});
    }
    catch (const std::exception& e)
    {
        flash(e.what());
        return false;
    }
    s->window->close();
    return true;
}
void editor_window::start_ai(std::shared_ptr<project> p, std::shared_ptr<prepared_ai_upload> upload, const ai_account& account, std::filesystem::path parent,
                             bool follow, std::function<void(project&)> before)
{
    if (p->busy)
        return;
    p->ai_running = true;
    following_up_ = follow;
    auto key_of = test_key_ ? test_key_ : std::function<std::string(const ai_account&)>(load_account_key);
    const auto label = upload->task == ai_task::reimagine ? std::string("Reimagine as shadowbox") : std::string(ai_task_name(upload->task)) + " with AI";
    auto weak = std::weak_ptr<void>(alive_);
    app_.task(
        p, std::format("Drawing with {} · {}…", account.name, upload->endpoint.model),
        [upload, parent, account, key_of, before](project& r)
        {
            std::string key;
            try
            {
                key = key_of(account);
                const auto result = request_ai_image(*upload, key, parent, r.cancel.get());
                if (r.cancel && r.cancel->load())
                    throw ai_cancelled();
                // The answer that led here is recorded in the same undo step.
                if (before)
                    before(r);
                apply_ai_result(r, *upload, result);
            }
            catch (const ai_cancelled&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error(account.name + ": " + redact_ai_secret(ai_failure_detail(e), key));
            }
        },
        [this, weak](bool)
        {
            if (weak.expired())
                return;
            following_up_ = false;
            refresh();
        },
        label);
}

// -- review bar ---------------------------------------------------------------

bool editor_window::reviewing() const
{
    return bottom_bar_ && bottom_bar_->shown() == "review";
}
std::string editor_window::review_line() const
{
    return review_line_;
}
void editor_window::sync_review()
{
    auto p = current();
    if (!p || !bottom_bar_)
        return;
    const auto* l = selected_item();
    const auto* v = l && l->version ? find_version(*l, l->version) : nullptr;
    const bool running = p->busy && p->ai_running && following_up_;
    const bool asking = auto_asking();
    const bool show = running || (v && v->state == review_state::pending && !p->busy);
    bottom_bar_->show(show ? "review" : "guidance");
    if (!show)
    {
        review_line_.clear();
        return;
    }
    // Pending chips belong to one version of one item.
    if (chips_project_ != p.get() || !l || chips_layer_ != l->id || !v || chips_version_ != v->id)
    {
        chips_.clear();
        chips_project_ = p.get();
        chips_layer_ = l ? l->id : 0;
        chips_version_ = v ? v->id : 0;
    }
    std::string status, text;
    if (running)
    {
        status = "AI";
        text = p->activity;
    }
    else
    {
        const auto issues = v->issues < 0 ? std::string("not checked") : v->issues == 0 ? std::string("no issues") : std::format("{} {}", v->issues, v->issues == 1 ? "issue" : "issues");
        status = std::format("Result {} · {} · {}{}", v->id, issues, seconds_text(v->seconds), v->round > 1 ? std::format(" · round {}", v->round) : "");
        if (!v->plan.empty() && !v->run.empty())
        {
            // An Auto run: the best of its rounds, and what the run took in all.
            const auto t = totals(*l, v->run);
            const auto st = auto_of(p.get());
            status = asking && st && st->run ? std::format("Auto · round {} of {} · {} · {}", v->round, st->run->settings.rounds, issues, minutes(t.seconds))
                                             : std::format("Auto · best of {} · {} · {}", t.rounds, issues, minutes(t.seconds));
        }
        if (!chips_.empty())
            text = "Pending: " + chip_review(chips(), chips_);  // the follow-up itself is in the tooltip
        else if (!v->explanation.empty())
            text = "“" + v->explanation + "”";
        else
            text = "Does this work as a shadowbox?";
    }
    review_status_->set_text(status);
    review_text_->set_text(text);
    review_text_->set_tooltip(chips_.empty() || running ? text : chip_instruction(chips(), chips_, p->options));
    review_answers_->set_visible(!running && !asking);
    review_ask_->set_visible(!running && asking);
    review_cancel_->set_visible(running);
    review_cancel_->set_enabled(p->cancel && !p->cancel->load());
    review_send_->set_visible(!running && !chips_.empty());
    show_plan_->set_enabled(v != nullptr);
    review_line_ = status + "   " + text;
}
rap::menu editor_window::chip_menu(const std::string& menu)
{
    rap::menu m;
    for (const auto& c : chips())
        if (c.menu == menu)
        {
            const bool on = std::ranges::any_of(chips_, [&](const chip_choice& x) { return x.id == c.id; });
            m.push_back({.label = c.label,
                         .action =
                             [this, id = c.id, asks = c.asks(), on]
                         {
                             if (!asks || on)
                             {
                                 toggle_chip(id);
                                 return;
                             }
                             // Use… and Lost a feature… name an element.
                             struct ask
                             {
                                 rap::entry* entry = nullptr;
                             };
                             auto a = std::make_shared<ask>();
                             auto weak = std::weak_ptr<void>(alive_);
                             element_window_ = app_.ui.create(
                                 L::dialog{"Review — Offcut",
                                           {L::vbox{
                                               L::form{{id.starts_with("support") ? "Use what?" : "Which feature?",
                                                        L::entry{}.bind(a->entry).placeholder(id.starts_with("support") ? "reeds" : "the white bib")}},
                                               L::buttons{L::dialog_button{dialog_role::ok, "Add"}, L::cancel},
                                           }}}
                                     .size(420_px, {}),
                                 window_.get());
                             element_window_->on_finished(
                                 [this, weak, a, id](dialog_role role)
                                 {
                                     if (weak.expired())
                                         return;
                                     const auto text = a->entry->text();
                                     if (role == dialog_role::ok && !text.empty())
                                         toggle_chip(id, text);
                                     app_.ui.post([this, weak] {
                                         if (!weak.expired())
                                             element_window_.reset();
                                     });
                                 });
                             element_window_->show();
                         },
                         .checked = on});
        }
    return m;
}
void editor_window::toggle_chip(const std::string& id, const std::string& element)
{
    sync_review();
    const auto it = std::ranges::find(chips_, id, &chip_choice::id);
    if (it != chips_.end())
        chips_.erase(it);
    else
        chips_.push_back({id, element});
    sync_review();
}
void editor_window::follow_up(ai_task task, const std::string& instructions, const std::string& chip_list, const std::string& review)
{
    auto p = current();
    const auto* l = selected_item();
    if (!p || p->busy || !l || !l->version)
        return;
    const auto* v = find_version(*l, l->version);
    if (!v)
        return;
    const auto item = l->id, version = v->id;
    // A follow-up in a thread uploads nothing new to that provider: it sends
    // at once, from the review bar, which shows the provider and Cancel. An
    // entry that has gone asks again through Send to AI.
    std::optional<ai_account> account;
    try
    {
        const auto accounts = ai_accounts_now();
        if (const auto* a = accounts.find(v->account))
            account = *a;
    }
    catch (const std::exception&)
    {
    }
    std::optional<std::filesystem::path> parent;
    std::error_code ec;
    if (!v->request.empty() && std::filesystem::is_directory(from_utf8(v->request).parent_path(), ec))
        parent = from_utf8(v->request).parent_path();
    else
        parent = ai_folder(*p);
    std::shared_ptr<prepared_ai_upload> upload;
    try
    {
        upload = std::make_shared<prepared_ai_upload>(prepare_refinement(*p, item, version, task, instructions));
        if (account)
            choose_ai_account(*upload, *account);
    }
    catch (const std::exception& e)
    {
        flash(e.what());
        return;
    }
    const auto record = [item, version, chip_list, review](project& r)
    {
        const int i = r.find_layer(item);
        if (i < 0)
            return;
        if (auto* answered = find_version(r.layers[size_t(i)], version))
        {
            answered->review = review;
            answered->chips = chip_list;
            if (answered->state == review_state::pending)
                answered->state = review_state::answered;
        }
    };
    chips_.clear();
    if (!account || !parent)
    {
        with_ai_folder(p, [this, p, upload](std::filesystem::path folder) { open_send_dialog(p, upload, folder); });
        return;
    }
    start_ai(p, upload, *account, *parent, true, record);
}
void editor_window::review_use()
{
    auto p = current();
    if (!p || p->busy || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
        return;
    auto& l = p->layers[size_t(p->selected_layer)];
    if (auto* v = find_version(l, l.version))
    {
        v->state = review_state::used;
        v->review = "used";
        v->chips.clear();
        chips_.clear();
        edited("Use AI result", {}, false);
        flash(std::format("Using AI result {}.", v->id));
    }
}
void editor_window::review_try_again()
{
    follow_up(ai_task::try_again, {}, {}, "try again");
}
void editor_window::review_send()
{
    auto p = current();
    if (!p || chips_.empty())
        return;
    const auto chosen = chips_;
    if (review_through_director(chip_review(chips(), chosen), chip_ids(chosen), chip_review(chips(), chosen)))
        return;
    follow_up(ai_task::refine, chip_instruction(chips(), chosen, p->options), chip_ids(chosen), chip_review(chips(), chosen));
}
void editor_window::review_discard()
{
    auto p = current();
    const auto* l = selected_item();
    if (!p || p->busy || !l || !l->version)
        return;
    const auto item = l->id, version = l->version;
    chips_.clear();
    app_.task(
        p, std::format("Discarding AI result {}…", version),
        [item, version](project& r)
        {
            const int i = r.find_layer(item);
            if (i < 0 || !discard_version(r.layers[size_t(i)], version))
                throw std::runtime_error("That result is already gone.");
            rebuild_sheets(r);
            r.activity = std::format("Discarded AI result {}. Ctrl+Z brings it back.", version);
        },
        {}, "Discard AI result");
}
void editor_window::write_review()
{
    auto p = current();
    if (!p || !selected_item())
        return;
    struct review
    {
        rap::text_view* text = nullptr;
    };
    auto r = std::make_shared<review>();
    auto weak = std::weak_ptr<void>(alive_);
    const auto initial = chip_instruction(chips(), chips_, p->options);
    review_window_ = app_.ui.create(
        L::dialog{"Write a review — Offcut",
                  {L::vbox{
                      L::label{"What should change? It is sent as one refinement, with what Offcut found."}.wrap(),
                      L::text_view{initial}.height(8_lines).bind(r->text),
                      L::hbox{L::button{"Mark"}.enabled(false).tooltip("Drawing round what should change comes with the next phase"), L::spacer{}},
                      L::buttons{L::dialog_button{dialog_role::ok, "Send"}, L::cancel},
                  }}}
            .size(520_px, {})
            .modeless(),
        window_.get());
    review_window_->on_finished(
        [this, weak, r](dialog_role role)
        {
            if (weak.expired())
                return;
            const auto text = r->text->text();
            if (role == dialog_role::ok && text.find_first_not_of(" \t\r\n") != std::string::npos)
            {
                const auto chosen = chips_;
                if (!review_through_director(text, chip_ids(chosen), "review: " + text.substr(0, 200)))
                    follow_up(ai_task::refine, text, chip_ids(chosen), "review: " + text.substr(0, 200));
            }
            app_.ui.post([this, weak] {
                if (!weak.expired())
                    review_window_.reset();
            });
        });
    review_window_->show();
}
void editor_window::show_record(int index, uint32_t version)
{
    if (auto p = current(); p && index >= 0 && index < int(p->layers.size()))
        if (const auto* v = find_version(p->layers[size_t(index)], version); v && !v->plan.empty())
        {
            show_plan(v->plan, index, version);
            return;
        }
    show_record_details(index, version);
}
void editor_window::show_record_details(int index, uint32_t version)
{
    auto p = current();
    if (!p || index < 0 || index >= int(p->layers.size()))
        return;
    const auto& l = p->layers[size_t(index)];
    const auto* v = find_version(l, version);
    if (!v)
    {
        flash("Version 0 is the original: it has no plan or prompt.");
        return;
    }
    std::string record = std::format(
        "{} · version {} (from {}) · run {} round {}\n{} · {} · {}\n{} · {} · issues {} · score {}\nPrompt files: {}\nRequest: {}\n{}{}{}",
        v->task, v->id, v->parent, v->run.empty() ? "–" : v->run, v->round, v->provider, v->model, v->created, seconds_text(v->seconds),
        v->illustrator.input_tokens + v->illustrator.output_tokens ? std::format("{} + {} tokens", v->illustrator.input_tokens, v->illustrator.output_tokens)
                                                                   : std::string("tokens not reported"),
        v->issues, v->score, v->prompt_files, v->request, v->findings.empty() ? "" : "Found: " + v->findings + "\n",
        v->review.empty() ? "" : "Answer: " + v->review + (v->chips.empty() ? "" : " (" + v->chips + ")") + "\n",
        v->plan.empty() ? "Plan: none (template prompt; a reasoning model writes plans)\n" : "Plan: " + v->plan + "\n");
    record_window_ = app_.ui.create(L::dialog{std::format("Version {} — Offcut", v->id),
                                              {L::vbox{
                                                  L::label{record}.wrap().selectable(),
                                                  L::label{"Prompt sent"},
                                                  L::text_view{v->prompt}.read_only().height(14_lines),
                                                  L::text_view{v->measurements}.read_only().monospace().height(4_lines),
                                                  L::buttons{L::dialog_button{dialog_role::close}},
                                              }}}
                                        .size(680_px, {})
                                        .modeless(),
                                    window_.get());
    auto weak = std::weak_ptr<void>(alive_);
    record_window_->on_finished(
        [this, weak](dialog_role)
        {
            app_.ui.post([this, weak] {
                if (!weak.expired())
                    record_window_.reset();
            });
        });
    record_window_->show();
}

// -- Versions strip -------------------------------------------------------------

namespace
{
constexpr float cell_w = 86, cell_h = 124, thumb_h = 70;
}
void editor_window::show_version(int index, uint32_t version)
{
    auto p = current();
    if (!p || p->busy || index < 0 || index >= int(p->layers.size()))
        return;
    const auto item = p->layers[size_t(index)].id;
    if (p->layers[size_t(index)].version == version)
        return;
    app_.task(
        p, std::format("Showing version {}…", version),
        [item, version](project& r)
        {
            const int i = r.find_layer(item);
            if (i < 0 || !set_current_version(r.layers[size_t(i)], version))
                throw std::runtime_error("That version is gone.");
            const auto found = rebuild_sheets(r);
            r.activity = std::format("Version {} is current: {}.", version,
                                     found.count() ? std::format("{} {}", found.count(), found.count() == 1 ? "issue" : "issues") : std::string("no issues"));
        },
        {}, std::format("Show version {}", version));
}
void editor_window::sync_versions()
{
    const auto* l = selected_item();
    auto p = current();
    const bool show = p && l && !l->versions.empty() && p->step != work_step::sheets;
    placement_box_->set_visible(!l || l->type != layer_type::ai_illustration);
    versions_box_->set_visible(show);
    if (!show)
        return;
    if (versions_canvas_)
        versions_canvas_->invalidate();
}
const rap::image* editor_window::version_thumbnail(const layer& l, uint32_t version)
{
    const auto* v = version ? find_version(l, version) : nullptr;
    const auto& image = v ? v->image : l.image;
    if (!image.valid())
        return nullptr;
    const auto key = std::format("v{}:{}:{}", l.id, version, static_cast<const void*>(image.rgba.data()));
    auto& slot = version_thumbs_[key];
    if (!slot)
    {
        if (version_thumbs_.size() > 256)
        {
            version_thumbs_.clear();
            return version_thumbnail(l, version);
        }
        const auto small = contain(image, 140);
        slot = app_.ui.create_image(small.width, small.height, pixel_format::rgba8, small.rgba.data(), size_t(small.width) * 4);
    }
    return slot.get();
}
void editor_window::paint_versions(painter& painter, size_f size)
{
    version_hits_.clear();
    const auto* l = selected_item();
    if (!l)
        return;
    painter.rect({0, 0, size.w, size.h}, 0, rap::paint{theme_color::base}, std::nullopt);
    const auto cells = version_layout(*l);
    const auto cell_of = [&](uint32_t id) -> const version_cell*
    {
        for (const auto& c : cells)
            if (c.id == id)
                return &c;
        return nullptr;
    };
    const auto origin = [](const version_cell& c) { return point_f{4 + c.column * cell_w, 2 + c.row * cell_h}; };
    // Runs: a band behind the versions of each run, so the strip reads by run.
    std::vector<std::string> runs;
    for (const auto& c : cells)
        if (const auto* v = c.id ? find_version(*l, c.id) : nullptr)
        {
            const auto o = origin(c);
            auto it = std::ranges::find(runs, v->run);
            if (it == runs.end())
                runs.push_back(v->run), it = runs.end() - 1;
            const bool odd = (it - runs.begin()) % 2;
            painter.rect({o.x - 1, o.y + thumb_h + 2, cell_w - 2, 4}, 2, odd ? rap::paint{theme_color::warning} : rap::paint{theme_color::accent}, std::nullopt);
        }
    // Branch rows hang from their parent.
    for (const auto& c : cells)
        if (c.id && c.row > 0)
            if (const auto* v = find_version(*l, c.id))
                if (const auto* parent = cell_of(v->parent); parent && parent->row != c.row)
                {
                    const auto a = origin(*parent), b = origin(c);
                    if (c.column == 0 || cell_of(v->parent)->column + 1 == c.column)
                    {
                        painter.line({a.x + cell_w / 2 - 6, a.y + thumb_h + 8}, {a.x + cell_w / 2 - 6, b.y + thumb_h / 2}, rap::paint{theme_color::mid}, 1.5f);
                        painter.line({a.x + cell_w / 2 - 6, b.y + thumb_h / 2}, {b.x, b.y + thumb_h / 2}, rap::paint{theme_color::mid}, 1.5f);
                    }
                }
    const text_style small{.size = 8};
    const text_style mark{.fg = rap::paint{theme_color::negative}, .bold = true, .size = 9};
    for (const auto& c : cells)
    {
        const auto o = origin(c);
        const rect_f box{o.x, o.y, cell_w - 6, thumb_h};
        if (const auto* image = version_thumbnail(*l, c.id))
        {
            const auto dst = fit_inside(box, float(image->width()), float(image->height()));
            painter.image(*image, {0, 0, float(image->width()), float(image->height())}, dst);
        }
        const bool current = l->version == c.id;
        painter.rect(box, 2, std::nullopt, current ? rap::paint{theme_color::highlight} : rap::paint{theme_color::border});
        if (current)
            painter.rect({box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 2, std::nullopt, rap::paint{theme_color::highlight});
        if (c.id == strip_focus_ && !current)
            painter.rect({box.x - 2, box.y - 2, box.w + 4, box.h + 4}, 3, std::nullopt, rap::paint{theme_color::accent});
        const auto* v = c.id ? find_version(*l, c.id) : nullptr;
        // Marks along the top: ✓ current, ⚑ to review, ●issues, ▲ drift.
        std::string marks = std::format("{}", c.id);
        if (current)
            marks += " ✓";
        if (v && v->state == review_state::pending)
            marks += " ⚑";
        painter.text({o.x + 2, o.y + thumb_h + 7}, marks, text_style{.bold = true, .size = 8});
        if (v && v->issues >= 0)
        {
            const auto badge = std::format("●{}{}", v->issues, drifted(*l, *v) ? " ▲" : "");
            const auto m = painter.measure(badge, mark);
            painter.text({o.x + cell_w - 8 - m.w, o.y + thumb_h + 7}, badge, v->issues ? mark : text_style{.fg = rap::paint{theme_color::positive}, .size = 9});
        }
        // Caption: the instruction, else what was done.
        std::string caption = !v ? std::string(l->type == layer_type::ai_illustration ? "composition" : "original")
                              : !v->plan.empty() && !v->run.empty() ? std::format("{} · round {}", v->task, v->round)
                              : !v->instructions.empty() ? "“" + v->instructions + "”"
                              : !v->review.empty() && false ? v->review
                                                            : std::string(v->task.empty() ? "AI" : v->task);
        // Two short lines.
        for (int line = 0; line < 2 && !caption.empty(); ++line)
        {
            size_t fit = caption.size();
            while (fit > 1 && painter.measure(caption.substr(0, fit), small).w > cell_w - 8)
            {
                --fit;
                while (fit > 1 && (static_cast<unsigned char>(caption[fit]) & 0xC0) == 0x80)
                    --fit;
            }
            auto shown = caption.substr(0, fit);
            if (line == 1 && fit < caption.size())
                shown = shown.substr(0, shown.size() > 3 ? shown.size() - 3 : shown.size()) + "…";
            painter.text({o.x + 2, o.y + thumb_h + 22 + line * 13}, shown, small);
            caption = fit < caption.size() ? caption.substr(fit) : std::string();
            caption.erase(0, caption.find_first_not_of(' '));
        }
        version_hits_.push_back({{o.x, o.y, cell_w - 4, cell_h - 4}, c.id});
    }
}
bool editor_window::versions_pointer(const pointer_event& e)
{
    if (e.type != pointer_event_type::press)
        return false;
    auto p = current();
    if (!p || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
        return false;
    for (const auto& [r, id] : version_hits_)
        if (e.pos.x >= r.x && e.pos.x < r.x + r.w && e.pos.y >= r.y && e.pos.y < r.y + r.h)
        {
            strip_focus_ = id;
            if (e.button == 3)
            {
                // Rapport has no canvas pop-up menu yet: right-click chooses
                // the version the ⋯ menu acts on.
                flash(std::format("Version {}: ⋯ above the strip shows its plan, refines, tries again or discards it.", id));
                versions_canvas_->invalidate();
            }
            else if (e.button == 1)
                show_version(p->selected_layer, id);
            return true;
        }
    return false;
}
rap::menu editor_window::version_menu()
{
    auto p = current();
    const auto* l = selected_item();
    if (!p || !l)
        return {};
    const uint32_t id = strip_focus_ && (find_version(*l, strip_focus_) || strip_focus_ == 0) ? strip_focus_ : l->version;
    const int index = p->selected_layer;
    const bool ready = !p->busy;
    return {
        {.label = std::format("Version {}", id), .enabled = false},
        {.label = "Show _plan and prompt", .action = [this, index, id] { show_record(index, id); }, .enabled = id != 0},
        {.label = "_Refine…", .action = [this, index, id] { refine_dialog(index, id, ai_task::refine); }, .enabled = ready},
        {.label = "_Try again",
         .action =
             [this, index, id]
         {
             if (auto q = current(); q && index < int(q->layers.size()) && q->layers[size_t(index)].version != id)
             {
                 // Try again acts on the current version: show this one first.
                 flash("Click the version first, then Try again.");
                 return;
             }
             review_try_again();
         },
         .enabled = ready && id != 0},
        {.label = "_Discard",
         .action =
             [this, index, id]
         {
             auto q = current();
             if (!q || q->busy || index >= int(q->layers.size()))
                 return;
             const auto item = q->layers[size_t(index)].id;
             app_.task(
                 q, std::format("Discarding version {}…", id),
                 [item, id](project& r)
                 {
                     const int i = r.find_layer(item);
                     if (i < 0 || !discard_version(r.layers[size_t(i)], id))
                         throw std::runtime_error("That version is already gone.");
                     rebuild_sheets(r);
                 },
                 {}, "Discard AI result");
         },
         .enabled = ready && id != 0},
    };
}

// -- Auto with AI ----------------------------------------------------------------
// docs/ux-design.md section 8, "Using it": the split button, one consent per
// run, progress in the guidance bar, Show plan, and the review bar at the end.


struct editor_window::consent_state
{
    std::unique_ptr<rap::window> window;
    std::shared_ptr<project> doc;
    std::shared_ptr<auto_state> run;
    std::vector<std::unique_ptr<rap::image>> thumbs;
    rap::combo* instructions = nullptr;
    rap::drop_down* until = nullptr;
    rap::spin* rounds = nullptr;
    rap::label* ceiling = nullptr;
    rap::check* remember = nullptr;
    bool started = false;
    auto_settings settings() const
    {
        auto_settings s;
        s.instructions = instructions->text();
        s.until = auto_until(std::clamp(until->current(), 0, 2));
        s.rounds = std::clamp(int(rounds->value()), 1, 5);
        return s;
    }
};
std::shared_ptr<editor_window::auto_state> editor_window::auto_of(const project* p) const
{
    const auto it = autos_.find(p);
    return it == autos_.end() ? nullptr : it->second;
}
bool editor_window::auto_running() const
{
    const auto st = auto_of(current().get());
    return st && st->running;
}
bool editor_window::auto_asking() const
{
    const auto p = current();
    const auto st = auto_of(p.get());
    return st && !st->running && st->run && st->run->stop == auto_stop::asking && p && !p->busy;
}
double editor_window::auto_elapsed() const
{
    const auto st = auto_of(current().get());
    return st && st->running ? std::chrono::duration<double>(std::chrono::steady_clock::now() - st->started).count() : 0;
}
std::string editor_window::auto_plan_now() const
{
    const auto st = auto_of(current().get());
    return st ? st->plan : std::string();
}
rap::menu editor_window::auto_menu()
{
    auto p = current();
    const bool ready = p && !p->busy && !p->layers.empty();
    bool plans = false;
    try
    {
        plans = planning_account(ai_accounts_now()) != nullptr;
    }
    catch (const std::exception&)
    {
    }
    const auto until = p ? auto_until(std::clamp(p->auto_until, 0, 2)) : auto_until::good;
    // Each item runs once the menu and its button have finished the click: a
    // dialog opened from inside it fights the popup (it flashes, reopens,
    // and the dialog waits until the menu is dismissed).
    auto weak = std::weak_ptr<void>(alive_);
    const auto later = [this, weak](std::function<void()> f)
    {
        return [this, weak, f = std::move(f)]
        {
            app_.ui.post(
                [weak, f]
                {
                    if (!weak.expired())
                        f();
                });
        };
    };
    return {
        {.label = "_Auto", .action = later([this] { auto_button(); }), .shortcut = "Ctrl+G", .enabled = ready},
        {.label = "_Local only", .action = later([this] { generate(); }), .enabled = ready},
        {.label = "_Plan only…", .action = later([this] { auto_with_ai(auto_until::plan_only); }), .enabled = ready && plans},
        {.label = "A_rrange for me…", .action = later([this] { auto_with_ai(auto_until::good, true); }), .enabled = ready && plans},
        menu_item::separator(),
        {.label = std::format("_Options… ({}, {} rounds)", auto_until_name(until), p ? p->auto_rounds : 3), .action = later([this] { auto_options(); }),
         .enabled = p && !p->busy},
    };
}
void editor_window::auto_button()
{
    auto p = current();
    if (!p || p->busy || p->layers.empty())
        return;
    ai_accounts accounts;
    try
    {
        accounts = ai_accounts_now();
    }
    catch (const std::exception&)
    {
        generate();
        return;
    }
    switch (native_ai_supported() ? auto_route_for(accounts) : auto_route::local)
    {
        case auto_route::local:
            generate();
            return;
        case auto_route::template_draw:
            reimagine();  // one drawing from the template prompt, as before a director
            return;
        case auto_route::plan_and_arrange:
            auto_with_ai(auto_until(std::clamp(p->auto_until, 0, 2)), true);
            return;
        case auto_route::loop:
            auto_with_ai(auto_until(std::clamp(p->auto_until, 0, 2)));
            return;
    }
}
void editor_window::auto_with_ai(auto_until until, bool arrange, std::string plan)
{
    auto p = current();
    if (!p || p->busy || p->layers.empty())
        return;
    ai_accounts accounts;
    try
    {
        accounts = ai_accounts_now();
    }
    catch (const std::exception& e)
    {
        app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "AI providers", .text = e.what()});
        return;
    }
    const auto* planner = planning_account(accounts);
    if (!planner)
    {
        flash("Plan only and Arrange for me need a reasoning model: Settings → AI providers → Plans and reviews with.");
        return;
    }
    auto st = std::make_shared<auto_state>();
    st->planner = *planner;
    if (const auto* d = drawing_account(accounts); d && !arrange)
        st->drawer = *d;
    st->arrange = arrange || !st->drawer;
    st->settings.rounds = std::clamp(p->auto_rounds, 1, 5);
    st->settings.until = until;
    st->settings.instructions = p->instructions;
    st->preset_plan = std::move(plan);
    if (!st->preset_plan.empty() && !st->drawer)
    {
        flash("Re-run from plan draws: choose a provider under Settings → AI providers → Draws with.");
        return;
    }
    with_ai_folder(p,
                   [this, p, st](std::filesystem::path parent)
                   {
                       st->parent = parent;
                       // Remembered for this project and these providers: no dialog.
                       if (!p->auto_consent.empty() && p->auto_consent == consent_key(st->planner, st->drawer, st->arrange))
                       {
                           start_auto(p, st, true);
                           return;
                       }
                       auto upload = std::make_shared<prepared_ai_upload>();
                       auto weak = std::weak_ptr<void>(alive_);
                       app_.task(
                           p, "Preparing Auto…", [upload](project& r) { *upload = prepare_ai_upload(r); },
                           [this, weak, p, st, upload](bool success)
                           {
                               if (success && !weak.expired())
                                   open_consent(p, st, upload);
                           });
                   });
}
void editor_window::open_consent(std::shared_ptr<project> p, std::shared_ptr<auto_state> st, std::shared_ptr<prepared_ai_upload> upload)
{
    if (consent_ && consent_->window)
        consent_->window->close();
    auto c = std::make_shared<consent_state>();
    c->doc = p;
    c->run = st;
    std::vector<raster> previews;
    previews.push_back(contain(upload->composition, 84));
    for (const auto& source : p->layers)
        if (source.visible)
            previews.push_back(contain(cropped(current_pixels(source), effective_transform(source, p->options).crop), 84));
    size_t bytes = 0;
    for (const auto& image : upload->images)
        bytes += image.png.size();
    const auto& a = st->planner;
    const auto plans = std::format("{} · {} · {}", a.name, a.reasoning_model, host_of(a.base_url));
    const auto draws = st->drawer ? std::format("{} · {} · {}", st->drawer->name, st->drawer->model, host_of(st->drawer->base_url))
                                  : std::string("Nothing draws: Offcut arranges the picture itself");
    const bool plan_only = st->settings.until == auto_until::plan_only;
    const auto title = st->arrange ? "Arrange for me with AI" : plan_only ? "Plan with AI" : "Auto with AI";
    const auto weak_state = std::weak_ptr<consent_state>(c);
    const auto update = [weak_state]
    {
        if (auto s = weak_state.lock())
            s->ceiling->set_text(auto_ceiling(s->settings(), s->run->drawer.has_value() && !s->run->arrange));
    };
    c->window = app_.ui.create(
        L::dialog{std::string(title) + " — Offcut",
                  {L::vbox{
                      L::form{
                          {"Plans and reviews", L::vbox{L::label{plans}.selectable(),
                                                        L::label{"sees: your pictures, each drawing, issue overlays and Offcut's measurements"}}},
                          {"Draws", L::vbox{L::label{draws}.selectable(),
                                            L::label{st->drawer ? "sees: your pictures, the prompt" : "nothing is drawn or uploaded for drawing"}}},
                      }
                          .align_group("consent"),
                      L::hbox{
                          L::canvas{}
                              .focusable(false)
                              .width(px(float(std::max<size_t>(1, previews.size()) * 92)))
                              .height(92_px)
                              .on_paint(
                                  [weak_state](painter& painter, size_f)
                                  {
                                      auto s = weak_state.lock();
                                      if (!s)
                                          return;
                                      float x = 0;
                                      for (const auto& t : s->thumbs)
                                      {
                                          const auto dst = fit_inside({x + 2, 2, 84, 84}, float(t->width()), float(t->height()));
                                          painter.image(*t, {0, 0, float(t->width()), float(t->height())}, dst);
                                          painter.rect(dst, 0, std::nullopt, rap::paint{theme_color::border});
                                          x += 92;
                                      }
                                  }),
                          L::label{std::format("{} {}, {} KB\nexactly what is uploaded", upload->images.size(), upload->images.size() == 1 ? "image" : "images",
                                               (bytes + 1023) / 1024)},
                      },
                      L::form{
                          {"Your instructions", L::combo{p->recent_instructions, st->settings.instructions}
                                                    .bind(c->instructions)
                                                    .tooltip("This run only; e.g. “plain sky; she is called Taco”. The director folds it into its plan.")},
                          {"Until", L::hbox{
                                        L::drop_down{{"Auto until good", "Ask after each round", "Plan only"}, int(st->settings.until)}
                                            .bind(c->until)
                                            .enabled(!st->arrange)
                                            .on_changed([update](int) { update(); }),
                                        L::label{"Rounds"},
                                        L::spin{1, 5, double(st->settings.rounds)}.bind(c->rounds).enabled(!st->arrange).on_changed([update](double) { update(); }),
                                    }},
                      }
                          .align_group("consent"),
                      L::label{}.bind(c->ceiling),
                      L::check{"Don't ask again for this project and these providers", false}.bind(c->remember),
                      L::buttons{L::dialog_button{dialog_role::ok, "Start"}, L::cancel},
                  }}}
            .size(660_px, {}),
        window_.get());
    for (const auto& preview : previews)
        if (preview.valid())
            c->thumbs.push_back(app_.ui.create_image(preview.width, preview.height, pixel_format::rgba8, preview.rgba.data(), size_t(preview.width) * 4));
    update();
    consent_ = c;
    auto weak = std::weak_ptr<void>(alive_);
    c->window->on_finished(
        [this, weak, weak_state](dialog_role role)
        {
            auto s = weak_state.lock();
            if (!s || weak.expired())
                return;
            if (role == dialog_role::ok && !s->started)
                press_start();
            else if (!s->started)
                flash("Nothing was sent.");
            if (consent_ == s)
                app_.ui.post([this, weak, s] {
                    if (!weak.expired() && consent_ == s)
                        consent_.reset();
                });
        });
    c->window->show();
}
rap::window* editor_window::consent_window() const
{
    return consent_ ? consent_->window.get() : nullptr;
}
bool editor_window::press_start(const std::string& instructions, bool remember)
{
    auto c = consent_;
    if (!c || !c->window || c->started || c->doc->busy)
        return false;
    if (!instructions.empty())
        c->instructions->set_text(instructions);
    if (remember)
        c->remember->set_checked(true);
    c->started = true;
    auto& st = *c->run;
    st.settings = c->settings();
    auto& p = *c->doc;
    p.auto_rounds = st.settings.rounds;
    if (st.settings.until != auto_until::plan_only)
        p.auto_until = int(st.settings.until);
    p.instructions = st.settings.instructions;
    remember_instruction(p, st.settings.instructions);
    p.auto_consent = c->remember->checked() ? consent_key(st.planner, st.drawer, st.arrange) : std::string();
    p.dirty = true;
    start_auto(c->doc, c->run, true);
    c->window->close();
    return true;
}
auto_roles editor_window::auto_roles_for(std::shared_ptr<project> p, std::shared_ptr<auto_state> st)
{
    auto_roles roles;
    const auto endpoint = director_endpoint_for(st->planner);
    roles.plan = [endpoint](const director_input& in, const std::filesystem::path& parent, const std::atomic_bool* cancel)
    { return director_plan(endpoint, in, utf8(parent), cancel); };
    roles.critique = [endpoint](const director_input& in, const std::filesystem::path& parent, const std::atomic_bool* cancel)
    { return director_critique(endpoint, in, utf8(parent), cancel); };
    if (st->drawer && !st->arrange)
    {
        roles.illustrator = *st->drawer;
        auto key_of = test_key_ ? test_key_ : std::function<std::string(const ai_account&)>(load_account_key);
        roles.draw = [key_of, account = *st->drawer](const prepared_ai_upload& u, const std::filesystem::path& parent, const std::atomic_bool* cancel)
        {
            std::string key;
            try
            {
                key = key_of(account);
                return request_ai_image(u, key, parent, cancel);
            }
            catch (const ai_cancelled&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error(account.name + ": " + redact_ai_secret(ai_failure_detail(e), key));
            }
        };
    }
    auto weak = std::weak_ptr<void>(alive_);
    auto doc = std::weak_ptr<project>(p);
    roles.progress = [this, weak, doc, st](const auto_progress& a)
    {
        app_.ui.post(
            [this, weak, doc, st, a]
            {
                auto q = doc.lock();
                if (weak.expired() || !q || !q->busy)
                    return;
                st->step = a.step;
                if (!a.plan.empty())
                    st->plan = a.plan;
                q->activity = "Auto · " + a.step;
                if (a.snapshot)
                {
                    // Each round's version appears in the Versions strip as it lands.
                    q->layers = a.snapshot->layers;
                    q->cuts = a.snapshot->cuts;
                    q->options = a.snapshot->options;
                    q->selected_layer = a.snapshot->selected_layer;
                    q->step = a.snapshot->step;
                    q->view = a.snapshot->view;
                }
                refresh();
            });
    };
    return roles;
}
void editor_window::start_auto(std::shared_ptr<project> p, std::shared_ptr<auto_state> st, bool fresh)
{
    if (p->busy)
        return;
    if (fresh)
    {
        auto run = std::make_shared<auto_run>();
        run->id = new_auto_id();
        run->settings = st->settings;
        run->mode = st->arrange ? auto_mode::arrange : auto_mode::full;
        run->folder = st->parent / run->id;
        if (!st->preset_plan.empty())
        {
            run->plan = st->preset_plan;
            run->compiled = validate_director_plan(run->plan, 32);
            run->next = auto_run::step::draw;
        }
        st->run = run;
        st->plan = run->plan;
    }
    st->running = true;
    st->started = std::chrono::steady_clock::now();
    st->step.clear();
    autos_[p.get()] = st;
    chips_.clear();
    following_up_ = false;
    p->ai_running = true;
    auto roles = auto_roles_for(p, st);
    auto run = st->run;
    auto weak = std::weak_ptr<void>(alive_);
    auto_timer_ = app_.ui.timer(1000, true,
                                [this, weak]
                                {
                                    if (!weak.expired())
                                        sync_guidance();
                                });
    const auto first = run->next == auto_run::step::measure_plan ? std::string("Measuring…") : std::string("Compiling the plan…");
    app_.task(
        p, "Auto · " + first,
        [run, roles](project& r)
        {
            run_auto(r, *run, roles, r.cancel.get());
            r.activity = auto_summary(r, *run);
        },
        [this, weak, p, st, run](bool success)
        {
            if (weak.expired())
                return;
            st->running = false;
            st->plan = run->plan;
            auto_timer_ = {};
            refresh();
            if (!success)
                return;
            if (run->stop == auto_stop::plan_only)
                show_plan(run->plan);
            else if (run->stop == auto_stop::failed || run->stop == auto_stop::no_drawing)
                app_.ui.message(window_.get(), {.icon = message_spec::icon::warning, .title = "Auto", .text = run->message});
            else if (run->stop == auto_stop::cancelled || run->stop == auto_stop::arranged)
                flash(p->activity);
        },
        st->arrange ? "Arrange for me" : "Auto with AI");
}
void editor_window::auto_continue()
{
    auto p = current();
    auto st = auto_of(p.get());
    if (!p || p->busy || !st || !st->run || st->run->stop != auto_stop::asking)
        return;
    start_auto(p, st, false);
}
void editor_window::auto_stop()
{
    auto p = current();
    auto st = auto_of(p.get());
    if (!p || p->busy || !st || !st->run || st->run->stop != auto_stop::asking)
        return;
    auto run = st->run;
    app_.task(
        p, "Auto · choosing the best round…",
        [run](project& r)
        {
            finish_auto(r, *run);
            r.activity = auto_summary(r, *run);
        },
        {}, "Auto: best round");
}
bool editor_window::review_through_director(const std::string& text, const std::string& chip_list, const std::string& review)
{
    // With a director, chips and reviews are critique input: it writes the
    // concrete refinement or local ops, and the loop runs one more round.
    auto p = current();
    const auto* l = selected_item();
    if (!p || p->busy || !l || !l->version)
        return false;
    const auto* v = find_version(*l, l->version);
    if (!v || v->plan.empty() || v->run.empty())
        return false;
    ai_accounts accounts;
    try
    {
        accounts = ai_accounts_now();
    }
    catch (const std::exception&)
    {
        return false;
    }
    const auto* planner = planning_account(accounts);
    if (!planner)
        return false;
    auto st = std::make_shared<auto_state>();
    st->planner = *planner;
    if (const auto* d = drawing_account(accounts))
        st->drawer = *d;
    try
    {
        st->run = std::make_shared<auto_run>(resume_auto_from(*p, l->id, v->id, text));
    }
    catch (const std::exception& e)
    {
        flash(e.what());
        return false;
    }
    if (st->run->folder.empty())
    {
        const auto folder = ai_folder(*p);
        if (!folder)
            return false;
        st->run->folder = *folder / st->run->id;
    }
    st->settings = st->run->settings;
    st->plan = st->run->plan;
    st->parent = st->run->folder.parent_path();
    // The answer is recorded on the version first, in the same undo step.
    const auto version = v->id;
    if (auto* answered = find_version(p->layers[size_t(p->selected_layer)], version))
    {
        answered->review = review;
        answered->chips = chip_list;
        answered->state = review_state::answered;
    }
    chips_.clear();
    start_auto(p, st, false);
    return true;
}
void editor_window::auto_options()
{
    auto p = current();
    if (!p || p->busy)
        return;
    struct options
    {
        std::unique_ptr<rap::window> window;
        rap::drop_down* until = nullptr;
        rap::spin* rounds = nullptr;
        rap::combo* instructions = nullptr;
    };
    auto o = std::make_shared<options>();
    auto weak = std::weak_ptr<void>(alive_);
    o->window = app_.ui.create(
        L::dialog{"Auto options — Offcut",
                  {L::vbox{
                      L::form{
                          {"Until", L::drop_down{{"Auto until good", "Ask after each round"}, std::clamp(p->auto_until, 0, 1)}.bind(o->until)},
                          {"Rounds", L::spin{1, 5, double(std::clamp(p->auto_rounds, 1, 5))}.bind(o->rounds).tooltip("Drawings per run, 1–5; 3 is the default")},
                          {"Your instructions", L::combo{p->recent_instructions, p->instructions}.bind(o->instructions)},
                      },
                      L::label{"Auto plans and reviews with the “Plans and reviews with” entry and draws with the “Draws with” entry "
                               "(Settings → AI providers). Without them it runs on this computer."}
                          .wrap(),
                      L::buttons{L::ok, L::cancel},
                  }}}
            .size(520_px, {}),
        window_.get());
    o->window->on_finished(
        [this, weak, o, p](dialog_role role) mutable
        {
            if (weak.expired())
                return;
            if (role == dialog_role::ok && !p->busy)
            {
                p->auto_until = std::clamp(o->until->current(), 0, 1);
                p->auto_rounds = std::clamp(int(o->rounds->value()), 1, 5);
                p->instructions = o->instructions->text();
                remember_instruction(*p, p->instructions);
                p->dirty = true;
                refresh();
            }
            app_.ui.post([keep = std::move(o)] {});
        });
    o->window->show();
}

// -- Show plan -------------------------------------------------------------------

namespace
{
using detail::json_value;
json_value* member(json_value& n, const std::string& key)
{
    if (n.kind != json_value::object)
        return nullptr;
    const auto it = n.members.find(key);
    return it == n.members.end() ? nullptr : &it->second;
}
std::string text_of(json_value* n)
{
    return n && n->kind == json_value::string ? n->text : std::string();
}
json_value string_value(std::string s)
{
    json_value v;
    v.kind = json_value::string;
    v.text = std::move(s);
    return v;
}
json_value null_value()
{
    json_value v;
    v.text = "null";
    return v;
}
std::array<uint8_t, 3> card_of(const std::string& hex)
{
    const auto p = parse_palette(hex);
    return p.empty() ? std::array<uint8_t, 3>{128, 128, 128} : p.front();
}
}  // namespace
struct editor_window::plan_state
{
    json_value root;
    std::weak_ptr<project> doc;
    int layer = -1;
    uint32_t version = 0;
    rap::list *sheet_list = nullptr, *feature_list = nullptr;
    rap::drop_down *kind = nullptr, *sheet = nullptr;
    rap::text_view* directions = nullptr;
    rap::label* status = nullptr;
    bool syncing = false;
    std::vector<json_value>& sheets() { return member(root, "sheets")->items; }
    std::vector<json_value>& features() { return member(root, "features")->items; }
    std::string json()
    {
        // The scene directions as edited, one per line.
        auto& d = *member(root, "scene_directions");
        d.items.clear();
        std::string text = directions ? directions->text() : std::string();
        size_t at = 0;
        while (at <= text.size())
        {
            auto end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            auto line = text.substr(at, end - at);
            line.erase(0, line.find_first_not_of(" \t\r-•"));
            line.erase(line.find_last_not_of(" \t\r") + 1);
            if (!line.empty())
                d.items.push_back(string_value(line));
            at = end + 1;
        }
        return detail::json_dump(root, {});
    }
    class sheet_rows : public rap::list_model
    {
    public:
        explicit sheet_rows(plan_state& s) : s_(s) {}
        size_t row_count() const override { return s_.sheets().size(); }
        std::string text(size_t row, size_t column) const override
        {
            auto& x = s_.sheets()[row];
            switch (column)
            {
                case 0:
                    return text_of(member(x, "card"));
                case 1:
                    return text_of(member(x, "id"));
                case 2:
                    return text_of(member(x, "role"));
                default:
                    return text_of(member(x, "holds"));
            }
        }
        rap::text_style style(size_t row, size_t column) const override
        {
            if (column != 0)
                return {};
            const auto c = card_of(text_of(member(s_.sheets()[row], "card")));
            const bool dark = c[0] * 299 + c[1] * 587 + c[2] * 114 < 128000;
            return {.fg = rap::paint{rap::color{uint8_t(dark ? 255 : 0), uint8_t(dark ? 255 : 0), uint8_t(dark ? 255 : 0), 255}},
                    .bg = rap::paint{rap::color{c[0], c[1], c[2], 255}}};
        }

    private:
        plan_state& s_;
    };
    class feature_rows : public rap::list_model
    {
    public:
        explicit feature_rows(plan_state& s) : s_(s) {}
        size_t row_count() const override { return s_.features().size(); }
        std::string text(size_t row, size_t column) const override
        {
            auto& f = s_.features()[row];
            auto* a = member(f, "arrangement");
            const auto shape = a ? text_of(member(*a, "kind")) : std::string();
            switch (column)
            {
                case 0:
                    return text_of(member(f, "label"));
                case 1:
                    return text_of(member(f, "keep"));
                case 2:
                    if (text_of(member(f, "keep")) == "drop" || !a || a->kind != json_value::object)
                        return "–";
                    if (shape == "hole")
                    {
                        std::string through;
                        if (auto* t = member(*a, "through"))
                            for (const auto& x : t->items)
                                through += (through.empty() ? "" : ", ") + x.text;
                        return "hole through " + through;
                    }
                    if (shape == "merge")
                        return "merged with " + text_of(member(*a, "merge_with"));
                    return "own sheet";
                case 3:
                    return a && a->kind == json_value::object ? text_of(member(*a, "sheet")) : std::string("–");
                case 4:
                {
                    auto* sup = member(f, "support");
                    return sup && sup->kind == json_value::object ? text_of(member(*sup, "by")) : std::string("–");
                }
                default:
                    return text_of(member(f, "simplify_to"));
            }
        }
        void changed(size_t row) { notify_changed(row, 1); }

    private:
        plan_state& s_;
    };
    std::unique_ptr<sheet_rows> sheet_model;
    std::unique_ptr<feature_rows> feature_model;
    std::unique_ptr<rap::window> window;  // last: destroyed before the models its lists show
};
rap::window* editor_window::plan_window() const
{
    return plan_ ? plan_->window.get() : nullptr;
}
void editor_window::show_plan(const std::string& plan_json, int layer, uint32_t version)
{
    auto s = std::make_shared<plan_state>();
    try
    {
        s->root = detail::json_reader(plan_json).parse();
        if (!member(s->root, "sheets") || !member(s->root, "features") || !member(s->root, "scene_directions"))
            throw std::runtime_error("This plan is incomplete.");
    }
    catch (const std::exception& e)
    {
        flash(std::string("The plan can't be shown: ") + e.what());
        return;
    }
    if (plan_ && plan_->window)
        plan_->window->close();
    s->doc = current();
    s->layer = layer;
    s->version = version;
    s->sheet_model = std::make_unique<plan_state::sheet_rows>(*s);
    s->feature_model = std::make_unique<plan_state::feature_rows>(*s);
    std::string directions, notes;
    for (const auto& d : member(s->root, "scene_directions")->items)
        directions += (directions.empty() ? "" : "\n") + d.text;
    for (const auto* key : {"avoid", "risks"})
        if (auto* a = member(s->root, key); a && !a->items.empty())
        {
            notes += std::string(notes.empty() ? "" : "\n") + (std::string(key) == "avoid" ? "Avoid: " : "Risks:\n");
            for (size_t i = 0; i < a->items.size(); ++i)
                notes += std::string(key) == "avoid" ? (i ? "; " : "") + a->items[i].text : "- " + a->items[i].text + "\n";
        }
    std::vector<std::string> sheet_names;
    for (auto& x : s->sheets())
        sheet_names.push_back(text_of(member(x, "id")) + " · " + text_of(member(x, "role")));
    const auto weak_state = std::weak_ptr<plan_state>(s);
    // The selected feature's form follows the list; edits change the plan.
    const auto selected = [weak_state]() -> json_value*
    {
        auto st = weak_state.lock();
        if (!st || !st->feature_list)
            return nullptr;
        const auto row = st->feature_list->current();
        return row && *row < st->features().size() ? &st->features()[*row] : nullptr;
    };
    const auto fill = [weak_state, selected]
    {
        auto st = weak_state.lock();
        if (!st)
            return;
        auto* f = selected();
        auto* a = f ? member(*f, "arrangement") : nullptr;
        const bool editable = a && a->kind == json_value::object && text_of(member(*f, "keep")) != "drop";
        st->syncing = true;
        st->kind->set_enabled(editable);
        st->sheet->set_enabled(editable);
        if (editable)
        {
            const auto kind = text_of(member(*a, "kind"));
            st->kind->set_current(kind == "hole" ? 1 : kind == "merge" ? 2 : 0);
            const auto sheet = text_of(member(*a, "sheet"));
            for (size_t i = 0; i < st->sheets().size(); ++i)
                if (text_of(member(st->sheets()[i], "id")) == sheet)
                    st->sheet->set_current(int(i));
        }
        st->syncing = false;
    };
    const auto edit = [weak_state, selected]
    {
        auto st = weak_state.lock();
        if (!st || st->syncing)
            return;
        auto* f = selected();
        auto* a = f ? member(*f, "arrangement") : nullptr;
        if (!a || a->kind != json_value::object)
            return;
        const int k = st->kind->current(), sheet = std::clamp(st->sheet->current(), 0, int(st->sheets().size()) - 1);
        const auto sheet_id = text_of(member(st->sheets()[size_t(sheet)], "id"));
        a->members["kind"] = string_value(k == 1 ? "hole" : k == 2 ? "merge" : "sheet");
        a->members["sheet"] = string_value(sheet_id);
        // A hole cuts the sheets in front of the one it shows; a merge needs a neighbour.
        json_value through;
        through.kind = json_value::array;
        if (k == 1)
            for (int i = 0; i < sheet; ++i)
                through.items.push_back(string_value(text_of(member(st->sheets()[size_t(i)], "id"))));
        a->members["through"] = through;
        if (k == 2)
        {
            if (text_of(member(*a, "merge_with")).empty())
                for (auto& other : st->features())
                    if (&other != f && text_of(member(other, "keep")) != "drop")
                    {
                        a->members["merge_with"] = string_value(text_of(member(other, "id")));
                        break;
                    }
        }
        else
            a->members["merge_with"] = null_value();
        st->status->set_text(k == 1 && sheet == 0 ? "A hole shows a deeper sheet: choose one behind the front." : "Edited: Re-run from plan draws from it.");
        st->feature_model->changed(*st->feature_list->current());
    };
    const auto summary = text_of(member(s->root, "summary"));
    s->window = app_.ui.create(
        L::dialog{"Plan — Offcut",
                  {L::vbox{
                      L::label{summary}.wrap().selectable(),
                      L::label{"Sheets, near to far"},
                      L::list{{{"Card", 9_ch}, {"Sheet", 6_ch}, {"Role", 9_ch}, {"Holds", {}, true}}, s->sheet_model.get()}
                          .selection(selection_mode::none)
                          .compact()
                          .height(px(float(32 + 25 * std::min<size_t>(6, s->sheets().size())))),
                      L::label{"Features"},
                      L::list{{{"Feature", 26_ch}, {"Keep", 8_ch}, {"Shown as", 18_ch}, {"Sheet", 6_ch}, {"Held by", 9_ch}, {"Simplify to", {}, true}},
                              s->feature_model.get()}
                          .bind(s->feature_list)
                          .selection(selection_mode::single)
                          .compact()
                          .height(px(float(32 + 25 * std::clamp<size_t>(s->features().size(), 3, 8))))
                          .on_selection_changed(fill),
                      L::form{
                          {"Selected feature", L::hbox{
                                                   L::drop_down{{"Own sheet", "Hole", "Merged with a neighbour"}, 0}
                                                       .bind(s->kind)
                                                       .enabled(false)
                                                       .on_changed([edit](int) { edit(); }),
                                                   L::label{"on sheet"},
                                                   L::drop_down{sheet_names, 0}.bind(s->sheet).enabled(false).on_changed([edit](int) { edit(); }),
                                               }},
                      },
                      L::label{"Scene directions, one per line: the illustrator draws from these"},
                      L::text_view{directions}.height(7_lines).bind(s->directions),
                      L::text_view{notes}.read_only().height(5_lines),
                      L::label{" "}.bind(s->status).ellipsize(),
                      L::hbox{
                          L::button{"Copy"}.tooltip("The plan as text, then as JSON").on_click(
                              [this, weak_state]
                              {
                                  if (auto st = weak_state.lock())
                                  {
                                      const auto json = st->json();
                                      app_.ui.set_clipboard(plan_text(json) + "\n" + json);
                                      st->status->set_text("Copied.");
                                  }
                              }),
                          L::button{"Prompt and record…"}.tooltip("What was sent for this version, and its record").enabled(version != 0).on_click(
                              [this, weak_state]
                              {
                                  if (auto st = weak_state.lock())
                                      show_record_details(st->layer, st->version);
                              }),
                          L::spacer{},
                          L::button{"Re-run from plan"}
                              .tooltip("Compile and draw from this plan as edited, without analysing again")
                              .on_click(
                                  [this, weak_state]
                                  {
                                      auto st = weak_state.lock();
                                      if (!st)
                                          return;
                                      const auto json = st->json();
                                      try
                                      {
                                          validate_director_plan(json, 32);
                                      }
                                      catch (const std::exception& e)
                                      {
                                          st->status->set_text(std::string("Can't draw from this plan: ") + e.what());
                                          return;
                                      }
                                      auto p = current();
                                      if (!p || p->busy)
                                      {
                                          st->status->set_text("Wait for the current job to finish.");
                                          return;
                                      }
                                      st->window->close();
                                      auto_with_ai(auto_until(std::clamp(p->auto_until, 0, 1)), false, json);
                                  }),
                          L::button{"Close"}.on_click(
                              [weak_state]
                              {
                                  if (auto st = weak_state.lock())
                                      st->window->close();
                              }),
                      },
                  }}}
            .size(900_px, {})
            .modeless(),
        window_.get());
    plan_ = s;
    auto weak = std::weak_ptr<void>(alive_);
    s->window->on_finished(
        [this, weak, weak_state](dialog_role)
        {
            auto st = weak_state.lock();
            if (weak.expired() || !st)
                return;
            app_.ui.post([this, weak, st] {
                if (!weak.expired() && plan_ == st)
                    plan_.reset();
            });
        });
    s->window->show();
}
}  // namespace offcut
