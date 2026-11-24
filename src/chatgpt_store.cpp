#include "chatgpt_store.h"
#include <algorithm>
#include <charconv>
#include <exception>
#include <set>
#include <stdexcept>
#include "chatgpt_auth_helpers.h"
namespace offcut
{
namespace
{
constexpr size_t digest_size = 43, header_size = 5 + 12 + digest_size, max_bytes = 256 * 1024;
constexpr uint32_t max_index = 1000000;
std::optional<uint32_t> index(std::string_view suffix)
{
    uint32_t n = 0;
    const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), n);
    if (result.ec != std::errc{} || result.ptr != suffix.data() + suffix.size() || n < 2 || std::to_string(n) != suffix)
        return {};
    return n;
}
bool belongs(const std::string& target, const std::string& name)
{
    const auto prefix = target + '#';
    return name.starts_with(prefix) && bool(index(std::string_view(name).substr(prefix.size())));
}
bool chunked(const credential_bytes& bytes)
{
    return bytes.size() >= 5 && std::equal(bytes.begin(), bytes.begin() + 5, "OCSC2");
}
void append(credential_bytes& bytes, uint32_t n)
{
    for (int i = 0; i < 4; ++i)
        bytes.push_back(uint8_t(n >> (8 * i)));
}
uint32_t number(const credential_bytes& bytes, size_t at)
{
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n |= uint32_t(bytes[at + size_t(i)]) << (8 * i);
    return n;
}
std::string digest(const credential_bytes& bytes)
{
    return pkce_s256({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}
[[noreturn]] void invalid()
{
    throw std::runtime_error("The saved ChatGPT session is incomplete or damaged. Sign in again.");
}
}  // namespace
bool is_chatgpt_chunk_target(const std::string& name)
{
    const auto mark = name.rfind('#');
    return mark != std::string::npos && bool(index(std::string_view(name).substr(mark + 1)));
}
std::optional<credential_bytes> read_chatgpt_blob(const std::string& target, const chatgpt_credential_store& store)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        auto root = store.read(target);
        if (!root)
            return {};
        try
        {
            // Old OCSI1 single-credential sessions remain readable without migration.
            if (!chunked(*root))
                return root;
            if (root->size() < header_size)
                invalid();
            const auto total = number(*root, 5), first = number(*root, 9), count = number(*root, 13);
            if (!total || total > max_bytes || !count || count > 4096 || first < 2 || first > max_index || count > max_index - first + 1)
                invalid();
            const std::string expected(root->begin() + 17, root->begin() + header_size);
            credential_bytes bytes(root->begin() + header_size, root->end());
            if (bytes.size() >= total)
                invalid();
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto part = store.read(target + '#' + std::to_string(first + i));
                if (!part || part->empty() || bytes.size() + part->size() > total)
                    invalid();
                bytes.insert(bytes.end(), part->begin(), part->end());
            }
            if (bytes.size() != total || digest(bytes) != expected)
                invalid();
            return bytes;
        }
        catch (...)
        {
            // Status probes do not take the long-lived OAuth mutex. If a
            // writer committed while we read, retry its new complete generation.
            if (store.read(target) == root)
                throw;
        }
    }
    throw std::runtime_error("ChatGPT session changed while being read. Try again.");
}
void write_chatgpt_blob(const std::string& target, const credential_bytes& bytes, const chatgpt_credential_store& store, size_t limit)
{
    if (bytes.empty() || bytes.size() > max_bytes || limit <= header_size || limit > 2560)
        throw std::runtime_error("Invalid ChatGPT credential payload.");
    const auto listed = store.list(target + '#');
    std::set<std::string> occupied;
    for (const auto& name : listed)
        if (belongs(target, name))
            occupied.insert(name);
    credential_bytes root = bytes;
    std::vector<std::pair<std::string, credential_bytes>> staged;
    if (bytes.size() > limit)
    {
        const size_t head = limit - header_size;
        const auto count = uint32_t((bytes.size() - head + limit - 1) / limit);
        uint32_t first = 2;
        for (;;)
        {
            if (first > max_index - count + 1)
                throw std::runtime_error("Too many saved ChatGPT credential chunks.");
            bool free = true;
            for (uint32_t i = 0; i < count; ++i)
                if (occupied.contains(target + '#' + std::to_string(first + i)))
                {
                    first += i + 1;
                    free = false;
                    break;
                }
            if (free)
                break;
        }
        root.clear();
        root.insert(root.end(), {'O', 'C', 'S', 'C', '2'});
        append(root, uint32_t(bytes.size()));
        append(root, first);
        append(root, count);
        const auto hash = digest(bytes);
        root.insert(root.end(), hash.begin(), hash.end());
        root.insert(root.end(), bytes.begin(), bytes.begin() + head);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto begin = head + size_t(i) * limit, end = std::min(bytes.size(), begin + limit);
            staged.push_back({target + '#' + std::to_string(first + i), {bytes.begin() + begin, bytes.begin() + end}});
        }
    }
    size_t written = 0;
    try
    {
        // Use vacant numbered slots. The old root and every old chunk remain
        // readable until this last atomic CredWrite replaces the root/manifest.
        for (const auto& [name, data] : staged)
        {
            store.write(name, data);
            ++written;
        }
        store.write(target, root);
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        for (size_t i = 0; i < written; ++i)
            try
            {
                store.erase(staged[i].first);
            }
            catch (...)
            {
            }
        std::rethrow_exception(failure);
    }
    std::set<std::string> active;
    for (const auto& part : staged)
        active.insert(part.first);
    // A cleanup failure leaves harmless, unreachable chunks for the next save
    // or sign-out. It must not turn a successfully rotated token into a failure.
    for (const auto& old : occupied)
        if (!active.contains(old))
            try
            {
                store.erase(old);
            }
            catch (...)
            {
            }
}
void clear_chatgpt_blobs(const std::string& target, const chatgpt_credential_store& store)
{
    const auto listed = store.list(target + '#');
    std::exception_ptr failure;
    const auto erase = [&](const std::string& name)
    {
        try
        {
            store.erase(name);
        }
        catch (...)
        {
            if (!failure)
                failure = std::current_exception();
        }
    };
    // Invalidate the commit marker first, then try every chunk even if any
    // individual deletion fails; a retry can still find orphaned numbered keys.
    erase(target);
    for (const auto& name : listed)
        if (belongs(target, name))
            erase(name);
    if (failure)
        std::rethrow_exception(failure);
}
}  // namespace offcut
