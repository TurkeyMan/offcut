#pragma once
// Deterministic protocol helpers; no credentials or network access.
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace offcut
{
std::string oauth_form(const std::map<std::string, std::string>&);
std::string pkce_s256(std::string_view verifier);
struct chatgpt_callback
{
    std::string code, client_id, error;
};
chatgpt_callback parse_chatgpt_callback(std::string_view target, std::string_view expected_state, std::string_view returning_client = {});
struct chatgpt_tokens
{
    std::string access_token, refresh_token, id_token, scope;
    long long expires_in = 0;
};
chatgpt_tokens parse_chatgpt_tokens(std::string_view json, bool refreshing = false);
bool chatgpt_terminal_refresh_error(std::string_view code);
std::vector<uint8_t> pack_chatgpt_session(std::string_view);
std::string unpack_chatgpt_session(const std::vector<uint8_t>&);
}  // namespace offcut
