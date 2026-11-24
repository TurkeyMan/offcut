#include "window.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numeric>
#include <stdexcept>
#include "ai_workflow.h"
#include "export.h"
#include "history.h"
#include "image_io.h"
#include "settings.h"
#include "window_parts.h"

namespace offcut
{
namespace L = rap::layout;
using namespace rap;

std::shared_ptr<project> editor_window::current() const
{
    for (const auto& t : tabs_)
        if (t.id == selected_)
            return t.document;
    return {};
}
namespace
{
std::vector<std::string> preset_names()
{
    std::vector<std::string> names;
    for (const auto& size : frame_sizes())
        names.emplace_back(size.name);
    return names;
}
struct setup_dialog
{
    std::unique_ptr<rap::window> window;
    transform_options options;
    rap::drop_down* preset = nullptr;
    rap::tool_button *portrait = nullptr, *landscape = nullptr;
    rap::spin *width = nullptr, *height = nullptr, *border = nullptr;
    void sync()
    {
        preset->set_current(options.size_preset);
        portrait->set_checked(!options.landscape);
        landscape->set_checked(options.landscape);
        if (std::abs(width->value() - options.width_mm) > .0001)
            width->set_value(options.width_mm);
        if (std::abs(height->value() - options.height_mm) > .0001)
            height->set_value(options.height_mm);
    }
};
void show_project_setup(rap::ui& ui, rap::window* parent, transform_options options, std::function<void(transform_options)> done)
{
    auto state = std::make_shared<setup_dialog>();
    state->options = options;
    const auto weak = std::weak_ptr<setup_dialog>(state);
    const auto size_changed = [weak](int preset)
    {
        if (auto s = weak.lock())
        {
            set_frame_size(s->options, preset, s->options.landscape);
            s->sync();
        }
    };
    const auto orient = [weak](bool landscape)
    {
        if (auto s = weak.lock())
        {
            set_frame_size(s->options, s->options.size_preset, landscape);
            s->sync();
        }
    };
    const auto custom = [weak](double)
    {
        if (auto s = weak.lock())
        {
            s->options.width_mm = s->width->value();
            s->options.height_mm = s->height->value();
            s->options.size_preset = 9;
            s->options.landscape = s->options.width_mm > s->options.height_mm;
            s->sync();
        }
    };
    state->window = ui.create(
        L::dialog{"New project — Offcut",
                  {L::vbox{
                      L::label{"Choose the outside size of your picture frame, including its border."}.wrap(),
                      L::form{
                          {"Frame size",
                           L::hbox{
                               L::drop_down{preset_names(), options.size_preset}.bind(state->preset).on_changed(size_changed).stretch(),
                               L::tool_button{orientation_icon(false), "Portrait"}
                                   .toggle(!options.landscape)
                                   .bind(state->portrait)
                                   .on_toggled([orient](bool) { orient(false); }),
                               L::tool_button{orientation_icon(true), "Landscape"}
                                   .toggle(options.landscape)
                                   .bind(state->landscape)
                                   .on_toggled([orient](bool) { orient(true); }),
                           }},
                          {"Width (mm)", L::spin{30, 1000, options.width_mm, 1, 1}.bind(state->width).on_changed(custom)},
                          {"Height (mm)", L::spin{30, 1000, options.height_mm, 1, 1}.bind(state->height).on_changed(custom)},
                          {"Border (mm)", L::spin{2, 100, options.border_mm, .5, 1}.bind(state->border)},
                      },
                      L::label{options.placement == placement_mode::touch_edges
                                   ? "Images start touching the bottom of the opening. Move, scale, rotate and crop each one on the canvas."
                                   : "Images start fitted inside the opening. Move, scale, rotate and crop each one on the canvas."}
                          .wrap(),
                      L::buttons{L::dialog_button{rap::dialog_role::ok, "Create project"}, L::cancel},
                  }}}
            .size(550_px, {})
            .resizable(false),
        parent);
    state->window->on_finished(
        [state, &ui, done = std::move(done)](rap::dialog_role role) mutable
        {
            auto keep = std::move(state);
            if (role == rap::dialog_role::ok)
            {
                try
                {
                    auto o = keep->options;
                    o.border_mm = keep->border->value();
                    o.border_top_mm = o.border_bottom_mm = o.border_left_mm = o.border_right_mm = o.border_mm;
                    validate(o);
                    done(o);
                }
                catch (const std::exception& e)
                {
                    ui.message(nullptr, {.icon = rap::message_spec::icon::error, .title = "New project", .text = e.what()});
                }
            }
            ui.post([keep] {});
        });
    state->window->show();
}
const char* stage_name(work_step s)
{
    return s == work_step::arrange ? "Arrange" : s == work_step::colours ? "Colours" : "Sheets";
}
}  // namespace

void editor_window::build()
{
    const auto changed = [this](double) { edit_options(); };
    const auto resized = [this](double) { edit_options(true); };
    const auto image_field = [this](int field, double lo, double hi, double step, int digits)
    {
        return L::spin{lo, hi, 0, step, digits}.bind(image_fields_[field]).on_changed([this, field](double) { edit_image_fields(field); });
    };
    const auto action = [](const char* text, const char* tip, std::function<void()> f) { return L::button{text}.tooltip(tip).on_click(std::move(f)); };
    const auto touch_button = [this](const char* text, touch_edge edge)
    {
        return L::button{text}
            .tooltip("Move the image so its subject overlaps this edge of the opening by Min. width, for support")
            .on_click([this, edge, text] { apply_to_selected([edge](const layer& l, const transform_options& o) { return touch(l, o, edge); }, text); });
    };
    const auto fit_action = [this]
    { apply_to_selected([](const layer& l, const transform_options& o) { return fit_to_opening(effective_transform(l, o), o); }, "Fit to frame"); };
    const auto reset_action = [this]
    {
        apply_to_selected(
            [](const layer& l, const transform_options& o)
            {
                auto fresh = l;
                fresh.transform = {};
                return place_image(fresh, o, o.placement, o.touch_edges);
            },
            "Reset image");
    };
    window_ = app_.ui.create(L::window{
        "Offcut",
        {L::vbox{
            // Main toolbar: file · history · stage switch · Auto ·· AI, Export, Settings.
            L::toolbar{
                L::tool_button{new_icon(), "New project from images… (Ctrl+N)"}.on_click([this] { choose_images(true); }),
                L::tool_button{icon::named("document-open-symbolic"), "Open project… (Ctrl+O)"}.on_click([this] { choose_project(); }),
                L::tool_button{icon::named("document-save-symbolic"), "Save project (Ctrl+S)"}.on_click([this] { save(current()); }).handle(save_),
                L::separator{},
                L::tool_button{undo_icon(false), "Undo (Ctrl+Z)"}.on_click([this] { undo(); }).bind(undo_),
                L::tool_button{undo_icon(true), "Redo (Ctrl+Y)"}.on_click([this] { redo(); }).bind(redo_),
                L::separator{},
                L::canvas{}
                    .focusable(false)
                    .bind(stages_)
                    .width(330_px)
                    .height(30_px)
                    .tooltip("Stages: Arrange (Ctrl+1), Colours (Ctrl+2), Sheets (Ctrl+3). Any stage opens at any time.")
                    .on_paint([this](painter& p, size_f s) { paint_stages(p, s); })
                    .on_pointer([this](const pointer_event& e) { return stage_pointer(e); }),
                L::separator{},
                L::tool_button{icon::named("list-add-symbolic"), "Add images… (Ctrl+I)"}.on_click([this] { choose_images(false); }).handle(add_),
                // Auto is a split button: Auto · ▾ Local only · Plan only… · Options….
                L::tool_button{generate_icon(), "Auto: make sheets from the composition (Ctrl+G)"}.on_click([this] { auto_button(); }).handle(auto_),
                L::menu_button{""}.tooltip("Auto: Local only, Plan only, Arrange for me, Options").on_menu([this] { return auto_menu(); }).handle(auto_more_),
                L::spacer{},
                L::menu_button{"AI"}
                    .icon(ai_icon())
                    .tooltip("AI: Reimagine as shadowbox, and your own AI app's prompt, request and import")
                    .on_menu([this] { return ai_menu(); })
                    .handle(ai_),
                L::tool_button{export_icon(), "Export… (Ctrl+E)"}.on_click([this] { export_dialog(); }).handle(export_),
                L::tool_button{icon::named("preferences-system-symbolic"), "Settings (Ctrl+,)"}.on_click([this] { show_settings(app_.ui, window_.get()); }),
            },
            // Tool row: the stage's tools, the active tool's options, and up to
            // three quick actions for the selection. Its height never changes.
            L::hbox{
                L::hbox{
                    L::tool_button{move_icon(), "Move (V): drag to move, corners scale, the handle above rotates"}
                        .toggle(true)
                        .bind(move_tool_)
                        .on_toggled([this](bool) { set_tool(tool::move); }),
                    L::tool_button{crop_icon(), "Crop (C, or double-click an image): drag the edges, Enter applies, Esc cancels"}
                        .toggle(false)
                        .bind(crop_tool_)
                        .on_toggled([this](bool) { set_tool(tool::crop); }),
                    L::separator{},
                }
                    .handle(touch_controls_),
                L::stack{
                    {"move", L::hbox{L::check{"Snap to opening", true}.bind(snap_).tooltip("Edges snap to the opening within 3 mm. Hold Alt to place freely.")}},
                    {"crop", L::hbox{L::label{"Drag an edge to trim the image. Enter applies; Esc cancels."}}},
                    {"colours", L::hbox{L::label{"Colour tools come next. For now, Auto reduces the composition to sheets."}}},
                    {"sheets", L::hbox{L::label{"Select a sheet to highlight it in the stack. Floating pieces show in red."}}},
                }
                    .bind(tool_options_),
                L::spacer{},
                L::hbox{
                    L::button{"Touch bottom"}
                        .tooltip("Sink the subject into the bottom border by Min. width, so it is supported")
                        .on_click([this] { apply_to_selected([](const layer& l, const transform_options& o) { return touch(l, o, touch_bottom); }, "Touch bottom"); }),
                    action("Fit", "Fit the image inside the opening", fit_action),
                    action("Reset", "Place the image afresh, as when it was added", reset_action),
                }
                    .handle(move_actions_),
                L::hbox{
                    action("Apply", "Apply the crop (Enter)", [this] { end_crop(true); }),
                    action("Cancel", "Leave the crop as it was (Esc)", [this] { end_crop(false); }),
                }
                    .handle(crop_actions_)
                    .visible(false),
                L::hbox{
                    action("Auto", "Make the sheets again from the composition (Ctrl+G)", [this] { generate(); }),
                }
                    .handle(sheets_actions_)
                    .visible(false),
            }
                .margins(),
            L::splitter{
                L::horizontal,
                L::canvas{}
                    .bind(canvas_)
                    .stretch()
                    .on_paint([this](painter& painter, size_f size) { paint(painter, size); })
                    .on_scroll(
                        [this](const scroll_event& e)
                        {
                            zoom_at(std::pow(1.15f, -e.dy), e.pos);
                            return true;
                        })
                    .on_key([this](const key_event& e) { return key(e); })
                    .on_pointer([this](const pointer_event& e) { return pointer(e); }),
                // Right panel: the list over Properties.
                L::splitter{
                    L::vertical,
                    L::vbox{
                        L::label{"ARTWORK"}.bind(panel_title_),
                        L::stack{
                            {"artwork", L::list{layer_columns(), layer_model_ptr(*layer_rows_)}
                                            .checkboxes()
                                            .selection(selection_mode::single)
                                            .headers(false)
                                            .row_height(40)
                                            .soft_selection()
                                            .bind(layers_list_)
                                            .on_selection_changed(
                                                [this]
                                                {
                                                    auto p = current();
                                                    if (!p || syncing_)
                                                        return;
                                                    const auto row = layers_list_->current();
                                                    const auto sel = layers_list_->selection();
                                                    select_layer(sel.empty() || !row ? -1 : int(p->layers.size()) - 1 - int(sel.front()));
                                                })
                                            .on_context_menu(
                                                [this](std::optional<size_t> row)
                                                {
                                                    auto p = current();
                                                    if (!p || !row)
                                                        return rap::menu{};
                                                    return layer_menu(int(p->layers.size()) - 1 - int(*row));
                                                })
                                            .on_reorder(
                                                [this](const std::vector<size_t>& rows, size_t destination)
                                                {
                                                    auto p = current();
                                                    if (!p || p->busy || rows.size() != 1)
                                                        return false;
                                                    // Rows are front first; layers bottom first.
                                                    const int n = int(p->layers.size());
                                                    const int from = n - 1 - int(rows.front());
                                                    int to_row = int(destination) - (destination > rows.front() ? 1 : 0);
                                                    const int to = n - 1 - std::clamp(to_row, 0, n - 1);
                                                    if (from == to)
                                                        return false;
                                                    auto moved = std::move(p->layers[from]);
                                                    p->layers.erase(p->layers.begin() + from);
                                                    p->layers.insert(p->layers.begin() + to, std::move(moved));
                                                    p->selected_layer = to;
                                                    edited("Reorder images");
                                                    return true;
                                                })
                                            .stretch()},
                            {"sheets", L::list{sheet_columns(), sheet_model_ptr(*sheet_rows_)}
                                           .checkboxes()
                                           .selection(selection_mode::single)
                                           .headers(false)
                                           .row_height(40)
                                           .soft_selection()
                                           .bind(sheets_list_)
                                           .on_selection_changed(
                                               [this]
                                               {
                                                   if (syncing_)
                                                       return;
                                                   const auto sel = sheets_list_->selection();
                                                   select_sheet(sel.empty() ? -1 : int(sel.front()));
                                               })
                                           .stretch()},
                        }
                            .bind(panel_)
                            .stretch(),
                        L::hbox{
                            L::button{"Add images"}.icon(icon::named("list-add")).icon_only().tooltip("Add images… (Ctrl+I)").on_click([this] { choose_images(false); }),
                            L::button{"Remove image"}.icon(icon::named("list-remove")).icon_only().tooltip("Remove image (Delete)").on_click([this] { remove_selected(); }),
                            L::button{"Bring forward"}.icon(icon::named("go-up")).icon_only().tooltip("Bring forward (Ctrl+])").on_click([this] { move_layer(1); }),
                            L::button{"Send backward"}.icon(icon::named("go-down")).icon_only().tooltip("Send backward (Ctrl+[)").on_click([this] { move_layer(-1); }),
                            L::spacer{},
                            L::button{"Flatten"}.tooltip("Merge the visible images into one image").on_click([this] { flatten_sources(); }).handle(flatten_),
                        }
                            .handle(panel_widget_),
                    }
                        .margins()
                        .height(14_lines),
                    L::scroll{
                        L::vbox{
                            L::label{"PROPERTIES"}.bind(properties_title_),
                            L::stack{
                                {"shadowbox",
                                 L::vbox{
                                     L::form{
                                         {"Name", L::entry{}
                                                      .placeholder("Untitled")
                                                      .bind(name_)
                                                      .on_changed(
                                                          [this](const std::string& s)
                                                          {
                                                              auto p = current();
                                                              if (!p || syncing_ || p->name == s)
                                                                  return;
                                                              p->name = s;
                                                              edited("Rename project", "name", false);
                                                          })},
                                         {"Frame size",
                                          L::hbox{
                                              L::drop_down{preset_names(), 9}.bind(size_).on_changed([this](int i) { choose_size(i); }).stretch(),
                                              L::tool_button{orientation_icon(false), "Portrait"}.toggle(true).bind(portrait_).on_toggled([this](bool) { orient(false); }),
                                              L::tool_button{orientation_icon(true), "Landscape"}.toggle(false).bind(landscape_).on_toggled([this](bool) { orient(true); }),
                                          }},
                                         {"Width (mm)", L::spin{30, 1000, 200, 1, 1}.bind(width_).on_changed(resized)},
                                         {"Height (mm)", L::spin{30, 1000, 200, 1, 1}.bind(height_).on_changed(resized)},
                                     }
                                         .align_group("inspector"),
                                     L::check{"Same border on all sides", true}.bind(same_border_).on_toggled([this](bool same) { border_mode(same); }),
                                     L::form{{"Border (mm)", L::spin{2, 100, 10, .5, 1}.bind(border_).on_changed(changed)}}.align_group("inspector").handle(uniform_width_),
                                     L::form{
                                         {"Top (mm)", L::spin{2, 100, 10, .5, 1}.bind(top_).on_changed(changed)},
                                         {"Bottom (mm)", L::spin{2, 100, 10, .5, 1}.bind(bottom_).on_changed(changed)},
                                         {"Left (mm)", L::spin{2, 100, 10, .5, 1}.bind(left_).on_changed(changed)},
                                         {"Right (mm)", L::spin{2, 100, 10, .5, 1}.bind(right_).on_changed(changed)},
                                     }
                                         .align_group("inspector")
                                         .handle(side_widths_)
                                         .visible(false),
                                     L::form{
                                         {"Sheets", L::spin{2, 12, 7}.bind(layers_).on_changed(changed).tooltip("Sheets of card, front to back, including the backing")},
                                         {"Smoothing", L::spin{0, 6, 2}.bind(smooth_).on_changed(changed)},
                                         {"Spacer gap (mm)", L::spin{0, 50, 5, .5, 1}.bind(gap_).on_changed(changed).tooltip(
                                                                 "Foam spacer thickness; sets hidden overlap for a 45° view")},
                                         {"Min. width (mm)", L::spin{.5, 10, 2, .5, 1}.bind(bridge_).on_changed(changed).tooltip(
                                                                 "How far Touch sinks a subject into the border, and the narrowest feature requested from AI")},
                                         {"New images", L::drop_down{{"Fit whole image", "Fill opening", "Touch edges"}, 0}
                                                            .bind(placement_)
                                                            .on_changed([this](int) { edit_options(); })
                                                            .tooltip("Where an image starts when it is added; Place in its menu applies it again")},
                                     }
                                         .align_group("inspector"),
                                     L::hbox{
                                         L::check{"Bottom", true}.bind(edges_[0]).on_toggled([this](bool) { edit_options(); }),
                                         L::check{"Top", false}.bind(edges_[1]).on_toggled([this](bool) { edit_options(); }),
                                         L::check{"Left", false}.bind(edges_[2]).on_toggled([this](bool) { edit_options(); }),
                                         L::check{"Right", false}.bind(edges_[3]).on_toggled([this](bool) { edit_options(); }),
                                     },
                                     L::check{"Remove floating pieces", true}
                                         .tooltip("Card that doesn't reach the frame can't hold itself up. Off: it stays, marked red in Sheets.")
                                         .bind(connect_)
                                         .on_toggled([this](bool) { edit_options(); }),
                                     L::label{"AI style"},
                                     L::text_view{}
                                         .height(3_lines)
                                         .bind(style_)
                                         .on_changed(
                                             [this]
                                             {
                                                 auto p = current();
                                                 if (!p || syncing_ || p->ai_style == style_->text())
                                                     return;
                                                 p->ai_style = style_->text();
                                                 edited("Edit AI style", "ai-style", false);
                                             }),
                                     L::label{"Optional, e.g. “bold folk-art shapes”. Sent with every AI request in this project, as a preference that never "
                                              "overrides the cutting limits."}
                                         .wrap(),
                                 }},
                                {"image",
                                 L::vbox{
                                     L::form{
                                         {"Name", L::entry{}.bind(image_name_).on_changed(
                                                      [this](const std::string& s)
                                                      {
                                                          auto p = current();
                                                          if (!p || syncing_ || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()) ||
                                                              p->layers[p->selected_layer].name == s)
                                                              return;
                                                          p->layers[p->selected_layer].name = s;
                                                          edited("Rename image", "image-name", false);
                                                      })},
                                     }
                                         .align_group("inspector"),
                                     L::vbox{
                                         L::hbox{
                                             L::label{"VERSIONS"},
                                             L::spacer{},
                                             L::menu_button{"⋯"}
                                                 .tooltip("The version last clicked: Show plan, Refine, Try again, Discard (right-click a version to choose it)")
                                                 .on_menu([this] { return version_menu(); }),
                                             L::button{"✦ Refine…"}
                                                 .tooltip("Send the current version back to AI with your instructions")
                                                 .on_click(
                                                     [this]
                                                     {
                                                         if (auto p = current(); p && p->selected_layer >= 0 && p->selected_layer < int(p->layers.size()))
                                                             refine_dialog(p->selected_layer, p->layers[p->selected_layer].version, ai_task::refine);
                                                     }),
                                         },
                                         // A painted strip in a horizontal scroll: room for 15 versions
                                         // across and three rows (widgets keep their built size).
                                         L::scroll{L::canvas{}
                                                       .focusable(false)
                                                       .bind(versions_canvas_)
                                                       .width(1300_px)
                                                       .height(376_px)
                                                       .on_paint([this](painter& painter, size_f size) { paint_versions(painter, size); })
                                                       .on_pointer([this](const pointer_event& e) { return versions_pointer(e); })}
                                             .horizontal()
                                             .height(270_px),
                                         L::label{"Click a version to use it. ✓ current · ⚑ to review · ●issues · ▲ more issues than the one it came from."}.wrap(),
                                     }
                                         .handle(versions_box_)
                                         .visible(false),
                                     // Placement and crop; an AI illustration covers the frame and has
                                     // neither (docs/ux-design.md section 5).
                                     L::vbox{
                                     L::form{
                                         {"X (mm)", image_field(0, -2000, 2000, .5, 1)},
                                         {"Y (mm)", image_field(1, -2000, 2000, .5, 1)},
                                         {"W (mm)", image_field(2, 1, 4000, .5, 1)},
                                         {"H (mm)", image_field(3, 1, 4000, .5, 1)},
                                         {"Rotation (°)", image_field(4, -180, 180, 1, 1)},
                                     }
                                         .align_group("inspector"),
                                     L::hbox{
                                         touch_button("Touch bottom", touch_bottom),
                                         touch_button("Touch left", touch_left),
                                     },
                                     L::hbox{
                                         touch_button("Touch right", touch_right),
                                         touch_button("Touch top", touch_top),
                                     },
                                     L::hbox{
                                         action("Fit to frame", "Fit the image inside the opening", fit_action),
                                         action("Reset", "Place the image afresh, as when it was added", reset_action),
                                     },
                                     L::label{"Crop (source pixels)"},
                                     L::form{
                                         {"Left", image_field(5, 0, 2048, 1, 0)},
                                         {"Top", image_field(6, 0, 2048, 1, 0)},
                                         {"Width", image_field(7, 1, 2048, 1, 0)},
                                         {"Height", image_field(8, 1, 2048, 1, 0)},
                                     }
                                         .align_group("inspector"),
                                     }
                                         .handle(placement_box_),
                                     L::label{}.bind(image_info_).wrap(),
                                     L::label{"Notes for AI"},
                                     L::text_view{}
                                         .height(3_lines)
                                         .bind(notes_)
                                         .on_changed(
                                             [this]
                                             {
                                                 auto p = current();
                                                 if (!p || syncing_ || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
                                                     return;
                                                 auto& l = p->layers[p->selected_layer];
                                                 if (l.notes == notes_->text())
                                                     return;
                                                 l.notes = notes_->text();
                                                 edited("Edit notes for AI", "notes", false);
                                             }),
                                     L::label{"About this item, e.g. “Mochi, a tabby; keep the white bib”. Sent with every AI request that sends it."}.wrap(),
                                 }},
                                {"sheet",
                                 L::vbox{
                                     L::label{}.bind(sheet_info_).wrap(),
                                     L::label{"Ticks choose what the Assembled view stacks; every sheet is exported."}.wrap(),
                                 }},
                            }
                                .homogeneous(false)
                                .bind(properties_),
                            L::spacer{},
                        }
                            .margins()}
                        .stretch(),  // Properties takes the window's extra height; the list keeps its size
                }
                    .thin()
                    .width(46_ch),
            }
                .thin()
                .stretch(),
            // Bottom bar: a stack of two bars of one height. Guidance says what
            // is next or wrong, with one action, then the Sheets view switch and
            // zoom; transient messages show there too. Review (answering an AI
            // result, docs/ux-design.md section 8) takes its place when it lands.
            L::stack{
                {"guidance", L::hbox{
                L::label{"●"}.bind(guidance_dot_),
                L::label{"Ready"}.bind(guidance_).ellipsize().stretch(),
                L::button{"…"}.bind(guidance_action_).on_click(
                    [this]
                    {
                        if (guidance_do_)
                            guidance_do_();
                    }),
                L::button{"Cancel"}.on_click([this] { cancel_ai(); }).handle(cancel_ai_).visible(false),
                L::label{}.bind(summary_),
                L::hbox{
                    L::tool_button{layers_icon(), "Assembled: the ticked sheets stacked (0)"}
                        .toggle(true)
                        .bind(views_[0])
                        .on_toggled([this](bool on) { if (on && !syncing_) set_view(view_mode::assembled); }),
                    L::tool_button{layer_icon(), "Single sheet: the selected sheet alone (1–9, Page Up/Down)"}
                        .toggle(false)
                        .bind(views_[1])
                        .on_toggled([this](bool on) { if (on && !syncing_) set_view(view_mode::layer); }),
                }
                    .handle(view_switch_),
                L::separator{},
                L::button{"Zoom out"}.icon(icon::named("zoom-out")).icon_only().tooltip("Zoom out (Ctrl+−, wheel)").on_click([this] { zoom(1 / 1.25f); }),
                L::label{"Fit"}.bind(zoom_label_).width(6_ch),
                L::button{"Zoom in"}.icon(icon::named("zoom-in")).icon_only().tooltip("Zoom in (Ctrl+=, wheel)").on_click([this] { zoom(1.25f); }),
                L::button{"Fit"}.icon(icon::named("zoom-fit-best")).icon_only().tooltip("Fit to window (Ctrl+0)").on_click([this] { fit(); }),
            }
                .margins()},
                // Review: an AI result waiting for an answer (section 8). The
                // status, then the question, the pending instruction from
                // chips, or the plan's one-sentence explanation.
                {"review", L::hbox{
                               L::label{"AI result"}.bind(review_status_),
                               L::label{"Does this work as a shadowbox?"}.bind(review_text_).ellipsize().stretch(),
                               L::hbox{
                                   L::button{"✓ Use this"}.tooltip("Keep this version as the item's current one").on_click([this] { review_use(); }),
                                   L::button{"↻ Try again"}.tooltip("The same prompt again: a new drawing, kept beside this one").on_click([this] { review_try_again(); }),
                                   L::menu_button{"Support"}.on_menu([this] { return chip_menu("Support"); }),
                                   L::menu_button{"Likeness"}.on_menu([this] { return chip_menu("Likeness"); }),
                                   L::menu_button{"Detail"}.on_menu([this] { return chip_menu("Detail"); }),
                                   L::menu_button{"Colours"}.on_menu([this] { return chip_menu("Colours"); }),
                                   L::button{"✎ Review…"}.tooltip("Write a review: what should change, sent as one refinement").on_click([this] { write_review(); }),
                                   L::button{"Send"}.tooltip("Send the chosen answers as one refinement").on_click([this] { review_send(); }).bind(review_send_).visible(false),
                                   L::button{"Show plan"}
                                       .tooltip("The plan, prompt and record behind this result")
                                       .on_click(
                                           [this]
                                           {
                                               if (auto p = current(); p && p->selected_layer >= 0 && p->selected_layer < int(p->layers.size()))
                                                   show_record(p->selected_layer, p->layers[p->selected_layer].version);
                                           })
                                       .bind(show_plan_),
                                   L::button{"✕ Discard"}.tooltip("Remove this result; the version before it is current again").on_click([this] { review_discard(); }),
                               }
                                   .handle(review_answers_),
                               // Ask after each round: the round's result, then Continue or Stop.
                               L::hbox{
                                   L::button{"▶ Continue"}.tooltip("One more round: the director's next fix").on_click([this] { auto_continue(); }),
                                   L::button{"■ Stop"}.tooltip("Stop here and show the best round").on_click([this] { auto_stop(); }),
                                   L::button{"Show plan"}
                                       .tooltip("The plan this run draws from")
                                       .on_click(
                                           [this]
                                           {
                                               if (auto p = current(); p && p->selected_layer >= 0 && p->selected_layer < int(p->layers.size()))
                                                   show_record(p->selected_layer, p->layers[p->selected_layer].version);
                                           }),
                               }
                                   .handle(review_ask_)
                                   .visible(false),
                               L::button{"Cancel"}.on_click([this] { cancel_ai(); }).bind(review_cancel_).visible(false),
                           }
                               .margins()},
            }
                .bind(bottom_bar_),
        }}}.size(160_ch, 50_lines)
                                 .margins(false)
                                 .base_surface()
                                 .tabs(L::document_tabs{}
                                           .bind(tab_bar_)
                                           .group("offcut.projects")
                                           .on_new([this] { insert({app_.next_id(), std::make_shared<project>()}); })
                                           .on_select(
                                               [this](const std::string& id)
                                               {
                                                   end_crop(false);
                                                   selected_ = id;
                                                   refresh();
                                               })
                                           .on_close([this](const std::string& id) { close_tab(id); })
                                           .on_reorder(
                                               [this](const std::string& id, int index)
                                               {
                                                   auto it = std::ranges::find_if(tabs_, [&](const auto& t) { return t.id == id; });
                                                   if (it == tabs_.end())
                                                       return;
                                                   auto t = std::move(*it);
                                                   tabs_.erase(it);
                                                   tabs_.insert(tabs_.begin() + std::clamp(index, 0, int(tabs_.size())), std::move(t));
                                               })
                                           .on_detach([this](const std::string& id, std::optional<point_f>) { return app_.detach(*this, id); })
                                           .on_receive([this](const document_tabs::transfer& t) { return app_.receive(*this, t); })
                                           .on_context_menu([this](const std::string& id) { return tab_menu(id); })));
    canvas_->set_cursor(cursor::default_);
    window_->on_key([this](const key_event& e) { return key(e); });
    window_->on_close_request(
        [this]
        {
            close_window();
            return true;
        });
    // Dropping pictures creates or extends a project at once, without the
    // frame dialog (docs/ux-design.md section 10).
    window_->on_files_dropped(
        [this](const std::vector<std::string>& paths)
        {
            auto p = current();
            if (paths.empty() || (p && p->busy))
                return;
            if (p && (!p->layers.empty() || !p->cuts.empty()))
                import_paths(paths, false);
            else
                try
                {
                    import_paths(paths, false, new_project_options(read_image(from_utf8(paths.front())).image));
                }
                catch (const std::exception& e)
                {
                    app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "Add images", .text = e.what()});
                }
        });
}
rap::menu editor_window::tab_menu(const std::string& id)
{
    std::shared_ptr<project> p;
    for (const auto& t : tabs_)
        if (t.id == id)
            p = t.document;
    if (!p)
        return {};
    rap::menu m;
    m.push_back({.label = "_Save", .action = [this, p] { save(p); }, .enabled = !p->busy});
    m.push_back({.label = "Move to new _window",
                 .action =
                     [this, id]
                 {
                     if (auto* w = app_.detach(*this, id))
                         w->show();
                 },
                 .enabled = tabs_.size() > 1});
    m.push_back(menu_item::separator());
    m.push_back({.label = "_Close", .action = [this, id] { close_tab(id); }, .shortcut = "Ctrl+W"});
    m.push_back({.label = "Close _others",
                 .action =
                     [this, id]
                 {
                     std::vector<std::string> others;
                     for (const auto& t : tabs_)
                         if (t.id != id)
                             others.push_back(t.id);
                     for (const auto& o : others)
                         close_tab(o);
                 },
                 .enabled = tabs_.size() > 1});
    return m;
}
rap::menu editor_window::ai_menu()
{
    auto p = current();
    const bool ready = p && !p->busy;
    return {
        {.label = "_Reimagine as shadowbox…", .action = [this] { reimagine(); }, .enabled = ready && !p->layers.empty() && native_ai_supported()},
        {.label = "R_efine with AI…",
         .action = [this, p] { refine_dialog(p->selected_layer, p->layers[p->selected_layer].version, ai_task::refine); },
         .enabled = ready && selected_item() && !selected_item()->versions.empty()},
        {.label = "_Fix with AI…",
         .action = [this, p] { refine_dialog(p->selected_layer, p->layers[p->selected_layer].version, ai_task::fix_support); },
         .enabled = ready && selected_item() && selected_item()->version != 0},
        {.label = "Cancel AI request", .action = [this] { cancel_ai(); }, .enabled = p && p->busy && p->ai_running && p->cancel && !p->cancel->load()},
        menu_item::separator(),
        {.label = "Use my own AI app",
         .children =
             {
                 {.label = "_Copy AI prompt",
                  .action =
                      [this]
                  {
                      if (auto p = current())
                      {
                          app_.ui.set_clipboard(ai_prompt(*p));
                          flash("AI prompt copied. Attach your reference images in your chosen AI app.");
                      }
                  },
                  .enabled = ready},
                 {.label = "_Export AI request…", .action = [this] { export_files(true); }, .enabled = ready},
                 {.label = "Import _flat illustration…", .action = [this] { choose_illustration(); }, .enabled = ready},
                 {.label = "_Import sheet images…", .action = [this] { choose_images(false, true); }, .enabled = ready},
             }},
        menu_item::separator(),
        {.label = "AI _providers…", .action = [this] { show_settings(app_.ui, window_.get(), settings_page::ai_providers); }},
    };
}
rap::menu editor_window::layer_menu(int index)
{
    auto p = current();
    if (!p || index < 0 || index >= int(p->layers.size()))
        return {};
    const bool ready = !p->busy;
    const auto on = [this, index](std::function<void()> f)
    {
        return [this, index, f]
        {
            select_layer(index);
            f();
        };
    };
    const auto place = [this](placement_mode mode)
    {
        return [this, mode]
        {
            apply_to_selected([mode](const layer& l, const transform_options& o) { return place_image(l, o, mode, o.touch_edges); },
                              mode == placement_mode::fit ? "Place: fit" : mode == placement_mode::fill ? "Place: fill" : "Place: touch edges");
        };
    };
    const auto touch_item = [this, on](const char* label, touch_edge edge)
    {
        return menu_item{.label = label, .action = on([this, edge, label] {
                             apply_to_selected([edge](const layer& l, const transform_options& o) { return touch(l, o, edge); }, label);
                         })};
    };
    return {
        {.label = "_Crop", .action = on([this] { begin_crop(); }), .shortcut = "C", .enabled = ready},
        {.label = "_Fit to frame",
         .action = on([this] { apply_to_selected([](const layer& l, const transform_options& o) { return fit_to_opening(effective_transform(l, o), o); }, "Fit to frame"); }),
         .enabled = ready},
        {.label = "_Place",
         .children = {{.label = "Fit whole image", .action = on(place(placement_mode::fit))},
                      {.label = "Fill opening", .action = on(place(placement_mode::fill))},
                      {.label = "Touch edges (as in Shadowbox)", .action = on(place(placement_mode::touch_edges))}},
         .enabled = ready},
        {.label = "_Touch",
         .children = {touch_item("Touch bottom", touch_bottom), touch_item("Touch left", touch_left), touch_item("Touch right", touch_right),
                      touch_item("Touch top", touch_top)},
         .enabled = ready},
        menu_item::separator(),
        {.label = "Bring _forward", .action = on([this] { move_layer(1); }), .shortcut = "Ctrl+]", .enabled = ready && index + 1 < int(p->layers.size())},
        {.label = "Send _backward", .action = on([this] { move_layer(-1); }), .shortcut = "Ctrl+[", .enabled = ready && index > 0},
        {.label = p->layers[index].visible ? "_Hide" : "_Show",
         .action = on(
             [this]
             {
                 if (auto q = current(); q && q->selected_layer >= 0)
                 {
                     auto& l = q->layers[q->selected_layer];
                     l.visible = !l.visible;
                     edited(l.visible ? "Show image" : "Hide image");
                 }
             }),
         .enabled = ready},
        {.label = "_Remove", .action = on([this] { remove_selected(); }), .shortcut = "Delete", .enabled = ready},
    };
}
void editor_window::set_stage(work_step s)
{
    auto p = current();
    if (!p)
        return;
    end_crop(false);
    p->step = s;
    // The stage is the view; Sheets keeps whether it showed one or all.
    if (s == work_step::arrange)
        p->view = view_mode::original;
    else if (s == work_step::colours)
        p->view = p->cuts.empty() ? view_mode::original : view_mode::assembled;
    else if (p->view == view_mode::original)
        p->view = view_mode::assembled;
    refresh();
}
void editor_window::set_view(view_mode mode)
{
    auto p = current();
    if (p)
    {
        if (p->step == work_step::arrange)  // views belong to Colours and Sheets; never a stage switch
            return;
        p->view = mode;
        if (mode == view_mode::layer && (p->selected_cut < 0 || p->selected_cut >= int(p->cuts.size())))
            p->selected_cut = 0;
        update_preview();
    }
    // The two act as one choice: the one picked stays down, even clicked again.
    const bool was_syncing = std::exchange(syncing_, true);
    views_[0]->set_checked(!p || p->view != view_mode::layer);
    views_[1]->set_checked(p && p->view == view_mode::layer);
    syncing_ = was_syncing;
    sync_lists();
    sync_guidance();
}
void editor_window::set_tool(tool t)
{
    auto p = current();
    if (t == tool::crop && !cropping_)
    {
        begin_crop();
        if (!cropping_)
            t = tool::move;  // nothing to crop
    }
    else if (t == tool::move && cropping_)
        end_crop(true);
    tool_ = t;
    sync_tool_row();
    canvas_->invalidate();
}
void editor_window::fit()
{
    if (auto p = current())
    {
        p->zoom = 1;
        p->pan_x = p->pan_y = 0;
    }
    zoom(1);
}
// A window's first, untouched project: opening something replaces it rather
// than leaving an empty tab beside it.
bool editor_window::reusable() const
{
    auto p = current();
    return p && tabs_.size() == 1 && !p->busy && !p->dirty && p->path.empty() && p->layers.empty() && p->cuts.empty();
}
void editor_window::insert(project_tab tab, int index)
{
    const auto id = tab.id;
    const auto p = tab.document;
    if (!p->undo)
    {
        p->undo = std::make_shared<history>();
        p->undo->reset(*p);
    }
    const int at = index < 0 ? int(tabs_.size()) : std::clamp(index, 0, int(tabs_.size()));
    tabs_.insert(tabs_.begin() + at, std::move(tab));
    end_crop(false);
    selected_ = id;
    tab_bar_->insert(at, {.id = id, .title = p->name});
    tab_bar_->select(id);
    refresh();
}
std::optional<project_tab> editor_window::take(const std::string& id)
{
    const auto it = std::ranges::find_if(tabs_, [&](const auto& t) { return t.id == id; });
    if (it == tabs_.end())
        return {};
    if (id == selected_)
        end_crop(false);
    const size_t i = size_t(it - tabs_.begin());
    auto tab = std::move(*it);
    tabs_.erase(it);
    tab_bar_->remove(id);
    if (id == selected_)
        selected_ = tabs_.empty() ? "" : tabs_[std::min(i, tabs_.size() - 1)].id;
    if (!selected_.empty())
    {
        tab_bar_->select(selected_);
        refresh();
    }
    return tab;
}
void editor_window::refresh()
{
    auto p = current();
    if (!p)
        return;
    if (cropping_ && (p->busy || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size())))
        end_crop(false);
    syncing_ = true;
    for (const auto& t : tabs_)
        tab_bar_->update({.id = t.id,
                          .title = t.document->name + (t.document->dirty ? " *" : ""),
                          .tooltip = t.document->path.empty() ? "Unsaved project" : utf8(t.document->path)});
    window_->set_title(p->name + " — Offcut");
    const bool idle = !p->busy, sources = !p->layers.empty();
    for (auto* w : {save_, add_, flatten_, panel_widget_})
        w->set_enabled(idle);
    properties_->set_enabled(idle);
    layers_list_->set_enabled(idle);
    sheets_list_->set_enabled(idle);
    ai_->set_enabled(idle || p->ai_running);
    cancel_ai_->set_visible(p->busy && p->ai_running);
    cancel_ai_->set_enabled(p->cancel && !p->cancel->load());
    auto_->set_enabled(idle && sources);
    auto_more_->set_enabled(idle && sources);
    export_->set_enabled(idle && !p->cuts.empty());
    undo_->set_enabled(idle && p->undo && p->undo->can_undo());
    redo_->set_enabled(idle && p->undo && p->undo->can_redo());
    undo_->set_tooltip(p->undo && p->undo->can_undo() ? "Undo " + p->undo->undo_label() + " (Ctrl+Z)" : "Undo (Ctrl+Z)");
    redo_->set_tooltip(p->undo && p->undo->can_redo() ? "Redo " + p->undo->redo_label() + " (Ctrl+Y)" : "Redo (Ctrl+Y)");
    // Do not replace the text being typed into a native field: that would
    // reset its caret on every edit. Only tab switches need different values.
    if (name_->text() != p->name)
        name_->set_text(p->name);
    const auto sync = [](rap::spin* field, double value)
    {
        if (std::abs(field->value() - value) > .0001)
            field->set_value(value);
    };
    sync(width_, p->options.width_mm);
    sync(height_, p->options.height_mm);
    sync(border_, p->options.border_mm);
    sync(top_, p->options.border_top_mm);
    sync(bottom_, p->options.border_bottom_mm);
    sync(left_, p->options.border_left_mm);
    sync(right_, p->options.border_right_mm);
    same_border_->set_checked(p->options.uniform_border);
    uniform_width_->set_visible(p->options.uniform_border);
    side_widths_->set_visible(!p->options.uniform_border);
    size_->set_current(p->options.size_preset);
    portrait_->set_checked(!p->options.landscape);
    landscape_->set_checked(p->options.landscape);
    placement_->set_current(int(p->options.placement));
    for (unsigned i = 0; i < 4; ++i)
    {
        edges_[i]->set_checked((p->options.touch_edges & (1u << i)) != 0);
        edges_[i]->set_enabled(p->options.placement == placement_mode::touch_edges);
    }
    sync(bridge_, p->options.bridge_mm);
    sync(layers_, p->options.layers);
    sync(smooth_, p->options.smoothing);
    sync(gap_, p->options.layer_gap_mm);
    connect_->set_checked(p->options.connect);
    if (style_->text() != p->ai_style)
        style_->set_text(p->ai_style);
    syncing_ = false;
    sync_lists();
    sync_image_fields();
    sync_tool_row();
    zoom_label_->set_text(p->zoom == 1 ? std::string("Fit") : std::format("{:.0f}%", p->zoom * 100));
    summary_->set_text(p->cuts.empty() ? std::format("{:g} × {:g} mm", p->options.width_mm, p->options.height_mm)
                                       : std::format("{:g} × {:g} mm · {} sheets", p->options.width_mm, p->options.height_mm, p->cuts.size()));
    stages_->invalidate();
    update_preview();
    sync_guidance();
    sync_versions();
    sync_review();
}
void editor_window::sync_tool_row()
{
    auto p = current();
    const auto stage = p ? p->step : work_step::arrange;
    const bool arrange = stage == work_step::arrange;
    touch_controls_->set_visible(arrange);
    move_tool_->set_checked(tool_ == tool::move);
    crop_tool_->set_checked(tool_ == tool::crop);
    tool_options_->show(arrange ? (cropping_ ? "crop" : "move") : stage == work_step::colours ? "colours" : "sheets");
    const bool image = p && p->selected_layer >= 0 && p->selected_layer < int(p->layers.size());
    move_actions_->set_visible(arrange && !cropping_);
    move_actions_->set_enabled(image && !p->busy);
    crop_actions_->set_visible(arrange && cropping_);
    sheets_actions_->set_visible(!arrange);
    sheets_actions_->set_enabled(p && !p->busy && !p->layers.empty());
    view_switch_->set_visible(stage == work_step::sheets);
    views_[0]->set_checked(!p || p->view != view_mode::layer);
    views_[1]->set_checked(p && p->view == view_mode::layer);
    // Properties: the selection's, else the shadowbox's.
    std::string page = "shadowbox", title = "PROPERTIES — Shadowbox";
    if (p && stage != work_step::sheets && image)
    {
        page = "image";
        title = "PROPERTIES — " + p->layers[p->selected_layer].name;
    }
    else if (p && stage == work_step::sheets && p->selected_cut >= 0 && p->selected_cut < int(p->cuts.size()))
    {
        page = "sheet";
        title = std::format("PROPERTIES — Sheet {}", p->selected_cut + 1);
        const int i = p->selected_cut, n = int(p->cuts.size());
        const size_t floating = size_t(i) < floating_.size() ? floating_[i] : 0;
        sheet_info_->set_text(std::format("Sheet {} of {}{}. {}. {}.\nDesign Space size: {:g} × {:g} mm.", i + 1, n,
                                          i == 0 ? ", the front" : i + 1 == n ? ", the backing" : "",
                                          p->cuts[i].visible ? "Shown in the stack" : "Hidden from the stack",
                                          floating ? std::format("{} floating {} (red)", floating, floating == 1 ? "piece" : "pieces")
                                                   : std::string("Every piece reaches the frame"),
                                          p->options.width_mm, p->options.height_mm));
    }
    properties_->show(page);
    properties_title_->set_text(title);
    panel_->show(stage == work_step::sheets ? "sheets" : "artwork");
    panel_title_->set_text(stage == work_step::sheets ? "SHEETS — front to back" : "ARTWORK — front to back");
    panel_widget_->set_visible(stage != work_step::sheets);
}
void editor_window::edited(const std::string& label, const std::string& merge, bool design)
{
    auto p = current();
    if (!p)
        return;
    if (design)
        p->stale = !p->cuts.empty();
    if (p->undo)
    {
        p->undo->commit(*p, label, merge);
        p->dirty = !p->undo->at_saved();
    }
    else
        p->dirty = true;
    app_.changed();
}
void editor_window::undo()
{
    auto p = current();
    if (!p || p->busy || !p->undo)
        return;
    end_crop(false);
    const auto label = p->undo->undo_label();
    if (p->undo->undo(*p))
    {
        flash("Undid " + label + ".");
        app_.changed();
    }
}
void editor_window::redo()
{
    auto p = current();
    if (!p || p->busy || !p->undo)
        return;
    end_crop(false);
    if (p->undo->redo(*p))
    {
        flash("Redid " + p->undo->undo_label() + ".");
        app_.changed();
    }
}
void editor_window::edit_options(bool custom_size)
{
    auto p = current();
    if (!p || p->busy || syncing_)
        return;
    auto& o = p->options;
    o.width_mm = width_->value();
    o.height_mm = height_->value();
    o.border_mm = border_->value();
    o.bridge_mm = bridge_->value();
    o.layers = int(layers_->value());
    o.smoothing = int(smooth_->value());
    o.connect = connect_->checked();
    o.layer_gap_mm = gap_->value();
    if (custom_size)
    {
        o.size_preset = 9;
        o.landscape = o.width_mm > o.height_mm;
    }
    if (o.uniform_border)
        o.border_top_mm = o.border_bottom_mm = o.border_left_mm = o.border_right_mm = o.border_mm;
    else
    {
        o.border_top_mm = top_->value();
        o.border_bottom_mm = bottom_->value();
        o.border_left_mm = left_->value();
        o.border_right_mm = right_->value();
    }
    o.placement = placement_mode(std::clamp(placement_->current(), 0, 2));
    o.touch_edges = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (edges_[i]->checked())
            o.touch_edges |= 1u << i;
    try
    {
        validate(o);
    }
    catch (const std::exception& e)
    {
        flash(e.what());
    }
    edited("Change shadowbox settings", "options");
}
void editor_window::choose_size(int preset)
{
    auto p = current();
    if (!p || p->busy || syncing_)
        return;
    set_frame_size(p->options, preset, p->options.landscape);
    edited("Change frame size");
}
void editor_window::orient(bool landscape)
{
    auto p = current();
    if (!p || p->busy || syncing_)
        return;
    set_frame_size(p->options, p->options.size_preset, landscape);
    edited(landscape ? "Landscape" : "Portrait");
}
void editor_window::border_mode(bool same)
{
    auto p = current();
    if (!p || p->busy || syncing_)
        return;
    auto& o = p->options;
    if (same)
        o.border_mm = o.border_top_mm;
    else
        o.border_top_mm = o.border_bottom_mm = o.border_left_mm = o.border_right_mm = o.border_mm;
    o.uniform_border = same;
    edited(same ? "Same border on all sides" : "Separate borders");
}
void editor_window::select_layer(int index)
{
    auto p = current();
    if (!p)
        return;
    if (index >= int(p->layers.size()))
        index = int(p->layers.size()) - 1;
    if (index != p->selected_layer)
        end_crop(true);
    p->selected_layer = index;
    // The stage is only ever the user's choice: selecting (which list syncs
    // also do on every refresh) never switches it.
    if (p->undo)
        p->undo->commit(*p, "Select");  // records only where the selection is
    sync_lists();
    sync_image_fields();
    sync_tool_row();
    sync_guidance();
    sync_versions();
    sync_review();
    stages_->invalidate();
    update_preview();
}
void editor_window::select_sheet(int index)
{
    auto p = current();
    if (!p)
        return;
    p->selected_cut = index < int(p->cuts.size()) ? index : int(p->cuts.size()) - 1;
    // Selecting a sheet highlights it in the stack; only the Single view
    // (its button, or 1–9) shows one sheet alone. It never switches stage.
    if (p->step == work_step::sheets && p->view == view_mode::original)
        p->view = view_mode::assembled;
    sync_lists();
    sync_tool_row();
    sync_guidance();
    stages_->invalidate();
    update_preview();
}
std::optional<image_transform> editor_window::selected_transform() const
{
    auto p = current();
    if (!p || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
        return {};
    return effective_transform(p->layers[p->selected_layer], p->options);
}
void editor_window::set_transform(const image_transform& t, const std::string& label, const std::string& merge)
{
    auto p = current();
    if (!p || p->busy || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
        return;
    p->layers[p->selected_layer].transform = t;
    edited(label, merge);
}
void editor_window::apply_to_selected(const std::function<image_transform(const layer&, const transform_options&)>& f, const std::string& label)
{
    auto p = current();
    if (!p || p->busy || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
    {
        flash("Select an image first.");
        return;
    }
    end_crop(true);
    try
    {
        set_transform(f(p->layers[p->selected_layer], p->options), label);
    }
    catch (const std::exception& e)
    {
        flash(e.what());
    }
}
void editor_window::sync_image_fields()
{
    auto p = current();
    const auto t = selected_transform();
    if (!t || !p)
        return;
    syncing_ = true;
    const auto& l = p->layers[p->selected_layer];
    if (image_name_->text() != l.name)
        image_name_->set_text(l.name);
    if (notes_->text() != l.notes)
        notes_->set_text(l.notes);
    // In crop mode the fields show the crop being edited.
    auto shown = *t;
    if (cropping_)
        shown = with_crop(*t, crop_);
    const double w = width_mm(shown), h = height_mm(shown);
    const double values[] = {shown.x_mm - w / 2, shown.y_mm - h / 2, w, h, shown.rotation_deg, shown.crop.x, shown.crop.y, shown.crop.w, shown.crop.h};
    for (int i = 0; i < 9; ++i)
        if (std::abs(image_fields_[i]->value() - values[i]) > .0001)
            image_fields_[i]->set_value(values[i]);
    image_info_->set_text(std::format("{} · {} × {} px{} · {:.1f} px/mm. X and Y are the top-left of the unrotated image in the frame.",
                                      layer_type_name(l.type), l.image.width, l.image.height,
                                      l.version ? std::format(" · showing version {}", l.version) : std::string(), 1 / t->mm_per_pixel));
    syncing_ = false;
}
void editor_window::edit_image_fields(int field)
{
    auto p = current();
    auto t = selected_transform();
    if (!p || !t || syncing_ || p->busy)
        return;
    const double v = image_fields_[field]->value();
    const double w = width_mm(*t), h = height_mm(*t);
    const double x = t->x_mm - w / 2, y = t->y_mm - h / 2;
    const auto& l = p->layers[p->selected_layer];
    switch (field)
    {
        case 0:
            t->x_mm = v + w / 2;
            break;
        case 1:
            t->y_mm = v + h / 2;
            break;
        case 2:
        case 3:
        {
            // Uniform scale, keeping the top-left where it is.
            const double factor = v / (field == 2 ? w : h);
            if (!(factor > 0))
                return;
            t->mm_per_pixel *= factor;
            t->x_mm = x + width_mm(*t) / 2;
            t->y_mm = y + height_mm(*t) / 2;
            break;
        }
        case 4:
            t->rotation_deg = v;
            break;
        default:
        {
            auto c = t->crop;
            const double values[] = {image_fields_[5]->value(), image_fields_[6]->value(), image_fields_[7]->value(), image_fields_[8]->value()};
            c.x = std::clamp(values[0], 0., l.image.width - 1.);
            c.y = std::clamp(values[1], 0., l.image.height - 1.);
            c.w = std::clamp(values[2], 1., l.image.width - c.x);
            c.h = std::clamp(values[3], 1., l.image.height - c.y);
            end_crop(false);
            *t = with_crop(*t, c);
            break;
        }
    }
    static const char* names[] = {"Move image", "Move image", "Scale image", "Scale image", "Rotate image", "Crop image"};
    set_transform(*t, names[std::min(field, 5)], std::format("field-{}", field));
}
void editor_window::begin_crop()
{
    auto p = current();
    const auto t = selected_transform();
    if (!p || !t || p->busy)
    {
        flash("Select an image to crop.");
        return;
    }
    if (p->step != work_step::arrange)
        set_stage(work_step::arrange);
    cropping_ = true;
    tool_ = tool::crop;
    crop_ = t->crop;
    sync_tool_row();
    sync_image_fields();
    update_preview();
}
void editor_window::end_crop(bool commit)
{
    if (!cropping_)
        return;
    cropping_ = false;
    tool_ = tool::move;
    drag_ = drag_kind::none;
    auto p = current();
    const auto t = selected_transform();
    if (commit && p && t && !(crop_ == t->crop))
        set_transform(with_crop(*t, crop_), "Crop image");
    else if (p)
    {
        sync_tool_row();
        sync_image_fields();
        update_preview();
    }
}
void editor_window::flash(std::string message)
{
    flash_ = std::move(message);
    flash_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto weak = std::weak_ptr<void>(alive_);
    flash_timer_ = app_.ui.timer(5100, false,
                                 [this, weak]
                                 {
                                     if (!weak.expired())
                                         sync_guidance();
                                 });
    sync_guidance();
}
std::string editor_window::guidance() const
{
    return guidance_text_;
}
void editor_window::sync_guidance()
{
    auto p = current();
    if (!p)
        return;
    const auto g = guidance_for(*p, floating_);
    std::string text = g.text;
    guidance_do_ = nullptr;
    std::string action;
    const auto run = auto_of(p.get());
    if (p->busy && run && auto_running())
    {
        // Auto's progress: the step, the time so far, Show plan and Cancel.
        const auto s = int(auto_elapsed());
        text = std::format("{} · {}:{:02}", p->activity, s / 60, s % 60);
        if (const auto plan = auto_plan_now(); !plan.empty())
        {
            action = "Show plan";
            guidance_do_ = [this, plan] { show_plan(plan); };
        }
    }
    else if (!p->busy && std::chrono::steady_clock::now() < flash_until_ && !flash_.empty())
        text = flash_;
    else if (!p->busy)
        switch (g.action)
        {
            case guidance_action::add_images:
                action = "Add images…";
                guidance_do_ = [this] { choose_images(false); };
                break;
            case guidance_action::auto_:
                action = "Auto";
                guidance_do_ = [this] { generate(); };
                break;
            case guidance_action::show_sheets:
                action = "Show sheets";
                guidance_do_ = [this] { set_stage(work_step::sheets); };
                break;
            case guidance_action::export_:
                action = "Export…";
                guidance_do_ = [this] { export_dialog(); };
                break;
            case guidance_action::none:
                break;
        }
    guidance_text_ = text;
    guidance_->set_text(text);
    guidance_->set_tooltip(text);
    guidance_dot_->set_text(g.level == guidance_level::problem ? "▲" : g.level == guidance_level::done ? "✓" : "●");
    guidance_action_->set_visible(!action.empty());
    if (!action.empty())
        guidance_action_->set_text(action);
}
void editor_window::choose_images(bool create, bool masks)
{
    auto p = current();
    if (p && p->busy && !create)
        return;
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.choose_files(window_.get(),
                         {.mode = file_dialog_spec::mode::open_many,
                          .title = masks ? "Import aligned sheet images — black means cardstock" : "Choose images",
                          .filters = {{"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif"}, {"All files", "*.*"}}},
                         [this, weak, create, masks](std::vector<std::string> paths)
                         {
                             if (!weak.expired() && !paths.empty())
                                 load_paths(std::move(paths), create, masks);
                         });
}
void editor_window::choose_illustration()
{
    auto p = current();
    if (!p || p->busy)
        return;
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.choose_files(window_.get(),
                         {.mode = file_dialog_spec::mode::open,
                          .title = "Import flat illustration",
                          .filters = {{"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif"}, {"All files", "*.*"}}},
                         [this, weak, p](std::vector<std::string> paths)
                         {
                             if (weak.expired() || paths.empty() || p->busy)
                                 return;
                             app_.task(
                                 p, "Separating illustration tones…",
                                 [path = from_utf8(paths.front())](project& r)
                                 {
                                     const auto image = read_image(path);
                                     apply_illustration(r, image.image);
                                     r.step = work_step::sheets;
                                 },
                                 {}, "Import illustration");
                         });
}
void editor_window::cancel_work()
{
    for (const auto& tab : tabs_)
        if (tab.document->cancel)
            tab.document->cancel->store(true);
}
void editor_window::cancel_ai()
{
    auto p = current();
    if (!p || !p->busy || !p->ai_running || !p->cancel)
        return;
    p->cancel->store(true);
    p->activity = "Cancelling AI request…";
    app_.changed();
}
void editor_window::load_paths(std::vector<std::string> paths, bool create, bool masks)
{
    if (paths.empty())
        return;
    if (!create)
    {
        import_paths(std::move(paths), masks);
        return;
    }
    try
    {
        // Inspect the first image before choosing orientation and subject placement.
        // No tab is created or existing project changed until the dialog is accepted.
        const auto first = read_image(from_utf8(paths.front()));
        const auto options = new_project_options(first.image);
        if (std::getenv("OFFCUT_SNAPSHOT"))
        {
            import_paths(std::move(paths), false, options);
            return;
        }
        auto weak = std::weak_ptr<void>(alive_);
        show_project_setup(app_.ui, window_.get(), options,
                           [this, weak, paths = std::move(paths)](transform_options chosen) mutable
                           {
                               if (!weak.expired())
                                   import_paths(std::move(paths), false, chosen);
                           });
    }
    catch (const std::exception& e)
    {
        app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "New project", .text = e.what()});
    }
}
void editor_window::import_paths(std::vector<std::string> paths, bool masks, std::optional<transform_options> setup)
{
    const bool create = setup.has_value();
    auto p = current();
    if (create && reusable())
        p->name = utf8(from_utf8(paths.front()).stem());
    else if (create)
    {
        p = std::make_shared<project>();
        p->name = utf8(from_utf8(paths.front()).stem());
        insert({app_.next_id(), p});
    }
    if (!p || p->busy)
        return;
    if (setup)
        p->options = *setup;
    end_crop(true);
    app_.task(
        p, masks ? "Reading sheet images…" : "Reading images…",
        [paths = std::move(paths), masks](project& result)
        {
            if (paths.size() > 32 || (!masks && result.layers.size() + paths.size() > 32))
                throw std::runtime_error("A project holds up to 32 images.");
            std::vector<source_image> images;
            for (auto& path : paths)
                images.push_back(read_image(from_utf8(path)));
            if (masks)
            {
                std::ranges::sort(images, {}, &source_image::name);
                for (const auto& image : images)
                    if (image.image.width != images.front().image.width || image.image.height != images.front().image.height)
                        throw std::runtime_error("Sheet images must have identical pixel dimensions and alignment.");
                result.cuts = masks_from_images(images, result.options);
                result.selected_cut = 0;
                result.stale = false;
                result.view = view_mode::assembled;
                result.step = work_step::sheets;
                result.activity = "Imported sheet images. Check each mask before cutting.";
            }
            else
            {
                // Each new image starts where the placement for new images puts it.
                for (auto& image : images)
                {
                    image.transform = place_image(image, result.options, result.options.placement, result.options.touch_edges);
                    result.layers.push_back(std::move(image));
                }
                result.assign_layer_ids();
                result.selected_layer = int(result.layers.size()) - 1;
                result.stale = !result.cuts.empty();
                result.view = view_mode::original;
                result.step = work_step::arrange;
                result.activity = images.size() == 1 ? "Image added." : std::format("{} images added.", images.size());
            }
            result.dirty = true;
        },
        {}, masks ? "Import sheet images" : (paths.size() == 1 ? "Add image" : "Add images"));
}
void editor_window::generate()
{
    auto p = current();
    if (!p || p->busy || p->layers.empty())
        return;
    end_crop(true);
    app_.task(
        p, "Making sheets…",
        [](project& r)
        {
            r.cuts = make_sheets(r.layers, r.options).layers;
            r.selected_cut = 0;
            r.stale = false;
            r.dirty = true;
            r.view = view_mode::assembled;
            r.step = work_step::sheets;
            // A layer left with only its frame has nothing that reached
            // it: the subject floats (an object on a plain background).
            const auto empty = std::count_if(r.cuts.begin(), r.cuts.end() - 1, [&](const cut_layer& c) { return frame_only(c, r.options); });
            size_t islands = 0;
            for (const auto& c : r.cuts)
                islands += disconnected_components(c);
            r.activity = std::format("Made {} sheets.", r.cuts.size());
            if (empty)
                r.activity += std::format(" {} had nothing reaching the frame: the subject needs to touch the edge or a ground, or an AI rework.", empty);
            else if (islands)
                r.activity += std::format(" {} floating pieces remain.", islands);
            else
                r.activity += " Every piece reaches the frame.";
        },
        {}, "Auto");
}
void editor_window::flatten_sources()
{
    auto p = current();
    if (!p || p->busy)
        return;
    end_crop(true);
    app_.task(
        p, "Flattening images…",
        [](project& r)
        {
            if (r.layers.empty())
                throw std::runtime_error("Add images first.");
            // The flattened composition already includes its frame: keep only
            // the opening, then place that exactly over the opening again.
            auto image = flatten(r.layers, r.options);
            const auto g = dimensions(r.options);
            raster inner{g.opening_width(), g.opening_height(), {}};
            inner.rgba.reserve(size_t(inner.width) * inner.height * 4);
            for (int y = g.top; y < image.height - g.bottom; ++y)
            {
                auto start = image.rgba.begin() + (size_t(y) * image.width + g.left) * 4;
                inner.rgba.insert(inner.rgba.end(), start, start + size_t(inner.width) * 4);
            }
            layer flat{"Flattened composition", std::move(inner)};
            flat.transform = place_image(flat, r.options, placement_mode::fit, 0);
            r.layers = {std::move(flat)};
            r.assign_layer_ids();
            r.selected_layer = 0;
            r.stale = !r.cuts.empty();
            r.dirty = true;
            r.view = view_mode::original;
            r.step = work_step::arrange;
            r.activity = "Flattened the visible images into one.";
        },
        {}, "Flatten images");
}
void editor_window::move_layer(int delta)
{
    auto p = current();
    if (!p || p->busy)
        return;
    const int i = p->selected_layer, j = i + delta;
    if (i < 0 || j < 0 || i >= int(p->layers.size()) || j >= int(p->layers.size()))
        return;
    end_crop(true);
    std::swap(p->layers[i], p->layers[j]);
    p->selected_layer = j;
    edited(delta > 0 ? "Bring forward" : "Send backward");
}
void editor_window::remove_selected()
{
    auto p = current();
    if (!p || p->busy || p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
        return;
    end_crop(false);
    const auto name = p->layers[p->selected_layer].name;
    p->layers.erase(p->layers.begin() + p->selected_layer);
    p->selected_layer = std::min(p->selected_layer, int(p->layers.size()) - 1);
    edited("Remove image");
    flash("Removed " + name + ". Ctrl+Z brings it back.");
}
void editor_window::export_dialog()
{
    auto p = current();
    if (!p || p->busy)
        return;
    if (p->cuts.empty())
    {
        flash("Nothing to export yet: Auto makes the sheets (Ctrl+G).");
        return;
    }
    // Preflight and cut list, then the folder.
    std::string preflight, cuts;
    if (p->stale)
        preflight += "• The design changed after the sheets were made: Auto makes them again. Export uses the sheets as they are.\n";
    size_t floating = 0;
    for (size_t i = 0; i < p->cuts.size(); ++i)
    {
        const auto& c = p->cuts[i];
        const size_t pieces = disconnected_components(c);
        floating += pieces;
        const bool empty = i + 1 < p->cuts.size() && frame_only(c, p->options);
        cuts += std::format("{:02}.svg  {}{}{}  {:g} × {:g} mm\n", i + 1, c.name, i == 0 ? " (front)" : i + 1 == p->cuts.size() ? " (backing)" : "",
                            empty ? " · frame only" : pieces ? std::format(" · {} floating", pieces) : "", p->options.width_mm, p->options.height_mm);
    }
    if (floating)
        preflight += std::format("• {} floating pieces would fall out: they reach no edge of their sheet.\n", floating);
    if (preflight.empty())
        preflight = "No problems found.";
    struct export_state
    {
        std::unique_ptr<rap::window> window;
    };
    auto dialog = std::make_shared<export_state>();
    auto weak = std::weak_ptr<void>(alive_);
    const bool stale = p->stale;
    dialog->window = app_.ui.create(
        L::dialog{"Export — Offcut",
                  {L::vbox{
                      L::group{"Preflight", L::label{preflight}.wrap()},
                      L::group{"Cut list (front first)", L::label{cuts}.selectable()},
                      L::label{"Writes numbered SVG sheets, black-on-white PNG masks, a preview and ASSEMBLY.txt into a new folder."}.wrap(),
                      L::buttons{L::dialog_button{dialog_role::ok, stale ? "Make sheets, then export" : "Export SVGs…"}, L::cancel},
                  }}}
            .size(560_px, {}),
        window_.get());
    dialog->window->on_finished(
        [this, weak, dialog, stale](dialog_role role) mutable
        {
            auto keep = std::move(dialog);
            if (role == dialog_role::ok && !weak.expired())
            {
                if (stale)
                    generate();
                else
                    export_files(false);
            }
            app_.ui.post([keep] {});
        });
    dialog->window->show();
}
void editor_window::export_files(bool ai)
{
    auto p = current();
    if (!p || p->busy)
        return;
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.choose_files(
        window_.get(), {.mode = file_dialog_spec::mode::folder, .title = ai ? "Choose a folder for the AI request" : "Choose a folder for numbered SVG sheets"},
        [this, weak, p, ai](std::vector<std::string> paths)
        {
            if (weak.expired() || paths.empty() || p->busy)
                return;
            last_export_parent_ = from_utf8(paths.front());
            app_.task(p, ai ? "Preparing AI request…" : "Tracing SVG contours…",
                      [parent = from_utf8(paths.front()), ai](project& r)
                      {
                          const auto folder = ai ? export_ai_request(r, parent) : export_layers(r, parent);
                          r.activity = "Exported to " + utf8(folder);
                      });
        });
}
void editor_window::choose_project()
{
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.choose_files(window_.get(), {.mode = file_dialog_spec::mode::open, .title = "Open Offcut project", .filters = {{"Offcut project", "*.offcut"}}},
                         [this, weak](std::vector<std::string> paths)
                         {
                             if (weak.expired() || paths.empty())
                                 return;
                             auto p = reusable() ? current() : std::make_shared<project>();
                             p->name = utf8(from_utf8(paths.front()).stem());
                             if (p != current())
                                 insert({app_.next_id(), p});
                             end_crop(false);
                             app_.task(p, "Opening project…",
                                       [path = from_utf8(paths.front())](project& r)
                                       {
                                           r = load_project(path);  // without a history: the window starts a new one
                                           r.activity = "Project opened.";
                                       });
                         });
}
void editor_window::save(std::shared_ptr<project> p, std::function<void()> done)
{
    if (!p || p->busy)
        return;
    if (p == current())
        end_crop(true);
    auto weak = std::weak_ptr<void>(alive_);
    auto write = [this, weak, p, done = std::move(done)](std::filesystem::path path)
    {
        if (weak.expired())
            return;
        try
        {
            save_project(*p, path);
            p->path = path;
            p->dirty = false;
            if (p->undo)
                p->undo->mark_saved();
            flash("Project saved.");
            app_.changed();
            if (done)
                done();
        }
        catch (const std::exception& e)
        {
            app_.ui.message(window_.get(), {.icon = message_spec::icon::error, .title = "Save project", .text = e.what()});
        }
    };
    if (!p->path.empty())
    {
        write(p->path);
        return;
    }
    app_.ui.choose_files(window_.get(),
                         {.mode = file_dialog_spec::mode::save,
                          .title = "Save Offcut project",
                          .initial_path = p->name + ".offcut",
                          .filters = {{"Offcut project", "*.offcut"}}},
                         [write = std::move(write)](std::vector<std::string> paths)
                         {
                             if (paths.empty())
                                 return;
                             auto path = from_utf8(paths.front());
                             if (!path.has_extension())
                                 path += ".offcut";
                             write(path);
                         });
}
void editor_window::confirm_close(std::vector<std::shared_ptr<project>> projects, std::function<void()> done)
{
    if (projects.empty())
    {
        done();
        return;
    }
    auto p = projects.back();
    projects.pop_back();
    if (p->busy)
    {
        app_.ui.message(window_.get(), {.title = "Project is working", .text = "Wait for the current operation to finish before closing this project."});
        return;
    }
    if (!p->dirty)
    {
        confirm_close(std::move(projects), std::move(done));
        return;
    }
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.message(window_.get(),
                    {.icon = message_spec::icon::question,
                     .title = "Save changes?",
                     .text = "Save changes to “" + p->name + "” before closing?",
                     .buttons = {dialog_role::yes, dialog_role::no, dialog_role::cancel}},
                    [this, weak, p, projects = std::move(projects), done = std::move(done)](dialog_role role) mutable
                    {
                        if (weak.expired())
                            return;
                        auto next = [this, weak, projects = std::move(projects), done = std::move(done)]() mutable
                        {
                            if (!weak.expired())
                                confirm_close(std::move(projects), std::move(done));
                        };
                        if (role == dialog_role::yes)
                            save(p, std::move(next));
                        else if (role == dialog_role::no)
                            next();
                    });
}
void editor_window::close_tab(const std::string& id)
{
    auto it = std::ranges::find_if(tabs_, [&](const auto& t) { return t.id == id; });
    if (it == tabs_.end())
        return;
    confirm_close({it->document},
                  [this, id]
                  {
                      take(id);
                      if (tabs_.empty())
                          app_.close(*this);
                  });
}
void editor_window::close_window()
{
    std::vector<std::shared_ptr<project>> projects;
    for (auto& t : tabs_)
        projects.push_back(t.document);
    confirm_close(std::move(projects), [this] { app_.close(*this); });
}
}  // namespace offcut
