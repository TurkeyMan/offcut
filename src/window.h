#pragma once
#include <chrono>
#include <map>
#include <optional>
#include "ai_workflow.h"
#include "app.h"
#include "auto_loop.h"
#include "prompts.h"
#include "transform.h"
namespace offcut
{
class history;
// The editor shell (docs/ux-design.md sections 3–5): main toolbar with the
// stage switch, a tool row, the canvas, the right panel (list over
// Properties) and the guidance bar.
class editor_window
{
public:
    editor_window(application&, project_tab);
    ~editor_window();
    rap::window& native() { return *window_; }
    size_t count() const { return tabs_.size(); }
    bool busy() const
    {
        const auto p = current();
        return p && p->busy;
    }
    std::optional<project_tab> take(const std::string&);
    void insert(project_tab, int index = -1);
    void refresh();
    void load_paths(std::vector<std::string> paths, bool new_project, bool masks = false);
    void cancel_work();  // application shutdown

    // For the smoke test and snapshots: what the user would do with the
    // pointer and keys, through the same handlers.
    enum class tool
    {
        move,
        crop,
    };
    void set_stage(work_step);
    void set_tool(tool);
    void select_layer(int index);  // -1: nothing
    void select_sheet(int index);
    rap::point_f screen_from_frame(point_mm) const;
    bool pointer(const rap::pointer_event&);
    bool key(const rap::key_event&);
    void undo();
    void redo();
    std::string guidance() const;  // the guidance bar's sentence

    // The AI loop (window_ai.cpp; docs/ux-design.md section 8).
    void reimagine();  // AI menu: Send to AI with the composition
    // Send to AI for a follow-up on a version of an item (Refine…, Fix with AI…).
    void refine_dialog(int layer, uint32_t version, ai_task);
    // The review bar's answers for the selected item's current version.
    void review_use();
    void review_try_again();
    void review_discard();
    void review_send();  // sends the pending chips
    void toggle_chip(const std::string& id, const std::string& element = {});
    void write_review();
    void show_record(int layer, uint32_t version);  // Show plan: the plan dialog when the version has one, else its record
    void show_record_details(int layer, uint32_t version);  // the prompt and record
    // Makes a version current; the pipeline follows (Versions strip click).
    void show_version(int layer, uint32_t version);
    std::string review_line() const;  // the review bar as text, for tests
    bool reviewing() const;           // the review bar is showing
    // For the smoke test and snapshots: the open Send dialog, pressing its
    // Send, and providers and keys that never touch Settings or Credential
    // Manager (a loopback mock).
    rap::window* send_dialog_window() const;
    bool press_send(const std::string& instructions = {});
    void set_ai_test_hooks(ai_accounts, std::function<std::string(const ai_account&)> key, std::filesystem::path folder);

    // Auto with AI (window_ai.cpp; docs/ux-design.md section 8): the split
    // button, the consent dialog, the run with its progress, Show plan.
    void auto_button();  // Auto: the loop when a director and an illustrator are set, else today's Auto
    void auto_with_ai(auto_until, bool arrange = false, std::string plan = {});  // consent, then the run
    void auto_options();
    void auto_continue();  // Ask after each round: one more round
    void auto_stop();      // … or present the best round
    bool auto_asking() const;
    bool auto_running() const;
    double auto_elapsed() const;        // seconds since the run started
    std::string auto_plan_now() const;  // the running run's plan so far
    // Show plan: a modeless dialog presenting the plan readably, editable.
    void show_plan(const std::string& plan_json, int layer = -1, uint32_t version = 0);
    rap::window* consent_window() const;
    bool press_start(const std::string& instructions = {}, bool remember = false);
    rap::window* plan_window() const;

    class layer_model;
    class sheet_model;

private:
    std::shared_ptr<project> current() const;
    void build();
    rap::menu tab_menu(const std::string&);
    rap::menu ai_menu();
    rap::menu layer_menu(int index);
    rap::menu chip_menu(const std::string& menu);
    rap::menu version_menu();
    void set_view(view_mode);
    void fit();
    bool reusable() const;
    void choose_images(bool new_project, bool masks = false);
    void choose_illustration();
    // AI loop internals (window_ai.cpp).
    struct send_state;
    struct ai_job;
    struct consent_state;
    struct auto_state;
    struct plan_state;
    rap::menu auto_menu();
    std::shared_ptr<auto_state> auto_of(const project*) const;
    void open_consent(std::shared_ptr<project>, std::shared_ptr<auto_state>, std::shared_ptr<prepared_ai_upload>);
    void start_auto(std::shared_ptr<project>, std::shared_ptr<auto_state>, bool fresh);
    auto_roles auto_roles_for(std::shared_ptr<project>, std::shared_ptr<auto_state>);
    bool review_through_director(const std::string& text, const std::string& chips, const std::string& review);
    ai_accounts ai_accounts_now() const;
    std::string ai_key(const ai_account&) const;
    std::optional<std::filesystem::path> ai_folder(const project&) const;
    void with_ai_folder(std::shared_ptr<project>, std::function<void(std::filesystem::path)>);
    bool offer_settings();  // no provider: says so, with Open Settings; true when there is none
    void open_send_dialog(std::shared_ptr<project>, std::shared_ptr<prepared_ai_upload>, std::filesystem::path parent);
    void start_ai(std::shared_ptr<project>, std::shared_ptr<prepared_ai_upload>, const ai_account&, std::filesystem::path parent, bool follow_up,
                  std::function<void(project&)> before);
    void follow_up(ai_task, const std::string& instructions, const std::string& chips, const std::string& review);
    void sync_review();
    void sync_versions();
    void paint_versions(rap::painter&, rap::size_f);
    bool versions_pointer(const rap::pointer_event&);
    const rap::image* version_thumbnail(const layer&, uint32_t version);
    const layer* selected_item() const;
    void cancel_ai();
    void choose_project();
    void save(std::shared_ptr<project>, std::function<void()> done = {});
    void confirm_close(std::vector<std::shared_ptr<project>>, std::function<void()>);
    void close_tab(const std::string&);
    void close_window();
    void edit_options(bool custom_size = false);
    void choose_size(int preset);
    void orient(bool landscape);
    void border_mode(bool same);
    void import_paths(std::vector<std::string>, bool masks, std::optional<transform_options> setup = {});
    void generate();
    void flatten_sources();
    void export_dialog();
    void export_files(bool ai);
    void update_preview();
    void paint(rap::painter&, rap::size_f);
    void paint_stages(rap::painter&, rap::size_f);
    bool stage_pointer(const rap::pointer_event&);
    void zoom(float factor);
    void zoom_at(float factor, rap::point_f at);
    void move_layer(int delta);
    void remove_selected();
    // Records an edit for undo and updates everything that shows it.
    void edited(const std::string& label, const std::string& merge = {}, bool design = true);
    // Transform edits of the selected image.
    void set_transform(const image_transform&, const std::string& label, const std::string& merge = {});
    void apply_to_selected(const std::function<image_transform(const layer&, const transform_options&)>&, const std::string& label);
    void edit_image_fields(int field);
    void sync_image_fields();
    void begin_crop();
    void end_crop(bool commit);
    void sync_lists();
    void sync_tool_row();
    void sync_guidance();
    void flash(std::string message);
    // Canvas geometry: the frame's rectangle on screen.
    rap::rect_f frame_rect() const;
    point_mm frame_from_screen(rap::point_f) const;
    std::optional<image_transform> selected_transform() const;
    const rap::image* thumbnail(const layer&);
    const rap::image* sheet_thumbnail(size_t sheet);
    application& app_;
    std::vector<project_tab> tabs_;
    std::string selected_;
    std::unique_ptr<rap::image> preview_;
    std::shared_ptr<void> alive_ = std::make_shared<int>(0);
    rap::document_tabs* tab_bar_ = nullptr;
    rap::canvas *canvas_ = nullptr, *stages_ = nullptr;
    rap::list *layers_list_ = nullptr, *sheets_list_ = nullptr;
    std::unique_ptr<layer_model> layer_rows_;
    std::unique_ptr<sheet_model> sheet_rows_;
    rap::stack *panel_ = nullptr, *properties_ = nullptr, *tool_options_ = nullptr;
    // The bottom bar's two pages: "guidance" and "review" (the review bar,
    // for the AI loop, is a placeholder until that work lands).
    rap::stack* bottom_bar_ = nullptr;
    rap::label *review_status_ = nullptr, *review_text_ = nullptr;
    rap::button *review_send_ = nullptr, *review_cancel_ = nullptr, *show_plan_ = nullptr;
    rap::widget* review_answers_ = nullptr;  // Use this … Discard, hidden while a follow-up runs
    rap::widget* review_ask_ = nullptr;      // Continue · Stop · Show plan: Ask after each round
    std::string review_line_;
    // Pending chips, for one version of one item of one project.
    std::vector<chip_choice> chips_;
    const project* chips_project_ = nullptr;
    uint32_t chips_layer_ = 0, chips_version_ = 0;
    bool following_up_ = false;  // the running job is a review-bar follow-up
    std::shared_ptr<send_state> send_;  // the open Send dialog
    std::shared_ptr<consent_state> consent_;  // the open Auto consent dialog
    std::shared_ptr<plan_state> plan_;        // the open Show plan dialog
    std::map<const project*, std::shared_ptr<auto_state>> autos_;  // each project's last Auto run
    rap::subscription auto_timer_;  // the guidance bar's elapsed time while a run goes
    std::unique_ptr<rap::window> review_window_, record_window_, element_window_;
    // Versions strip: a painted canvas in a horizontal scroll.
    rap::widget *versions_box_ = nullptr, *placement_box_ = nullptr;
    rap::canvas* versions_canvas_ = nullptr;
    std::vector<std::pair<rap::rect_f, uint32_t>> version_hits_;
    uint32_t strip_focus_ = 0;  // the version the strip's ⋯ menu acts on
    std::map<std::string, std::unique_ptr<rap::image>> version_thumbs_;
    std::optional<ai_accounts> test_accounts_;
    std::function<std::string(const ai_account&)> test_key_;
    std::filesystem::path test_folder_;
    rap::spin *width_ = nullptr, *height_ = nullptr, *border_ = nullptr, *bridge_ = nullptr, *layers_ = nullptr, *smooth_ = nullptr;
    rap::spin* gap_ = nullptr;
    rap::spin *top_ = nullptr, *bottom_ = nullptr, *left_ = nullptr, *right_ = nullptr;
    rap::spin* image_fields_[10] = {};  // X Y W H rotation · crop left top width height
    rap::label *image_info_ = nullptr, *properties_title_ = nullptr, *sheet_info_ = nullptr, *panel_title_ = nullptr;
    rap::entry* image_name_ = nullptr;
    rap::drop_down *size_ = nullptr, *placement_ = nullptr;
    rap::tool_button *portrait_ = nullptr, *landscape_ = nullptr;
    rap::tool_button *move_tool_ = nullptr, *crop_tool_ = nullptr, *undo_ = nullptr, *redo_ = nullptr;
    rap::tool_button* views_[2] = {};  // Assembled, Single sheet
    rap::check *same_border_ = nullptr, *edges_[4] = {}, *snap_ = nullptr;
    rap::widget *uniform_width_ = nullptr, *side_widths_ = nullptr, *touch_controls_ = nullptr, *view_switch_ = nullptr;
    rap::widget *move_actions_ = nullptr, *crop_actions_ = nullptr, *sheets_actions_ = nullptr;
    rap::check* connect_ = nullptr;
    rap::entry* name_ = nullptr;
    rap::text_view *style_ = nullptr, *notes_ = nullptr;
    rap::label *guidance_ = nullptr, *summary_ = nullptr, *zoom_label_ = nullptr, *guidance_dot_ = nullptr;
    rap::button* guidance_action_ = nullptr;
    std::function<void()> guidance_do_;
    rap::widget *panel_widget_ = nullptr, *save_ = nullptr, *add_ = nullptr, *auto_ = nullptr, *export_ = nullptr, *ai_ = nullptr;
    rap::widget *flatten_ = nullptr, *cancel_ai_ = nullptr, *auto_more_ = nullptr;
    std::filesystem::path last_export_parent_;
    // Canvas interaction.
    enum class drag_kind
    {
        none,
        pan,
        move,
        scale,
        rotate,
        crop_edge,
    };
    tool tool_ = tool::move;
    drag_kind drag_ = drag_kind::none;
    int handle_ = -1;  // corner (scale) or edge (crop) being dragged
    rap::point_f drag_start_screen_{}, pan_start_{};
    point_mm drag_start_{};
    image_transform start_transform_{};
    bool space_down_ = false, drag_moved_ = false;
    std::optional<double> guide_x_, guide_y_;
    // Crop mode: the crop being edited, for the selected layer.
    bool cropping_ = false;
    pixel_rect crop_{};
    // The stage switch's segments, as last painted.
    std::vector<rap::rect_f> stage_rects_;
    // Thumbnails by layer id and by sheet, with what they were made from.
    std::map<std::string, std::unique_ptr<rap::image>> thumbs_;
    std::vector<std::unique_ptr<rap::image>> sheet_thumbs_;
    uint64_t sheet_thumbs_key_ = 0;
    std::vector<size_t> floating_;  // floating pieces per sheet, for sheet_thumbs_key_
    std::string flash_, guidance_text_;
    std::chrono::steady_clock::time_point flash_until_{};
    rap::subscription flash_timer_;
    bool syncing_ = false;  // filling fields: their change handlers do nothing
    std::unique_ptr<rap::window> window_;
};
}  // namespace offcut
