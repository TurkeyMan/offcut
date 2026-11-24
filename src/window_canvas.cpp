// The editor's canvas: drawing the frame and its handles, and editing image
// transforms with the pointer and keys.
#include <algorithm>
#include <cmath>
#include <format>
#include "guidance.h"
#include "history.h"
#include "settings.h"
#include "window.h"
namespace offcut
{
using namespace rap;
namespace
{
constexpr double snap_mm = 3;
constexpr float handle_radius = 7, rotate_reach = 28;
constexpr double pi = 3.14159265358979323846;
const color selection_colour{0, 153, 255}, guide_colour{255, 64, 160}, card_line{231, 224, 209};
float distance(point_f a, point_f b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}
point_f mid(point_f a, point_f b)
{
    return {(a.x + b.x) / 2, (a.y + b.y) / 2};
}
}  // namespace

// -- geometry -----------------------------------------------------------------

rect_f editor_window::frame_rect() const
{
    auto p = current();
    if (!p)
        return {};
    float w = 1, h = 1;
    if (preview_)
        w = float(preview_->width()), h = float(preview_->height());
    else
        w = float(p->options.width_mm), h = float(p->options.height_mm);
    const auto size = canvas_->size();
    auto dst = fit_inside({32, 32, std::max(1.f, size.w - 64), std::max(1.f, size.h - 64)}, w, h);
    const float cx = dst.x + dst.w / 2, cy = dst.y + dst.h / 2;
    dst.w *= p->zoom;
    dst.h *= p->zoom;
    dst.x = cx - dst.w / 2 + p->pan_x;
    dst.y = cy - dst.h / 2 + p->pan_y;
    return dst;
}
point_f editor_window::screen_from_frame(point_mm m) const
{
    auto p = current();
    const auto r = frame_rect();
    if (!p)
        return {};
    return {float(r.x + m.x / p->options.width_mm * r.w), float(r.y + m.y / p->options.height_mm * r.h)};
}
point_mm editor_window::frame_from_screen(point_f s) const
{
    auto p = current();
    const auto r = frame_rect();
    if (!p || r.w <= 0 || r.h <= 0)
        return {};
    return {(s.x - r.x) / r.w * p->options.width_mm, (s.y - r.y) / r.h * p->options.height_mm};
}

// -- drawing ------------------------------------------------------------------

void editor_window::update_preview()
{
    auto p = current();
    preview_.reset();
    if (!p)
        return;
    try
    {
        raster image;
        if (p->view == view_mode::original && !p->layers.empty())
        {
            // While cropping, the whole image shows; what's trimmed is shaded.
            if (cropping_ && p->selected_layer >= 0 && p->selected_layer < int(p->layers.size()))
            {
                auto layers = p->layers;
                auto& l = layers[p->selected_layer];
                l.transform = with_crop(effective_transform(l, p->options), full_crop(l.image));
                image = source_preview(layers, p->options);
            }
            else
                image = source_preview(p->layers, p->options);
        }
        else if (p->view != view_mode::original)
        {
            const int sheet = p->selected_cut >= 0 && p->selected_cut < int(p->cuts.size()) ? p->selected_cut : -1;
            image = render_cuts(p->cuts, {.single = p->view == view_mode::layer ? std::max(0, sheet) : -1,
                                          .highlight = p->view == view_mode::assembled ? sheet : -1,
                                          .floating = p->step == work_step::sheets});
        }
        if (image.valid())
            preview_ = app_.ui.create_image(image.width, image.height, pixel_format::rgba8, image.rgba.data(), size_t(image.width) * 4);
    }
    catch (const std::exception& e)
    {
        flash(e.what());
    }
    canvas_->invalidate();
}
void editor_window::paint(painter& painter, size_f size)
{
    painter.rect({0, 0, size.w, size.h}, 0, color{36, 41, 44}, std::nullopt);
    for (float x = 24; x < size.w; x += 32)
        for (float y = 24; y < size.h; y += 32)
            painter.circle({x, y}, .7f, color{58, 64, 66}, std::nullopt);
    auto p = current();
    const auto centred = [&](const std::string& s, float y, const text_style& style)
    {
        const auto m = painter.measure(s, style);
        painter.text({std::max(24.f, (size.w - m.w) / 2), y}, s, style);
    };
    const text_style heading{.fg = color{231, 224, 209}, .bold = true, .size = 23};
    const text_style body{.fg = color{163, 176, 176}};
    if (!preview_)
    {
        const bool empty = !p || (p->layers.empty() && p->cuts.empty());
        centred(empty ? "Drop a picture here" : "No sheets yet", size.h * .43f, heading);
        centred(empty ? "Or choose Add images (Ctrl+I), or Open project (Ctrl+O). Offcut makes it into stacked card sheets."
                      : "Auto (Ctrl+G) makes the sheets from your arranged images.",
                size.h * .43f + 48, body);
        return;
    }
    const auto dst = frame_rect();
    painter.rect({dst.x + 7, dst.y + 9, dst.w, dst.h}, 0, color{0, 0, 0, 90}, std::nullopt);
    painter.image(*preview_, {0, 0, float(preview_->width()), float(preview_->height())}, dst,
                  p->zoom * dst.w / std::max(1.f, float(preview_->width())) > 2.5f ? filter::nearest : filter::linear);
    if (p->step == work_step::colours && p->view == view_mode::original)
    {
        painter.rect(dst, 0, color{36, 41, 44, 120}, std::nullopt);
        centred("Not reduced yet", dst.y + dst.h * .45f, heading);
        centred("Auto (Ctrl+G) reduces the composition to sheet colours.", dst.y + dst.h * .45f + 44, body);
    }
    if (p->view != view_mode::original)
        return;
    // The opening, so the frame reads as card even where artwork overlaps it.
    {
        const auto open = opening_mm(p->options);
        const auto a = screen_from_frame({open.left, open.top}), b = screen_from_frame({open.right, open.bottom});
        painter.rect({a.x, a.y, b.x - a.x, b.y - a.y}, 0, std::nullopt, color{120, 110, 90, 160});
    }
    if (p->step != work_step::arrange)
        return;
    // Snap guides while dragging.
    const auto open = opening_mm(p->options);
    if (guide_x_)
    {
        const auto a = screen_from_frame({*guide_x_, 0}), b = screen_from_frame({*guide_x_, p->options.height_mm});
        painter.line({a.x, a.y - 12}, {b.x, b.y + 12}, guide_colour, 1.5f);
    }
    if (guide_y_)
    {
        const auto a = screen_from_frame({0, *guide_y_}), b = screen_from_frame({p->options.width_mm, *guide_y_});
        painter.line({a.x - 12, a.y}, {b.x + 12, b.y}, guide_colour, 1.5f);
    }
    (void)open;
    const auto t = selected_transform();
    if (!t || p->busy)
        return;
    const auto screen = [&](const image_transform& tr)
    {
        const auto c = corners(tr);
        return std::array{screen_from_frame(c[0]), screen_from_frame(c[1]), screen_from_frame(c[2]), screen_from_frame(c[3])};
    };
    const auto outline = [&](const std::array<point_f, 4>& c, color col, float w)
    {
        for (int i = 0; i < 4; ++i)
            painter.line(c[i], c[(i + 1) % 4], col, w);
    };
    if (cropping_)
    {
        const auto full = screen(with_crop(*t, full_crop(p->layers[p->selected_layer].image)));
        const auto kept = screen(with_crop(*t, crop_));
        const color shade{20, 22, 24, 150};
        for (int i = 0; i < 4; ++i)
            painter.polygon({full[i], full[(i + 1) % 4], kept[(i + 1) % 4], kept[i]}, shade, std::nullopt);
        outline(full, color{255, 255, 255, 90}, 1);
        // A two-tone outline reads on light and dark artwork alike.
        outline(kept, color{0, 0, 0, 160}, 3);
        outline(kept, color{255, 255, 255}, 1.2f);
        for (int i = 0; i < 4; ++i)
        {
            const auto m = mid(kept[i], kept[(i + 1) % 4]);
            const float dx = kept[(i + 1) % 4].x - kept[i].x, dy = kept[(i + 1) % 4].y - kept[i].y, len = std::max(1.f, std::hypot(dx, dy));
            const point_f along{dx / len * 10, dy / len * 10};
            painter.line({m.x - along.x, m.y - along.y}, {m.x + along.x, m.y + along.y}, color{0, 0, 0, 180}, 7, line_cap::round);
            painter.line({m.x - along.x, m.y - along.y}, {m.x + along.x, m.y + along.y}, color{255, 255, 255}, 4, line_cap::round);
        }
        return;
    }
    const auto c = screen(*t);
    outline(c, color{0, 0, 0, 120}, 3);
    outline(c, selection_colour, 1.5f);
    // Rotation handle above the top edge, on the image's own "up".
    const double a = t->rotation_deg * pi / 180;
    const point_f up{float(std::sin(a)), float(-std::cos(a))};
    const auto top = mid(c[0], c[1]);
    const point_f knob{top.x + up.x * rotate_reach, top.y + up.y * rotate_reach};
    painter.line(top, knob, selection_colour, 1.5f);
    painter.circle(knob, 5.5f, color{255, 255, 255}, selection_colour);
    for (const auto& corner : c)
        painter.rect({corner.x - 5, corner.y - 5, 10, 10}, 1, color{255, 255, 255}, selection_colour);
}
void editor_window::paint_stages(painter& p, size_f size)
{
    stage_rects_.clear();
    auto doc = current();
    const char* names[] = {"1  Arrange", "2  Colours", "3  Sheets"};
    const float w = (size.w - 2) / 3, h = size.h - 4;
    // State marks: done ✓, needs attention •.
    std::string marks[3];
    if (doc)
    {
        if (!doc->layers.empty())
            marks[0] = "✓";
        if (!doc->cuts.empty())
        {
            marks[1] = doc->stale ? "•" : "✓";
            bool problem = doc->stale;
            for (size_t i = 0; i < floating_.size(); ++i)
                problem = problem || floating_[i] != 0;
            for (size_t i = 0; i + 1 < doc->cuts.size(); ++i)
                problem = problem || frame_only(doc->cuts[i], doc->options);
            marks[2] = problem ? "•" : "✓";
        }
    }
    p.rect({1, 2, size.w - 2, h}, 5, rap::paint{theme_color::base}, rap::paint{theme_color::border});
    for (int i = 0; i < 3; ++i)
    {
        const rect_f r{1 + w * i, 2, w, h};
        stage_rects_.push_back(r);
        const bool on = doc && int(doc->step) == i;
        if (on)
            p.rect({r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 4, rap::paint{theme_color::highlight}, std::nullopt);
        if (i > 0)
            p.line({r.x, r.y + 5}, {r.x, r.y + r.h - 5}, rap::paint{theme_color::divider});
        text_style style{.fg = on ? rap::paint{theme_color::highlight_text} : rap::paint{theme_color::window_text}, .bold = on};
        const std::string label = names[i];
        const auto m = p.measure(label, style);
        const auto mark_style = text_style{.fg = on                ? rap::paint{theme_color::highlight_text}
                                                 : marks[i] == "•" ? rap::paint{theme_color::warning}
                                                                   : rap::paint{theme_color::positive},
                                           .bold = true};
        // Room for a mark is always kept, so labels don't shift as marks come and go.
        const float mark_w = p.measure("✓", mark_style).w + 6;
        const float x = r.x + (r.w - m.w - mark_w) / 2, y = r.y + (r.h - m.h) / 2;
        p.text({x, y}, label, style);
        if (!marks[i].empty())
            p.text({x + m.w + 6, y}, marks[i], mark_style);
    }
}
bool editor_window::stage_pointer(const pointer_event& e)
{
    if (e.type != pointer_event_type::press || e.button != 1)
        return false;
    for (size_t i = 0; i < stage_rects_.size(); ++i)
    {
        const auto& r = stage_rects_[i];
        if (e.pos.x >= r.x && e.pos.x < r.x + r.w)
        {
            set_stage(work_step(i));
            return true;
        }
    }
    return false;
}
void editor_window::zoom(float factor)
{
    if (auto p = current())
    {
        p->zoom = std::clamp(p->zoom * factor, .1f, 16.f);
        zoom_label_->set_text(p->zoom == 1 ? std::string("Fit") : std::format("{:.0f}%", p->zoom * 100));
        canvas_->invalidate();
    }
}
void editor_window::zoom_at(float factor, point_f at)
{
    auto p = current();
    if (!p)
        return;
    // Keep the point under the pointer where it is.
    const float before = p->zoom;
    const float after = std::clamp(before * factor, .1f, 16.f);
    const auto size = canvas_->size();
    const point_f centre{size.w / 2, size.h / 2};
    const float k = after / before;
    p->pan_x = at.x - centre.x - (at.x - centre.x - p->pan_x) * k;
    p->pan_y = at.y - centre.y - (at.y - centre.y - p->pan_y) * k;
    zoom(k);
}

// -- pointer ------------------------------------------------------------------

bool editor_window::pointer(const pointer_event& e)
{
    auto p = current();
    if (!p)
        return false;
    const bool editing = p->step == work_step::arrange && p->view == view_mode::original && !p->busy;
    const auto pt = frame_from_screen(e.pos);
    auto t = selected_transform();
    // Which handle of the selection, if any, is under a screen point.
    const auto handle_at = [&](point_f s, int& corner) -> drag_kind
    {
        corner = -1;
        if (!t || !editing)
            return drag_kind::none;
        if (cropping_)
        {
            const auto c = corners(with_crop(*t, crop_));
            for (int i = 0; i < 4; ++i)
            {
                const auto a = screen_from_frame(c[i]), b = screen_from_frame(c[(i + 1) % 4]);
                // Near the edge's line, between its ends.
                const float dx = b.x - a.x, dy = b.y - a.y, len2 = std::max(1.f, dx * dx + dy * dy);
                const float u = ((s.x - a.x) * dx + (s.y - a.y) * dy) / len2;
                const point_f q{a.x + u * dx, a.y + u * dy};
                if (u > -.02f && u < 1.02f && distance(q, s) <= handle_radius)
                {
                    corner = i;  // 0 top, 1 right, 2 bottom, 3 left
                    return drag_kind::crop_edge;
                }
            }
            return drag_kind::none;
        }
        const auto c = corners(*t);
        const double a = t->rotation_deg * pi / 180;
        const auto top = mid(screen_from_frame(c[0]), screen_from_frame(c[1]));
        const point_f knob{top.x + float(std::sin(a)) * rotate_reach, top.y - float(std::cos(a)) * rotate_reach};
        if (distance(knob, s) <= handle_radius + 2)
            return drag_kind::rotate;
        for (int i = 0; i < 4; ++i)
            if (distance(screen_from_frame(c[i]), s) <= handle_radius + 1)
            {
                corner = i;
                return drag_kind::scale;
            }
        return drag_kind::none;
    };
    const auto set_hover_cursor = [&]
    {
        int corner;
        const auto kind = handle_at(e.pos, corner);
        cursor c = cursor::default_;
        if (space_down_)
            c = cursor::grab;
        else if (kind == drag_kind::rotate)
            c = cursor::pointer;
        else if (kind == drag_kind::scale)
            c = (corner % 2 == 0) == (std::fmod(std::abs(t->rotation_deg) + 45, 180) < 90) ? cursor::resize_nwse : cursor::resize_nesw;
        else if (kind == drag_kind::crop_edge)
            c = corner % 2 ? cursor::resize_ew : cursor::resize_ns;
        else if (editing && !cropping_ && hit_test(p->layers, p->options, pt) >= 0)
            c = cursor::move;
        canvas_->set_cursor(c);
    };
    const auto start_pan = [&]
    {
        drag_ = drag_kind::pan;
        drag_start_screen_ = e.pos;
        pan_start_ = {p->pan_x, p->pan_y};
        canvas_->set_cursor(cursor::grabbing);
    };
    switch (e.type)
    {
        case pointer_event_type::press:
        {
            canvas_->focus();
            if (drag_ != drag_kind::none)
                return true;
            if (e.button == 2 || (e.button == 1 && space_down_))
            {
                start_pan();
                return true;
            }
            if (e.button != 1)
                return false;
            if (!editing)
            {
                start_pan();
                return true;
            }
            int corner;
            auto kind = handle_at(e.pos, corner);
            const int hit = hit_test(p->layers, p->options, pt);
            if (cropping_ && kind == drag_kind::none)
            {
                // A click away from the crop's edges applies it.
                end_crop(true);
                t = selected_transform();
            }
            drag_start_screen_ = e.pos;
            drag_start_ = pt;
            drag_moved_ = false;
            handle_ = corner;
            if (kind != drag_kind::none)
            {
                drag_ = kind;
                start_transform_ = *t;
                return true;
            }
            if (hit >= 0)
            {
                if (hit != p->selected_layer)
                    select_layer(hit);
                if (e.clicks == 2)
                {
                    begin_crop();
                    return true;
                }
                drag_ = drag_kind::move;
                start_transform_ = *selected_transform();
                canvas_->set_cursor(cursor::move);
                return true;
            }
            // Empty canvas: deselect, and drag to pan.
            if (p->selected_layer >= 0)
                select_layer(-1);
            start_pan();
            return true;
        }
        case pointer_event_type::move:
        {
            if (drag_ == drag_kind::none)
            {
                set_hover_cursor();
                return true;
            }
            if (distance(e.pos, drag_start_screen_) > 2)
                drag_moved_ = true;
            if (drag_ == drag_kind::pan)
            {
                p->pan_x = pan_start_.x + e.pos.x - drag_start_screen_.x;
                p->pan_y = pan_start_.y + e.pos.y - drag_start_screen_.y;
                canvas_->invalidate();
                return true;
            }
            if (p->selected_layer < 0 || p->selected_layer >= int(p->layers.size()))
                return true;
            const bool snapping = snap_->checked() && !(e.modifiers & mod_alt);
            const auto open = opening_mm(p->options);
            guide_x_.reset();
            guide_y_.reset();
            auto next = start_transform_;
            if (drag_ == drag_kind::move)
            {
                next.x_mm += pt.x - drag_start_.x;
                next.y_mm += pt.y - drag_start_.y;
                if (snapping)
                {
                    const auto s = snap_to_opening(bounds(next), open, snap_mm);
                    next.x_mm += s.dx;
                    next.y_mm += s.dy;
                    guide_x_ = s.guide_x;
                    guide_y_ = s.guide_y;
                }
            }
            else if (drag_ == drag_kind::scale)
            {
                // Uniform scale about the opposite corner.
                const auto c = corners(start_transform_);
                const auto anchor = c[(handle_ + 2) % 4];
                const double d0x = c[handle_].x - anchor.x, d0y = c[handle_].y - anchor.y;
                const double factor_raw = ((pt.x - anchor.x) * d0x + (pt.y - anchor.y) * d0y) / std::max(1e-9, d0x * d0x + d0y * d0y);
                const double least = 2 / std::max(1e-9, std::min(width_mm(start_transform_), height_mm(start_transform_)));
                double factor = std::max(least, factor_raw);
                next = scaled_about(start_transform_, factor, anchor);
                if (snapping && std::fmod(std::abs(start_transform_.rotation_deg), 90) < 1e-6)
                {
                    // Snap the moving corner's edges, keeping the anchor.
                    const auto moving = corners(next)[handle_];
                    const unsigned edges = (moving.x < anchor.x ? touch_left : touch_right) | (moving.y < anchor.y ? touch_top : touch_bottom);
                    const auto s = snap_to_opening(bounds(next), open, snap_mm, edges);
                    if (s.guide_x && std::abs(moving.x - anchor.x) > 1e-6)
                    {
                        factor *= (*s.guide_x - anchor.x) / (moving.x - anchor.x);
                        guide_x_ = s.guide_x;
                    }
                    else if (s.guide_y && std::abs(moving.y - anchor.y) > 1e-6)
                    {
                        factor *= (*s.guide_y - anchor.y) / (moving.y - anchor.y);
                        guide_y_ = s.guide_y;
                    }
                    next = scaled_about(start_transform_, std::max(least, factor), anchor);
                }
            }
            else if (drag_ == drag_kind::rotate)
            {
                const double a0 = std::atan2(drag_start_.y - start_transform_.y_mm, drag_start_.x - start_transform_.x_mm);
                const double a1 = std::atan2(pt.y - start_transform_.y_mm, pt.x - start_transform_.x_mm);
                double deg = start_transform_.rotation_deg + (a1 - a0) * 180 / pi;
                if (e.modifiers & mod_shift)
                    deg = std::round(deg / 15) * 15;
                deg = std::remainder(deg, 360.);
                next.rotation_deg = std::abs(deg) < 1e-9 ? 0 : deg;
            }
            else if (drag_ == drag_kind::crop_edge)
            {
                const auto& image = p->layers[p->selected_layer].image;
                const auto s = to_source(start_transform_, pt);
                auto c = crop_;
                const double least = 4;
                if (handle_ == 0)
                {
                    const double bottom = c.y + c.h;
                    c.y = std::clamp(std::round(s.y), 0., bottom - least);
                    c.h = bottom - c.y;
                }
                else if (handle_ == 2)
                    c.h = std::clamp(std::round(s.y), c.y + least, double(image.height)) - c.y;
                else if (handle_ == 3)
                {
                    const double right = c.x + c.w;
                    c.x = std::clamp(std::round(s.x), 0., right - least);
                    c.w = right - c.x;
                }
                else
                    c.w = std::clamp(std::round(s.x), c.x + least, double(image.width)) - c.x;
                crop_ = c;
                sync_image_fields();
                canvas_->invalidate();
                return true;
            }
            // Live: the step is recorded when the drag ends.
            p->layers[p->selected_layer].transform = next;
            sync_image_fields();
            update_preview();
            return true;
        }
        case pointer_event_type::release:
        {
            const auto kind = drag_;
            drag_ = drag_kind::none;
            guide_x_.reset();
            guide_y_.reset();
            if ((kind == drag_kind::move || kind == drag_kind::scale || kind == drag_kind::rotate) && drag_moved_)
            {
                edited(kind == drag_kind::move ? "Move image" : kind == drag_kind::scale ? "Scale image" : "Rotate image");
                if (p->undo)
                    p->undo->seal();
            }
            else if (kind != drag_kind::none)
                canvas_->invalidate();
            set_hover_cursor();
            return true;
        }
        case pointer_event_type::leave:
            if (drag_ == drag_kind::none)
                canvas_->set_cursor(cursor::default_);
            return true;
        default:
            return false;
    }
}

// -- keys -----------------------------------------------------------------------

bool editor_window::key(const key_event& e)
{
    auto p = current();
    if (e.key == rap::key::space && canvas_->has_focus())
    {
        // Hold Space to pan with the left button.
        space_down_ = e.press;
        if (drag_ == drag_kind::none)
            canvas_->set_cursor(e.press ? cursor::grab : cursor::default_);
        return true;
    }
    if (!e.press)
        return false;
    if (e.modifiers & mod_ctrl)
    {
        switch (e.codepoint)
        {
            case 'n':
                choose_images(true);
                break;
            case 'o':
                choose_project();
                break;
            case 's':
                save(current());
                break;
            case 'w':
                close_tab(selected_);
                break;
            case 't':
                insert({app_.next_id(), std::make_shared<project>()});
                break;
            case 'i':
                choose_images(false);
                break;
            case 'g':
                auto_button();
                break;
            case 'e':
                export_dialog();
                break;
            case 'z':
                if (e.modifiers & mod_shift)
                    redo();
                else
                    undo();
                break;
            case 'y':
                redo();
                break;
            case '1':
                set_stage(work_step::arrange);
                break;
            case '2':
                set_stage(work_step::colours);
                break;
            case '3':
                set_stage(work_step::sheets);
                break;
            case ']':
                move_layer(1);
                break;
            case '[':
                move_layer(-1);
                break;
            case '=':
            case '+':
                zoom(1.25f);
                break;
            case '-':
                zoom(1 / 1.25f);
                break;
            case '0':
                fit();
                break;
            case ',':
                show_settings(app_.ui, window_.get(), settings_page::general);
                break;
            default:
                return false;
        }
        return true;
    }
    if (!p || (e.modifiers & mod_alt))
        return false;
    const bool arrange = p->step == work_step::arrange;
    const bool canvas = canvas_->has_focus();
    switch (e.key)
    {
        case rap::key::escape:
            if (cropping_)
                end_crop(false);
            else if (drag_ != drag_kind::none && drag_ != drag_kind::pan && p->selected_layer >= 0)
            {
                // Put a half-finished drag back.
                p->layers[p->selected_layer].transform = start_transform_;
                drag_ = drag_kind::none;
                guide_x_.reset();
                guide_y_.reset();
                update_preview();
            }
            else if (canvas && arrange && p->selected_layer >= 0)
                select_layer(-1);
            else
                return false;
            return true;
        case rap::key::enter:
            if (!cropping_)
                return false;
            end_crop(true);
            return true;
        case rap::key::delete_:
            if (!arrange || !(canvas || layers_list_->has_focus()))
                return false;
            remove_selected();
            return true;
        case rap::key::left:
        case rap::key::right:
        case rap::key::up:
        case rap::key::down:
        {
            auto t = selected_transform();
            if (!canvas || !arrange || !t || cropping_)
                return false;
            const double step = e.modifiers & mod_shift ? 10 : 1;
            t->x_mm += e.key == rap::key::left ? -step : e.key == rap::key::right ? step : 0;
            t->y_mm += e.key == rap::key::up ? -step : e.key == rap::key::down ? step : 0;
            set_transform(*t, "Nudge image", "nudge");
            return true;
        }
        case rap::key::page_up:
        case rap::key::page_down:
            if (p->step != work_step::sheets || p->cuts.empty())
                return false;
            p->selected_cut = std::clamp(p->selected_cut + (e.key == rap::key::page_down ? 1 : -1), 0, int(p->cuts.size()) - 1);
            set_view(view_mode::layer);
            return true;
        case rap::key::character:
            break;
        default:
            return false;
    }
    const auto c = e.codepoint;
    if (c == 'v')
    {
        if (!arrange)
            flash("Move works in Arrange (Ctrl+1).");
        else
            set_tool(tool::move);
        return true;
    }
    if (c == 'c')
    {
        if (!arrange)
            flash("Crop works in Arrange (Ctrl+1).");
        else
            begin_crop();
        return true;
    }
    if (p->step == work_step::sheets && !p->cuts.empty() && c >= '0' && c <= '9')
    {
        if (c == '0')
            set_view(view_mode::assembled);
        else if (int(c - '1') < int(p->cuts.size()))
        {
            p->selected_cut = int(c - '1');
            set_view(view_mode::layer);
        }
        return true;
    }
    return false;
}
}  // namespace offcut
