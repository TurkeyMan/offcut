#include "local_ops.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include "ai_json.h"
#include "director_helpers.h"
#include "transform.h"
namespace offcut
{
namespace
{
void check(bool ok, const char* message)
{
    if (!ok)
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
local_ops_input fixture()
{
    local_ops_input in;
    in.options.width_mm = 64;
    in.options.height_mm = 64;
    in.options.border_mm = 4;
    in.options.layers = 3;
    in.options.smoothing = 0;
    in.options.connect = false;
    in.options.bridge_mm = 1;
    in.palette = {{{20, 30, 40}}, {{100, 120, 140}}, {{230, 240, 250}}};
    in.image = {64, 64, std::vector<uint8_t>(64 * 64 * 4, 255)};
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const auto c = in.palette[x < 4 || y < 4 || x >= 60 || y >= 60 ? 0 : 2];
            std::copy(c.begin(), c.end(), in.image.rgba.begin() + (y * 64 + x) * 4);
        }
    return in;
}
void box(local_ops_input& in, int x0, int y0, int x1, int y1, int tone)
{
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            std::copy(in.palette[size_t(tone)].begin(), in.palette[size_t(tone)].end(), in.image.rgba.begin() + (y * in.image.width + x) * 4);
}
int tone(const local_ops_input& in, const raster& image, int x, int y)
{
    const size_t p = size_t(y * image.width + x) * 4;
    for (size_t i = 0; i < in.palette.size(); ++i)
        if (std::equal(in.palette[i].begin(), in.palette[i].end(), image.rgba.begin() + p))
            return int(i);
    throw std::runtime_error("Local op emitted a colour outside the palette");
}
tone_map map(const local_ops_input& in, const raster& image)
{
    const auto g = dimensions(in.options);
    tone_map out{g.w, g.h, std::vector<uint8_t>(size_t(g.w) * g.h)};
    const double scale = std::max(double(g.w) / image.width, double(g.h) / image.height), left = (image.width - g.w / scale) / 2,
                 top = (image.height - g.h / scale) / 2;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            out.indices[size_t(y) * g.w + x] = uint8_t(tone(in, image, int(std::floor(left + (x + .5) / scale)), int(std::floor(top + (y + .5) / scale))));
    return out;
}
void lock(const local_ops_input& in, const local_ops_result& result)
{
    const auto tones = map(in, result.image);
    const auto sheets = build_sheets(tones, in.options);
    check(composite_mismatch(sheets.layers, tones) == 0, "Local edit broke the visible-picture lock");
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (x < 4 || y < 4 || x >= 60 || y >= 60)
                check(tone(in, result.image, x, y) == 0, "Local operation recoloured reserved frame");
    size_t changed = 0;
    for (size_t p = 0; p < in.image.rgba.size(); p += 4)
        changed += !std::equal(in.image.rgba.begin() + p, in.image.rgba.begin() + p + 4, result.image.rgba.begin() + p);
    check(changed == result.changed_pixels, "Changed pixel count is not final-image difference");
}
std::string measure(local_ops_input& in, int sheet, const std::string& kind = "floating")
{
    const auto sheets = build_sheets(map(in, in.image), in.options);
    auto v = validate_sheets(sheets.layers, in.options, sheets.removed);
    v.tones_found = 3;
    v.tones_wanted = 3;
    in.measurements = measurements_json(v, in.options);
    const auto root = detail::json_reader(in.measurements).parse();
    for (const auto& f : detail::json_at(root, {"findings"})->items)
        if (detail::json_at(f, {"kind"})->text == kind && detail::json_at(f, {"sheet"})->text == std::to_string(sheet))
            return detail::json_at(f, {"id"})->text;
    throw std::runtime_error("Fixture did not produce expected measurement");
}
std::string score_json(int pixels, int thin, int found = 3, int wanted = 3)
{
    return "{\"floating\":" + std::to_string(pixels ? 1 : 0) + ",\"floating_pixels\":" + std::to_string(pixels) + ",\"thin\":" + std::to_string(thin) +
           ",\"tones\":{\"found\":" + std::to_string(found) + ",\"wanted\":" + std::to_string(wanted) + "}}";
}
}  // namespace
void test_local_ops()
{
    {
        auto in = fixture();
        box(in, 20, 20, 26, 26, 0);
        box(in, 26, 20, 60, 26, 1);
        const auto original = in.image;
        in.image = {128, 64, std::vector<uint8_t>(128 * 64 * 4, 255)};
        box(in, 0, 0, 128, 64, 2);
        for (int y = 0; y < 64; ++y)
            std::copy_n(original.rgba.begin() + y * 64 * 4, 64 * 4, in.image.rgba.begin() + (y * 128 + 32) * 4);
        const auto id = measure(in, 1);
        in.ops = "[{\"op\":\"push_back\",\"target\":\"" + id + "\"}]";
        const auto out = apply_local_ops(in);
        check(out.applied.size() == 1 && out.changed_pixels == 36 && tone(in, out.image, 54, 22) == 1,
              "Aspect-mismatched picture did not use centre-cover measurement geometry");
        const auto tones = map(in, out.image);
        check(composite_mismatch(build_sheets(tones, in.options).layers, tones) == 0, "Cover-cropped edit broke composite lock");
        check(tone(in, out.image, 1, 22) == 2, "An edit changed pixels outside the frame's cover crop");
    }
    {
        auto in = fixture();
        box(in, 20, 20, 26, 26, 0);
        box(in, 26, 20, 60, 26, 1);
        const auto id = measure(in, 1);
        in.ops = "[{\"op\":\"push_back\",\"target\":\"" + id + "\"}]";
        const auto out = apply_local_ops(in);
        check(out.applied.size() == 1 && out.skipped.empty() && out.changed_pixels == 36, "Floating piece push-back failed");
        check(tone(in, out.image, 22, 22) == 1, "Push-back skipped nearest supported deeper sheet");
        const auto sheets = build_sheets(map(in, out.image), in.options);
        check(disconnected_components(sheets.layers[1]) == 0, "Pushed-back piece still floats");
        lock(in, out);
        in.ops = "[{\"op\":\"show_through\",\"target\":\"" + id + "\",\"sheet\":\"s3\"}]";
        const auto hole = apply_local_ops(in);
        check(tone(in, hole.image, 22, 22) == 2 && hole.changed_pixels == 36, "Show-through did not open a backing-colour hole");
        lock(in, hole);
        in.ops = "[{\"op\":\"push_back\",\"target\":\"" + id + "\"},{\"op\":\"show_through\",\"target\":\"" + id + "\"}]";
        const auto twice = apply_local_ops(in);
        check(twice.applied.size() == 2 && twice.changed_pixels == 36 && tone(in, twice.image, 22, 22) == 2,
              "Sequential measurement target lost its original piece");
        lock(in, twice);
    }
    {
        auto in = fixture();
        box(in, 20, 20, 26, 26, 0);
        in.ops = R"([{"op":"push_back","target":"s1"}])";
        const auto out = apply_local_ops(in);
        check(out.applied.size() == 1 && tone(in, out.image, 22, 22) == 2, "Unsupported middle sheet should be bypassed for backing");
        lock(in, out);
        in.ops = R"([{"op":"push_back","target":"s1","sheet":"s2"}])";
        const auto blocked = apply_local_ops(in);
        check(blocked.applied.empty() && blocked.skipped.size() == 1 && blocked.changed_pixels == 0, "Explicit unsupported destination was accepted");
    }
    {
        auto in = fixture();
        box(in, 10, 10, 16, 16, 1);
        box(in, 30, 30, 36, 36, 1);
        for (const auto op : {"merge_colours", "map_colour"})
        {
            in.ops = "[{\"op\":\"" + std::string(op) + "\",\"target\":\"#64788c\",\"colour\":\"#e6f0fa\"}]";
            const auto out = apply_local_ops(in);
            check(out.changed_pixels == 72 && tone(in, out.image, 12, 12) == 2 && tone(in, out.image, 32, 32) == 2,
                  "Colour mapping did not replace every matching interior pixel");
            lock(in, out);
        }
        box(in, 10, 10, 16, 16, 0);
        const auto id = measure(in, 1);
        in.ops = "[{\"op\":\"remove\",\"target\":\"" + id + "\"},{\"op\":\"remove\",\"target\":\"" + id + "\"}]";
        const auto out = apply_local_ops(in);
        check(out.changed_pixels == 36 && out.applied.size() == 1 && out.skipped.size() == 1 && tone(in, out.image, 12, 12) == 2,
              "Remove did not use surrounding majority or allowed deleted m-id reuse");
        lock(in, out);
    }
    {
        auto in = fixture();
        box(in, 32, 15, 33, 60, 0);
        box(in, 26, 8, 39, 18, 0);
        in.options.bridge_mm = 4;
        const auto id = measure(in, 1, "thin");
        in.ops = "[{\"op\":\"thicken\",\"target\":\"" + id + "\",\"width_mm\":4}]";
        const auto out = apply_local_ops(in);
        int width = 0;
        for (int x = 4; x < 60; ++x)
            width += tone(in, out.image, x, 35) == 0;
        check(width == 4 && out.changed_pixels > 0, "One-pixel stalk did not grow to requested minimum width");
        check(tone(in, out.image, 25, 10) == 2 && tone(in, out.image, 39, 10) == 2, "Thicken grew an already wide feature");
        lock(in, out);
        const auto repeat = apply_local_ops(in);
        check(out.image.rgba == repeat.image.rgba, "Local operation is not deterministic");
        // Nearer card cannot be painted over while thickening a deeper sheet.
        in = fixture();
        box(in, 32, 15, 33, 60, 1);
        box(in, 33, 20, 34, 50, 0);
        in.ops = R"([{"op":"thicken","target":"s2","width_mm":4}])";
        const auto safe = apply_local_ops(in);
        check(tone(in, safe.image, 33, 35) == 0, "Thicken erased nearer card");
        lock(in, safe);
    }
    {
        auto in = fixture();
        in.ops =
            R"([{"op":"set_sheet_count","count":4},{"op":"set_card","sheet":"s1","colour":"#010203"},{"op":"extend_to_frame","target":"m1"},{"op":"extract","target":"s1"},{"op":"remove","target":"m999"},{"op":"unknown"},{"op":"show_through","target":"s99"},null])";
        const auto out = apply_local_ops(in);
        check(out.applied.empty() && out.skipped.size() == 8 && out.changed_pixels == 0, "Invalid/UI ops were not safely skipped");
        for (const auto& reason : out.skipped)
            check(!reason.empty(), "Missing skip reason");
        in.ops = "bad json";
        bad([&] { apply_local_ops(in); }, "Malformed operations JSON accepted");
        in.ops = "[]";
        in.image.rgba[7] = 0;
        bad([&] { apply_local_ops(in); }, "Nonopaque raster accepted");
    }
    {
        const std::string plan =
            R"({"plan_version":1,"route":"generate","summary":"A subject standing on a bank.","style":{"notes":"paper"},"sheets":[{"id":"s1","depth":1,"card":"#123456","role":"front","holds":"frame and subject"},{"id":"s2","depth":2,"card":"#eeeeee","role":"backing","holds":"sky"}],"features":[{"id":"outline","label":"line work","source":"reference","keep":"drop","evidence":["m1"]},{"id":"subject","label":"subject","source":"reference","keep":"keep","arrangement":{"kind":"sheet","sheet":"s1","through":[]},"support":{"by":"frame","with":"bottom","contact_mm":4},"extends_behind":[],"hidden_support":false,"evidence":[]}],"scene_directions":["A subject on a bank."],"avoid":["loose spots"],"local":{"sheet_count":2,"palette":[{"colour":"#123456","sheet":"s1"},{"colour":"#eeeeee","sheet":"s2"}],"ops":[]},"splits":[],"risks":[]})";
        const auto changed = apply_plan_patch(
            plan,
            R"([{"op":"test","path":"/plan_version","value":"1.0"},{"op":"replace","path":"/summary","value":"New bank"},{"op":"add","path":"/avoid/-","value":"Floating birds"},{"op":"remove","path":"/avoid/0"},{"op":"copy","from":"/summary","path":"/style/notes"},{"op":"move","from":"/avoid/0","path":"/avoid/-"},{"op":"replace","path":"/features/1/support/contact_mm","value":"5"}])");
        const auto root = detail::json_reader(changed).parse();
        check(detail::json_at(root, {"summary"})->text == "New bank" && detail::json_at(root, {"style", "notes"})->text == "New bank" &&
                  detail::json_at(root, {"avoid", "0"})->text == "Floating birds" &&
                  detail::json_at(root, {"features", "1", "support", "contact_mm"})->text == "5",
              "JSON Patch did not apply all operations");
        const auto object_patch = R"([{"op":"replace","path":"/style","value":"{\"notes\":\"flat\"}"}])";
        check(detail::json_at(detail::json_reader(apply_plan_patch(plan, object_patch)).parse(), {"style", "notes"})->text == "flat",
              "String-encoded object patch rejected");
        for (const auto patch : {R"([{"op":"test","path":"/plan_version","value":"2"}])", R"([{"op":"remove","path":"/sheets/0"}])",
                                 R"([{"op":"replace","path":"/local/sheet_count","value":"3"}])", R"([{"op":"replace","path":"/missing","value":"x"}])",
                                 R"([{"op":"move","from":"/style","path":"/style/notes/x"}])", R"([{"op":"add","path":"/avoid/01","value":"x"}])",
                                 R"([{"op":"remove","path":"/style~2notes"}])"})
            bad([&] { apply_plan_patch(plan, patch); }, "Invalid patch/result accepted");
        check(!apply_plan_patch(plan, "[]").empty(), "Empty patch failed");
    }
    {
        check(auto_round_score(score_json(1, 4194303, 127, 1)) < auto_round_score(score_json(2, 0)), "Thin/tone penalties outweighed unsupported area");
        check(auto_round_score(score_json(0, 1, 127, 1)) < auto_round_score(score_json(0, 2)), "Tone surplus outweighed a thin feature");
        check(auto_round_score(score_json(0, 0)) == 0, "Clean score was not zero");
        check(auto_round_score(score_json(0, 0, 2, 3)) == 0, "Tone deficit was counted as surplus");
        for (const auto json : {"{}", "oops", R"({"floating":0,"thin":0,"tones":{"found":0,"wanted":3}})",
                                R"({"floating":2,"thin":0,"tones":{"found":3,"wanted":3},"findings":[{"kind":"floating","area_mm2":1}]})",
                                R"({"floating":0,"floating_pixels":5,"thin":0,"tones":{"found":3,"wanted":3}})"})
            check(std::isinf(auto_round_score(json)), "Unknown/inconsistent score did not rank worst");
        const auto legacy = auto_round_score(
            R"({"floating":1,"thin":0,"tones":{"found":3,"wanted":3},"frame_mm":[64,64],"canvas_px":[640,640],"findings":[{"kind":"floating","area_mm2":1.2}]})");
        check(legacy == auto_round_score(score_json(120, 0)), "Legacy measurement area not converted to pixel units");
    }
    {
        transform_options options;
        options.width_mm = 333;
        options.height_mm = 200;
        validation findings;
        findings.tones_found = findings.tones_wanted = 3;
        findings.floating.assign(205, piece_note{0, 100, 100, 1});
        findings.floating.front().pixels = 40000;
        findings.floating.back().pixels = 2;
        findings.thin = {{0, 50, 50, 7}, {1, 70, 70, 13}};
        const auto serialized = measurements_json(findings, options);
        auto root = detail::json_reader(serialized).parse();
        check(detail::json_at(root, {"findings"})->items.size() == 200 && detail::json_at(root, {"floating"})->text == "205" &&
                  detail::json_at(root, {"thin"})->text == "2",
              "Aggregate fields changed legacy finding counts or cap");
        check(detail::json_at(root, {"floating_pixels"})->text == "40205" && detail::json_at(root, {"thin_pixels"})->text == "20",
              "Aggregate totals did not include pieces beyond capped findings");
        const double scale = working_scale(options), area = 40205. / (scale * scale);
        check(std::stod(detail::json_at(root, {"floating_area_mm2"})->text) == area, "Floating area was rounded instead of preserving double precision");
        check(auto_round_score(serialized) == auto_round_score(score_json(40205, 2)), "Capped findings did not rank by exact aggregate pixels");
        ++findings.floating.back().pixels;
        check(auto_round_score(serialized) < auto_round_score(measurements_json(findings, options)), "One-pixel change beyond finding cap was lost from score");
        root.members.erase("floating_pixels");
        check(auto_round_score(detail::json_dump(root, "")) == auto_round_score(score_json(40205, 2)), "Unrounded area aggregate fallback failed");
        findings.floating.clear();
        findings.thin.clear();
        root = detail::json_reader(measurements_json(findings, options)).parse();
        check(detail::json_at(root, {"floating_pixels"})->text == "0" && detail::json_at(root, {"thin_pixels"})->text == "0" &&
                  std::stod(detail::json_at(root, {"floating_area_mm2"})->text) == 0 && auto_round_score(detail::json_dump(root, "")) == 0,
              "Clean measurements omitted zero aggregates or scored nonzero");
    }
    std::cout
        << "Passed: local raster operations, measurement targeting, composite lock, plan patches, exact measurement aggregates and round-score ordering.\n";
}
}  // namespace offcut
