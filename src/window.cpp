#include "window.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include "export.h"
#include "image_io.h"
#include "settings.h"
#include "transform.h"

namespace offcut
{
namespace L = rap::layout;
using namespace rap;

editor_window::editor_window(application& a, project_tab first) : app_(a)
{
    build();
    insert(std::move(first));
}
std::shared_ptr<project> editor_window::current() const
{
    for (const auto& t : tabs_)
        if (t.id == selected_)
            return t.document;
    return {};
}
void editor_window::build()
{
    const auto changed = [this](double) { edit_options(); };
    window_ = app_.ui.create(L::window{"Offcut",{L::vbox{
        L::toolbar{
            L::button{"New from image…"}.on_click([this] { choose_images(true); }),
            L::button{"Open…"}.on_click([this] { choose_project(); }),
            L::hbox{
                L::button{"Add images…"}.on_click([this] { choose_images(false); }),
                L::button{"Save"}.on_click([this] { save(current()); }),
                L::separator{},
                L::button{"Generate layers"}.on_click([this] { generate(); }),
                L::button{"Export SVGs…"}.on_click([this] { export_files(false); }),
            }.handle(actions_),
            L::spacer{},
            L::tool_button{rap::icon::named("preferences-system-symbolic"),"Settings"}.on_click([this] { show_settings(app_.ui,window_.get()); }),
        },
        L::splitter{L::horizontal,
            L::vbox{
                L::hbox{
                    L::label{"Preview"},
                    L::drop_down{{"Source composition","Assembled layers","Selected cut layer"},0}.bind(mode_).on_changed([this](int i) {
                        if (auto p = current()) { p->view = view_mode(std::clamp(i,0,2)); update_preview(); }
                    }),
                    L::spacer{},
                    L::button{"−"}.tooltip("Zoom out").on_click([this] { zoom(1/1.2f); }),
                    L::label{"Fit"}.bind(zoom_label_),
                    L::button{"+"}.tooltip("Zoom in").on_click([this] { zoom(1.2f); }),
                    L::button{"Fit"}.on_click([this] { if (auto p = current()) { p->zoom=1; p->pan_x=p->pan_y=0; } canvas_->invalidate(); zoom_label_->set_text("Fit"); }),
                }.margins(),
                L::canvas{}.bind(canvas_).stretch()
                    .on_paint([this](painter& painter,size_f size) { paint(painter,size); })
                    .on_scroll([this](const scroll_event& e) { zoom(std::pow(1.15f,-e.dy)); return true; })
                    .on_pointer([this](const pointer_event& e) {
                        auto p = current(); if (!p) return false;
                        if (e.type == pointer_event_type::press && (e.button == 1 || e.button == 2)) {
                            dragging_=true; drag_={e.pos.x-p->pan_x,e.pos.y-p->pan_y}; canvas_->set_cursor(cursor::grabbing);
                        } else if (e.type == pointer_event_type::release || e.type == pointer_event_type::leave) {
                            dragging_=false; canvas_->set_cursor(cursor::grab);
                        } else if (e.type == pointer_event_type::move && dragging_) {
                            p->pan_x=e.pos.x-drag_.x; p->pan_y=e.pos.y-drag_.y; canvas_->invalidate();
                        }
                        return true;
                    }),
                L::hbox{L::label{"Scroll to zoom · drag to pan"}.stretch(),L::label{}.bind(summary_)}.margins(),
            }.stretch(),
            L::tabs{
              L::page{"Design",L::vbox{
                L::group{"Project",L::vbox{
                    L::entry{}.placeholder("Project name").bind(name_).on_changed([this](const std::string& s) {
                        if (auto p=current()) { p->name=s; p->dirty=true; app_.changed(); }
                    }),
                    L::form{
                        {"Width (mm)",L::spin{30,1000,200,1,1}.bind(width_).on_changed(changed)},
                        {"Height (mm)",L::spin{30,1000,200,1,1}.bind(height_).on_changed(changed)},
                        {"Frame (mm)",L::spin{2,100,10,.5,1}.bind(border_).on_changed(changed)},
                    },
                }},
                L::group{"Source images · bottom → top",L::vbox{
                    L::tree{}.bind(sources_).height(5_lines).on_selection_changed([this] {
                        if (auto p=current(); p && sources_->selected()) p->selected_source=std::stoi(*sources_->selected());
                    }).on_toggled([this](const std::string& id) {
                        auto p=current(); if (!p || p->busy) return; const auto i=size_t(std::stoul(id));
                        if (i<p->sources.size()) { p->sources[i].visible=!p->sources[i].visible; p->dirty=true; p->stale=!p->cuts.empty(); app_.changed(); }
                    }),
                    L::hbox{
                        L::button{"Add…"}.on_click([this] { choose_images(false); }),
                        L::button{"↑"}.tooltip("Move earlier in stack").on_click([this] { move_source(-1); }),
                        L::button{"↓"}.tooltip("Move later in stack").on_click([this] { move_source(1); }),
                        L::button{"Remove"}.on_click([this] { remove_selected(); }),
                    },
                    L::button{"Flatten visible sources"}.on_click([this] { flatten_sources(); }),
                }},
                L::group{"Simplify into cut layers",L::vbox{
                    L::form{
                        {"Layers",L::spin{2,12,7}.bind(layers_).on_changed(changed)},
                        {"Smoothing",L::spin{0,6,2}.bind(smooth_).on_changed(changed)},
                        {"Bridge (mm)",L::spin{.5,10,2,.5,1}.bind(bridge_).on_changed(changed)},
                    },
                    L::check{"Connect pieces to frame",true}.bind(connect_).on_toggled([this](bool) { edit_options(); }),
                    L::button{"Generate layers locally"}.on_click([this] { generate(); }),
                    L::label{"Experimental tonal separation. Review the layers and test-cut before making the full design."}.wrap(),
                }},
                L::spacer{},
              }.margins()},
              L::page{"Layers",L::vbox{
                L::label{"Cut layers · front → back"},
                    L::tree{}.bind(cuts_).stretch().on_selection_changed([this] {
                        auto p=current(); if (!p || !cuts_->selected()) return;
                        p->selected_cut=std::stoi(*cuts_->selected()); p->view=view_mode::layer; mode_->set_current(2); update_preview();
                    }).on_toggled([this](const std::string& id) {
                        auto p=current(); if (!p || p->busy) return; const auto i=size_t(std::stoul(id));
                        if (i<p->cuts.size()) { p->cuts[i].visible=!p->cuts[i].visible; p->dirty=true; app_.changed(); }
                    }),
                    L::label{"Boxes affect the preview. Export includes all numbered layers."}.wrap(),
              }.margins()},
              L::page{"AI",L::vbox{
                L::group{"AI-assisted design",L::vbox{
                    L::text_view{}.height(6_lines).bind(instructions_).on_changed([this] {
                        if (auto p=current()) { p->instructions=instructions_->text(); p->dirty=true; }
                    }),
                    L::label{"Optional instructions: e.g. hearts around Jay’s name."}.wrap(),
                    L::button{"Copy detailed AI prompt"}.on_click([this] { if (auto p=current()) { app_.ui.set_clipboard(ai_prompt(*p)); status_->set_text("AI prompt copied. Attach your reference images in your chosen AI app."); } }),
                    L::button{"Export AI request…"}.on_click([this] { export_files(true); }),
                    L::button{"Import layer images…"}.on_click([this] { choose_images(false,true); }),
                    L::label{"Manual handoff · configure your preferred app in Settings."}.wrap(),
                }},
                L::spacer{},
              }.margins()},
            }.handle(controls_).width(43_ch),
        }.thin().stretch(),
        L::hbox{L::label{"Ready"}.bind(status_).ellipsize().stretch()}.margins(),
    }}}.size(160_ch,48_lines).margins(false).base_surface()
      .tabs(L::document_tabs{}.bind(tab_bar_).group("offcut.projects")
        .on_new([this] { insert({app_.next_id(),std::make_shared<project>()}); })
        .on_select([this](const std::string& id) { selected_=id; refresh(); })
        .on_close([this](const std::string& id) { close_tab(id); })
        .on_reorder([this](const std::string& id,int index) {
            auto it=std::ranges::find_if(tabs_,[&](const auto& t) { return t.id==id; });
            if (it==tabs_.end()) return; auto t=std::move(*it); tabs_.erase(it);
            tabs_.insert(tabs_.begin()+std::clamp(index,0,int(tabs_.size())),std::move(t));
        })
        .on_detach([this](const std::string& id,std::optional<point_f>) { return app_.detach(*this,id); })
        .on_receive([this](const document_tabs::transfer& t) { return app_.receive(*this,t); }))
      .menu_bar({
        {"_File",[this] { return rap::menu{
            {.label="_New from image…",.action=[this] { choose_images(true); },.shortcut="Ctrl+N"},
            {.label="_Open project…",.action=[this] { choose_project(); },.shortcut="Ctrl+O"},
            {.label="_Save project",.action=[this] { save(current()); },.shortcut="Ctrl+S"},
            {.label="Export _SVG layers…",.action=[this] { export_files(false); }},
            rap::menu_item::separator(),
            {.label="New _window",.action=[this] { app_.open(); }},
            {.label="_Close project",.action=[this] { close_tab(selected_); },.shortcut="Ctrl+W"},
            {.label="Settings…",.action=[this] { show_settings(app_.ui,window_.get()); }},
        }; }},
        {"_Project",[this] { return rap::menu{
            {.label="_Add source images…",.action=[this] { choose_images(false); }},
            {.label="_Flatten visible sources",.action=[this] { flatten_sources(); }},
            {.label="_Generate layers",.action=[this] { generate(); }},
            {.label="Export _AI request…",.action=[this] { export_files(true); }},
            {.label="_Import layer images…",.action=[this] { choose_images(false,true); }},
        }; }},
      }));
    window_->on_key([this](const key_event& e) { return key(e); });
    window_->on_close_request(
        [this]
        {
            close_window();
            return true;
        });
}
void editor_window::insert(project_tab tab, int index)
{
    const auto id = tab.id;
    const auto p = tab.document;
    const int at = index < 0 ? int(tabs_.size()) : std::clamp(index, 0, int(tabs_.size()));
    tabs_.insert(tabs_.begin() + at, std::move(tab));
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
    for (const auto& t : tabs_)
        tab_bar_->update({.id = t.id,
                          .title = t.document->name + (t.document->dirty ? " *" : ""),
                          .tooltip = t.document->path.empty() ? "Unsaved project" : utf8(t.document->path)});
    window_->set_title(p->name + " — Offcut");
    controls_->set_enabled(!p->busy);
    actions_->set_enabled(!p->busy);
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
    sync(bridge_, p->options.bridge_mm);
    sync(layers_, p->options.layers);
    sync(smooth_, p->options.smoothing);
    connect_->set_checked(p->options.connect);
    if (instructions_->text() != p->instructions)
        instructions_->set_text(p->instructions);
    std::vector<tree_item> sources, cuts;
    for (size_t i = 0; i < p->sources.size(); ++i)
        sources.push_back(
            {.label = p->sources[i].name, .id = std::to_string(i), .check = p->sources[i].visible ? check_state::checked : check_state::unchecked});
    for (size_t i = 0; i < p->cuts.size(); ++i)
        cuts.push_back({.label = std::format("{:02}  {}", i + 1, p->cuts[i].name),
                        .id = std::to_string(i),
                        .check = p->cuts[i].visible ? check_state::checked : check_state::unchecked});
    sources_->set_items(std::move(sources));
    cuts_->set_items(std::move(cuts));
    if (!p->sources.empty())
        sources_->select(std::to_string(p->selected_source));
    if (!p->cuts.empty())
        cuts_->select(std::to_string(p->selected_cut));
    mode_->set_current(int(p->view));
    zoom_label_->set_text(std::format("{:.0f}% fit", p->zoom * 100));
    summary_->set_text(std::format("{} × {} mm · {} layers", p->options.width_mm, p->options.height_mm, p->cuts.size()));
    status_->set_text(p->busy ? p->activity
                              : (p->stale ? "Design changed — regenerate or reimport cut layers before export."
                                          : (p->activity.empty() ? "Ready · create a project from an image to begin" : p->activity)));
    update_preview();
}
void editor_window::edit_options()
{
    auto p = current();
    if (!p || p->busy)
        return;
    p->options = {width_->value(), height_->value(), border_->value(), bridge_->value(), int(layers_->value()), int(smooth_->value()), connect_->checked()};
    p->dirty = true;
    p->stale = !p->cuts.empty();
    app_.changed();
}
void editor_window::update_preview()
{
    auto p = current();
    preview_.reset();
    if (!p)
        return;
    try
    {
        raster image;
        if (p->view == view_mode::original && !p->sources.empty())
            image = flatten(p->sources, p->options);
        else if (p->view != view_mode::original)
            image = render_cuts(p->cuts, p->view == view_mode::layer ? p->selected_cut : -1);
        if (image.valid())
            preview_ = app_.ui.create_image(image.width, image.height, pixel_format::rgba8, image.rgba.data(), size_t(image.width) * 4);
    }
    catch (const std::exception& e)
    {
        status_->set_text(e.what());
    }
    canvas_->invalidate();
}
void editor_window::paint(painter& painter, size_f size)
{
    painter.rect({0, 0, size.w, size.h}, 0, color{36, 41, 44}, std::nullopt);
    for (float x = 24; x < size.w; x += 32)
        for (float y = 24; y < size.h; y += 32)
            painter.circle({x, y}, .7f, color{58, 64, 66}, std::nullopt);
    if (!preview_)
    {
        const bool empty = !current() || current()->sources.empty();
        const std::string title = empty ? "Make something of it." : "Your cut layers will appear here.";
        text_style heading{.fg = color{231, 224, 209}, .bold = true, .size = 23};
        const auto measure = painter.measure(title, heading);
        painter.text({std::max(24.f, (size.w - measure.w) / 2), size.h * .43f}, title, heading);
        const std::string detail =
            empty ? "Create a project from a photo, drawing or design." : "Generate layers locally or import layer images from your AI app.";
        text_style body{.fg = color{163, 176, 176}};
        const auto m = painter.measure(detail, body);
        painter.text({std::max(24.f, (size.w - m.w) / 2), size.h * .43f + 48}, detail, body);
        return;
    }
    auto p = current();
    auto dst = fit_inside({32, 32, std::max(1.f, size.w - 64), std::max(1.f, size.h - 64)}, float(preview_->width()), float(preview_->height()));
    const float cx = dst.x + dst.w / 2, cy = dst.y + dst.h / 2;
    dst.w *= p->zoom;
    dst.h *= p->zoom;
    dst.x = cx - dst.w / 2 + p->pan_x;
    dst.y = cy - dst.h / 2 + p->pan_y;
    painter.rect({dst.x + 7, dst.y + 9, dst.w, dst.h}, 0, color{0, 0, 0, 90}, std::nullopt);
    painter.image(*preview_, {0, 0, float(preview_->width()), float(preview_->height())}, dst);
}
void editor_window::zoom(float factor)
{
    if (auto p = current())
    {
        p->zoom = std::clamp(p->zoom * factor, .1f, 16.f);
        zoom_label_->set_text(std::format("{:.0f}% fit", p->zoom * 100));
        canvas_->invalidate();
    }
}
void editor_window::choose_images(bool create, bool masks)
{
    auto p = current();
    if (p && p->busy && !create)
        return;
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.choose_files(window_.get(),
                         {.mode = file_dialog_spec::mode::open_many,
                          .title = masks ? "Import aligned layer images — black means cardstock" : "Choose source images",
                          .filters = {{"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif"}, {"All files", "*.*"}}},
                         [this, weak, create, masks](std::vector<std::string> paths)
                         {
                             if (!weak.expired() && !paths.empty())
                                 load_paths(std::move(paths), create, masks);
                         });
}
void editor_window::load_paths(std::vector<std::string> paths, bool create, bool masks)
{
    auto p = current();
    if (create)
    {
        p = std::make_shared<project>();
        p->name = utf8(from_utf8(paths.front()).stem());
        insert({app_.next_id(), p});
    }
    if (!p || p->busy)
        return;
    app_.task(p, "Reading images…",
              [paths = std::move(paths), masks](project& result)
              {
                  if (paths.size() > 32 || (!masks && result.sources.size() + paths.size() > 32))
                      throw std::runtime_error("A project supports up to 32 source images.");
                  std::vector<source_image> images;
                  for (auto& path : paths)
                      images.push_back(read_image(from_utf8(path)));
                  if (masks)
                  {
                      std::ranges::sort(images, {}, &source_image::name);
                      for (const auto& image : images)
                          if (image.image.width != images.front().image.width || image.image.height != images.front().image.height)
                              throw std::runtime_error("Layer images must have identical pixel dimensions and alignment.");
                      result.cuts = masks_from_images(images, result.options);
                      result.selected_cut = 0;
                      result.stale = false;
                      result.view = view_mode::assembled;
                      result.activity = "Imported layer images. Check the masks and bridge placement before cutting.";
                  }
                  else
                  {
                      for (auto& image : images)
                          result.sources.push_back(std::move(image));
                      result.stale = !result.cuts.empty();
                      result.view = view_mode::original;
                      result.activity = "Images added. Set the size and generate cut layers, or export an AI request.";
                  }
                  result.dirty = true;
              });
}
void editor_window::generate()
{
    auto p = current();
    if (!p || p->busy)
        return;
    app_.task(p, "Simplifying and connecting layers…",
              [](project& r)
              {
                  r.cuts = simplify(r.sources, r.options);
                  r.selected_cut = 0;
                  r.stale = false;
                  r.dirty = true;
                  r.view = view_mode::assembled;
                  size_t islands = 0;
                  for (const auto& c : r.cuts)
                      islands += disconnected_components(c);
                  r.activity = "Generated " + std::to_string(r.cuts.size()) + " layers · " + std::to_string(islands) +
                               " disconnected regions. Review each layer before cutting.";
              });
}
void editor_window::flatten_sources()
{
    auto p = current();
    if (!p || p->busy)
        return;
    app_.task(p, "Flattening source composition…",
              [](project& r)
              {
                  if (r.sources.empty())
                      throw std::runtime_error("Add source images first.");
                  // The flattened composition already includes its frame: strip that area
                  // before replacing the stack, so subsequent fitting adds it just once.
                  auto image = flatten(r.sources, r.options);
                  const double scale = 640. / std::max(r.options.width_mm, r.options.height_mm);
                  const int b = std::max(1, int(std::ceil(r.options.border_mm * scale)));
                  raster inner{image.width - 2 * b, image.height - 2 * b, {}};
                  inner.rgba.reserve(size_t(inner.width) * inner.height * 4);
                  for (int y = b; y < image.height - b; ++y)
                  {
                      auto start = image.rgba.begin() + (size_t(y) * image.width + b) * 4;
                      inner.rgba.insert(inner.rgba.end(), start, start + size_t(inner.width) * 4);
                  }
                  r.sources = {{"Flattened composition", std::move(inner)}};
                  r.selected_source = 0;
                  r.stale = !r.cuts.empty();
                  r.dirty = true;
                  r.view = view_mode::original;
                  r.activity = "Flattened visible source images into one image.";
              });
}
void editor_window::move_source(int delta)
{
    auto p = current();
    if (!p || p->busy)
        return;
    const int i = p->selected_source, j = i + delta;
    if (i < 0 || j < 0 || i >= int(p->sources.size()) || j >= int(p->sources.size()))
        return;
    std::swap(p->sources[i], p->sources[j]);
    p->selected_source = j;
    p->dirty = true;
    p->stale = !p->cuts.empty();
    app_.changed();
}
void editor_window::remove_selected()
{
    auto p = current();
    if (!p || p->busy || p->selected_source < 0 || p->selected_source >= int(p->sources.size()))
        return;
    p->sources.erase(p->sources.begin() + p->selected_source);
    p->selected_source = std::max(0, p->selected_source - 1);
    p->dirty = true;
    p->stale = !p->cuts.empty();
    app_.changed();
}
void editor_window::export_files(bool ai)
{
    auto p = current();
    if (!p || p->busy)
        return;
    auto weak = std::weak_ptr<void>(alive_);
    app_.ui.choose_files(
        window_.get(), {.mode = file_dialog_spec::mode::folder, .title = ai ? "Choose a folder for the AI request" : "Choose a folder for numbered SVG layers"},
        [this, weak, p, ai](std::vector<std::string> paths)
        {
            if (weak.expired() || paths.empty() || p->busy)
                return;
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
                             auto p = std::make_shared<project>();
                             p->name = utf8(from_utf8(paths.front()).stem());
                             insert({app_.next_id(), p});
                             app_.task(p, "Opening project…",
                                       [path = from_utf8(paths.front())](project& r)
                                       {
                                           r = load_project(path);
                                           r.activity = "Project opened.";
                                       });
                         });
}
void editor_window::save(std::shared_ptr<project> p, std::function<void()> done)
{
    if (!p || p->busy)
        return;
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
            p->activity = "Project saved.";
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
bool editor_window::key(const key_event& e)
{
    if (!e.press || !(e.modifiers & mod_ctrl))
        return false;
    if (e.codepoint == 'n')
        choose_images(true);
    else if (e.codepoint == 'o')
        choose_project();
    else if (e.codepoint == 's')
        save(current());
    else if (e.codepoint == 'w')
        close_tab(selected_);
    else if (e.codepoint == ',')
        show_settings(app_.ui, window_.get());
    else
        return false;
    return true;
}
}  // namespace offcut
