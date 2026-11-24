#include "app.h"
#include <algorithm>
#include "ai_client.h"
#include "history.h"
#include "window.h"
namespace offcut
{
application::application(rap::ui& toolkit)
    : ui(toolkit),
      worker_(
          [this](std::stop_token stop)
          {
              while (!stop.stop_requested())
              {
                  std::function<void()> job;
                  {
                      std::unique_lock lock(mutex_);
                      ready_.wait(lock, stop, [this] { return !queue_.empty(); });
                      if (stop.stop_requested())
                          return;
                      job = std::move(queue_.front());
                      queue_.pop_front();
                  }
                  job();
              }
          })
{
}
application::~application()
{
    alive_.reset();
    for (const auto& w : windows_)
        w->cancel_work();
    for (const auto& w : retired_)
        w->cancel_work();
    worker_.request_stop();
    ready_.notify_all();
    worker_.join();
}
std::string application::next_id()
{
    return std::to_string(next_++);
}
editor_window& application::open(std::shared_ptr<project> p, bool show, std::string id)
{
    auto w = std::make_unique<editor_window>(*this, project_tab{id.empty() ? next_id() : id, std::move(p)});
    auto& out = *w;
    windows_.push_back(std::move(w));
    if (show)
        out.native().show();
    return out;
}
void application::close(editor_window& w)
{
    const auto it = std::ranges::find_if(windows_, [&](const auto& x) { return x.get() == &w; });
    if (it == windows_.end())
        return;
    w.native().close();
    retired_.push_back(std::move(*it));
    windows_.erase(it);
    if (windows_.empty())
    {
        ui.quit();
        return;
    }
    ui.post(
        [this, weak = std::weak_ptr<void>(alive_)]
        {
            if (!weak.expired())
                retired_.clear();
        });
}
editor_window* application::find(rap::window* native)
{
    for (auto& w : windows_)
        if (&w->native() == native)
            return w.get();
    return nullptr;
}
rap::window* application::detach(editor_window& from, const std::string& id)
{
    if (from.count() <= 1)
        return nullptr;
    auto tab = from.take(id);
    if (!tab)
        return nullptr;
    auto& w = open(tab->document, false, tab->id);
    if (auto pos = from.native().position())
        w.native().move({pos->x + 32, pos->y + 32});
    return &w.native();
}
bool application::receive(editor_window& to, const rap::document_tabs::transfer& t)
{
    auto* from = find(t.source);
    if (!from || from == &to)
        return false;
    auto tab = from->take(t.id);
    if (!tab)
        return false;
    to.insert(std::move(*tab), t.index);
    if (!from->count())
        close(*from);
    return true;
}
void application::changed()
{
    for (auto& w : windows_)
        w->refresh();
}
void application::task(std::shared_ptr<project> p, std::string activity, std::function<void(project&)> work, std::function<void(bool)> finished,
                       std::string undo_label)
{
    if (p->busy)
        return;
    if (undo_label.empty())
    {
        undo_label = activity;
        if (undo_label.ends_with("…"))
            undo_label.resize(undo_label.size() - std::string_view("…").size());
    }
    p->cancel = std::make_shared<std::atomic_bool>(false);
    auto result = std::make_shared<project>(*p);
    p->busy = true;
    p->activity = std::move(activity);
    changed();
    auto weak = std::weak_ptr<void>(alive_);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(
            [this, weak, p, result, work = std::move(work), finished = std::move(finished), undo_label = std::move(undo_label)]
            {
                std::string error;
                bool cancelled = false;
                try
                {
                    work(*result);
                }
                catch (const ai_cancelled&)
                {
                    cancelled = true;
                    error = "AI request cancelled.";
                }
                catch (const std::exception& e)
                {
                    error = e.what();
                }
                ui.post(
                    [this, weak, p, result, error, cancelled, finished, undo_label]
                    {
                        if (weak.expired())
                            return;
                        if (error.empty())
                        {
                            result->zoom = p->zoom;
                            result->pan_x = p->pan_x;
                            result->pan_y = p->pan_y;
                            *p = std::move(*result);
                            if (!p->undo)
                            {
                                p->undo = std::make_shared<history>();
                                p->undo->reset(*p);
                            }
                            else
                            {
                                p->undo->commit(*p, undo_label);
                                p->undo->seal();
                            }
                            p->dirty = !p->undo->at_saved();
                        }
                        else
                            p->activity = cancelled ? error : "Failed: " + error;
                        p->busy = false;
                        p->ai_running = false;
                        p->cancel.reset();
                        changed();
                        if (!error.empty() && !cancelled)
                            ui.message(nullptr, {.icon = rap::message_spec::icon::error, .title = "Offcut", .text = error});
                        if (finished)
                            finished(error.empty());
                    });
            });
    }
    ready_.notify_one();
}
}  // namespace offcut
