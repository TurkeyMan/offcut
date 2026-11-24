#pragma once
#include <filesystem>
#include <functional>
#include "ai_client.h"
namespace offcut
{
enum class ai_auth
{
    api_key,       // a secret key is required
    optional_key,  // sent only when the server wants one
    none,          // never sent
    oauth_session  // Sign in with ChatGPT; renewable session, never an API key
};
// One kind of provider Offcut can talk to. Fixed; the user's configured
// entries (ai_account) refer to one of these by id.
struct ai_provider
{
    std::string id, name, base_url;  // base_url: the default; local/custom type their own
    ai_auth auth;
    std::string key_page, key_hint;  // key_page empty: no "Make a key" link
    ai_model_list model_list;
    std::string models_url;  // absolute, or a path after the base URL
    std::string preferred_model;
    bool typed_url = false;  // the server URL is the user's
    // Next choices when the preferred model isn't listed or needs a paid
    // balance, in order.
    std::vector<std::string> fallback_models = {};
    bool can_plan = true, can_draw = true;
};
const std::vector<ai_provider>& ai_providers();
const ai_provider& find_ai_provider(const std::string& id);

// A configured provider: a row of Settings → AI providers. Only nonsecret
// fields; the key lives in Windows Credential Manager under credential_target
// (empty: keyless).
// Two roles (docs/ux-design.md section 8): `model` draws (the image model),
// `reasoning_model` plans and reviews (the director, phase 2). Either may be
// empty.
struct ai_account
{
    std::string id, provider, name, base_url, model, credential_target;
    std::vector<std::string> models;  // the image-capable models last listed
    // Of those, the ones that need a paid balance (Pollinations' paid_only),
    // from the last listing in this session: not saved.
    std::vector<std::string> paid_models = {};
    std::string reasoning_model = {};
};
struct ai_accounts
{
    std::string default_id;  // "Draws with": the image model's entry
    std::string plans_id;    // "Plans and reviews with": empty until a reasoning model is set
    std::vector<ai_account> entries;
    ai_account* find(const std::string& id);
    const ai_account* find(const std::string& id) const;
    const ai_account* default_account() const;  // the default, else the first; null if none
    const ai_account* plans_account() const;    // the planning entry, if it has a reasoning model; else null
};
// Whether Windows Credential Manager holds a key at a target. Injected so
// migration is testable without touching real credentials.
using ai_key_probe = std::function<bool(const std::string& target)>;
// ai-accounts.txt, or on first use what older Offcut settings described:
// ai-providers.txt (per-provider profiles) or ai-endpoint.txt, plus a key
// saved as Offcut/OpenAI. Migrated entries keep their old credential target,
// so no key is read or copied. Throws if ai-accounts.txt is unreadable.
ai_accounts read_ai_accounts(const std::filesystem::path& directory, const ai_key_probe& key_saved);
void write_ai_accounts(const ai_accounts&, const std::filesystem::path& directory);
std::string new_ai_account_id(const ai_accounts&, const std::string& provider);
std::string new_ai_account_name(const ai_accounts&, const ai_provider&, std::string typed);
std::string ai_credential_target(const std::string& account_id);  // "Offcut/AI/<id>"

std::string trim_ai_key(std::string);
std::string validate_provider_key(const ai_provider&, std::string);
std::string provider_models_url(const ai_provider&, const std::string& base_url);
// GET models with the key: never uploads anything. Compatible servers that
// don't list models (404) give an empty list, for a typed model name.
// `paid`, when given, receives the listed models that need a paid balance.
std::vector<std::string> list_ai_models(const ai_provider&, const std::string& base_url, const std::string& key, const std::atomic_bool* cancel = nullptr,
                                        std::vector<std::string>* paid = nullptr);
// The model to use from a list: `keep` if still listed, else the provider's
// preferred one, then its fallbacks, else the first (for OpenAI, the newest
// GPT Image model). Models in `paid` are chosen only when nothing else is.
std::string default_ai_model(const ai_provider&, const std::vector<std::string>& models, const std::string& keep = {},
                             const std::vector<std::string>& paid = {});
// How a model shows in a list: "flux.2-pro (paid)".
std::string ai_model_label(const ai_account&, const std::string& model);
// A failed check in a few words (Settings' Status column), and with the
// provider's own explanation (the Add result).
std::string ai_failure_summary(const std::exception&);
std::string ai_failure_detail(const std::exception&);
// The account's key from Credential Manager; throws if one is needed and missing.
std::string load_account_key(const ai_account&);
std::string load_account_access(const ai_account&, const std::atomic_bool* cancel);
// The image-edit request for an account and a frame: size matches the frame's
// orientation; OpenAI gets quality auto and needs a GPT Image model.
ai_endpoint account_image_endpoint(const ai_account&, double frame_width, double frame_height);
// Whether the endpoint's model takes input_fidelity (OpenAI's GPT Image
// models other than the mini ones); refinements ask for "high" there.
bool supports_input_fidelity(const ai_endpoint&);
}  // namespace offcut
