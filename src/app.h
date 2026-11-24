#pragma once
#include <rapport/rapport.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "project.h"
namespace offcut
{
class editor_window;
struct project_tab
{
    std::string id;
    std::shared_ptr<project> document;
};
class application
{
public:
    explicit application(rap::ui&);
    ~application();
    rap::ui& ui;
    editor_window& open(std::shared_ptr<project> = std::make_shared<project>(), bool show = true, std::string id = {});
    void close(editor_window&);
    editor_window* find(rap::window*);
    rap::window* detach(editor_window&, const std::string&);
    bool receive(editor_window&, const rap::document_tabs::transfer&);
    void changed();
    // Runs `work` on a copy of the project and publishes it on the UI thread.
    // The result is an undo step named `undo_label` (by default the activity);
    // a result without a history (an opened project) starts a new one.
    void task(std::shared_ptr<project>, std::string activity, std::function<void(project&)>, std::function<void(bool)> finished = {},
              std::string undo_label = {});
    std::string next_id();

private:
    std::vector<std::unique_ptr<editor_window>> windows_, retired_;
    std::shared_ptr<void> alive_ = std::make_shared<int>(0);
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::function<void()>> queue_;
    std::jthread worker_;
    uint64_t next_ = 1;
};
}  // namespace offcut
