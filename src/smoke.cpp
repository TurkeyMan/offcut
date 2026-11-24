#include "smoke.h"
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>
#include "ai_provider.h"
#include "ai_workflow.h"
#include "export.h"
#include "history.h"
#include "image_io.h"
#include "settings.h"
#include "transform.h"
#include "versions.h"
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
                 result.layers = {read_image(image)};
                 result.options = new_project_options(result.layers.front().image);
                 auto& l = result.layers.front();
                 l.transform = place_image(l, result.options, result.options.placement, result.options.touch_edges);
                 result.assign_layer_ids();
                 result.selected_layer = 0;
                 result.cuts = simplify(result.layers, result.options);
                 if (result.options.placement == placement_mode::touch_edges)
                     require(!frame_only(result.cuts.front(), result.options), "Placed subject vanished from the front layer");
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
                                    require(p->layers.size() == 1 && p->cuts.size() == 7, "Image job failed or result was lost during detach");
                                    require(p->zoom == 1.2f && p->pan_x == 3, "View state lost during generation");
                                    require(app.receive(original, {.id = id.id, .index = 0, .source = detached}), "Cross-window tab receive failed");
                                    require(original.count() == 2, "Transferred tab missing");
                                    auto moved = original.take(id.id);
                                    require(moved && moved->document == p, "Transfer changed document identity");
                                    original.insert(*moved);
                                    save_project(*p, output / "smoke.offcut");
                                    const auto loaded = load_project(output / "smoke.offcut");
                                    require(loaded.layers.front().image.rgba == p->layers.front().image.rgba, "Embedded source changed on reopen");
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
                                else if (*stage == 1)
                                {
                                    // The editor: drag the image on the canvas, undo it, and
                                    // tick a sheet without losing the selected one.
                                    original.set_stage(work_step::arrange);
                                    original.select_layer(0);
                                    require(p->selected_layer == 0 && p->step == work_step::arrange && p->view == view_mode::original,
                                            "Selecting an image did not show Arrange");
                                    const auto before = p->layers[0].transform;
                                    const auto from = original.screen_from_frame({before.x_mm, before.y_mm});
                                    const auto to = original.screen_from_frame({before.x_mm + 10, before.y_mm + 5});
                                    original.pointer({.type = rap::pointer_event_type::press, .pos = from, .button = 1});
                                    original.pointer({.type = rap::pointer_event_type::move, .pos = {(from.x + to.x) / 2, (from.y + to.y) / 2}, .button = 1});
                                    original.pointer({.type = rap::pointer_event_type::move, .pos = to, .button = 1, .modifiers = rap::mod_alt});
                                    original.pointer({.type = rap::pointer_event_type::release, .pos = to, .button = 1});
                                    const auto moved = p->layers[0].transform;
                                    require(std::abs(moved.x_mm - before.x_mm - 10) < .5 && std::abs(moved.y_mm - before.y_mm - 5) < .5,
                                            "Dragging the image did not move it");
                                    require(p->stale && p->dirty && p->undo && p->undo->undo_label() == "Move image", "The move was not an undo step");
                                    original.undo();
                                    require(p->layers[0].transform == before && !p->stale, "Undo did not put the image back");
                                    original.redo();
                                    require(p->layers[0].transform == moved, "Redo did not move it again");
                                    original.undo();
                                    original.set_stage(work_step::sheets);
                                    original.select_sheet(2);
                                    require(p->view == view_mode::assembled && p->selected_cut == 2, "Selecting a sheet left the Assembled view");
                                    p->cuts[1].visible = false;
                                    original.refresh();
                                    require(p->selected_cut == 2 && p->view == view_mode::assembled, "Unticking a sheet lost the selection");
                                    p->cuts[1].visible = true;
                                    original.refresh();
                                    *stage = 2;
                                }
                                else
                                {
                                    require(app.ui.save_snapshot(original.native(), utf8(output / "workspace.png")), "Native snapshot failed");
                                    const auto snapshot = read_image(output / "workspace.png");
                                    require(std::ranges::any_of(snapshot.image.rgba, [](uint8_t value) { return value > 0 && value < 255; }),
                                            "Native snapshot was blank");
                                    std::ofstream(output / "result.txt")
                                        << "PASS: image decoding, queued transform, detach during job, cross-window receive, preserved document identity/view, "
                                           "portable save/reopen, numbered SVG/PNG exports, PNG decoding, mask reimport, AI request export, canvas drag with undo/redo, "
                                           "and sheet selection and ticks that keep the Assembled view.\n";
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
rap::subscription ai_smoke_test(application& app, const std::string& endpoint, const std::filesystem::path& image, const std::filesystem::path& output)
{
    const auto url = split_ai_url(endpoint);
    const auto loopback = [](const std::string& host) { return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1"; };
    require(loopback(url.host), "AI smoke test requires a loopback endpoint");
    std::filesystem::create_directories(output);
    auto p = std::make_shared<project>();
    p->name = "Native AI mock round trip";
    p->layers = {read_image(image), read_image(image)};
    p->options = new_project_options(p->layers.front().image);
    for (auto& l : p->layers)
        l.transform = place_image(l, p->options, p->options.placement, p->options.touch_edges);
    p->layers[0].notes = "Taco, a tortoise with a taco shell";
    p->ai_style = "bold folk-art shapes";
    p->assign_layer_ids();
    p->options.layers = 4;
    auto& window = app.open(p);
    // A keyless OpenAI entry at the loopback mock, as Settings adds one: GET
    // models, then the newest GPT Image model. Never reads a credential.
    const auto& openai = find_ai_provider("openai");
    ai_account account{"smoke", "openai", "Mock OpenAI", endpoint, {}, {}, {}};
    account.models = list_ai_models(openai, endpoint, "");
    account.model = default_ai_model(openai, account.models);
    require(account.model.starts_with("gpt-image"), "OpenAI model verification failed");
    ai_accounts accounts;
    accounts.default_id = "smoke";
    accounts.entries = {account};
    // The Auto loop's director: an OpenAI-compatible entry at the same mock
    // (Chat Completions), keyless, so no credential is read.
    ai_account director{"smoke-director", "local", "Mock director", endpoint, {}, {}, {}, {}, "mock-director"};
    accounts.entries.push_back(director);
    accounts.plans_id = director.id;
    window.set_ai_test_hooks(
        accounts,
        [loopback](const ai_account& a)
        {
            require(loopback(split_ai_url(a.base_url).host), "The smoke test only sends to loopback");
            return std::string();
        },
        output);
    auto stage = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    auto mid = std::make_shared<bool>(false);
    const auto item = [p]() -> layer&
    {
        for (auto& l : p->layers)
            if (l.type == layer_type::ai_illustration)
                return l;
        throw std::runtime_error("Reimagine made no AI illustration item: " + p->activity);
    };
    return app.ui.timer(300, true,
                        [&app, &window, p, output, stage, ticks, item, mid]
                        {
                            try
                            {
                                // Progress mid-run, once: the guidance bar while Auto reviews.
                                if (p->busy && *stage == 10 && !*mid && window.auto_running() && p->activity.contains("Reviewing"))
                                {
                                    *mid = true;
                                    require(window.guidance().starts_with("Auto · Reviewing…") && window.guidance().contains(" · 0:"),
                                            "Auto's progress is not in the guidance bar");
                                    require(app.ui.save_snapshot(window.native(), utf8(output / "auto-progress.png")), "Progress snapshot failed");
                                }
                                if (p->busy)
                                    return;
                                switch (*stage)
                                {
                                    case 0:
                                        window.native().show();
                                        window.reimagine();  // the AI menu's Reimagine as shadowbox…
                                        ++*stage;
                                        return;
                                    case 1:
                                    {
                                        auto* dialog = window.send_dialog_window();
                                        if (!dialog)
                                        {
                                            require(++*ticks < 40, "Send to AI did not open");
                                            return;
                                        }
                                        if (++*ticks < 4)  // let it paint
                                            return;
                                        require(app.ui.save_snapshot(*dialog, utf8(output / "send-dialog.png")), "Send dialog snapshot failed");
                                        require(window.press_send("plain sky, a hill running off both sides"), "Send did not send");
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    }
                                    case 2:
                                    {
                                        // Reimagine: a new item over the hidden sources, version 1 pending review.
                                        auto& l = item();
                                        require(l.versions.size() == 1 && l.version == 1 && l.versions[0].state == review_state::pending &&
                                                    std::ranges::none_of(p->layers, [&](const layer& s) { return s.id != l.id && s.visible; }) &&
                                                    p->layers.size() == 3 && p->cuts.size() == 4,
                                                "Reimagine did not add version 1 for review over hidden sources");
                                        const auto& v = l.versions[0];
                                        require(v.instructions == "plain sky, a hill running off both sides" && v.style == "bold folk-art shapes" &&
                                                    v.prompt.contains("Instructions for this request: plain sky") && v.prompt_files.contains("reimagine.txt@") &&
                                                    v.model.starts_with("gpt-image") && v.issues >= 0 && !v.request.empty() && !v.run.empty() &&
                                                    p->recent_instructions.front() == v.instructions,
                                                "Version 1 record incomplete");
                                        require(window.reviewing() && window.review_line().starts_with("Result 1"), "Review bar not shown");
                                        if (++*ticks < 3)
                                            return;
                                        require(app.ui.save_snapshot(window.native(), utf8(output / "review.png")), "Review snapshot failed");
                                        window.toggle_chip("support.added-on");
                                        require(window.review_line().contains("Pending: support looks added on"), "Chip did not become the pending instruction");
                                        require(app.ui.save_snapshot(window.native(), utf8(output / "review-chip.png")), "Review chip snapshot failed");
                                        window.review_send();
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    }
                                    case 3:
                                    {
                                        // Refine: version 2 from 1; 1 records the answer as words and chip ids.
                                        auto& l = item();
                                        require(l.versions.size() == 2 && l.version == 2 && l.versions[1].parent == 1, "Refine did not add version 2");
                                        const auto& v1 = l.versions[0];
                                        const auto& v2 = l.versions[1];
                                        require(v1.state == review_state::answered && v1.review == "support looks added on" && v1.chips == "support.added-on",
                                                "The chip answer was not recorded on version 1");
                                        require(v2.task == "Refine" && v2.prompt.contains("The supports look added on") &&
                                                    v2.prompt.contains("Already done in earlier rounds, keep it: reimagined as a shadowbox") &&
                                                    v2.run == v1.run && v2.round == 2,
                                                "Version 2's prompt or run is wrong");
                                        window.review_try_again();
                                        ++*stage;
                                        return;
                                    }
                                    case 4:
                                    {
                                        // Try again: a sibling of 2, from 1, with 2's own prompt.
                                        auto& l = item();
                                        require(l.versions.size() == 3 && l.versions[2].parent == 1 && l.versions[2].prompt == l.versions[1].prompt &&
                                                    l.versions[1].review == "try again",
                                                "Try again did not make a sibling");
                                        window.show_version(p->find_layer(l.id), 2);  // the strip: click version 2
                                        ++*stage;
                                        return;
                                    }
                                    case 5:
                                    {
                                        auto& l = item();
                                        require(l.version == 2, "Clicking a version did not make it current");
                                        window.refine_dialog(p->find_layer(l.id), 2, ai_task::refine);  // ✦ Refine… on an older version
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    }
                                    case 6:
                                        if (!window.send_dialog_window())
                                        {
                                            require(++*ticks < 40, "Refine's Send to AI did not open");
                                            return;
                                        }
                                        require(window.press_send("simpler fur"), "Refine's Send did not send");
                                        ++*stage;
                                        return;
                                    case 7:
                                    {
                                        // A branch: 4 refines 2 after 3 arrived.
                                        auto& l = item();
                                        require(l.versions.size() == 4 && l.versions[3].parent == 2 && l.version == 4, "Refining an older version did not branch");
                                        const auto cells = version_layout(l);
                                        require(std::ranges::find(cells, 4u, &version_cell::id)->row == 1, "The branch is not on a row of its own");
                                        window.review_use();
                                        require(l.versions[3].state == review_state::used && !window.reviewing() && p->undo->undo_label() == "Use AI result",
                                                "Use this did not record the answer");
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    }
                                    case 8:
                                        if (++*ticks < 3)
                                            return;
                                        require(app.ui.save_snapshot(window.native(), utf8(output / "versions.png")), "Versions snapshot failed");
                                        save_project(*p, output / "mock.offcut");
                                        {
                                            const auto back = load_project(output / "mock.offcut");
                                            const auto& l = back.layers.back();
                                            require(l.versions.size() == 4 && l.version == 4 && l.versions[0].chips == "support.added-on" &&
                                                        l.versions[3].state == review_state::used,
                                                    "Versions did not survive save and reopen");
                                        }
                                        export_layers(*p, output);
                                        // Auto with AI: the split button's Auto, with a director and an illustrator.
                                        window.auto_button();
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    case 9:
                                    {
                                        auto* dialog = window.consent_window();
                                        if (!dialog)
                                        {
                                            require(++*ticks < 40, "Auto's consent dialog did not open");
                                            return;
                                        }
                                        if (++*ticks < 4)
                                            return;
                                        require(app.ui.save_snapshot(*dialog, utf8(output / "auto-consent.png")), "Consent snapshot failed");
                                        require(window.press_start("plain sky; she is called Taco"), "Start did not start");
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    }
                                    case 10:
                                    {
                                        // plan → draw → critique fix_local → Local fix → critique refine → draw → critique pass.
                                        const auto& l = p->layers.back();
                                        require(l.type == layer_type::ai_illustration && l.versions.size() == 3, ("Auto did not make three versions: " + p->activity).c_str());
                                        const auto& v1 = l.versions[0];
                                        const auto& v2 = l.versions[1];
                                        const auto& v3 = l.versions[2];
                                        require(v1.run == v2.run && v2.run == v3.run && !v1.run.empty() && v1.round == 1 && v2.round == 1 && v3.round == 2,
                                                "Auto's versions are not one run in two rounds");
                                        require(v2.task == "Local fix" && v2.parent == 1 && v2.ops.contains("set_card") && v2.palette.contains("#e0a030"),
                                                "The local fix did not apply the verdict's set_card");
                                        require(v3.task == "Refine" && v3.parent == 2 && v3.prompt.starts_with("Change the first attached image") &&
                                                    v3.prompt.contains("The aubergine bank runs off both sides"),
                                                "The refinement was not compiled from the verdict");
                                        require(v1.prompt.contains("honey gold") && v1.palette == "#46283f #e7a236 #54863a #e8f4f5" && !v1.plan.empty() &&
                                                    v1.director.input_tokens >= 2400 && v1.verdict.contains("fix_local"),
                                                "Version 1 was not drawn from the plan, or its record is incomplete");
                                        require(v3.verdict.contains("\"pass\"") && v3.explanation.starts_with("Sun-free sky"), "The pass was not recorded");
                                        require(*mid, "No progress was seen mid-run");
                                        require(window.reviewing() && window.review_line().starts_with("Auto · best of 2"), ("Review bar after Auto: " + window.review_line()).c_str());
                                        if (++*ticks < 3)
                                            return;
                                        require(app.ui.save_snapshot(window.native(), utf8(output / "auto-review.png")), "Auto review snapshot failed");
                                        window.show_record(p->find_layer(l.id), l.version);  // Show plan
                                        *ticks = 0;
                                        ++*stage;
                                        return;
                                    }
                                    case 11:
                                    {
                                        auto* dialog = window.plan_window();
                                        if (!dialog)
                                        {
                                            require(++*ticks < 40, "Show plan did not open");
                                            return;
                                        }
                                        if (++*ticks < 4)
                                            return;
                                        require(app.ui.save_snapshot(*dialog, utf8(output / "auto-plan.png")), "Plan snapshot failed");
                                        {
                                            // The Auto thread, for reading: each version's task, round, score and findings.
                                            std::ofstream thread(output / "auto-thread.txt");
                                            for (const auto& v : p->layers.back().versions)
                                                thread << std::format("v{} {} round {} parent {} · score {} · issues {} · {}\n  ops: {}\n  explanation: {}\n", v.id,
                                                                      v.task, v.round, v.parent, v.score, v.issues, v.findings, v.ops, v.explanation);
                                        }
                                        std::ofstream(output / "result.txt")
                                            << "PASS: Send to AI (snapshot), Reimagine to a new AI illustration over hidden sources, review bar, chip → "
                                               "pending instruction → Refine (answer recorded as words and ids), Try again as a sibling, Versions strip "
                                               "click, Refine of an older version as a branch, Use this, save/reopen, export; Auto with AI: consent, plan, "
                                               "draw, critique fix_local → Local fix, critique refine → draw, critique pass, review bar, Show plan; all "
                                               "through the loopback mock.\n"
                                            << p->activity << '\n';
                                        app.ui.quit();
                                        return;
                                    }
                                }
                            }
                            catch (const std::exception& e)
                            {
                                std::ofstream(output / "result.txt") << "FAIL (stage " << *stage << "): " << e.what() << '\n';
                                app.ui.quit(1);
                            }
                        });
}
rap::subscription ai_live_run(application& app, const std::string& account_name, const std::filesystem::path& image,
                              const std::filesystem::path& output, const std::string& instructions)
{
    const auto accounts = configured_ai_accounts();
    const ai_account* chosen = accounts.find(account_name == "default" ? accounts.default_id : account_name);
    if (!chosen)
        throw std::runtime_error("No AI provider entry \"" + account_name + "\" in Settings.");
    const ai_account account = *chosen;
    std::filesystem::create_directories(output);
    auto p = std::make_shared<project>();
    p->name = "Live AI run";
    p->layers = {read_image(image)};
    p->options = new_project_options(p->layers.front().image);
    for (auto& l : p->layers)
        l.transform = place_image(l, p->options, p->options.placement, p->options.touch_edges);
    p->assign_layer_ids();
    p->options.layers = 4;
    p->instructions = instructions;
    auto& window = app.open(p);
    p->ai_running = true;
    auto folder = std::make_shared<std::filesystem::path>();
    auto model = std::make_shared<std::string>();
    app.task(p, "Waiting for " + account.model + "…",
             [account = ai_account(account), output, folder, model](project& r) mutable
             {
                 const auto key = load_account_key(account);
                 if (const char* m = std::getenv("OFFCUT_AI_MODEL"); m && *m)  // try another model without editing Settings
                     account.model = m;
                 if (account.model.empty())  // a recovered entry before Settings has listed its models
                 {
                     const auto& provider = find_ai_provider(account.provider);
                     account.model = default_ai_model(provider, list_ai_models(provider, account.base_url, key, r.cancel.get()));
                 }
                 auto upload = prepare_ai_upload(r);
                 upload.endpoint = account_image_endpoint(account, upload.frame_width_mm, upload.frame_height_mm);
                 if (const char* raw = std::getenv("OFFCUT_AI_RAW_PROMPT"); raw && *raw)  // experiments: send exactly this text
                 {
                     upload.prompt = raw;
                     upload.images.resize(1);
                 }
                 *model = upload.endpoint.model;
                 *folder = run_ai_roundtrip(r, upload, key, output, r.cancel.get());
             });
    auto stage = std::make_shared<int>(0);
    return app.ui.timer(300, true,
                        [&app, &window, p, output, folder, model, account, stage]
                        {
                            if (p->busy)
                                return;
                            try
                            {
                                if (folder->empty() || p->cuts.empty())
                                    throw std::runtime_error(p->activity);
                                if ((*stage)++ == 0)
                                {
                                    save_project(*p, output / "live.offcut");
                                    export_layers(*p, output);
                                    window.native().show();
                                    return;
                                }
                                app.ui.save_snapshot(window.native(), utf8(output / "workspace.png"));
                                std::ofstream(output / "result.txt") << "OK: " << account.name << " · " << *model << "\nRequest: " << utf8(*folder) << '\n'
                                                                     << p->activity << '\n';
                                app.ui.quit();
                            }
                            catch (const std::exception& e)
                            {
                                std::ofstream(output / "result.txt") << "FAIL: " << e.what() << '\n';
                                app.ui.quit(1);
                            }
                        });
}
}  // namespace offcut
namespace offcut
{
namespace
{
// A second picture for scenes: a dark tree on a transparent background.
raster sample_tree()
{
    raster r{240, 300, std::vector<uint8_t>(size_t(240) * 300 * 4, 0)};
    const auto put = [&](int x, int y, uint8_t g)
    {
        const size_t i = (size_t(y) * r.width + x) * 4;
        r.rgba[i] = uint8_t(g / 2);
        r.rgba[i + 1] = g;
        r.rgba[i + 2] = uint8_t(g / 3);
        r.rgba[i + 3] = 255;
    };
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x)
        {
            const double dx = x - 120, dy = y - 110;
            if (dx * dx / (95. * 95) + dy * dy / (90. * 90) < 1)
                put(x, y, uint8_t(70 + (x * 7 + y * 3) % 40));
            else if (x >= 105 && x < 135 && y >= 150)
                put(x, y, 40);
        }
    return r;
}
}  // namespace
rap::subscription scene_snapshot(application& app, const std::string& scene, std::vector<std::filesystem::path> images, const std::filesystem::path& out)
{
    require(scene == "arrange" || scene == "sheets" || scene == "crop", "Scenes: arrange, sheets, crop");
    std::filesystem::create_directories(out.parent_path());
    auto p = std::make_shared<project>();
    p->name = "Shadowbox scene";
    if (images.size() < 2)
    {
        const auto tree = out.parent_path() / "generated-tree.png";
        write_png(sample_tree(), tree);
        images.push_back(tree);
    }
    for (const auto& path : images)
        p->layers.push_back(read_image(path));
    p->options = new_project_options(p->layers.front().image);
    p->options.layers = 5;
    p->options.connect = false;  // keep floating pieces, to show them in red
    for (auto& l : p->layers)
        l.transform = place_image(l, p->options, placement_mode::fit, 0);
    p->assign_layer_ids();
    auto& window = app.open(p);
    auto stage = std::make_shared<int>(0);
    return app.ui.timer(400, true,
                        [&app, &window, p, scene, out, stage]
                        {
                            try
                            {
                                if (p->busy)
                                    return;
                                if (*stage == 0)
                                {
                                    window.native().show();
                                    ++*stage;
                                    return;
                                }
                                if (*stage == 1)
                                {
                                    // Arrange both images with the pointer, as a user would.
                                    const auto drag = [&](point_mm from, point_mm to, unsigned modifiers = 0)
                                    {
                                        const auto a = window.screen_from_frame(from), b = window.screen_from_frame(to);
                                        window.pointer({.type = rap::pointer_event_type::press, .pos = a, .button = 1, .modifiers = modifiers});
                                        for (int i = 1; i <= 4; ++i)
                                            window.pointer({.type = rap::pointer_event_type::move,
                                                            .pos = {a.x + (b.x - a.x) * i / 4, a.y + (b.y - a.y) * i / 4},
                                                            .button = 1,
                                                            .modifiers = modifiers});
                                        window.pointer({.type = rap::pointer_event_type::release, .pos = b, .button = 1, .modifiers = modifiers});
                                    };
                                    // The tree: select it by clicking it, scale it down, sink it into the
                                    // bottom border, then turn it slightly with the rotation handle.
                                    const auto o = opening_mm(p->options);
                                    auto t = p->layers[1].transform;
                                    window.pointer({.type = rap::pointer_event_type::press, .pos = window.screen_from_frame({t.x_mm, t.y_mm}), .button = 1});
                                    window.pointer({.type = rap::pointer_event_type::release, .pos = window.screen_from_frame({t.x_mm, t.y_mm}), .button = 1});
                                    require(p->selected_layer == 1, "Clicking the tree did not select it");
                                    auto c = corners(t);
                                    drag(c[0], {c[0].x + (c[2].x - c[0].x) * .45, c[0].y + (c[2].y - c[0].y) * .45}, rap::mod_alt);
                                    t = p->layers[1].transform;
                                    drag({t.x_mm, t.y_mm}, {o.right - width_mm(t) / 2 - 12, o.bottom - height_mm(t) / 2 + 8}, rap::mod_alt);
                                    t = p->layers[1].transform;
                                    const auto top = window.screen_from_frame({t.x_mm, t.y_mm - height_mm(t) / 2});
                                    const rap::point_f knob{top.x, top.y - 28};
                                    const auto centre = window.screen_from_frame({t.x_mm, t.y_mm});
                                    const double a = 8 * 3.14159265358979 / 180;
                                    const rap::point_f turned{float(centre.x + (knob.x - centre.x) * std::cos(a) - (knob.y - centre.y) * std::sin(a)),
                                                              float(centre.y + (knob.x - centre.x) * std::sin(a) + (knob.y - centre.y) * std::cos(a))};
                                    window.pointer({.type = rap::pointer_event_type::press, .pos = knob, .button = 1});
                                    window.pointer({.type = rap::pointer_event_type::move, .pos = turned, .button = 1});
                                    window.pointer({.type = rap::pointer_event_type::release, .pos = turned, .button = 1});
                                    require(std::abs(p->layers[1].transform.rotation_deg - 8) < 1, "The rotation handle did not turn the tree");
                                    // The first image: shrink it from its bottom-right corner, then move it up and left.
                                    window.select_layer(0);
                                    t = p->layers[0].transform;
                                    c = corners(t);
                                    drag(c[2], {c[0].x + (c[2].x - c[0].x) * .62, c[0].y + (c[2].y - c[0].y) * .62}, rap::mod_alt);
                                    t = p->layers[0].transform;
                                    // Up, and into the left border so it is supported.
                                    {
                                        auto probe = p->layers[0];
                                        probe.transform.y_mm -= 34;
                                        const auto target = touch(probe, p->options, touch_left);
                                        drag({t.x_mm, t.y_mm}, {target.x_mm, target.y_mm}, rap::mod_alt);
                                    }
                                    require(p->selected_layer == 0 && bounds(p->layers[0].transform).left < o.left, "Dragging the first image did not move it");
                                    // The tree again, to show its handles.
                                    window.select_layer(1);
                                    require(p->undo && p->undo->size() >= 6, "Canvas edits were not undo steps");
                                    if (scene == "sheets")
                                    {
                                        app.task(
                                            p, "Making sheets…",
                                            [](project& r)
                                            {
                                                r.cuts = simplify(r.layers, r.options);
                                                r.stale = false;
                                            },
                                            {}, "Auto");
                                        *stage = 2;
                                        return;
                                    }
                                    if (scene == "crop")
                                    {
                                        // C, then drag the top edge down and the right edge in.
                                        window.key({.press = true, .key = rap::key::character, .codepoint = 'c'});
                                        t = p->layers[1].transform;
                                        const auto edge_top = corners(t);
                                        const point_mm top_mid{(edge_top[0].x + edge_top[1].x) / 2, (edge_top[0].y + edge_top[1].y) / 2};
                                        const point_mm right_mid{(edge_top[1].x + edge_top[2].x) / 2, (edge_top[1].y + edge_top[2].y) / 2};
                                        drag(top_mid, {top_mid.x, top_mid.y + 25});
                                        drag(right_mid, {right_mid.x - 18, right_mid.y});
                                        require(p->layers[1].transform.crop == t.crop, "The crop applied before Enter");
                                    }
                                    *stage = 3;
                                    return;
                                }
                                if (*stage == 2)
                                {
                                    window.set_stage(work_step::sheets);
                                    // Untick sheet 2; highlight sheet 3 in the stack.
                                    p->cuts[1].visible = false;
                                    window.refresh();
                                    window.select_sheet(2);
                                    require(p->view == view_mode::assembled, "Selecting a sheet left the Assembled view");
                                    *stage = 3;
                                    return;
                                }
                                require(app.ui.save_snapshot(window.native(), utf8(out)), "Snapshot failed");
                                std::ofstream(out.parent_path() / (scene + "-guidance.txt")) << window.guidance() << '\n'
                                                                                      << std::format("image 1: {:.1f} {:.1f} {:.4f} {:.1f}°; image 2: {:.1f} {:.1f} {:.4f} {:.1f}°\n",
                                                                                                     p->layers[0].transform.x_mm, p->layers[0].transform.y_mm,
                                                                                                     p->layers[0].transform.mm_per_pixel, p->layers[0].transform.rotation_deg,
                                                                                                     p->layers[1].transform.x_mm, p->layers[1].transform.y_mm,
                                                                                                     p->layers[1].transform.mm_per_pixel, p->layers[1].transform.rotation_deg);
                                app.ui.quit();
                            }
                            catch (const std::exception& e)
                            {
                                std::ofstream(out.parent_path() / (scene + "-result.txt")) << "FAIL: " << e.what() << '\n';
                                app.ui.quit(1);
                            }
                        });
}
}  // namespace offcut
