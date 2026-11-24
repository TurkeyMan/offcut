#include "ai_provider.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include "chatgpt_auth.h"
namespace offcut
{
namespace
{
constexpr const char* missing_key = "No key in Windows Credential Manager";
const char* accounts_magic = "OFFCUT_AI_ACCOUNTS_2";  // _1: no reasoning models, one default
bool plain_text(const std::string& s)
{
    return std::ranges::none_of(s, [](char c) { return static_cast<unsigned char>(c) < 32; });
}
void validate_account(const ai_account& a)
{
    const auto& p = find_ai_provider(a.provider);
    split_ai_url(a.base_url);
    if (a.id.empty() || !plain_text(a.id) || a.id.find_first_of("/\\\"") != std::string::npos || !plain_text(a.name) || !plain_text(a.model) ||
        !plain_text(a.credential_target) || !plain_text(a.reasoning_model) || (!a.credential_target.empty() && !a.credential_target.starts_with("Offcut/")) ||
        (p.auth == ai_auth::none && !a.credential_target.empty()))
        throw std::runtime_error("AI provider settings contain an invalid entry.");
}
// The single-provider settings before the list: Codex's per-provider
// profiles (OFFCUT_AI_PROVIDERS_1) or the original ai-endpoint.txt. Each
// profile that was connected, or whose key is still saved, becomes an entry
// under its old id and credential target. Unreadable old files are ignored.
ai_accounts migrate(const std::filesystem::path& directory, const ai_key_probe& key_saved)
{
    ai_accounts out;
    const auto legacy_target = [](const std::string& id) -> std::string {
        return id == "openai" ? "Offcut/OpenAI" : id == "pollinations" ? "Offcut/Pollinations" : id == "custom" ? "Offcut/Custom" : "";
    };
    const auto add = [&](const std::string& id, std::string base_url, std::string model, std::vector<std::string> models)
    {
        const auto& p = find_ai_provider(id);
        ai_account a{id, id, p.name, std::move(base_url), std::move(model), legacy_target(id), std::move(models)};
        if (!a.credential_target.empty() && p.auth == ai_auth::optional_key && !key_saved(a.credential_target))
            a.credential_target.clear();
        try
        {
            validate_account(a);
            out.entries.push_back(std::move(a));
        }
        catch (const std::exception&)
        {
        }
    };
    if (std::ifstream in(directory / "ai-providers.txt"); in)
    {
        std::string magic, selected;
        if (in >> magic >> std::quoted(selected) && magic == "OFFCUT_AI_PROVIDERS_1")
        {
            for (;;)
            {
                std::string id, url, model, size, quality;
                bool verified = false;
                size_t count = 0;
                if (!(in >> std::quoted(id) >> std::quoted(url) >> std::quoted(model) >> std::quoted(size) >> std::quoted(quality) >> verified >> count) ||
                    count > 4096)
                    break;
                std::vector<std::string> models(count);
                bool complete = true;
                for (auto& m : models)
                    complete = complete && bool(in >> std::quoted(m));
                if (!complete)
                    break;
                if (std::ranges::none_of(ai_providers(), [&](const auto& p) { return p.id == id; }))
                    continue;
                const auto target = legacy_target(id);
                if (verified || (!target.empty() && key_saved(target)))
                    add(id, url, model, std::move(models));
            }
            if (out.find(selected))
                out.default_id = selected;
        }
    }
    else if (std::ifstream legacy(directory / "ai-endpoint.txt"); legacy)
    {
        std::string url, model, size, quality;
        if (legacy >> std::quoted(url) >> std::quoted(model))
        {
            // A non-OpenAI URL was a compatible server sharing the OpenAI key.
            std::string id = "openai";
            try
            {
                const auto host = split_ai_url(url).host;
                if (host != "api.openai.com")
                    id = host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1" ? "local" : "custom";
            }
            catch (const std::exception&)
            {
            }
            if (id == "local")
                add("local", url, model, {});
            else
            {
                add(id, url, model, {});
                if (!out.entries.empty())
                    out.entries.back().credential_target = key_saved("Offcut/OpenAI") || id == "openai" ? "Offcut/OpenAI" : "";
            }
        }
    }
    else if (key_saved("Offcut/OpenAI"))
        add("openai", find_ai_provider("openai").base_url, find_ai_provider("openai").preferred_model, {});
    if (out.default_id.empty() && !out.entries.empty())
        out.default_id = out.entries.front().id;
    return out;
}
}  // namespace
const std::vector<ai_provider>& ai_providers()
{
    static const std::vector<ai_provider> registry{
        {"chatgpt",
         "OpenAI (Sign in with ChatGPT)",
         "https://api.openai.com/v1",
         ai_auth::oauth_session,
         "https://chatgpt.com/",
         "Uses your ChatGPT plan to plan and review; cannot draw.",
         ai_model_list::chatgpt_reasoning,
         "/models",
         "",
         false,
         {},
         true,
         false},
        {"openai", "OpenAI", "https://api.openai.com/v1", ai_auth::api_key, "https://platform.openai.com/api-keys",
         "A secret key (sk-…). Image API use is billed separately from ChatGPT;\nsome GPT Image models need a verified organization.",
         ai_model_list::openai_images, "/models", "", false},
        {"pollinations",
         "Pollinations",
         "https://gen.pollinations.ai/v1",
         ai_auth::api_key,
         "https://enter.pollinations.ai/keys",
         "A secret key (sk_…) from your Pollinations account;\npublishable pk_ keys don't work.",
         ai_model_list::pollinations_images,
         "https://gen.pollinations.ai/image/models",
         // Image-input models a key on the free (Quest) balance can use.
         "microsoft/mai-image-2.7",
         false,
         {"flux.2-flex", "flux.1-kontext-pro"}},
        {"local", "Local OpenAI-compatible server", "http://127.0.0.1:8188/v1", ai_auth::none, "",
         "A server on this computer, such as ComfyUI or LocalAI,\nwith an OpenAI-style API. No key is sent.", ai_model_list::compatible, "/models", "", true},
        {"custom", "Custom OpenAI-compatible", "https://", ai_auth::optional_key, "",
         "Any server with an OpenAI-style /images/edits endpoint.\nA key only if the server asks for one.", ai_model_list::compatible, "/models", "", true},
    };
    return registry;
}
const ai_provider& find_ai_provider(const std::string& id)
{
    for (const auto& p : ai_providers())
        if (p.id == id)
            return p;
    throw std::runtime_error("Unknown AI provider.");
}
ai_account* ai_accounts::find(const std::string& id)
{
    for (auto& a : entries)
        if (a.id == id)
            return &a;
    return nullptr;
}
const ai_account* ai_accounts::find(const std::string& id) const
{
    return const_cast<ai_accounts*>(this)->find(id);
}
const ai_account* ai_accounts::default_account() const
{
    if (const auto* a = find(default_id))
        return a;
    return entries.empty() ? nullptr : &entries.front();
}
const ai_account* ai_accounts::plans_account() const
{
    const auto* a = find(plans_id);
    return a && !a->reasoning_model.empty() ? a : nullptr;
}
ai_accounts read_ai_accounts(const std::filesystem::path& directory, const ai_key_probe& key_saved)
{
    std::ifstream in(directory / "ai-accounts.txt");
    if (!in)
        return migrate(directory, key_saved);
    ai_accounts c;
    std::string magic;
    if (!(in >> magic >> std::quoted(c.default_id)) || (magic != accounts_magic && magic != "OFFCUT_AI_ACCOUNTS_1"))
        throw std::runtime_error("AI provider settings could not be read.");
    const bool roles = magic == accounts_magic;
    if (roles && !(in >> std::quoted(c.plans_id)))
        throw std::runtime_error("AI provider settings are truncated.");
    for (;;)
    {
        in >> std::ws;
        if (in.peek() == std::char_traits<char>::eof())
            break;
        ai_account a;
        size_t count = 0;
        if (!(in >> std::quoted(a.id) >> std::quoted(a.provider) >> std::quoted(a.name) >> std::quoted(a.base_url) >> std::quoted(a.model)) ||
            (roles && !(in >> std::quoted(a.reasoning_model))) || !(in >> std::quoted(a.credential_target) >> count) || count > 4096)
            throw std::runtime_error("AI provider settings are truncated.");
        a.models.resize(count);
        for (auto& m : a.models)
            if (!(in >> std::quoted(m)))
                throw std::runtime_error("AI provider settings are truncated.");
        validate_account(a);
        if (c.find(a.id))
            throw std::runtime_error("AI provider settings repeat an entry.");
        c.entries.push_back(std::move(a));
    }
    if (!c.find(c.default_id))
        c.default_id = c.entries.empty() ? std::string() : c.entries.front().id;
    if (!c.find(c.plans_id))
        c.plans_id.clear();
    return c;
}
void write_ai_accounts(const ai_accounts& c, const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    const auto pending = directory / "ai-accounts.pending";
    {
        std::ofstream out(pending);
        out << accounts_magic << ' ' << std::quoted(c.default_id) << ' ' << std::quoted(c.plans_id) << '\n';
        for (const auto& a : c.entries)
        {
            validate_account(a);
            out << std::quoted(a.id) << ' ' << std::quoted(a.provider) << ' ' << std::quoted(a.name) << ' ' << std::quoted(a.base_url) << ' '
                << std::quoted(a.model) << ' ' << std::quoted(a.reasoning_model) << ' ' << std::quoted(a.credential_target) << ' ' << a.models.size() << '\n';
            for (const auto& m : a.models)
                out << "  " << std::quoted(m) << '\n';
        }
        out.close();
        if (!out)
            throw std::runtime_error("AI provider settings could not be saved.");
    }
    // Replaced only after a complete write; no key ever enters this file.
    std::filesystem::copy_file(pending, directory / "ai-accounts.txt", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(pending);
}
std::string new_ai_account_id(const ai_accounts& c, const std::string& provider)
{
    find_ai_provider(provider);
    if (!c.find(provider))
        return provider;
    for (int n = 2;; ++n)
        if (auto id = provider + "-" + std::to_string(n); !c.find(id))
            return id;
}
std::string new_ai_account_name(const ai_accounts& c, const ai_provider& p, std::string typed)
{
    typed = trim_ai_key(std::move(typed));
    std::erase_if(typed, [](char ch) { return static_cast<unsigned char>(ch) < 32; });
    const auto taken = [&](const std::string& n) { return std::ranges::any_of(c.entries, [&](const auto& a) { return a.name == n; }); };
    if (!typed.empty())
        return typed;
    if (!taken(p.name))
        return p.name;
    for (int n = 2;; ++n)
        if (auto name = p.name + " " + std::to_string(n); !taken(name))
            return name;
}
std::string ai_credential_target(const std::string& id)
{
    return "Offcut/AI/" + id;
}
std::string trim_ai_key(std::string key)
{
    const auto start = key.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    return key.substr(start, key.find_last_not_of(" \t\r\n") - start + 1);
}
std::string validate_provider_key(const ai_provider& p, std::string key)
{
    if (p.auth == ai_auth::oauth_session)
        throw std::runtime_error("Use Sign in with ChatGPT for this provider, not an API key.");
    key = trim_ai_key(std::move(key));
    if (p.auth == ai_auth::none)
        return {};
    if (key.empty() && p.auth == ai_auth::api_key)
        throw std::runtime_error("Paste a secret API key from " + p.name + ".");
    if (key.find_first_of(" \t\r\n") != std::string::npos || key.find('\0') != std::string::npos)
        throw std::runtime_error("An API key cannot contain spaces or line breaks.");
    if (p.id == "pollinations" && !key.starts_with("sk_"))
        throw std::runtime_error("Pollinations needs an sk_ secret key; publishable pk_ keys don't work.");
    return key;
}
std::string provider_models_url(const ai_provider& p, const std::string& base_url)
{
    if (p.models_url.starts_with("https://"))
        return p.models_url;
    auto base = base_url;
    while (base.ends_with('/'))
        base.pop_back();
    return base + p.models_url;
}
std::vector<std::string> list_ai_models(const ai_provider& p, const std::string& base_url, const std::string& key, const std::atomic_bool* cancel,
                                        std::vector<std::string>* paid)
{
    split_ai_url(base_url);
    if (base_url.find_first_of("?#") != std::string::npos)
        throw std::runtime_error("Use a server URL without a query, such as http://127.0.0.1:8188/v1.");
    const bool compatible = p.model_list == ai_model_list::compatible;
    const auto json = get_ai_models_json(provider_models_url(p, base_url), key, cancel, compatible);
    std::vector<std::string> models;
    try
    {
        models = filter_ai_models(p.model_list, json);
        if (paid)
            *paid = paid_ai_models(p.model_list, json);
    }
    catch (const std::exception&)
    {
        if (!compatible)
            throw;  // a compatible server's odd list just means typing the model
    }
    if (models.empty() && !compatible)
    {
        if (p.model_list == ai_model_list::chatgpt_reasoning)
            throw std::runtime_error("ChatGPT listed no reasoning models for this account. Check the selected account and plan permission.");
        throw std::runtime_error(p.id == "openai" ? "This key can't use any GPT Image model. Check the project's model access and the organization's "
                                                    "verification at platform.openai.com."
                                                  : "The provider listed no models that edit images.");
    }
    return models;
}
std::string default_ai_model(const ai_provider& p, const std::vector<std::string>& models, const std::string& keep, const std::vector<std::string>& paid)
{
    if (!keep.empty() && (std::ranges::find(models, keep) != models.end() || (models.empty() && p.model_list == ai_model_list::compatible)))
        return keep;
    const auto usable = [&](const std::string& m)
    { return !m.empty() && std::ranges::find(models, m) != models.end() && std::ranges::find(paid, m) == paid.end(); };
    if (usable(p.preferred_model))
        return p.preferred_model;
    for (const auto& m : p.fallback_models)
        if (usable(m))
            return m;
    for (const auto& m : models)
        if (usable(m))
            return m;
    return models.empty() ? keep : models.front();
}
std::string ai_model_label(const ai_account& a, const std::string& model)
{
    return std::ranges::find(a.paid_models, model) != a.paid_models.end() ? model + " (paid)" : model;
}
std::string ai_failure_summary(const std::exception& e)
{
    if (const auto* h = dynamic_cast<const ai_http_error*>(&e))
    {
        const auto code = std::to_string(h->status);
        switch (h->status)
        {
            case 401:
                return "Key rejected (401)";
            case 403:
                return "Access denied (403)";
            case 404:
                return "Not found (404): check the server URL";
            case 429:
                return "Rate limit or quota reached (429)";
            default:
                return h->status >= 500 ? "Provider error (" + code + ")" : "Refused (HTTP " + code + ")";
        }
    }
    if (const auto* n = dynamic_cast<const ai_network_error*>(&e))
        switch (n->code)
        {
            case 12007:  // ERROR_WINHTTP_NAME_NOT_RESOLVED
                return "Server not found";
            case 12029:  // ERROR_WINHTTP_CANNOT_CONNECT
            case 12030:  // ERROR_WINHTTP_CONNECTION_ERROR
                return "Can't connect to the server";
            case 12002:  // ERROR_WINHTTP_TIMEOUT
                return "Timed out";
            case 12175:  // ERROR_WINHTTP_SECURE_FAILURE
            case 12037:
            case 12038:
            case 12044:
            case 12045:
            case 12057:
                return "Secure connection failed";
            default:
                return "Network error (WinHTTP " + std::to_string(n->code) + ")";
        }
    std::string text = e.what();
    while (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}
std::string ai_failure_detail(const std::exception& e)
{
    const auto summary = ai_failure_summary(e);
    std::string detail = summary + ".";
    if (dynamic_cast<const ai_http_error*>(&e) || dynamic_cast<const ai_network_error*>(&e))
    {
        const std::string what = e.what();
        if (!what.empty() && !what.starts_with("Provider returned HTTP") && !dynamic_cast<const ai_network_error*>(&e))
            detail = summary + ": " + what;
    }
    // Pollinations refuses a paid-only model to a key on the free balance.
    std::string lower = e.what();
    std::ranges::transform(lower, lower.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    if (lower.find("paid") != std::string::npos && (lower.find("balance") != std::string::npos || lower.find("pollen") != std::string::npos))
    {
        if (!detail.ends_with('.'))
            detail += '.';
        detail +=
            " This model needs a paid balance. Pick a free model in Settings → AI providers (paid models are marked \"(paid)\"), or allow "
            "this key to use the paid balance in your Pollinations account.";
    }
    return detail;
}
std::string load_account_key(const ai_account& a)
{
    return load_account_access(a, nullptr);
}
std::string load_account_access(const ai_account& a, const std::atomic_bool* cancel)
{
    const auto& p = find_ai_provider(a.provider);
    if (p.auth == ai_auth::oauth_session)
        return chatgpt_access_token(a.credential_target, cancel);
    if (p.auth == ai_auth::none || (p.auth == ai_auth::optional_key && a.credential_target.empty()))
        return {};
    auto key = load_ai_key(a.credential_target);
    if (key.empty())
        throw std::runtime_error(missing_key);
    return key;
}
ai_endpoint account_image_endpoint(const ai_account& a, double width, double height)
{
    const auto& p = find_ai_provider(a.provider);
    if (!p.can_draw)
        throw std::runtime_error("This provider can plan and review but cannot draw. Choose an image provider as well.");
    ai_endpoint e;
    e.provider_id = a.provider;
    e.base_url = a.base_url;
    e.model = a.model;
    if (e.model.empty())
        throw std::runtime_error("Choose a model for " + a.name + " in Settings → AI providers.");
    // GPT Image edits take 1024x1024, 1536x1024 or 1024x1536 (or auto):
    // the one matching the frame keeps the composition's proportions.
    e.size = width > height * 1.05 ? "1536x1024" : height > width * 1.05 ? "1024x1536" : "1024x1024";
    if (p.id == "openai")
    {
        if (!e.model.starts_with("gpt-image"))
            throw std::runtime_error("Choose a GPT Image model for " + a.name + " in Settings → AI providers.");
        e.quality = "auto";
    }
    else
        e.quality.clear();
    validate_ai_endpoint(e);
    return e;
}
bool supports_input_fidelity(const ai_endpoint& e)
{
    return e.provider_id == "openai" && e.model.starts_with("gpt-image") && e.model.find("mini") == std::string::npos;
}
}  // namespace offcut
