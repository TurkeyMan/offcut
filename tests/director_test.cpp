#include <iostream>
#include "ai_json.h"
#include "ai_responses.h"
#include "chatgpt_auth_helpers.h"
#include "chatgpt_store.h"
#include "director_helpers.h"
#include "export.h"
#ifdef _WIN32
#include <windows.h>
#endif
namespace offcut
{
namespace
{
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
template <class F>
void bad(F f, const char* message)
{
    bool failed = false;
    try
    {
        f();
    }
    catch (const std::exception&)
    {
        failed = true;
    }
    check(failed, message);
}
std::string replace(std::string s, const std::string& from, const std::string& to)
{
    const auto at = s.find(from);
    check(at != std::string::npos, "Bad test fixture substitution");
    s.replace(at, from.size(), to);
    return s;
}
}  // namespace
void test_local_ops();
void test_director_protocol()
{
    test_local_ops();
    {
        director_input input;
        input.frame_width_mm = 200;
        input.frame_height_mm = 300;
        input.min_width_mm = 2;
        input.sheets = 4;
        input.border_widths_mm = {5, 15, 20, 30};
        input.instructions = "Keep the bank.";
        input.layer_gap_mm = 7.5;
        const auto values = director_prompt_values(input);
        check(values.at("frame") == "200 × 300 mm" && values.at("opening") == "150 × 280 mm", "Director opening geometry incorrect");
        check(values.at("borders") == "top 5 mm, bottom 15 mm, left 20 mm, right 30 mm", "Director per-side border values missing");
        const auto rendered = build_director_prompt(false, input);
        check(rendered.find("7.5 mm") != std::string::npos && rendered.find("150 × 280 mm") != std::string::npos,
              "Director brief was not filled with project geometry");
        for (const auto name : {"gap", "size", "opening", "borders", "frame", "sheets", "min_width", "min_width_px"})
            check(!rendered.contains("{" + std::string(name) + "}"), "Literal placeholder leaked from director brief");
        const auto builtin = load_prompt_library({});
        for (const auto name : {"director-plan.txt", "director-critique.txt"})
            check(!builtin.text(name).empty() && builtin.version(name).starts_with(std::string(name) + "@"),
                  "Director template missing compiled fallback or hash");
        const ai_image primary{"primary.png", {1}}, reference{"original.png", {2}}, overlay{"overlay.png", {3}};
        const auto planned = director_image_order(false, primary, {reference}, overlay), critiqued = director_image_order(true, primary, {reference}, overlay);
        check(planned[0].png == primary.png && planned[1].png == reference.png && planned[2].png == overlay.png, "Plan image order incorrect");
        check(critiqued[0].png == primary.png && critiqued[1].png == overlay.png && critiqued[2].png == reference.png, "Critique image order incorrect");
        check(
            fill_prompt("Opening: {opening}\nBorders: {borders}\nNotes: {notes}\nInstructions: {instructions?}\nOptional label: {notes?}\n{notes?}", values) ==
                "Opening: 150 × 280 mm\nBorders: top 5 mm, bottom 15 mm, left 20 mm, right 30 mm\nInstructions: Keep the bank.\nOptional label: ",
            "Director optional placeholder semantics differ from fill_prompt");
        input.border_widths_mm = {10, 10, 10, 10};
        check(director_prompt_values(input).at("borders") == "10 mm wide on every side", "Uniform director borders differ from prompt_values");
        input.frame_width_mm = 0;
        check(director_prompt_values(input).at("opening").empty(), "Unknown director geometry invented an opening");
    }
    {
        const std::string target = "Offcut/AI/fixture";
        std::map<std::string, credential_bytes> values;
        std::vector<std::string> events;
        int fail_write = -1, writes = 0;
        bool fail_cleanup = false;
        chatgpt_credential_store store{[&](const std::string& key) -> std::optional<credential_bytes>
                                       {
                                           const auto it = values.find(key);
                                           return it == values.end() ? std::optional<credential_bytes>{} : it->second;
                                       },
                                       [&](const std::string& key, const credential_bytes& data)
                                       {
                                           events.push_back("write:" + key);
                                           if (writes++ == fail_write)
                                               throw std::runtime_error("fixture write failure");
                                           check(data.size() <= 2560, "Oversize credential chunk");
                                           values[key] = data;
                                       },
                                       [&](const std::string& key)
                                       {
                                           events.push_back("erase:" + key);
                                           if (fail_cleanup && key.ends_with("#2"))
                                               throw std::runtime_error("fixture cleanup failure");
                                           values.erase(key);
                                       },
                                       [&](const std::string& prefix)
                                       {
                                           std::vector<std::string> out;
                                           for (const auto& [key, data] : values)
                                           {
                                               (void)data;
                                               if (key.starts_with(prefix))
                                                   out.push_back(key);
                                           }
                                           return out;
                                       }};
        const credential_bytes old{'O', 'C', 'S', 'I', '1', 1, 2, 3};
        values[target] = old;
        check(read_chatgpt_blob(target, store) == old, "Legacy single-credential session rejected");
        credential_bytes large(12000);
        uint32_t random = 0x12345678;
        for (auto& c : large)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            c = uint8_t(random);
        }
        write_chatgpt_blob(target, large, store);
        check(read_chatgpt_blob(target, store) == large && values.size() > 2, "Oversize session did not round trip through chunks");
        const auto baseline = values;
        // Every numbered write and the final root commit can fail. Until the
        // commit succeeds, the old root and old chunks must remain usable.
        for (int failure = 0; failure < 6; ++failure)
        {
            values = baseline;
            events.clear();
            writes = 0;
            fail_write = failure;
            bad([&] { write_chatgpt_blob(target, credential_bytes(14000, 42), store); }, "Injected chunk failure was not reported");
            check(read_chatgpt_blob(target, store) == large && values == baseline, "Failed write damaged the previous session");
        }
        fail_write = -1;
        writes = 0;
        values = baseline;
        events.clear();
        const credential_bytes changed(15000, 27);
        write_chatgpt_blob(target, changed, store);
        check(read_chatgpt_blob(target, store) == changed, "Chunked session replacement failed");
        const auto commit = std::ranges::find(events, "write:" + target);
        const auto erase = std::ranges::find_if(events, [](const auto& event) { return event.starts_with("erase:"); });
        check(commit != events.end() && erase != events.end() && commit < erase, "Old chunks removed before the root commit");
        const auto committed = values;
        values = baseline;
        bool replaced = false;
        auto racing = store;
        racing.read = [&](const std::string& key) -> std::optional<credential_bytes>
        {
            if (!replaced && key != target)
            {
                replaced = true;
                write_chatgpt_blob(target, changed, store);
            }
            return store.read(key);
        };
        check(read_chatgpt_blob(target, racing) == changed, "Reader did not recover from a concurrent generation commit");
        values = committed;
        const auto part = std::ranges::find_if(values, [&](const auto& v) { return v.first != target; });
        const auto part_name = part->first;
        values.erase(part_name);
        bad([&] { read_chatgpt_blob(target, store); }, "Missing session chunk accepted");
        values = committed;
        values[part_name][0] ^= 1;
        bad([&] { read_chatgpt_blob(target, store); }, "Corrupt session chunk accepted");
        values = baseline;
        fail_cleanup = true;
        write_chatgpt_blob(target, changed, store);
        fail_cleanup = false;
        check(read_chatgpt_blob(target, store) == changed, "Post-commit cleanup error broke the new session");
        write_chatgpt_blob(target, old, store);
        check(values.size() == 1 && read_chatgpt_blob(target, store) == old, "Shrinking session left stale chunks");
        write_chatgpt_blob(target, large, store);
        values[target + "#99"] = {1};
        values[target + "#2-other"] = {2};
        values["Offcut/AI/other#2"] = {3};
        clear_chatgpt_blobs(target, store);
        check(values.size() == 2 && values.contains(target + "#2-other") && values.contains("Offcut/AI/other#2"),
              "Sign-out missed chunks or erased another target");
        check(!read_chatgpt_blob(target, store), "Signed-out session still readable");
        values.clear();
        write_chatgpt_blob(target, large, store);
        values[target + "#99"] = {1};
        values.erase(target);
        clear_chatgpt_blobs(target, store);
        check(values.empty(), "Orphan-only session chunks survived sign-out");
        values[target] = old;
        values[target + "#2"] = {1};
        values[target + "#3"] = {2};
        fail_cleanup = true;
        bad([&] { clear_chatgpt_blobs(target, store); }, "Deletion failure was hidden");
        fail_cleanup = false;
        check(values.size() == 1 && values.contains(target + "#2"), "Sign-out stopped before trying every chunk");
        clear_chatgpt_blobs(target, store);
        check(is_chatgpt_chunk_target(target + "#2") && !is_chatgpt_chunk_target(target + "#02") && !is_chatgpt_chunk_target(target + "#2-other"),
              "Account recovery chunk filter failed");
        std::string random_text;
        for (const auto c : large)
            random_text += "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[c & 63];
        const auto packed = pack_chatgpt_session(random_text);
        check(packed.size() > 2560, "Chunk fixture compressed below credential limit");
        write_chatgpt_blob(target, packed, store);
        check(unpack_chatgpt_session(*read_chatgpt_blob(target, store)) == random_text, "Large compressed session did not round trip");
        clear_chatgpt_blobs(target, store);
#ifdef _WIN32
        const auto native = native_chatgpt_credential_store();
        const auto fixture = "Offcut/AI/offcut-chatgpt-chunks-test-" + std::to_string(GetCurrentProcessId());
        try
        {
            write_chatgpt_blob(fixture, packed, native);
            check(unpack_chatgpt_session(*read_chatgpt_blob(fixture, native)) == random_text, "Native Credential Manager chunk round trip failed");
            check(native.list(fixture + '#').size() > 1, "Native oversized session was not split");
            check(saved_ai_key_targets(fixture) == std::vector<std::string>{fixture}, "Native account recovery exposed storage chunks");
            write_chatgpt_blob(fixture, old, native);
            check(read_chatgpt_blob(fixture, native) == old && native.list(fixture + '#').empty(), "Native session shrink failed");
            write_chatgpt_blob(fixture, packed, native);
            clear_chatgpt_blobs(fixture, native);
            check(!native.read(fixture) && native.list(fixture + '#').empty(), "Native chunk cleanup failed");
        }
        catch (...)
        {
            try
            {
                clear_chatgpt_blobs(fixture, native);
            }
            catch (...)
            {
            }
            throw;
        }
#endif
        std::cout
            << "Passed: oversized credential chunks, legacy reads, atomic replacement/failure rollback, corruption detection, shrink and sign-out cleanup.\n";
    }
    check(pkce_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "RFC7636 S256 vector failed");
    check(oauth_form({{"a", "hello +/&"}}) == "a=hello%20%2B%2F%26", "OAuth form encoding failed");
    const auto callback = parse_chatgpt_callback("/auth/callback?state=state&code=a%2Bb&client_id=oaiapp_test", "state");
    check(callback.code == "a+b" && callback.client_id == "oaiapp_test", "New client callback parsing failed");
    check(parse_chatgpt_callback("/auth/callback?state=s&code=x", "s", "oaiapp_old").client_id == "oaiapp_old", "Returning callback lost client");
    check(parse_chatgpt_callback("/auth/callback?state=s&error=access_denied", "s").error == "access_denied", "Consent refusal not parsed");
    bad([] { parse_chatgpt_callback("/auth/callback?state=wrong&error=access_denied", "s"); }, "Refusal bypassed state validation");
    bad([] { parse_chatgpt_callback("/callback?state=s&code=x&client_id=oaiapp_x", "s"); }, "Wrong callback path accepted");
    bad([] { parse_chatgpt_callback("/auth/callback?state=s&state=s&code=x&client_id=oaiapp_x", "s"); }, "Duplicate callback state accepted");
    bad([] { parse_chatgpt_callback("/auth/callback?state=s&code=x&client_id=dynamic_agent_client", "s"); }, "Dynamic client was stored as issued ID");
    bad([] { parse_chatgpt_callback("/auth/callback?state=s&code=x&client_id=oaiapp_other", "s", "oaiapp_old"); }, "Callback replaced selected client");
    bad([] { parse_chatgpt_callback("/auth/callback?state=s&code=%00&client_id=oaiapp_x", "s"); }, "Encoded callback control accepted");
    const std::string token =
        R"({"access_token":"fixture-access","refresh_token":"fixture-refresh","id_token":"fixture-id","scope":"openid offline_access chatgpt.tokens.use.direct","token_type":"Bearer","expires_in":3600})";
    check(parse_chatgpt_tokens(token).expires_in == 3600, "Initial token parsing failed");
    check(parse_chatgpt_tokens(replace(token, "fixture-id", ""), true).refresh_token == "fixture-refresh", "Refresh response parsing failed");
    bad([&] { parse_chatgpt_tokens(replace(token, "fixture-id", "")); }, "Missing initial ID token accepted");
    bad([&] { parse_chatgpt_tokens(replace(token, "3600", "0")); }, "Zero token lifetime accepted");
    bad([&] { parse_chatgpt_tokens(replace(token, "fixture-refresh", ""), true); }, "Rotating refresh token omitted");
    check(chatgpt_terminal_refresh_error("invalid_grant") && chatgpt_terminal_refresh_error("refresh_token_reused") &&
              !chatgpt_terminal_refresh_error("server_error"),
          "Refresh error classification failed");
    check(unpack_chatgpt_session(pack_chatgpt_session(token)) == token, "Single-blob session codec changed data");
    bad([] { unpack_chatgpt_session({0, 1, 2, 3}); }, "Invalid session blob accepted");
    const auto models = chatgpt_models_from_json(
        R"({"models":[{"slug":"first","visibility":"list"},{"slug":"hidden","visibility":"hidden"},{"slug":"second","visibility":"list"}]})");
    check(models == std::vector<std::string>{"first", "second"}, "ChatGPT catalog order/visibility failed");
    const std::string answer = R"({"explain":"good"})";
    const std::string completed =
        "event: response.completed\r\ndata: "
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
        "\"text\":" +
        ai_json_quote(answer) + "}]}],\"usage\":{\"input_tokens\":12,\"output_tokens\":9}}}\r\n\r\n";
    responses_sse stream;
    const auto events =
        ": heartbeat\r\n\r\nevent: response.output_text.delta\r\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"ignored partial\"}\r\n\r\n" +
        completed;
    for (char c : events)
        stream.feed(std::string_view(&c, 1));
    const auto reply = stream.finish();
    check(reply.text == answer && reply.input_tokens == 12 && reply.output_tokens == 9, "Split-chunk SSE completion/usage failed");
    bad(
        []
        {
            responses_sse stream;
            stream.feed("data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"subscription_sharing_usage_limit_exceeded\"}}}\n\n");
        },
        "SSE failed event treated as success");
    bad(
        []
        {
            responses_sse stream;
            stream.feed("data: [DONE]\n\n");
            stream.finish();
        },
        "DONE substituted for response.completed");
    bad(
        []
        {
            responses_sse stream;
            stream.feed("data: {\"type\":\"response.incomplete\"}\n\n");
        },
        "Incomplete SSE accepted");
    check(responses_error_message("subscription_sharing_usage_limit_exceeded").find("Settings → Usage") != std::string::npos,
          "Usage limit recovery message missing");
    for (const auto code : {"subscription_sharing_user_not_eligible", "subscription_sharing_usage_unavailable", "subscription_sharing_unsupported_capability",
                            "subscription_sharing_route_not_supported", "subscription_sharing_invalid_user", "subscription_sharing_user_unavailable",
                            "chatpass_v2_scope_not_authorized"})
        check(!responses_error_message(code).empty(), "Sharing error mapping missing");
    const std::string plan =
        R"({"plan_version":1,"route":"generate","summary":"A subject standing on a bank.","style":{"notes":"paper"},"sheets":[{"id":"s1","depth":1,"card":"#123456","role":"front","holds":"frame and subject"},{"id":"s2","depth":2,"card":"#eeeeee","role":"backing","holds":"sky"}],"features":[{"id":"outline","label":"line work","source":"reference","keep":"drop","evidence":["m1"]},{"id":"subject","label":"subject","source":"reference","keep":"keep","arrangement":{"kind":"sheet","sheet":"s1","through":[]},"support":{"by":"frame","with":"bottom","contact_mm":4},"extends_behind":[],"hidden_support":false,"evidence":[]}],"scene_directions":["A subject standing on a bank that reaches both sides."],"avoid":["loose spots"],"local":{"sheet_count":2,"palette":[{"colour":"#123456","sheet":"s1"},{"colour":"#eeeeee","sheet":"s2"}],"ops":[]},"splits":[],"risks":[]})";
    const auto compiled = validate_director_plan(plan, 4);
    bad([&] { validate_director_plan(replace(plan, "\"plan_version\":1", "\"plan_version\":1,\"unexpected\":true"), 4); }, "Unknown plan property accepted");
    check(compiled.planes.size() == 2 && compiled.scene_directions.size() == 1 && compiled.palette[0][0] == 0x12, "Plan did not compile to ai_plan");
    bad([&] { validate_director_plan(plan, 1); }, "Too many sheets accepted");
    bad([&] { validate_director_plan(replace(plan, "#123456", "invalid"), 4); }, "Invalid plan colour accepted");
    bad([&] { validate_director_plan(replace(plan, "\"sheet\":\"s1\"", "\"sheet\":\"unknown\""), 4); }, "Unknown sheet accepted");
    bad([&] { validate_director_plan(replace(plan, "\"depth\":2", "\"depth\":1"), 4); }, "Duplicate depth accepted");
    bad([&] { validate_director_plan(replace(plan, "\"sheet_count\":2", "\"sheet_count\":3"), 4); }, "Mismatched local sheet count accepted");
    const std::string verdict =
        R"({"verdict":"pass","explain":"Every visible piece is supported.","local_ops":[],"refine":{"mode":"edit","instruction":"","marks":[]},"plan_patch":[]})";
    check(validate_director_verdict(verdict) == "Every visible piece is supported.", "Valid verdict rejected");
    const auto no_refine = replace(verdict, R"("refine":{"mode":"edit","instruction":"","marks":[]})", R"("refine":null)");
    check(validate_director_verdict(no_refine) == "Every visible piece is supported.", "Nullable refine rejected for pass");
    bad([&] { validate_director_verdict(replace(no_refine, "\"pass\"", "\"refine\"")); }, "Refine verdict accepted null instruction");
    const auto schema = detail::json_reader(director_verdict_schema()).parse();
    check(detail::json_at(schema, {"properties", "refine", "anyOf", "1", "type"})->text == "null", "Refine schema is not nullable");
    bad([&] { validate_director_verdict(replace(verdict, "\"pass\"", "\"refine\"")); }, "Refine accepted without instruction");
    bad([&] { validate_director_verdict(replace(verdict, "\"pass\"", "\"fix_local\"")); }, "Local-fix accepted without operations");
    bad([&] { validate_director_verdict(replace(verdict, "\"pass\"", "\"unknown\"")); }, "Unknown verdict accepted");
    detail::json_reader(director_plan_schema()).parse();
    detail::json_reader(director_verdict_schema()).parse();
    reasoning_request request;
    request.chatgpt = true;
    request.model = "test-model";
    request.instructions = "Director instructions";
    request.input = "User instructions";
    request.schema = director_plan_schema();
    raster white{1, 1, {255, 255, 255, 255}};
    request.images = {{"test.png", encode_png(white)}};
    const auto body = responses_request_json(request);
    const auto json = detail::json_reader(body).parse();
    check(detail::json_at(json, {"store"})->text == "false" && detail::json_at(json, {"stream"})->text == "true" &&
              ai_json_string(body, {"input", "0", "role"}) == "user" &&
              ai_json_string(body, {"input", "0", "content", "1", "image_url"})->starts_with("data:image/png;base64,"),
          "Responses request shape failed");
    for (const auto field : {"temperature", "max_output_tokens", "previous_response_id", "metadata", "user"})
        check(!detail::json_at(json, {field}), "Unsupported ChatGPT parameter emitted");
    request.base_url = "http://127.0.0.1:1234/v1";
    bad([&] { responses_request_json(request); }, "ChatGPT token could be routed to a custom server");
    std::cout << "Passed: PKCE, callback/state, refresh/session codec, ChatGPT catalog, split SSE/errors, plan/verdict validation and Responses request "
                 "constraints.\n";
}
}  // namespace offcut
