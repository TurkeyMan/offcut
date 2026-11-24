#include "ai_responses.h"
#include <algorithm>
#include <chrono>
#include "ai_json.h"
namespace offcut
{
using namespace detail;
namespace
{
std::string text_at(const json_value& n, std::initializer_list<std::string_view> p)
{
    const auto* v = json_at(n, p);
    return v && v->kind == json_value::string ? v->text : std::string();
}
unsigned long long count_at(const json_value& n, std::initializer_list<std::string_view> p)
{
    const auto* v = json_at(n, p);
    if (!v || v->kind != json_value::scalar)
        return 0;
    try
    {
        if (v->text.starts_with('-'))
            return 0;
        return std::stoull(v->text);
    }
    catch (...)
    {
        return 0;
    }
}
void check_reply(const ai_http_result& reply, const std::string& key)
{
    if (reply.status >= 200 && reply.status < 300)
        return;
    std::string code, message;
    try
    {
        const auto root = json_reader(reply.body).parse();
        code = text_at(root, {"error", "code"});
        message = text_at(root, {"error", "message"});
        if (message.empty())
            message = text_at(root, {"detail"});
    }
    catch (...)
    {
    }
    if (message.empty())
        message = "Provider returned HTTP " + std::to_string(reply.status) + ".";
    message = responses_error_message(code, message);
    message = redact_ai_secret(std::move(message), key);
    if (message.size() > 4000)
        message.resize(4000);
    throw ai_http_error(reply.status, message);
}
std::string completed_text(const json_value& response)
{
    std::string text;
    const auto* out = json_at(response, {"output"});
    if (!out || out->kind != json_value::array)
        return {};
    for (const auto& item : out->items)
    {
        const auto* content = json_at(item, {"content"});
        if (!content || content->kind != json_value::array)
            continue;
        for (const auto& block : content->items)
        {
            if (text_at(block, {"type"}) == "refusal")
                throw std::runtime_error("The director declined this request. Revise the instructions and try again.");
            if (text_at(block, {"type"}) == "output_text")
                text += text_at(block, {"text"});
        }
    }
    return text;
}
}  // namespace
std::string responses_error_message(std::string_view code, std::string_view message)
{
    if (code == "subscription_sharing_usage_limit_exceeded")
        return "Your ChatGPT plan's limit for Offcut is reached (Settings → Usage at chatgpt.com).";
    if (code == "subscription_sharing_user_not_eligible")
        return "ChatGPT plan usage is unavailable for this account, workspace or policy. Choose another planning entry.";
    if (code == "subscription_sharing_usage_unavailable")
        return "ChatGPT usage availability could not be checked. Your sign-in is preserved; try again later.";
    if (code == "subscription_sharing_user_unavailable")
        return "ChatGPT account or workspace information is temporarily unavailable. Try again later.";
    if (code == "subscription_sharing_unsupported_capability")
        return "This ChatGPT route does not support a requested model, input or response option. Change the model or request; do not repeat the same request.";
    if (code == "subscription_sharing_route_not_supported")
        return "ChatGPT plan usage requires POST /v1/responses. This route was not accepted.";
    if (code == "subscription_sharing_invalid_user")
        return "ChatGPT could not validate this subscriber. Check the selected account; sign in again if access was revoked.";
    if (code == "chatpass_v2_scope_not_authorized" || code == "chatpass_v2_invalid_authorization_context")
        return "The ChatGPT grant does not authorize this operation. Check the account's plan permission and client registration.";
    if (message.empty())
        return code.empty() ? "The director request failed." : "The director request failed (" + std::string(code) + ").";
    return std::string(message);
}
std::vector<std::string> chatgpt_models_from_json(std::string_view text)
{
    const auto root = json_reader(text).parse();
    const auto* models = json_at(root, {"models"});
    if (!models || models->kind != json_value::array)
        throw std::runtime_error("ChatGPT returned an invalid model catalog.");
    std::vector<std::string> out;
    for (const auto& m : models->items)
        if (text_at(m, {"visibility"}) == "list")
        {
            const auto slug = text_at(m, {"slug"});
            if (!slug.empty() && std::ranges::find(out, slug) == out.end())
                out.push_back(slug);
        }
    return out;
}
std::string responses_request_json(const reasoning_request& r)
{
    if (r.model.empty())
        throw std::runtime_error("Choose a reasoning model for the director.");
    if (r.chatgpt && r.base_url != "https://api.openai.com/v1")
        throw std::runtime_error("Sign in with ChatGPT uses the public OpenAI Responses endpoint only.");
    std::string body = "{\"model\":" + ai_json_quote(r.model) + ",\"store\":false,\"stream\":true,\"instructions\":" + ai_json_quote(r.instructions) +
                       ",\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":" + ai_json_quote(r.input) + "}";
    for (const auto& image : r.images)
    {
        if (ai_image_extension(image.png) != ".png")
            throw std::runtime_error("Director inputs must be PNG images.");
        body += ",{\"type\":\"input_image\",\"image_url\":" + ai_json_quote("data:image/png;base64," + encode_ai_base64(image.png)) + "}";
    }
    body += "]}]";
    if (r.structured && !r.schema.empty())
    {
        json_reader(r.schema).parse();
        body += ",\"text\":{\"format\":{\"type\":\"json_schema\",\"name\":" + ai_json_quote(r.schema_name) + ",\"strict\":true,\"schema\":" + r.schema + "}}";
    }
    return body + "}";
}
struct responses_sse::state
{
    std::string pending, data, event, deltas;
    bool completed = false;
    reasoning_reply reply;
    void dispatch()
    {
        if (data.empty())
        {
            event.clear();
            return;
        }
        if (data.ends_with('\n'))
            data.pop_back();
        if (data == "[DONE]")
        {
            data.clear();
            event.clear();
            return;
        }
        const auto root = json_reader(data).parse();
        auto type = text_at(root, {"type"});
        if (type.empty())
            type = event;
        if (type == "response.failed" || type == "error")
        {
            auto code = text_at(root, {"response", "error", "code"});
            auto message = text_at(root, {"response", "error", "message"});
            if (code.empty())
                code = text_at(root, {"code"});
            if (message.empty())
                message = text_at(root, {"message"});
            if (code.empty())
                code = text_at(root, {"error", "code"});
            if (message.empty())
                message = text_at(root, {"error", "message"});
            throw std::runtime_error(responses_error_message(code, message));
        }
        if (type == "response.incomplete")
            throw std::runtime_error("The director response was incomplete. Try again.");
        if (type == "response.refusal.delta")
            throw std::runtime_error("The director declined this request. Revise the instructions.");
        if (type == "response.output_text.delta")
            deltas += text_at(root, {"delta"});
        if (deltas.size() > 4 * 1024 * 1024)
            throw std::runtime_error("The director response is too large.");
        if (type == "response.completed")
        {
            const auto* response = json_at(root, {"response"});
            if (!response || response->kind != json_value::object)
                throw std::runtime_error("Invalid completed director response.");
            const auto status = text_at(*response, {"status"});
            if (!status.empty() && status != "completed")
                throw std::runtime_error("The director response did not complete.");
            reply.text = completed_text(*response);
            if (reply.text.empty())
                reply.text = deltas;
            reply.input_tokens = count_at(*response, {"usage", "input_tokens"});
            reply.output_tokens = count_at(*response, {"usage", "output_tokens"});
            completed = true;
        }
        data.clear();
        event.clear();
    }
    void line(std::string line)
    {
        if (line.ends_with('\r'))
            line.pop_back();
        if (line.empty())
        {
            dispatch();
            return;
        }
        if (line.starts_with(':'))
            return;
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            return;
        auto value = line.substr(colon + 1);
        if (value.starts_with(' '))
            value.erase(0, 1);
        if (line.substr(0, colon) == "data")
            data += value + '\n';
        else if (line.substr(0, colon) == "event")
            event = std::move(value);
        if (data.size() > 8 * 1024 * 1024)
            throw std::runtime_error("The director event is too large.");
    }
};
responses_sse::responses_sse() : state_(std::make_unique<state>()) {}
responses_sse::~responses_sse() = default;
responses_sse::responses_sse(responses_sse&&) noexcept = default;
responses_sse& responses_sse::operator=(responses_sse&&) noexcept = default;
bool responses_sse::feed(std::string_view chunk)
{
    if (state_->completed)
        return false;
    state_->pending += chunk;
    if (state_->pending.size() > 8 * 1024 * 1024)
        throw std::runtime_error("The director stream line is too large.");
    size_t end = 0;
    while ((end = state_->pending.find('\n')) != std::string::npos)
    {
        auto line = state_->pending.substr(0, end);
        state_->pending.erase(0, end + 1);
        state_->line(std::move(line));
        if (state_->completed)
            return false;
    }
    return true;
}
reasoning_reply responses_sse::finish()
{
    if (!state_->pending.empty())
    {
        state_->line(std::move(state_->pending));
        state_->pending.clear();
    }
    if (!state_->completed && !state_->data.empty())
        state_->dispatch();
    if (!state_->completed)
        throw std::runtime_error("Director stream ended without response.completed.");
    if (state_->reply.text.empty())
        throw std::runtime_error("The director returned no text.");
    return state_->reply;
}
reasoning_reply call_responses(const reasoning_request& request, const std::string& bearer, const std::atomic_bool* cancel)
{
    auto r = request;
    const auto start = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        responses_sse stream;
        auto base = r.base_url;
        while (base.ends_with('/'))
            base.pop_back();
        try
        {
            const auto reply = ai_http(base + "/responses", "POST", responses_request_json(r), "application/json", bearer, cancel,
                                       [&](std::string_view bytes) { return stream.feed(bytes); });
            if (attempt == 0 && r.structured && reply.status == 400)
            {
                const auto root = json_reader(reply.body).parse();
                const auto param = text_at(root, {"error", "param"});
                auto message = text_at(root, {"error", "message"});
                std::ranges::transform(message, message.begin(), [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
                if (param.starts_with("text.format") || message.find("json_schema") != std::string::npos)
                {
                    r.structured = false;
                    r.instructions += "\nReturn only valid JSON matching this schema:\n" + r.schema;
                    continue;
                }
            }
            check_reply(reply, bearer);
            auto result = stream.finish();
            result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return result;
        }
        catch (const ai_cancelled&)
        {
            throw;
        }
        catch (const ai_http_error& e)
        {
            throw ai_http_error(e.status, redact_ai_secret(e.what(), bearer));
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(redact_ai_secret(e.what(), bearer));
        }
    }
    throw std::runtime_error("The director rejected the response format.");
}
std::string chat_completions_request_json(const reasoning_request& r)
{
    if (r.chatgpt)
        throw std::runtime_error("ChatGPT sign-in cannot use Chat Completions.");
    std::string body = "{\"model\":" + ai_json_quote(r.model) +
                       ",\"stream\":false,\"messages\":[{\"role\":\"system\",\"content\":" + ai_json_quote(r.instructions) +
                       "},{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + ai_json_quote(r.input) + "}";
    for (const auto& image : r.images)
    {
        if (ai_image_extension(image.png) != ".png")
            throw std::runtime_error("Director inputs must be PNG.");
        body += ",{\"type\":\"image_url\",\"image_url\":{\"url\":" + ai_json_quote("data:image/png;base64," + encode_ai_base64(image.png)) + "}}";
    }
    body += "]}]";
    if (r.structured && !r.schema.empty())
        body += ",\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":" + ai_json_quote(r.schema_name) +
                ",\"strict\":true,\"schema\":" + r.schema + "}}";
    body += '}';
    return body;
}
reasoning_reply call_chat_completions(const reasoning_request& request, const std::string& bearer, const std::atomic_bool* cancel)
{
    if (request.chatgpt)
        throw std::runtime_error("ChatGPT sign-in cannot use Chat Completions.");
    auto r = request;
    const auto start = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto body = chat_completions_request_json(r);
        auto base = r.base_url;
        while (base.ends_with('/'))
            base.pop_back();
        const auto reply = ai_http(base + "/chat/completions", "POST", body, "application/json", bearer, cancel);
        if (attempt == 0 && r.structured && reply.status == 400)
        {
            try
            {
                const auto error = json_reader(reply.body).parse();
                const auto param = text_at(error, {"error", "param"});
                const auto message = text_at(error, {"error", "message"});
                if (param.starts_with("response_format") || message.find("json_schema") != std::string::npos)
                {
                    r.structured = false;
                    r.instructions += "\nReturn only JSON matching this schema:\n" + r.schema;
                    continue;
                }
            }
            catch (...)
            {
            }
        }
        check_reply(reply, bearer);
        try
        {
            const auto root = json_reader(reply.body).parse();
            const auto reason = text_at(root, {"choices", "0", "finish_reason"});
            if (reason == "length")
                throw std::runtime_error("The director response was incomplete.");
            if (reason == "content_filter" || !text_at(root, {"choices", "0", "message", "refusal"}).empty())
                throw std::runtime_error("The director declined this request.");
            const auto text = text_at(root, {"choices", "0", "message", "content"});
            if (text.empty())
                throw std::runtime_error("The director returned no text.");
            return {redact_ai_secret(text, bearer), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    count_at(root, {"usage", "prompt_tokens"}), count_at(root, {"usage", "completion_tokens"})};
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(redact_ai_secret(e.what(), bearer));
        }
    }
    throw std::runtime_error("The director rejected the JSON response format.");
}
}  // namespace offcut
