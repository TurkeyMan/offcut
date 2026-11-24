#pragma once
#include <memory>
#include "ai_client.h"
namespace offcut
{
struct reasoning_request
{
    std::string base_url = "https://api.openai.com/v1", model, instructions, input, schema;
    std::string schema_name = "offcut_result";
    std::vector<ai_image> images;
    bool chatgpt = false, structured = true;
};
struct reasoning_reply
{
    std::string text;
    double seconds = 0;
    unsigned long long input_tokens = 0, output_tokens = 0;
};
std::string responses_request_json(const reasoning_request&);
std::string chat_completions_request_json(const reasoning_request&);
std::string responses_error_message(std::string_view code, std::string_view message = {});
std::vector<std::string> chatgpt_models_from_json(std::string_view);
// Incremental, network-free SSE decoder. finish() rejects an interrupted stream.
class responses_sse
{
public:
    responses_sse();
    ~responses_sse();
    responses_sse(responses_sse&&) noexcept;
    responses_sse& operator=(responses_sse&&) noexcept;
    bool feed(std::string_view);  // false once response.completed arrived
    reasoning_reply finish();

private:
    struct state;
    std::unique_ptr<state> state_;
};
reasoning_reply call_responses(const reasoning_request&, const std::string& bearer, const std::atomic_bool* cancel = nullptr);
reasoning_reply call_chat_completions(const reasoning_request&, const std::string& bearer, const std::atomic_bool* cancel = nullptr);
}  // namespace offcut
