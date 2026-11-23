#pragma once
#include <optional>
#include "app.h"
namespace offcut
{
class editor_window
{
public:
    editor_window(application&, project_tab);
    rap::window& native() { return *window_; }
    size_t count() const { return tabs_.size(); }
    std::optional<project_tab> take(const std::string&);
    void insert(project_tab, int index = -1);
    void refresh();
    void load_paths(std::vector<std::string> paths, bool new_project, bool masks = false);

private:
    std::shared_ptr<project> current() const;
    void build();
    void choose_images(bool new_project, bool masks = false);
    void choose_project();
    void save(std::shared_ptr<project>, std::function<void()> done = {});
    void confirm_close(std::vector<std::shared_ptr<project>>, std::function<void()>);
    void close_tab(const std::string&);
    void close_window();
    void edit_options();
    void generate();
    void flatten_sources();
    void export_files(bool ai);
    void update_preview();
    void paint(rap::painter&, rap::size_f);
    void zoom(float factor);
    void move_source(int);
    void remove_selected();
    bool key(const rap::key_event&);
    application& app_;
    std::vector<project_tab> tabs_;
    std::string selected_;
    std::unique_ptr<rap::image> preview_;
    std::shared_ptr<void> alive_ = std::make_shared<int>(0);
    rap::document_tabs* tab_bar_ = nullptr;
    rap::canvas* canvas_ = nullptr;
    rap::tree* sources_ = nullptr;
    rap::tree* cuts_ = nullptr;
    rap::spin *width_ = nullptr, *height_ = nullptr, *border_ = nullptr, *bridge_ = nullptr, *layers_ = nullptr, *smooth_ = nullptr;
    rap::check* connect_ = nullptr;
    rap::entry* name_ = nullptr;
    rap::text_view* instructions_ = nullptr;
    rap::label *status_ = nullptr, *summary_ = nullptr, *zoom_label_ = nullptr;
    rap::drop_down* mode_ = nullptr;
    rap::widget *controls_ = nullptr, *actions_ = nullptr;
    bool dragging_ = false;
    rap::point_f drag_{};
    std::unique_ptr<rap::window> window_;
};
}  // namespace offcut
