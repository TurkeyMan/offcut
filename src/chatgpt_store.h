#pragma once
// Credential operations are injected so transaction ordering and failures can
// be tested without reading or writing any real credentials.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace offcut
{
using credential_bytes = std::vector<uint8_t>;
struct chatgpt_credential_store
{
    std::function<std::optional<credential_bytes>(const std::string&)> read;
    std::function<void(const std::string&, const credential_bytes&)> write;
    std::function<void(const std::string&)> erase;
    std::function<std::vector<std::string>(const std::string&)> list;
};
// Native adapter. Tests use only their own throwaway targets, never accounts.
chatgpt_credential_store native_chatgpt_credential_store();
bool is_chatgpt_chunk_target(const std::string&);
std::optional<credential_bytes> read_chatgpt_blob(const std::string& target, const chatgpt_credential_store&);
void write_chatgpt_blob(const std::string& target, const credential_bytes&, const chatgpt_credential_store&, size_t limit = 2560);
void clear_chatgpt_blobs(const std::string& target, const chatgpt_credential_store&);
}  // namespace offcut
