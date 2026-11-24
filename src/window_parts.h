#pragma once
#include <rapport/rapport.h>
#include <memory>
#include <string>
#include <vector>
#include "guidance.h"
#include "window.h"
// Pieces of the editor window that don't need its state: icons, the right
// panel's list models and the guidance bar's wording.
namespace offcut
{
rap::icon new_icon();
rap::icon picture_icon();
rap::icon layers_icon();
rap::icon layer_icon();
rap::icon generate_icon();
rap::icon export_icon();
rap::icon ai_icon();
rap::icon orientation_icon(bool landscape);
rap::icon move_icon();
rap::icon crop_icon();
rap::icon undo_icon(bool redo);

std::unique_ptr<editor_window::layer_model> make_layer_model(editor_window&);
std::unique_ptr<editor_window::sheet_model> make_sheet_model(editor_window&);
rap::list_model* layer_model_ptr(editor_window::layer_model&);
rap::list_model* sheet_model_ptr(editor_window::sheet_model&);
std::vector<rap::column> layer_columns();
std::vector<rap::column> sheet_columns();

}  // namespace offcut
