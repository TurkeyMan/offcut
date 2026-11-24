#pragma once
#include <rapport/rapport.h>
#include <filesystem>
#include "ai_provider.h"
namespace offcut
{
// Settings' pages, in the order the sidebar lists them.
enum class settings_page
{
    general,
    ai_providers
};
// Opens Settings at `first`; if it is already open, shows that page there.
// Returns the Settings window (owned by Settings itself).
rap::window* show_settings(rap::ui&, rap::window* parent, settings_page first = settings_page::general);
// The colour scheme saved by Settings, applied as Offcut starts.
void apply_saved_appearance(rap::ui&);
// Offcut's settings folder; OFFCUT_CONFIG_DIR overrides it (tests, snapshots).
std::filesystem::path settings_directory();
// The saved list, plus entries recovered for Offcut/AI/ keys it lost
// (skipped with OFFCUT_NO_KEY_RECOVERY=1, for isolated snapshots).
ai_accounts configured_ai_accounts();
void save_ai_accounts(const ai_accounts&);
}  // namespace offcut
