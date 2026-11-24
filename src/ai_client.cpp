#include "ai_client.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>
#include "ai_json.h"
#include "ai_responses.h"
#include "chatgpt_store.h"
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <wincred.h>
#include <winhttp.h>
// clang-format on
#endif

namespace offcut
{
namespace
{
constexpr size_t max_png = 64 * 1024 * 1024, max_json = 96 * 1024 * 1024;
void check_cancel(const std::atomic_bool* flag)
{
    if (flag && flag->load())
        throw ai_cancelled();
}
using namespace detail;
bool png_signature(const std::vector<uint8_t>& data)
{
    constexpr uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    return data.size() >= 8 && std::equal(std::begin(signature), std::end(signature), data.begin());
}
}  // namespace
std::string ai_image_extension(const std::vector<uint8_t>& data)
{
    if (png_signature(data))
        return ".png";
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return ".jpg";
    if (data.size() >= 12 && std::equal(data.begin(), data.begin() + 4, "RIFF") && std::equal(data.begin() + 8, data.begin() + 12, "WEBP"))
        return ".webp";
    return {};
}
namespace
{
#ifdef _WIN32
std::wstring wide(std::string_view s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 in AI connection settings.");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n);
    return out;
}
std::string narrow(std::wstring_view s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
struct internet_handle
{
    HINTERNET value = nullptr;
    explicit internet_handle(HINTERNET h) : value(h)
    {
        if (!h)
            throw std::runtime_error("Could not create a WinHTTP connection.");
    }
    ~internet_handle()
    {
        if (value)
            WinHttpCloseHandle(value);
    }
    internet_handle(const internet_handle&) = delete;
};
struct async_state
{
    std::mutex mutex;
    std::condition_variable ready;
    DWORD completed = 0, error = 0, amount = 0;
    bool closed = false;
};
void CALLBACK response_callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size)
{
    if (!context)
        return;
    auto& state = *reinterpret_cast<async_state*>(context);
    std::lock_guard lock(state.mutex);
    switch (status)
    {
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            state.closed = true;
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            state.error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            state.completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            state.amount = *static_cast<DWORD*>(info);
            state.completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            state.amount = size;
            state.completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            state.completed = status;
            break;
        default:
            return;
    }
    state.ready.notify_all();
}
class async_request
{
public:
    HINTERNET handle = nullptr;
    explicit async_request(HINTERNET h, const std::atomic_bool* flag) : handle(h), cancel_(flag)
    {
        if (!h)
            throw std::runtime_error("Could not create the AI HTTP request.");
        const DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state_);
        if (!WinHttpSetOption(h, WINHTTP_OPTION_CONTEXT_VALUE, const_cast<DWORD_PTR*>(&context), sizeof(context)))
        {
            WinHttpCloseHandle(handle);
            handle = nullptr;
            throw std::runtime_error("Could not configure the AI HTTP request.");
        }
        if (WinHttpSetStatusCallback(h, response_callback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        {
            WinHttpCloseHandle(handle);
            handle = nullptr;
            throw std::runtime_error("Could not configure AI HTTP callbacks.");
        }
    }
    ~async_request() { close(); }
    DWORD_PTR context() { return reinterpret_cast<DWORD_PTR>(&state_); }
    void reset()
    {
        check_cancel(cancel_);
        std::lock_guard lock(state_.mutex);
        state_.completed = state_.error = state_.amount = 0;
    }
    void started(BOOL ok, const char* operation)
    {
        if (!ok && GetLastError() != ERROR_IO_PENDING)
            fail(operation, GetLastError());
    }
    DWORD wait(DWORD expected, const char* operation)
    {
        std::unique_lock lock(state_.mutex);
        while (!state_.error && state_.completed != expected)
        {
            if (cancel_ && cancel_->load())
            {
                lock.unlock();
                close();
                throw ai_cancelled();
            }
            state_.ready.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (state_.error)
        {
            const auto error = state_.error;
            lock.unlock();
            fail(operation, error);
        }
        check_cancel(cancel_);
        return state_.amount;
    }

private:
    [[noreturn]] void fail(const char* operation, DWORD code)
    {
        check_cancel(cancel_);
        throw ai_network_error(code, std::string(operation) + " failed (WinHTTP " + std::to_string(code) + ").");
    }
    void close()
    {
        if (!handle)
            return;
        const auto h = std::exchange(handle, nullptr);
        WinHttpCloseHandle(h);
        std::unique_lock lock(state_.mutex);
        state_.ready.wait(lock, [&] { return state_.closed; });
    }
    async_state state_;
    const std::atomic_bool* cancel_;
};
std::string header_text(HINTERNET request, DWORD header)
{
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring text(bytes / sizeof(wchar_t), 0);
    if (!WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    text.resize(bytes / sizeof(wchar_t));
    while (!text.empty() && text.back() == 0)
        text.pop_back();
    return narrow(text);
}
struct http_reply
{
    DWORD status = 0;
    std::vector<uint8_t> bytes;
    std::string location;
    std::string request_id;
};
http_reply http(const std::string& url, const char* method, const ai_multipart* multipart, const std::string& key, const std::atomic_bool* cancel,
                const std::function<bool(std::string_view)>& on_chunk = {})
{
    check_cancel(cancel);
    const auto parsed = split_ai_url(url);
    const bool loopback = parsed.host == "localhost" || parsed.host == "127.0.0.1" || parsed.host == "[::1]" || parsed.host == "::1";
    internet_handle session(WinHttpOpen(L"Offcut/0.1", loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!WinHttpSetTimeouts(session.value, 30000, 30000, 60000, 300000))
        throw std::runtime_error("Could not set AI HTTP timeouts.");
    internet_handle connection(WinHttpConnect(session.value, wide(parsed.host).c_str(), parsed.port, 0));
    async_request request(WinHttpOpenRequest(connection.value, wide(method).c_str(), wide(parsed.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.secure ? WINHTTP_FLAG_SECURE : 0),
                          cancel);
    // Uploads must not be redirected to a different destination after consent.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
        throw std::runtime_error("Could not configure AI redirects.");
    std::string headers = "Accept: application/json, image/png, application/octet-stream\r\n";
    if (multipart)
        headers += "Content-Type: " + multipart->content_type + "\r\n";
    if (!key.empty())
    {
        if (key.find_first_of("\r\n") != std::string::npos || key.find('\0') != std::string::npos)
            throw std::runtime_error("The API key contains invalid characters.");
        headers += "Authorization: Bearer " + key + "\r\n";
    }
    const auto encoded = wide(headers);
    const DWORD total = multipart ? DWORD(multipart->body.size()) : 0;
    request.reset();
    request.started(WinHttpSendRequest(request.handle, encoded.c_str(), DWORD(encoded.size()), WINHTTP_NO_REQUEST_DATA, 0, total, request.context()),
                    "Sending AI request");
    request.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, "Sending AI request");
    if (multipart)
        for (size_t offset = 0; offset < multipart->body.size();)
        {
            const DWORD chunk = DWORD(std::min(size_t(1024 * 1024), multipart->body.size() - offset));
            request.reset();
            request.started(WinHttpWriteData(request.handle, multipart->body.data() + offset, chunk, nullptr), "Uploading AI images");
            const DWORD written = request.wait(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, "Uploading AI images");
            if (!written || written > chunk)
                throw std::runtime_error("The AI image upload did not complete.");
            offset += written;
        }
    request.reset();
    request.started(WinHttpReceiveResponse(request.handle, nullptr), "Receiving AI response");
    request.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, "Receiving AI response");
    http_reply out;
    DWORD status_size = sizeof(out.status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &out.status, &status_size,
                             WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Could not read the provider HTTP status.");
    out.location = header_text(request.handle, WINHTTP_QUERY_LOCATION);
    // A provider's diagnostic ID is safe to keep; never expose headers wholesale.
    DWORD id_bytes = 0;
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CUSTOM, L"x-request-id", nullptr, &id_bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        std::wstring id(id_bytes / sizeof(wchar_t), 0);
        if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CUSTOM, L"x-request-id", id.data(), &id_bytes, WINHTTP_NO_HEADER_INDEX))
        {
            id.resize(id_bytes / sizeof(wchar_t));
            while (!id.empty() && !id.back())
                id.pop_back();
            out.request_id = narrow(id);
        }
    }
    const size_t limit = multipart ? max_json : max_png;
    for (;;)
    {
        request.reset();
        request.started(WinHttpQueryDataAvailable(request.handle, nullptr), "Reading AI response");
        const DWORD available = request.wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, "Reading AI response");
        if (!available)
            break;
        if (out.bytes.size() + available > limit)
            throw std::runtime_error("The provider response exceeds the image size limit.");
        std::array<uint8_t, 65536> buffer{};
        const DWORD wanted = DWORD(std::min(size_t(available), buffer.size()));
        request.reset();
        request.started(WinHttpReadData(request.handle, buffer.data(), wanted, nullptr), "Reading AI response");
        const DWORD read = request.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, "Reading AI response");
        if (!read)
            break;
        if (read > wanted)
            throw std::runtime_error("Invalid AI response read size.");
        out.bytes.insert(out.bytes.end(), buffer.begin(), buffer.begin() + read);
        if (on_chunk && out.status >= 200 && out.status < 300 && !on_chunk({reinterpret_cast<const char*>(buffer.data()), read}))
            break;
    }
    return out;
}
void check_http(const http_reply& reply, const std::string& key)
{
    if (reply.status >= 200 && reply.status < 300)
        return;
    std::string message = "Provider returned HTTP " + std::to_string(reply.status) + ".";
    try
    {
        const auto parsed = json_reader(std::string_view(reinterpret_cast<const char*>(reply.bytes.data()), reply.bytes.size())).parse();
        if (const auto* error = json_at(parsed, {"error", "message"}); error && error->kind == json_value::string)
            message = error->text;
    }
    catch (...)
    {
    }
    message = redact_ai_secret(message, key);
    if (message.size() > 4000)
        message.resize(4000);
    throw ai_http_error(reply.status, message);
}
#endif
}  // namespace
std::string ai_json_quote(std::string_view text)
{
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 32)
                {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
                else
                    out += char(c);
        }
    }
    return out + '"';
}
std::string encode_ai_base64(const std::vector<uint8_t>& bytes)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t value =
            (uint32_t(bytes[i]) << 16) | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += table[(value >> 18) & 63];
        out += table[(value >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(value >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? table[value & 63] : '=';
    }
    return out;
}
ai_http_result ai_http(const std::string& url, const std::string& method, const std::string& body, const std::string& content_type, const std::string& bearer,
                       const std::atomic_bool* cancel, const std::function<bool(std::string_view)>& chunk)
{
    check_cancel(cancel);
    if (method != "GET" && method != "POST")
        throw std::runtime_error("Unsupported AI HTTP method.");
    if (body.size() > 96 * 1024 * 1024 || content_type.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Invalid AI HTTP payload.");
#ifdef _WIN32
    const ai_multipart payload{content_type, {body.begin(), body.end()}};
    const auto result = http(url, method.c_str(), method == "POST" ? &payload : nullptr, bearer, cancel, chunk);
    return {result.status, {reinterpret_cast<const char*>(result.bytes.data()), result.bytes.size()}, result.request_id};
#else
    (void)url;
    (void)content_type;
    (void)bearer;
    (void)chunk;
    throw std::runtime_error("Native AI connections require Windows.");
#endif
}
std::string redact_ai_secret(std::string text, std::string_view secret)
{
    if (secret.empty())
        return text;
    size_t at = 0;
    while ((at = text.find(secret, at)) != std::string::npos)
    {
        text.replace(at, secret.size(), "[redacted]");
        at += 10;
    }
    return text;
}
std::optional<std::string> ai_json_string(std::string_view text, std::initializer_list<std::string_view> path)
{
    const auto root = json_reader(text).parse();
    const auto* found = json_at(root, path);
    if (found && found->kind == json_value::string)
        return found->text;
    return std::nullopt;
}
ai_url split_ai_url(const std::string& url)
{
    if (url.empty() || url.find_first_of("\r\n\t ") != std::string::npos || url.find('\0') != std::string::npos)
        throw std::runtime_error("Use an absolute HTTP or HTTPS endpoint URL.");
#ifdef _WIN32
    const auto text = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(text.c_str(), DWORD(text.size()), ICU_REJECT_USERPWD, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) || !parts.dwHostNameLength || !parts.nPort)
        throw std::runtime_error("Use an absolute HTTP or HTTPS URL without embedded credentials.");
    std::wstring path;
    if (parts.dwUrlPathLength)
        path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path.empty())
        path = L"/";
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (const auto fragment = path.find(L'#'); fragment != std::wstring::npos)
        path.resize(fragment);
    return {narrow({parts.lpszHostName, parts.dwHostNameLength}), narrow(path), parts.nPort, parts.nScheme == INTERNET_SCHEME_HTTPS};
#else
    const bool secure = url.starts_with("https://");
    const size_t start = secure ? 8 : 7;
    if (!secure && !url.starts_with("http://"))
        throw std::runtime_error("Use HTTP or HTTPS.");
    const auto end = url.find_first_of("/?#", start);
    auto authority = url.substr(start, end - start);
    if (authority.empty() || authority.find('@') != std::string::npos)
        throw std::runtime_error("Invalid endpoint host.");
    std::string host = authority;
    unsigned port = secure ? 443 : 80;
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && (authority.front() != '[' || colon > authority.find(']')))
    {
        host = authority.substr(0, colon);
        const auto value = authority.substr(colon + 1);
        if (value.empty())
            throw std::runtime_error("Invalid port.");
        port = 0;
        for (char c : value)
        {
            if (c < '0' || c > '9' || port > 6553)
                throw std::runtime_error("Invalid port.");
            port = port * 10 + unsigned(c - '0');
        }
        if (!port || port > 65535)
            throw std::runtime_error("Invalid port.");
    }
    auto path = end == std::string::npos ? std::string("/") : url.substr(end);
    if (path.front() != '/')
        path = '/' + path;
    if (auto fragment = path.find('#'); fragment != std::string::npos)
        path.resize(fragment);
    return {host, path, uint16_t(port), secure};
#endif
}
void validate_ai_endpoint(const ai_endpoint& endpoint)
{
    split_ai_url(endpoint.base_url);
    if (endpoint.base_url.find_first_of("?#") != std::string::npos || endpoint.model.empty() || endpoint.size.empty() ||
        endpoint.model.find_first_of("\r\n") != std::string::npos || endpoint.size.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Set an endpoint base URL (without a query), a model and an image size.");
}
ai_multipart make_ai_multipart(const ai_endpoint& endpoint, const std::string& prompt, const std::vector<ai_image>& images, const std::string& boundary)
{
    validate_ai_endpoint(endpoint);
    if (images.empty() || boundary.empty() || boundary.size() > 70 ||
        boundary.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw std::runtime_error("Invalid AI image upload.");
    ai_multipart out{"multipart/form-data; boundary=" + boundary, {}};
    const auto append = [&](std::string_view text) { out.body.insert(out.body.end(), text.begin(), text.end()); };
    const auto field = [&](const char* name, const std::string& value)
    {
        if (value.find("--" + boundary) != std::string::npos)
            throw std::runtime_error("Multipart boundary conflicts with the request.");
        append("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n");
        append(value);
        append("\r\n");
    };
    field("model", endpoint.model);
    field("prompt", prompt);
    field("n", "1");
    field("size", endpoint.size);
    if (endpoint.provider_id == "openai" && endpoint.model.starts_with("gpt-image"))
        field("output_format", "png");
    if (!endpoint.quality.empty())
        field("quality", endpoint.quality);
    if (!endpoint.input_fidelity.empty())
        field("input_fidelity", endpoint.input_fidelity);
    for (const auto& image : images)
    {
        if (!png_signature(image.png) || image.png.size() > max_png || image.filename.empty() ||
            image.filename.find_first_of("\r\n\"\\/") != std::string::npos || image.filename.find('\0') != std::string::npos)
            throw std::runtime_error("An AI upload image is not a valid PNG attachment.");
        const auto delimiter = "\r\n--" + boundary;
        if (std::search(image.png.begin(), image.png.end(), delimiter.begin(), delimiter.end()) != image.png.end())
            throw std::runtime_error("Multipart boundary conflicts with the image.");
        append("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + (images.size() == 1 ? std::string("image") : std::string("image[]")) +
               "\"; filename=\"" + image.filename + "\"\r\nContent-Type: image/png\r\n\r\n");
        out.body.insert(out.body.end(), image.png.begin(), image.png.end());
        append("\r\n");
        if (out.body.size() > 256 * 1024 * 1024)
            throw std::runtime_error("AI upload exceeds 256 MB.");
    }
    append("--" + boundary + "--\r\n");
    return out;
}
std::vector<uint8_t> decode_ai_base64(std::string_view text)
{
    std::string clean;
    clean.reserve(text.size());
    for (char c : text)
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t')
            clean += c;
    if (clean.size() > ((max_png + 2) / 3) * 4 || clean.size() % 4 == 1)
        throw std::runtime_error("Invalid or oversized base64 image.");
    const size_t first_pad = clean.find('=');
    const size_t data = first_pad == std::string::npos ? clean.size() : first_pad;
    const size_t padding = clean.size() - data;
    if (padding > 2 || (padding && (clean.size() % 4 || std::ranges::any_of(std::string_view(clean).substr(data), [](char c) { return c != '='; }) ||
                                    (padding == 1 ? data % 4 != 3 : data % 4 != 2))))
        throw std::runtime_error("Invalid base64 image padding.");
    std::vector<uint8_t> out;
    out.reserve(data * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (size_t i = 0; i < data; ++i)
    {
        const char c = clean[i];
        int value = c >= 'A' && c <= 'Z'   ? c - 'A'
                    : c >= 'a' && c <= 'z' ? c - 'a' + 26
                    : c >= '0' && c <= '9' ? c - '0' + 52
                    : c == '+'             ? 62
                    : c == '/'             ? 63
                                           : -1;
        if (value < 0)
            throw std::runtime_error("Invalid base64 image data.");
        bits = (bits << 6) | uint32_t(value);
        count += 6;
        if (count >= 8)
        {
            count -= 8;
            out.push_back(uint8_t(bits >> count));
        }
    }
    if (count && (bits & ((1u << count) - 1)))
        throw std::runtime_error("Invalid base64 image tail.");
    return out;
}
ai_reply edit_ai_image(const ai_endpoint& endpoint, const std::string& prompt, const std::vector<ai_image>& images, const std::string& key,
                       const std::atomic_bool* cancel)
{
    check_cancel(cancel);
    validate_ai_endpoint(endpoint);
#ifdef _WIN32
    const auto start = std::chrono::steady_clock::now();
    static std::atomic_uint64_t serial{0};
    const auto boundary = "Offcut-" + std::to_string(start.time_since_epoch().count()) + "-" + std::to_string(serial++);
    const auto multipart = make_ai_multipart(endpoint, prompt, images, boundary);
    std::string base = endpoint.base_url;
    while (base.ends_with('/'))
        base.pop_back();
    auto response = http(base + "/images/edits", "POST", &multipart, key, cancel);
    check_http(response, key);
    const auto root = json_reader(std::string_view(reinterpret_cast<const char*>(response.bytes.data()), response.bytes.size())).parse();
    ai_reply result;
    if (const auto* encoded = json_at(root, {"data", "0", "b64_json"}); encoded && encoded->kind == json_value::string && !encoded->text.empty())
        result.png = decode_ai_base64(encoded->text);
    else if (const auto* location = json_at(root, {"data", "0", "url"}); location && location->kind == json_value::string)
    {
        auto url = location->text;
        for (int redirect = 0;; ++redirect)
        {
            auto image = http(url, "GET", nullptr, "", cancel);
            if (image.status >= 300 && image.status < 400 && !image.location.empty() && redirect < 5)
            {
                if (image.location.starts_with("http://") || image.location.starts_with("https://"))
                    url = image.location;
                else
                {
                    const auto at = url.find('/', url.find("://") + 3);
                    const auto origin = url.substr(0, at);
                    url = image.location.starts_with('/') ? origin + image.location : url.substr(0, url.rfind('/') + 1) + image.location;
                }
                continue;
            }
            check_http(image, key);
            result.png = std::move(image.bytes);
            break;
        }
    }
    else
        throw std::runtime_error("The provider returned no image in data[0].");
    check_cancel(cancel);
    // Asked for PNG, but compatible providers often answer JPEG or WebP.
    if (ai_image_extension(result.png).empty())
        throw std::runtime_error("The provider did not return a PNG, JPEG or WebP image.");
    if (const auto* usage = json_at(root, {"usage"}))
        result.usage_json = json_dump(*usage, key);
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
#else
    (void)prompt;
    (void)images;
    (void)key;
    throw std::runtime_error("The native AI provider connection currently requires Windows.");
#endif
}
std::string get_ai_models_json(const std::string& url, const std::string& key, const std::atomic_bool* cancel, bool missing_ok)
{
    check_cancel(cancel);
#ifdef _WIN32
    const auto reply = http(url, "GET", nullptr, key, cancel);
    if (missing_ok && reply.status == 404)
        return "{\"data\":[]}";
    check_http(reply, key);
    return {reinterpret_cast<const char*>(reply.bytes.data()), reply.bytes.size()};
#else
    (void)url;
    (void)key;
    throw std::runtime_error("Native AI connections require Windows.");
#endif
}
std::vector<std::string> filter_ai_models(ai_model_list kind, std::string_view text)
{
    if (kind == ai_model_list::chatgpt_reasoning)
        return chatgpt_models_from_json(text);
    const auto root = json_reader(text).parse();
    const auto* array = kind == ai_model_list::pollinations_images ? &root : json_at(root, {"data"});
    if (!array || array->kind != json_value::array)
        throw std::runtime_error("The provider returned an invalid model list.");
    std::vector<std::string> result;
    for (const auto& model : array->items)
    {
        const auto* id = json_at(model, {kind == ai_model_list::pollinations_images ? "name" : "id"});
        if (!id || id->kind != json_value::string || id->text.empty())
            continue;
        // Only GPT Image models take several images[] in one edit.
        if (kind == ai_model_list::openai_images && !id->text.starts_with("gpt-image"))
            continue;
        if (kind == ai_model_list::pollinations_images)
        {
            const auto* inputs = json_at(model, {"input_modalities"});
            if (!inputs || inputs->kind != json_value::array ||
                std::ranges::none_of(inputs->items, [](const auto& input) { return input.kind == json_value::string && input.text == "image"; }))
                continue;
        }
        result.push_back(id->text);
    }
    // GPT Image models first (compatible servers often offer them too), the
    // newest numeric family first, not a lexical ordering that ranks 1.5 after
    // 1 or 10 before 2; the stable alias before its snapshot. Then the rest.
    const auto version = [](const std::string& id)
    {
        std::vector<unsigned> v;
        if (!id.starts_with("gpt-image-"))
            return v;
        size_t at = 10;
        while (at < id.size() && id[at] >= '0' && id[at] <= '9')
        {
            unsigned n = 0;
            while (at < id.size() && id[at] >= '0' && id[at] <= '9')
            {
                n = std::min(100000u, n * 10 + unsigned(id[at++] - '0'));
            }
            v.push_back(n);
            if (at == id.size() || id[at] != '.')
                break;
            ++at;
        }
        return v;
    };
    std::ranges::sort(result,
                      [&](const auto& a, const auto& b)
                      {
                          const bool ag = a.starts_with("gpt-image"), bg = b.starts_with("gpt-image");
                          if (ag != bg)
                              return ag;
                          if (ag)
                          {
                              const auto av = version(a), bv = version(b);
                              if (av != bv)
                                  return av > bv;
                              if (a.size() != b.size())
                                  return a.size() < b.size();
                          }
                          return a < b;
                      });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::vector<std::string> paid_ai_models(ai_model_list kind, std::string_view text)
{
    const auto models = filter_ai_models(kind, text);
    if (kind != ai_model_list::pollinations_images)
        return {};
    const auto root = json_reader(text).parse();
    std::vector<std::string> paid;
    for (const auto& model : root.items)
    {
        const auto* id = json_at(model, {"name"});
        const auto* flag = json_at(model, {"paid_only"});
        if (id && id->kind == json_value::string && flag && flag->kind == json_value::scalar && flag->text == "true")
            paid.push_back(id->text);
    }
    std::vector<std::string> out;
    for (const auto& m : models)
        if (std::ranges::find(paid, m) != paid.end())
            out.push_back(m);
    return out;
}
bool native_ai_supported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
bool ai_key_saved(const std::string& target)
{
#ifdef _WIN32
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(wide(target).c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return false;
    const bool set = credential->CredentialBlobSize != 0;
    CredFree(credential);
    return set;
#else
    return false;
#endif
}
std::vector<std::string> saved_ai_key_targets(const std::string& prefix)
{
    std::vector<std::string> out;
#ifdef _WIN32
    DWORD count = 0;
    PCREDENTIALW* found = nullptr;
    if (!CredEnumerateW(wide(prefix + "*").c_str(), 0, &count, &found))
        return out;
    for (DWORD i = 0; i < count; ++i)
        if (found[i]->Type == CRED_TYPE_GENERIC && found[i]->TargetName)
        {
            const auto name = narrow(found[i]->TargetName);
            // Numbered ChatGPT storage chunks are not separate API accounts.
            if (!is_chatgpt_chunk_target(name))
                out.push_back(name);
        }
    CredFree(found);
#else
    (void)prefix;
#endif
    return out;
}
void store_ai_key(const std::string& key, const std::string& target)
{
#ifdef _WIN32
    if (key.empty() || key.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE || key.find_first_of("\r\n") != std::string::npos || key.find('\0') != std::string::npos)
        throw std::runtime_error("Enter a non-empty API key without line breaks.");
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    auto target_w = wide(target);
    credential.TargetName = target_w.data();
    credential.CredentialBlobSize = DWORD(key.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0))
        throw std::runtime_error("Windows Credential Manager could not save the API key.");
#else
    (void)key;
    throw std::runtime_error("API key storage currently requires Windows Credential Manager.");
#endif
}
void clear_ai_key(const std::string& target)
{
#ifdef _WIN32
    if (!CredDeleteW(wide(target).c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
        throw std::runtime_error("Windows Credential Manager could not clear the API key.");
#endif
}
std::string load_ai_key(const std::string& target)
{
#ifdef _WIN32
    PCREDENTIALW credential = nullptr;
    if (target.empty() || !CredReadW(wide(target).c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return {};
    std::string key(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return key;
#else
    (void)target;
    return {};
#endif
}
}  // namespace offcut
