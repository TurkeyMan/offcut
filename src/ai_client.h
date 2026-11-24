#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace offcut
{
struct ai_endpoint
{
    std::string provider_id = "openai";
    std::string base_url = "https://api.openai.com/v1";
    std::string model;  // no default: each entry chooses one (gpt-image-1 was shut down on 23 Oct 2026)
    std::string size = "auto";
    std::string quality;
    std::string input_fidelity;  // "high" for refinements, where the model takes it
};
enum class ai_model_list
{
    openai_images,
    pollinations_images,
    compatible,
    chatgpt_reasoning
};
std::vector<std::string> filter_ai_models(ai_model_list, std::string_view json);
// Of those, the ones a key can use only with a paid balance (Pollinations'
// paid_only), in the same order.
std::vector<std::string> paid_ai_models(ai_model_list, std::string_view json);
std::string get_ai_models_json(const std::string& url, const std::string& key, const std::atomic_bool* cancel = nullptr, bool missing_ok = false);
struct ai_image
{
    std::string filename;
    std::vector<uint8_t> png;
};
struct ai_url
{
    std::string host, path;
    uint16_t port = 0;
    bool secure = false;
};
struct ai_multipart
{
    std::string content_type;
    std::vector<uint8_t> body;
};
struct ai_reply
{
    std::vector<uint8_t> png;
    std::string usage_json = "null";
    double seconds = 0;
};
struct ai_cancelled : std::runtime_error
{
    ai_cancelled() : std::runtime_error("AI request cancelled.") {}
};
// A provider's non-2xx reply: what() is its error.message when it gave one.
struct ai_http_error : std::runtime_error
{
    unsigned status;
    ai_http_error(unsigned code, const std::string& message) : std::runtime_error(message), status(code) {}
};
// The connection itself failed (name lookup, connect, TLS, timeout).
struct ai_network_error : std::runtime_error
{
    unsigned code;  // WinHTTP's ERROR_WINHTTP_*
    ai_network_error(unsigned winhttp, const std::string& message) : std::runtime_error(message), code(winhttp) {}
};

// Deterministic helpers: no network, credentials or application state.
ai_url split_ai_url(const std::string&);
void validate_ai_endpoint(const ai_endpoint&);
ai_multipart make_ai_multipart(const ai_endpoint&, const std::string& prompt, const std::vector<ai_image>&, const std::string& boundary);
std::vector<uint8_t> decode_ai_base64(std::string_view);
std::optional<std::string> ai_json_string(std::string_view, std::initializer_list<std::string_view> path);
std::string ai_json_quote(std::string_view);
std::string redact_ai_secret(std::string text, std::string_view secret);
std::string encode_ai_base64(const std::vector<uint8_t>&);
struct ai_http_result
{
    unsigned status = 0;
    std::string body, request_id;
};
// Native shared transport: no redirects, no logging. A streaming callback is
// called only for 2xx bodies; return false after a terminal SSE event.
ai_http_result ai_http(const std::string& url, const std::string& method, const std::string& body, const std::string& content_type, const std::string& bearer,
                       const std::atomic_bool* cancel = nullptr, const std::function<bool(std::string_view)>& chunk = {});

// WinHTTP only. Keyless endpoints are supported. Image downloads never carry
// the Bearer key. Cancellation closes an asynchronous request on its owning
// worker thread and waits for HANDLE_CLOSING before releasing callback state.
ai_reply edit_ai_image(const ai_endpoint&, const std::string& prompt, const std::vector<ai_image>&, const std::string& key,
                       const std::atomic_bool* cancel = nullptr);
bool native_ai_supported();
// Generic Windows Credential Manager entries, one per configured provider.
bool ai_key_saved(const std::string& target);
// ".png", ".jpg" or ".webp" by signature; empty for anything else.
std::string ai_image_extension(const std::vector<uint8_t>&);
// The generic credential targets starting with `prefix` (names only, never keys).
std::vector<std::string> saved_ai_key_targets(const std::string& prefix);
void store_ai_key(const std::string&, const std::string& target);
void clear_ai_key(const std::string& target);
std::string load_ai_key(const std::string& target);
}  // namespace offcut
