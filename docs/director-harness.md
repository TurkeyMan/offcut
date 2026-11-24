# Native director harness

Build without replacing a running copy:

```powershell
cmake -S D:\Code\offcut -B D:\Code\offcut\build\codex-director -A x64
cmake --build D:\Code\offcut\build\codex-director --config Debug --parallel 6
```

From `build\codex-director\bin\Debug`:

```powershell
.\offcut.exe --chatgpt-sign-in personal-chatgpt
.\offcut.exe --director plan personal-chatgpt D:\images\subject.png D:\runs\plan-01 "Keep the subject on a broad bank"
.\offcut.exe --director critique personal-chatgpt D:\images\drawing.png D:\runs\critique-01 D:\runs\plan-01\plan.json
```

Sign-in opens the system browser. The user signs in and approves the grant;
Offcut waits on 127.0.0.1 and never receives their password. A new entry is
saved in ai-accounts.txt. `default` selects the "Plans and reviews with" entry,
not the image-model default. Plan and critique commands explicitly send the
composition/reference/issue PNGs and measurements to that entry's model.

Each run writes plan.json or verdict.json and summary.txt. A fresh
director-request-NN folder retains prompts, input PNGs, request JSON, reply,
usage, and any repair turn or error. Director output is validated locally;
there is at most one JSON repair turn. These commands do not apply suggested
local operations or generate a drawing.

ChatGPT and OpenAI API-key entries use Responses. Compatible local/custom and
Pollinations entries use Chat Completions. Anthropic Messages is not implemented
in this mechanical slice; selecting that protocol returns a clear error.

ChatGPT always uses the public OpenAI endpoint, store:false and stream:true;
success requires response.completed. Inference cannot silently switch billing
routes. Only visible model slugs from the selected account are listed.

Sign-in uses PKCE S256, state and OIDC nonce. RSA/SHA-256 verification checks the
ID-token signature, issuer, audience, expiry, issue time and nonce. Returning
sign-in checks the saved subject, and access requires the granted direct-plan
scope. The nonsecret installation UUID and issued client/subject mapping live
beside settings and survive sign-out; tokens never enter those files.

Renewable sessions are compressed and stored entirely in generic Windows
Credential Manager credentials under Offcut/AI/<entry-id>, followed by numbered
chunks (#2, #3, …) when needed. The root holds the first payload bytes and a
length/hash manifest. New chunks are written to vacant numbered slots first;
the root is replaced atomically only after every new chunk is saved. Old chunks
are removed after that commit. A failed write keeps the previous session usable.
Old single-credential sessions remain readable; shrinking a session reclaims
extra chunks. Sign-out deletes the root and all of its numbered chunks, including
orphans left by an interrupted write. Numbered chunks are excluded from provider
account recovery. No token data spills into settings or files.

The payload holds refresh token, issued client ID, retained ID token, verified
identity and granted scopes. Access tokens are cached only in memory. Refreshes
are serialized across processes with a per-target Windows mutex; each rotated
refresh token is saved before further use. Each credential remains within the
Windows 2560-byte blob limit; larger sessions use more credentials.

Sign-out discovers the revocation endpoint, attempts remote revocation, clears
the local session, and reports if remote revocation was not confirmed. Temporary
request/refresh failures preserve the session; terminal refresh errors clear
tokens while retaining the registration for sign-in again.

Offline tests cover PKCE, callback/state parsing, refresh responses, session
encoding, catalog filtering, split SSE, error mapping, and plan/verdict schemas.
Real browser sign-in, token rotation/revocation and subscriber inference remain
unverified until the user runs them. Tests must not use real credentials.

Protocol references:
- https://developers.openai.com/siwc/token-sharing-open-source/sign-in
- https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions
- https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference
- https://developers.openai.com/siwc/token-sharing-open-source/errors-and-recovery
