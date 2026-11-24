// Settings' AI providers page: the image-edit services Offcut is set up to
// use for Generate with AI. As on Tortilla's Accounts page, adding and
// removing take effect at once: keys go to Windows Credential Manager, the
// rest to ai-accounts.txt, so there's nothing for OK to save.
#pragma once
#include <rapport/rapport.h>
#include <functional>
namespace offcut
{
// `parent` gives the window for its messages.
rap::node ai_providers_page(rap::ui&, std::function<rap::window*()> parent);
}  // namespace offcut
