#pragma once
// Sign in with ChatGPT (OpenAI, preview): a ChatGPT Plus/Pro subscription
// used for reasoning requests through the Responses API. It can plan and
// review (images in, text out); it cannot draw (no image generation).
//
// Interface shared by the mechanical implementation (chatgpt_auth.cpp) and
// the Settings UI (ai_providers_page.cpp). Keep these signatures stable.
#include <atomic>
#include <string>
namespace offcut
{
struct chatgpt_identity
{
    std::string label;  // what the provider list shows: the account's email or name
};
// The browser sign-in: OAuth 2.0 authorization code with PKCE (S256) through
// a 127.0.0.1 loopback redirect, dynamic client registration the first time.
// Opens the system browser, waits for the callback (cancellable), exchanges
// the code, and stores the session (refresh token, issued client id, id
// token) in Windows Credential Manager under `target`. Throws with a
// user-readable message on refusal, timeout or error; ai_cancelled on cancel.
// Never logs or returns tokens.
chatgpt_identity chatgpt_sign_in(const std::string& target, const std::atomic_bool* cancel);
// Revokes the refresh token at OpenAI (best effort) and deletes the stored
// session.
void chatgpt_sign_out(const std::string& target);
// True when a session is stored under `target`.
bool chatgpt_signed_in(const std::string& target);
// A current access token for `target`, refreshing (and storing the rotated
// refresh token) when needed. Refreshes are serialised per target. Throws
// "sign in again" style errors when the session is gone or revoked.
std::string chatgpt_access_token(const std::string& target, const std::atomic_bool* cancel);
}  // namespace offcut
