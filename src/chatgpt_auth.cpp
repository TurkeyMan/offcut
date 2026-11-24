#include "chatgpt_auth.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include "ai_client.h"
#include "ai_json.h"
#include "chatgpt_auth_helpers.h"
#include "chatgpt_store.h"
#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincred.h>
// clang-format on
#endif
namespace offcut
{
using namespace detail;
namespace
{
constexpr const char* issuer = "https://auth.openai.com";
constexpr const char* resource = "https://api.openai.com/v1";
constexpr const char* token_url = "https://auth.openai.com/api/accounts/oauth/token";
void cancelled(const std::atomic_bool* flag)
{
    if (flag && flag->load())
        throw ai_cancelled();
}
std::string string_at(const json_value& n, std::initializer_list<std::string_view> path)
{
    const auto* v = json_at(n, path);
    return v && v->kind == json_value::string ? v->text : std::string();
}
long long integer_at(const json_value& n, std::initializer_list<std::string_view> path)
{
    const auto* v = json_at(n, path);
    if (!v || v->kind != json_value::scalar)
        throw std::runtime_error("ChatGPT returned invalid token timing.");
    size_t end = 0;
    long long out = 0;
    try
    {
        out = std::stoll(v->text, &end);
    }
    catch (...)
    {
        throw std::runtime_error("ChatGPT returned invalid token timing.");
    }
    if (end != v->text.size())
        throw std::runtime_error("ChatGPT returned invalid token timing.");
    return out;
}
bool has_scope(const std::string& scopes, const std::string& wanted)
{
    std::istringstream in(scopes);
    std::string word;
    while (in >> word)
        if (word == wanted)
            return true;
    return false;
}
bool same(std::string_view a, std::string_view b)
{
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
bool issued(std::string_view id)
{
    return id.starts_with("oaiapp_") && id.size() > 7 && id.size() < 256 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos;
}
std::vector<uint8_t> unurl(std::string text)
{
    for (auto& c : text)
    {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    return decode_ai_base64(text);
}
std::string url64(const std::vector<uint8_t>& bytes)
{
    auto s = encode_ai_base64(bytes);
    for (auto& c : s)
    {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    while (s.ends_with('='))
        s.pop_back();
    return s;
}
#ifdef _WIN32
std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid sign-in setting.");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), n);
    return out;
}
std::vector<uint8_t> sha256(std::string_view text)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Windows SHA-256 is unavailable.");
    std::vector<uint8_t> out(32);
    const auto status =
        BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), ULONG(text.size()), out.data(), ULONG(out.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0)
        throw std::runtime_error("Windows SHA-256 failed.");
    return out;
}
std::string random_value(size_t length = 32)
{
    std::vector<uint8_t> bytes(length);
    if (BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Windows secure random generator failed.");
    return url64(bytes);
}
std::filesystem::path config_folder()
{
    if (const char* override_dir = std::getenv("OFFCUT_CONFIG_DIR"); override_dir && *override_dir)
    {
        auto folder = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(override_dir)));
        std::filesystem::create_directories(folder);
        return folder;
    }
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)))
        throw std::runtime_error("The Offcut settings folder is unavailable.");
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    result /= "offcut";
    std::filesystem::create_directories(result);
    return result;
}
// Named mutexes serialize rotation across all Offcut processes, not only
// threads. Tokens read before acquiring this lock are never used for refresh.
class target_lock
{
    HANDLE handle_ = nullptr;

public:
    target_lock(const std::string& target, const std::atomic_bool* flag)
    {
        auto name = L"Local\\Offcut-ChatGPT-" + wide(pkce_s256(target));
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_)
            throw std::runtime_error("Could not lock the ChatGPT session.");
        for (;;)
        {
            if (flag && flag->load())
            {
                CloseHandle(handle_);
                handle_ = nullptr;
                throw ai_cancelled();
            }
            const auto result = WaitForSingleObject(handle_, 100);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
                break;
            if (result != WAIT_TIMEOUT)
            {
                CloseHandle(handle_);
                handle_ = nullptr;
                throw std::runtime_error("Could not lock the ChatGPT session.");
            }
        }
    }
    ~target_lock()
    {
        if (handle_)
        {
            ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
    }
};
std::string host_id()
{
    target_lock lock("host-id", nullptr);
    const auto file = config_folder() / "chatgpt-host-id.txt";
    std::ifstream in(file);
    std::string id;
    if (in >> id)
    {
        if (id.starts_with("urn:uuid:") && id.size() == 45)
            return id;
        throw std::runtime_error("The saved ChatGPT host ID is invalid.");
    }
    std::array<uint8_t, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Could not create the ChatGPT host ID.");
    bytes[6] = (bytes[6] & 15) | 0x40;
    bytes[8] = (bytes[8] & 63) | 0x80;
    std::ostringstream out;
    out << "urn:uuid:" << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out << '-';
        out << std::setw(2) << int(bytes[i]);
    }
    id = out.str();
    std::ofstream save(file);
    save << id;
    save.close();
    if (!save)
        throw std::runtime_error("Could not save the ChatGPT host ID.");
    return id;
}
struct registration
{
    std::string client, subject;
};
std::filesystem::path registration_path(const std::string& target)
{
    return config_folder() / ("chatgpt-registration-" + pkce_s256(target) + ".txt");
}
registration read_registration(const std::string& target)
{
    registration r;
    std::ifstream in(registration_path(target));
    if (in && !(in >> std::quoted(r.client) >> std::quoted(r.subject)))
        throw std::runtime_error("The saved ChatGPT registration is invalid.");
    if (!r.client.empty() && !issued(r.client))
        throw std::runtime_error("The saved ChatGPT client ID is invalid.");
    return r;
}
void save_registration(const std::string& target, const registration& r)
{
    auto file = registration_path(target);
    auto pending = file;
    pending += ".pending";
    std::ofstream out(pending);
    out << std::quoted(r.client) << '\n' << std::quoted(r.subject) << '\n';
    out.close();
    if (!out || !MoveFileExW(pending.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not save the ChatGPT registration.");
}
struct session
{
    std::string refresh, client, id_token, subject, label, scope;
};
std::string session_json(const session& s)
{
    return "{\"refresh_token\":" + ai_json_quote(s.refresh) + ",\"client_id\":" + ai_json_quote(s.client) + ",\"id_token\":" + ai_json_quote(s.id_token) +
           ",\"subject\":" + ai_json_quote(s.subject) + ",\"label\":" + ai_json_quote(s.label) + ",\"scope\":" + ai_json_quote(s.scope) + "}";
}
chatgpt_credential_store credential_store()
{
    return {[](const std::string& target) -> std::optional<credential_bytes>
            {
                PCREDENTIALW cred = nullptr;
                if (!CredReadW(wide(target).c_str(), CRED_TYPE_GENERIC, 0, &cred))
                {
                    if (GetLastError() == ERROR_NOT_FOUND)
                        return {};
                    throw std::runtime_error("Windows Credential Manager could not read the ChatGPT session.");
                }
                credential_bytes bytes(cred->CredentialBlob, cred->CredentialBlob + cred->CredentialBlobSize);
                SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
                CredFree(cred);
                return bytes;
            },
            [](const std::string& target, const credential_bytes& bytes)
            {
                if (bytes.empty() || bytes.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
                    throw std::runtime_error("Invalid ChatGPT credential chunk.");
                auto name = wide(target);
                CREDENTIALW cred{};
                cred.Type = CRED_TYPE_GENERIC;
                cred.TargetName = name.data();
                cred.CredentialBlob = const_cast<LPBYTE>(bytes.data());
                cred.CredentialBlobSize = DWORD(bytes.size());
                cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
                if (!CredWriteW(&cred, 0))
                    throw std::runtime_error("Windows Credential Manager could not save the ChatGPT session.");
            },
            [](const std::string& target) { clear_ai_key(target); },
            [](const std::string& prefix)
            {
                std::vector<std::string> targets;
                DWORD count = 0;
                PCREDENTIALW* found = nullptr;
                if (!CredEnumerateW(wide(prefix + "*").c_str(), 0, &count, &found))
                {
                    if (GetLastError() == ERROR_NOT_FOUND)
                        return targets;
                    throw std::runtime_error("Windows Credential Manager could not list the ChatGPT session chunks.");
                }
                for (DWORD i = 0; i < count; ++i)
                {
                    if (found[i]->Type == CRED_TYPE_GENERIC && found[i]->TargetName)
                    {
                        const std::wstring_view name(found[i]->TargetName);
                        const int n = WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), nullptr, 0, nullptr, nullptr);
                        std::string text(n, 0);
                        WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), text.data(), n, nullptr, nullptr);
                        targets.push_back(std::move(text));
                    }
                    if (found[i]->CredentialBlobSize)
                        SecureZeroMemory(found[i]->CredentialBlob, found[i]->CredentialBlobSize);
                }
                CredFree(found);
                return targets;
            }};
}
session read_session(const std::string& target)
{
    const auto bytes = read_chatgpt_blob(target, credential_store());
    if (!bytes)
        return {};
    const auto root = json_reader(unpack_chatgpt_session(*bytes)).parse();
    session s{string_at(root, {"refresh_token"}), string_at(root, {"client_id"}), string_at(root, {"id_token"}),
              string_at(root, {"subject"}),       string_at(root, {"label"}),     string_at(root, {"scope"})};
    if (s.refresh.empty() || !issued(s.client) || s.id_token.empty() || s.subject.empty())
        throw std::runtime_error("The saved ChatGPT session is invalid. Sign in again.");
    return s;
}
void save_session(const std::string& target, const session& s)
{
    auto bytes = pack_chatgpt_session(session_json(s));
    try
    {
        write_chatgpt_blob(target, bytes, credential_store(), CRED_MAX_CREDENTIAL_BLOB_SIZE);
    }
    catch (...)
    {
        SecureZeroMemory(bytes.data(), bytes.size());
        throw;
    }
    SecureZeroMemory(bytes.data(), bytes.size());
}
struct cached_access
{
    std::string access, refresh;
    std::chrono::steady_clock::time_point expires;
};
std::mutex cache_mutex;
std::map<std::string, cached_access> cache;
void forget(const std::string& target)
{
    std::exception_ptr failure;
    try
    {
        clear_chatgpt_blobs(target, credential_store());
    }
    catch (...)
    {
        failure = std::current_exception();
    }
    {
        std::lock_guard lock(cache_mutex);
        cache.erase(target);
    }
    if (failure)
        std::rethrow_exception(failure);
}
void cache_token(const std::string& target, const session& s, const chatgpt_tokens& tokens)
{
    std::lock_guard lock(cache_mutex);
    cache[target] = {tokens.access_token, s.refresh, std::chrono::steady_clock::now() + std::chrono::seconds(std::max(0LL, tokens.expires_in - 60))};
}
std::string scrub(std::string text, const session& s)
{
    for (const auto& secret : {s.refresh, s.id_token})
        text = redact_ai_secret(std::move(text), secret);
    return text;
}
ai_http_result token_request(const std::map<std::string, std::string>& fields, const std::atomic_bool* cancel)
{
    return ai_http(token_url, "POST", oauth_form(fields), "application/x-www-form-urlencoded", "", cancel);
}
std::string oauth_error(const ai_http_result& response)
{
    try
    {
        auto root = json_reader(response.body).parse();
        auto code = string_at(root, {"error"});
        if (code.empty())
            code = string_at(root, {"error", "code"});
        return code;
    }
    catch (...)
    {
        return {};
    }
}
void token_status(const ai_http_result& response, const session& s, bool refreshing)
{
    if (response.status >= 200 && response.status < 300)
        return;
    const auto code = oauth_error(response);
    if (refreshing && chatgpt_terminal_refresh_error(code))
        throw std::runtime_error("ChatGPT session expired or was revoked. Sign in again.");
    if (code == "access_denied")
        throw std::runtime_error("ChatGPT sign-in was declined.");
    throw ai_http_error(response.status, scrub("ChatGPT authorization failed (HTTP " + std::to_string(response.status) + "). " +
                                                   (code == "invalid_client" ? "The saved client registration was not accepted."
                                                                             : "Try signing in again, or retry later for a temporary error."),
                                               s));
}
struct identity
{
    std::string subject, label;
};
identity verify_identity(const std::string& jwt, const std::string& client, const std::string& nonce, const std::atomic_bool* flag)
{
    const auto a = jwt.find('.'), b = jwt.find('.', a == std::string::npos ? a : a + 1);
    if (a == std::string::npos || b == std::string::npos || jwt.find('.', b + 1) != std::string::npos)
        throw std::runtime_error("ChatGPT identity token is invalid.");
    const auto header_bytes = unurl(jwt.substr(0, a)), claims_bytes = unurl(jwt.substr(a + 1, b - a - 1)), signature = unurl(jwt.substr(b + 1));
    const auto header = json_reader({reinterpret_cast<const char*>(header_bytes.data()), header_bytes.size()}).parse();
    if (string_at(header, {"alg"}) != "RS256" || string_at(header, {"kid"}).empty())
        throw std::runtime_error("ChatGPT identity signature is not supported.");
    const auto reply = ai_http("https://auth.openai.com/.well-known/jwks.json", "GET", "", "", "", flag);
    if (reply.status != 200)
        throw std::runtime_error("ChatGPT identity verification is temporarily unavailable. Try again.");
    const auto keys = json_reader(reply.body).parse();
    const auto* list = json_at(keys, {"keys"});
    const json_value* selected = nullptr;
    if (list && list->kind == json_value::array)
        for (const auto& k : list->items)
            if (string_at(k, {"kid"}) == string_at(header, {"kid"}) && string_at(k, {"kty"}) == "RSA" &&
                (string_at(k, {"alg"}).empty() || string_at(k, {"alg"}) == "RS256") && (string_at(k, {"use"}).empty() || string_at(k, {"use"}) == "sig"))
                selected = &k;
    if (!selected)
        throw std::runtime_error("ChatGPT identity verification key is unavailable. Try again.");
    const auto modulus = unurl(string_at(*selected, {"n"})), exponent = unurl(string_at(*selected, {"e"}));
    if (modulus.size() < 256 || modulus.size() > 1024 || exponent.empty() || exponent.size() > 8 || signature.size() != modulus.size())
        throw std::runtime_error("ChatGPT identity verification key is invalid.");
    BCRYPT_RSAKEY_BLOB head{BCRYPT_RSAPUBLIC_MAGIC, ULONG(modulus.size() * 8), ULONG(exponent.size()), ULONG(modulus.size()), 0, 0};
    std::vector<uint8_t> blob(sizeof(head));
    memcpy(blob.data(), &head, sizeof(head));
    blob.insert(blob.end(), exponent.begin(), exponent.end());
    blob.insert(blob.end(), modulus.begin(), modulus.end());
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Windows RSA verification is unavailable.");
    const auto imported = BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), ULONG(blob.size()), 0);
    if (imported < 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("ChatGPT identity verification key is invalid.");
    }
    const auto hash = sha256(std::string_view(jwt).substr(0, b));
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    const auto valid = BCryptVerifySignature(key, &padding, const_cast<PUCHAR>(hash.data()), ULONG(hash.size()), const_cast<PUCHAR>(signature.data()),
                                             ULONG(signature.size()), BCRYPT_PAD_PKCS1);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (valid < 0)
        throw std::runtime_error("ChatGPT identity signature was not accepted. Sign in again.");
    const auto claims = json_reader({reinterpret_cast<const char*>(claims_bytes.data()), claims_bytes.size()}).parse();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    bool audience = string_at(claims, {"aud"}) == client;
    const auto* audiences = json_at(claims, {"aud"});
    if (audiences && audiences->kind == json_value::array)
    {
        audience = std::ranges::any_of(audiences->items, [&](const auto& v) { return v.kind == json_value::string && v.text == client; });
        if (audiences->items.size() > 1 && string_at(claims, {"azp"}) != client)
            audience = false;
    }
    const auto azp = string_at(claims, {"azp"});
    if (string_at(claims, {"iss"}) != issuer || !audience || (!azp.empty() && azp != client) || integer_at(claims, {"exp"}) <= now - 5 ||
        integer_at(claims, {"iat"}) > now + 5 || (!nonce.empty() && !same(string_at(claims, {"nonce"}), nonce)) || string_at(claims, {"sub"}).empty())
        throw std::runtime_error("ChatGPT identity could not be verified. Sign in again.");
    if (const auto* nbf = json_at(claims, {"nbf"}); nbf && integer_at(claims, {"nbf"}) > now + 5)
        throw std::runtime_error("ChatGPT identity is not valid yet.");
    identity id{string_at(claims, {"sub"}), string_at(claims, {"email"})};
    if (id.label.empty())
        id.label = string_at(claims, {"name"});
    if (id.label.empty())
        id.label = "ChatGPT account";
    for (auto& c : id.label)
        if (static_cast<unsigned char>(c) < 32)
            c = ' ';
    if (id.label.size() > 200)
        id.label.resize(200);
    return id;
}
struct socket_owner
{
    SOCKET value = INVALID_SOCKET;
    ~socket_owner()
    {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
};
struct winsock
{
    winsock()
    {
        WSADATA d{};
        if (WSAStartup(MAKEWORD(2, 2), &d))
            throw std::runtime_error("The sign-in callback listener is unavailable.");
    }
    ~winsock() { WSACleanup(); }
};
bool readable(SOCKET s, int ms)
{
    fd_set f;
    FD_ZERO(&f);
    FD_SET(s, &f);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = select(0, &f, nullptr, nullptr, &tv);
    if (r < 0)
        throw std::runtime_error("The sign-in callback listener failed.");
    return r > 0;
}
chatgpt_callback browser_callback(const std::string& client, const std::string& hint, std::string& redirect, const std::string& state, const std::string& nonce,
                                  const std::string& challenge, const std::atomic_bool* flag)
{
    winsock ws;
    socket_owner listener;
    listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener.value == INVALID_SOCKET)
        throw std::runtime_error("Could not open the sign-in callback listener.");
    BOOL exclusive = TRUE;
    setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(1455);
    if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        address.sin_port = 0;
        if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
            throw std::runtime_error("Could not bind the sign-in callback to 127.0.0.1.");
    }
    int len = sizeof(address);
    if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &len) || listen(listener.value, 4))
        throw std::runtime_error("Could not listen for ChatGPT sign-in.");
    redirect = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/auth/callback";
    std::map<std::string, std::string> fields{{"client_id", client.empty() ? "dynamic_agent_client" : client},
                                              {"response_type", "code"},
                                              {"redirect_uri", redirect},
                                              {"scope", "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"},
                                              {"resource", resource},
                                              {"state", state},
                                              {"nonce", nonce},
                                              {"code_challenge_method", "S256"},
                                              {"code_challenge", challenge},
                                              {"ext_agent_host_id", host_id()}};
    if (client.empty())
        fields["agent_name_hint"] = "Offcut";
    else if (!hint.empty())
        fields["id_token_hint"] = hint;
    const auto url = wide(std::string(issuer) + "/api/accounts/authorize?" + oauth_form(fields));
    cancelled(flag);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Could not open the browser for ChatGPT sign-in.");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        cancelled(flag);
        if (!readable(listener.value, 100))
            continue;
        socket_owner peer;
        peer.value = accept(listener.value, nullptr, nullptr);
        if (peer.value == INVALID_SOCKET)
            continue;
        u_long nonblock = 1;
        ioctlsocket(peer.value, FIONBIO, &nonblock);
        std::string request;
        const auto short_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192 && std::chrono::steady_clock::now() < short_deadline)
        {
            cancelled(flag);
            if (!readable(peer.value, 100))
                continue;
            std::array<char, 2048> bytes{};
            const int n = recv(peer.value, bytes.data(), int(bytes.size()), 0);
            if (n <= 0)
                break;
            request.append(bytes.data(), size_t(n));
        }
        bool accepted = false;
        chatgpt_callback callback;
        try
        {
            const auto end = request.find("\r\n");
            const auto first = request.substr(0, end);
            const auto space = first.find(' ', 4);
            if (!first.starts_with("GET ") || space == std::string::npos || request.find("\r\n\r\n") == std::string::npos)
                throw std::runtime_error("Invalid callback.");
            std::string headers = request.substr(end + 2);
            std::ranges::transform(headers, headers.begin(), [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
            const auto host = "host: 127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "\r\n";
            if (!headers.starts_with(host) && headers.find("\r\n" + host) == std::string::npos)
                throw std::runtime_error("Invalid callback host.");
            callback = parse_chatgpt_callback(std::string_view(first).substr(4, space - 4), state, client);
            accepted = true;
        }
        catch (const std::exception&)
        {
        }
        const std::string page = accepted ? "<!doctype html><meta charset=utf-8><title>Offcut</title><p>You can close this tab and return to Offcut.</p>"
                                          : "<!doctype html><title>Offcut</title><p>This callback was not accepted. Return to Offcut.</p>";
        const auto response = std::string("HTTP/1.1 ") + (accepted ? "200 OK" : "400 Bad Request") +
                              "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Security-Policy: default-src "
                              "'none'\r\nConnection: close\r\nContent-Length: " +
                              std::to_string(page.size()) + "\r\n\r\n" + page;
        send(peer.value, response.data(), int(response.size()), 0);
        if (accepted)
            return callback;
    }
    throw std::runtime_error("ChatGPT sign-in timed out. Try again.");
}
#endif
}  // namespace
chatgpt_credential_store native_chatgpt_credential_store()
{
#ifdef _WIN32
    return credential_store();
#else
    throw std::runtime_error("Credential Manager currently requires Windows.");
#endif
}
std::string oauth_form(const std::map<std::string, std::string>& fields)
{
    const auto escape = [](std::string_view s)
    {
        std::string out;
        constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c : s)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                out += char(c);
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        return out;
    };
    std::string out;
    for (const auto& [name, value] : fields)
    {
        if (!out.empty())
            out += '&';
        out += escape(name) + '=' + escape(value);
    }
    return out;
}
std::string pkce_s256(std::string_view verifier)
{
#ifdef _WIN32
    return url64(sha256(verifier));
#else
    (void)verifier;
    throw std::runtime_error("PKCE requires Windows CNG in this build.");
#endif
}
chatgpt_callback parse_chatgpt_callback(std::string_view target, std::string_view expected, std::string_view returning)
{
    const auto mark = target.find('?');
    if (mark == std::string_view::npos || target.substr(0, mark) != "/auth/callback" || target.find('#') != std::string_view::npos)
        throw std::runtime_error("Invalid ChatGPT callback path.");
    const auto decode = [](std::string_view text)
    {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '+')
                c = ' ';
            else if (c == '%')
            {
                if (i + 2 >= text.size())
                    throw std::runtime_error("Invalid callback encoding.");
                unsigned n = 0;
                for (int j = 0; j < 2; ++j)
                {
                    const char h = text[++i];
                    const int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (v < 0)
                        throw std::runtime_error("Invalid callback encoding.");
                    n = n * 16 + unsigned(v);
                }
                c = char(n);
            }
            if (static_cast<unsigned char>(c) < 32 || c == 127)
                throw std::runtime_error("Invalid callback value.");
            out += c;
        }
        return out;
    };
    std::map<std::string, std::string> values;
    auto query = target.substr(mark + 1);
    while (!query.empty())
    {
        const auto end = query.find('&');
        const auto pair = query.substr(0, end);
        const auto equal = pair.find('=');
        if (equal == std::string_view::npos || !values.emplace(decode(pair.substr(0, equal)), decode(pair.substr(equal + 1))).second)
            throw std::runtime_error("Duplicate or invalid callback parameter.");
        if (end == std::string_view::npos)
            break;
        query.remove_prefix(end + 1);
    }
    if (expected.empty() || !same(values["state"], expected))
        throw std::runtime_error("ChatGPT callback state did not match.");
    chatgpt_callback out{values["code"], values["client_id"], values["error"]};
    if (!out.error.empty())
        return out;
    if (out.code.empty())
        throw std::runtime_error("ChatGPT callback did not include a code.");
    if (!returning.empty())
    {
        if (out.client_id.empty())
            out.client_id = returning;
        else if (out.client_id != returning)
            throw std::runtime_error("ChatGPT callback changed the selected client.");
    }
    if (!issued(out.client_id))
        throw std::runtime_error("ChatGPT did not issue a client registration.");
    return out;
}
chatgpt_tokens parse_chatgpt_tokens(std::string_view text, bool refreshing)
{
    const auto root = json_reader(text).parse();
    chatgpt_tokens t{string_at(root, {"access_token"}), string_at(root, {"refresh_token"}), string_at(root, {"id_token"}), string_at(root, {"scope"}),
                     integer_at(root, {"expires_in"})};
    const auto type = string_at(root, {"token_type"});
    if ((type != "Bearer" && type != "bearer") || t.access_token.empty() || t.refresh_token.empty() || (!refreshing && t.id_token.empty()) ||
        t.expires_in <= 0 || t.expires_in > 86400 || !json_at(root, {"scope"}) || json_at(root, {"scope"})->kind != json_value::string)
        throw std::runtime_error("ChatGPT returned incomplete credentials. Sign in again.");
    return t;
}
bool chatgpt_terminal_refresh_error(std::string_view code)
{
    return code == "invalid_grant" || code == "invalid_refresh_token" || code == "token_expired" || code == "refresh_token_expired" ||
           code == "refresh_token_invalidated" || code == "refresh_token_reused";
}
std::vector<uint8_t> pack_chatgpt_session(std::string_view text)
{
#ifdef _WIN32
    if (text.size() > 128 * 1024)
        throw std::runtime_error("ChatGPT session is too large.");
    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &compressor))
        throw std::runtime_error("Windows session compression is unavailable.");
    SIZE_T size = 0;
    Compress(compressor, text.data(), text.size(), nullptr, 0, &size);
    std::vector<uint8_t> out(size + 5);
    memcpy(out.data(), "OCSI1", 5);
    const bool ok = Compress(compressor, text.data(), text.size(), out.data() + 5, size, &size);
    CloseCompressor(compressor);
    if (!ok)
        throw std::runtime_error("Could not encode the ChatGPT session.");
    out.resize(size + 5);
    return out;
#else
    return {text.begin(), text.end()};
#endif
}
std::string unpack_chatgpt_session(const std::vector<uint8_t>& bytes)
{
#ifdef _WIN32
    if (bytes.size() < 6 || memcmp(bytes.data(), "OCSI1", 5))
        throw std::runtime_error("The saved ChatGPT session is invalid. Sign in again.");
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &decompressor))
        throw std::runtime_error("Windows session decompression is unavailable.");
    SIZE_T size = 0;
    Decompress(decompressor, bytes.data() + 5, bytes.size() - 5, nullptr, 0, &size);
    if (!size || size > 128 * 1024)
    {
        CloseDecompressor(decompressor);
        throw std::runtime_error("The saved ChatGPT session is invalid.");
    }
    std::string out(size, 0);
    const bool ok = Decompress(decompressor, bytes.data() + 5, bytes.size() - 5, out.data(), out.size(), &size);
    CloseDecompressor(decompressor);
    if (!ok)
        throw std::runtime_error("The saved ChatGPT session is invalid.");
    out.resize(size);
    return out;
#else
    return {bytes.begin(), bytes.end()};
#endif
}
chatgpt_identity chatgpt_sign_in(const std::string& target, const std::atomic_bool* flag)
{
#ifdef _WIN32
    cancelled(flag);
    target_lock lock(target, flag);
    const auto old = read_session(target);
    auto registration = read_registration(target);
    if (registration.client.empty())
        registration = {old.client, old.subject};
    const auto verifier = random_value(48), state = random_value(), nonce = random_value();
    std::string redirect;
    const auto callback = browser_callback(registration.client, old.id_token, redirect, state, nonce, pkce_s256(verifier), flag);
    if (!callback.error.empty())
        throw std::runtime_error(callback.error == "access_denied" ? "ChatGPT sign-in was declined." : "ChatGPT sign-in did not complete. Try again.");
    registration.client = callback.client_id;
    save_registration(target, registration);
    const auto response = token_request({{"grant_type", "authorization_code"},
                                         {"client_id", callback.client_id},
                                         {"code", callback.code},
                                         {"code_verifier", verifier},
                                         {"redirect_uri", redirect},
                                         {"resource", resource}},
                                        flag);
    token_status(response, old, false);
    const auto tokens = parse_chatgpt_tokens(response.body);
    const auto id = verify_identity(tokens.id_token, callback.client_id, nonce, flag);
    if (!registration.subject.empty() && id.subject != registration.subject)
        throw std::runtime_error("ChatGPT returned a different account. Add another entry instead.");
    session s{tokens.refresh_token, callback.client_id, tokens.id_token, id.subject, id.label, tokens.scope};
    save_session(target, s);
    registration.subject = id.subject;
    save_registration(target, registration);
    cache_token(target, s, tokens);
    return {id.label};
#else
    (void)target;
    (void)flag;
    throw std::runtime_error("ChatGPT sign-in currently requires Windows.");
#endif
}
bool chatgpt_signed_in(const std::string& target)
{
#ifdef _WIN32
    try
    {
        return !read_session(target).refresh.empty();
    }
    catch (...)
    {
        return false;
    }
#else
    (void)target;
    return false;
#endif
}
std::string chatgpt_access_token(const std::string& target, const std::atomic_bool* flag)
{
#ifdef _WIN32
    cancelled(flag);
    target_lock lock(target, flag);
    auto s = read_session(target);
    if (s.refresh.empty())
        throw std::runtime_error("Sign in with ChatGPT again in Settings → AI providers.");
    if (!has_scope(s.scope, "chatgpt.tokens.use.direct"))
        throw std::runtime_error("ChatGPT plan usage was not enabled. Sign in again and allow Offcut to use your plan, or choose an API-key entry.");
    {
        std::lock_guard guard(cache_mutex);
        const auto found = cache.find(target);
        if (found != cache.end() && found->second.refresh == s.refresh && found->second.expires > std::chrono::steady_clock::now())
            return found->second.access;
    }
    const auto response = token_request({{"grant_type", "refresh_token"}, {"client_id", s.client}, {"refresh_token", s.refresh}, {"resource", resource}}, flag);
    if (response.status < 200 || response.status >= 300)
    {
        if (chatgpt_terminal_refresh_error(oauth_error(response)))
            forget(target);
        token_status(response, s, true);
    }
    const auto tokens = parse_chatgpt_tokens(response.body, true);
    s.refresh = tokens.refresh_token;
    s.scope = tokens.scope;
    // Persist rotation before any fallible identity network lookup. Keeping an
    // old refresh token after the provider rotated it would lose the session.
    save_session(target, s);
    if (!tokens.id_token.empty())
    {
        const auto id = verify_identity(tokens.id_token, s.client, "", flag);
        if (id.subject != s.subject)
        {
            forget(target);
            throw std::runtime_error("ChatGPT refreshed a different account. Sign in again.");
        }
        s.id_token = tokens.id_token;
        save_session(target, s);
    }
    if (!has_scope(s.scope, "chatgpt.tokens.use.direct"))
        throw std::runtime_error("ChatGPT plan usage is no longer enabled. Sign in again to allow it.");
    cache_token(target, s, tokens);
    return tokens.access_token;
#else
    (void)target;
    (void)flag;
    throw std::runtime_error("ChatGPT sign-in currently requires Windows.");
#endif
}
void chatgpt_sign_out(const std::string& target)
{
#ifdef _WIN32
    target_lock lock(target, nullptr);
    session s;
    bool damaged = false;
    try
    {
        s = read_session(target);
    }
    catch (...)
    {
        damaged = true;
    }
    bool revoked = s.refresh.empty() && !damaged;
    if (!s.client.empty())
        save_registration(target, {s.client, s.subject});
    if (!s.refresh.empty())
        try
        {
            const auto discovery = ai_http("https://auth.openai.com/.well-known/openid-configuration", "GET", "", "", "", nullptr);
            const auto data = json_reader(discovery.body).parse();
            const auto url = string_at(data, {"revocation_endpoint"});
            const auto parsed = split_ai_url(url);
            if (discovery.status == 200 && parsed.secure && parsed.host == "auth.openai.com" && parsed.port == 443)
            {
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    const auto reply = ai_http(url, "POST", oauth_form({{"token", s.refresh}, {"token_type_hint", "refresh_token"}, {"client_id", s.client}}),
                                               "application/x-www-form-urlencoded", "", nullptr);
                    if (reply.status == 200)
                    {
                        revoked = true;
                        break;
                    }
                    if (reply.status < 500)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        }
        catch (...)
        {
        }
    forget(target);
    if (!revoked)
        throw std::runtime_error("Signed out locally. Remote revocation was not confirmed; disconnect Offcut in ChatGPT Settings if needed.");
#else
    (void)target;
#endif
}
}  // namespace offcut
