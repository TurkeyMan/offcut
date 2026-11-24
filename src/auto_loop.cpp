#include "auto_loop.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <set>
#include "ai_json.h"
#include "director_helpers.h"
#include "export.h"
#include "image_io.h"
#include "versions.h"
namespace offcut
{
using detail::json_reader;
using detail::json_value;
namespace
{
std::string now_iso()
{
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}
std::vector<uint8_t> upload_png(const raster& r)
{
    constexpr int limit = 1024;
    if (std::max(r.width, r.height) <= limit)
        return encode_png(r);
    const double k = double(limit) / std::max(r.width, r.height);
    return encode_png(fit_raster(r, std::max(1, int(std::lround(r.width * k))), std::max(1, int(std::lround(r.height * k)))));
}
std::optional<json_value> parse(const std::string& json)
{
    try
    {
        return json_reader(json).parse();
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}
std::string str(const json_value* n)
{
    return n && n->kind == json_value::string ? n->text : std::string();
}
std::string dump(const json_value* n)
{
    return n ? detail::json_dump(*n, {}) : std::string("[]");
}
std::string lower(std::string s)
{
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
void add_usage(item_version::role_usage& to, const item_version::role_usage& from)
{
    to.seconds += from.seconds;
    to.input_tokens += from.input_tokens;
    to.output_tokens += from.output_tokens;
}
item_version::role_usage usage_of(const director_reply& r)
{
    return {r.seconds, r.input_tokens, r.output_tokens};
}
layer& item_of(project& p, uint32_t id)
{
    const int i = p.find_layer(id);
    if (i < 0)
        throw std::runtime_error("The Auto item is no longer in the project.");
    return p.layers[size_t(i)];
}
// Check: the version becomes current, the sheets are rebuilt from it
// (snapped to its palette), and validation, measurements and score are
// recorded on it.
validation check(project& p, uint32_t item, uint32_t id, const auto_roles& roles)
{
    auto& l = item_of(p, item);
    if (!set_current_version(l, id))
        throw std::runtime_error("The Auto version is gone.");
    const auto found = rebuild_sheets(p);
    auto* v = find_version(item_of(p, item), id);
    v->issues = found.count();
    v->findings = describe_findings(found, p.options);
    v->measurements = measurements_json(found, p.options);
    v->score = roles.score ? roles.score(v->measurements) : auto_round_score(v->measurements);
    p.selected_layer = p.find_layer(item);
    return found;
}
void report(const auto_roles& roles, const auto_run& run, const std::string& step, const project* snapshot = nullptr)
{
    if (!roles.progress)
        return;
    auto_progress a;
    a.step = step;
    a.round = run.drawings;
    a.rounds = run.settings.rounds;
    a.plan = run.plan;
    if (snapshot)
        a.snapshot = std::make_shared<project>(*snapshot);
    roles.progress(a);
}
void cancelled(const std::atomic_bool* cancel)
{
    if (cancel && cancel->load())
        throw ai_cancelled();
}
director_input base_input(const project& p, const auto_run& run)
{
    director_input in;
    in.style = p.ai_style;
    in.notes = ai_notes(p);
    in.instructions = run.settings.instructions;
    in.sheets = p.options.layers;
    in.frame_width_mm = p.options.width_mm;
    in.frame_height_mm = p.options.height_mm;
    in.min_width_mm = p.options.bridge_mm;
    in.border_widths_mm = borders(p.options);
    in.layer_gap_mm = p.options.layer_gap_mm;
    return in;
}
struct verdict_parts
{
    std::string kind, explain, ops = "[]", instruction, mode, patch = "[]";
    std::vector<std::string> marks;
};
verdict_parts read_verdict(const std::string& json)
{
    verdict_parts out;
    const auto root = parse(json);
    if (!root)
        return out;
    out.kind = str(detail::json_at(*root, {"verdict"}));
    out.explain = str(detail::json_at(*root, {"explain"}));
    if (const auto* ops = detail::json_at(*root, {"local_ops"}); ops && ops->kind == json_value::array)
        out.ops = dump(ops);
    if (const auto* patch = detail::json_at(*root, {"plan_patch"}); patch && patch->kind == json_value::array)
        out.patch = dump(patch);
    if (const auto* r = detail::json_at(*root, {"refine"}); r && r->kind == json_value::object)
    {
        out.instruction = str(detail::json_at(*r, {"instruction"}));
        out.mode = str(detail::json_at(*r, {"mode"}));
        if (const auto* m = detail::json_at(*r, {"marks"}); m && m->kind == json_value::array)
            for (const auto& x : m->items)
                if (x.kind == json_value::string)
                    out.marks.push_back(x.text);
    }
    return out;
}
}  // namespace
raster snapped_picture(const std::vector<cut_layer>& cuts, const std::vector<std::array<uint8_t, 3>>& palette)
{
    if (cuts.empty() || palette.size() != cuts.size())
        throw std::runtime_error("The sheets don't match the plan's palette.");
    raster out{cuts.front().width, cuts.front().height, {}};
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (size_t i = 0; i < size_t(out.width) * out.height; ++i)
    {
        size_t sheet = cuts.size() - 1;  // nothing solid: the backing shows
        for (size_t s = 0; s < cuts.size(); ++s)
            if (cuts[s].solid.size() > i && cuts[s].solid[i])
            {
                sheet = s;
                break;
            }
        const auto& c = palette[sheet];
        out.rgba[i * 4] = c[0], out.rgba[i * 4 + 1] = c[1], out.rgba[i * 4 + 2] = c[2], out.rgba[i * 4 + 3] = 255;
    }
    return out;
}
namespace
{
// Applies ops to the last version as a new "Local fix" version in the same
// round, then checks it. False when nothing applied.
bool local_fix(project& p, auto_run& run, const auto_roles& roles, const std::string& ops, const std::string& why)
{
    auto& l = item_of(p, run.item);
    const auto* v = find_version(l, run.last);
    if (!v || ops == "[]" || ops.empty())
        return false;
    local_ops_input in;
    in.palette = parse_palette(v->palette);
    // The ops work on the picture as the import sees it: snapped to the
    // palette and cover-cropped at the frame's working size. The sheets just
    // built from this version reproduce exactly that, front to back (the
    // visible-picture lock), so it is composited from them, and the
    // measurements' ids refer to the same geometry.
    set_current_version(l, run.last);
    rebuild_sheets(p);
    in.image = snapped_picture(p.cuts, in.palette);
    v = find_version(item_of(p, run.item), run.last);
    in.options = p.options;
    if (!in.palette.empty())
        in.options.layers = int(in.palette.size());
    in.measurements = v->measurements;
    // apply_local_ops edits pixels; four ops are Offcut's own: set_card
    // recolours a sheet here, the rest are said, not done.
    std::string pixel_ops = "[";
    std::vector<std::pair<int, std::array<uint8_t, 3>>> cards;
    std::vector<std::string> own_applied, own_skipped;
    if (const auto root = parse(ops); root && root->kind == json_value::array)
    {
        std::vector<std::string> sheet_ids;
        if (const auto plan = parse(run.plan))
            if (const auto* s = detail::json_at(*plan, {"sheets"}); s && s->kind == json_value::array)
                for (const auto& x : s->items)
                    sheet_ids.push_back(str(detail::json_at(x, {"id"})));
        for (const auto& op : root->items)
        {
            const auto name = str(detail::json_at(op, {"op"}));
            if (name == "set_card")
            {
                const auto sheet = str(detail::json_at(op, {"sheet"}));
                const auto colour = parse_palette(str(detail::json_at(op, {"colour"})));
                const auto at = std::ranges::find(sheet_ids, sheet);
                if (colour.size() == 1 && at != sheet_ids.end() && size_t(at - sheet_ids.begin()) < in.palette.size())
                {
                    cards.push_back({int(at - sheet_ids.begin()), colour.front()});
                    own_applied.push_back(std::format("Card of {} set to {}", sheet, palette_text(colour)));
                }
                else
                    own_skipped.push_back("set_card: unknown sheet or colour");
            }
            else if (name == "set_sheet_count")
            {
                const auto* count = detail::json_at(op, {"count"});
                const int n = count ? std::atoi(count->text.c_str()) : 0;
                own_skipped.push_back(std::format("set_sheet_count {}: proposed, not applied (it needs a new plan)", n));
                if (n > 0 && n < int(in.palette.size()) && run.sheets_note.empty())
                    run.sheets_note = std::format("The director proposes {} sheets; set Sheets to {} and run Auto again to use it.", n, n);
            }
            else if (name == "extend_to_frame")
                own_skipped.push_back("extend_to_frame " + str(detail::json_at(op, {"target"})) + ": left for you (Arrange ▸ Touch an edge)");
            else if (name == "extract")
                own_skipped.push_back("extract " + str(detail::json_at(op, {"target"})) + ": left for you (Extract to item is a step in the app)");
            else
                pixel_ops += (pixel_ops.size() > 1 ? "," : "") + detail::json_dump(op, {});
        }
    }
    pixel_ops += "]";
    in.ops = pixel_ops;
    auto result = pixel_ops == "[]" ? local_ops_result{in.image, {}, {}, 0} : roles.local_ops ? roles.local_ops(in) : apply_local_ops(in);
    auto palette = in.palette;
    for (const auto& [sheet, colour] : cards)
    {
        const auto was = palette[size_t(sheet)];
        for (size_t i = 0; i + 3 < result.image.rgba.size(); i += 4)
            if (result.image.rgba[i] == was[0] && result.image.rgba[i + 1] == was[1] && result.image.rgba[i + 2] == was[2])
                result.image.rgba[i] = colour[0], result.image.rgba[i + 1] = colour[1], result.image.rgba[i + 2] = colour[2];
        palette[size_t(sheet)] = colour;
    }
    result.applied.insert(result.applied.end(), own_applied.begin(), own_applied.end());
    result.skipped.insert(result.skipped.end(), own_skipped.begin(), own_skipped.end());
    std::string record = "{\"ops\":" + ops + ",\"applied\":[";
    for (size_t i = 0; i < result.applied.size(); ++i)
        record += (i ? "," : "") + ai_json_quote(result.applied[i]);
    record += "],\"skipped\":[";
    for (size_t i = 0; i < result.skipped.size(); ++i)
        record += (i ? "," : "") + ai_json_quote(result.skipped[i]);
    record += "]}";
    std::string left_for_you;
    for (const auto& x : own_skipped)
        if (x.contains("left for you"))
            left_for_you += (left_for_you.empty() ? "Left for you: " : "; ") + x;
    if (result.applied.empty() || !result.image.valid())
    {
        // Recorded on the version judged, so the thread says what was tried.
        find_version(l, run.last)->ops = record;
        if (!left_for_you.empty())
            run.left_for_you = left_for_you;
        return false;
    }
    item_version fixed;
    fixed.parent = v->id;
    fixed.image = result.image;
    fixed.task = "Local fix";
    fixed.palette = palette_text(palette);
    fixed.plan = run.plan;
    fixed.run = run.id;
    fixed.round = v->round;
    fixed.style = v->style;
    fixed.notes = v->notes;
    fixed.created = now_iso();
    fixed.ops = record;
    std::string applied;
    for (const auto& a : result.applied)
        applied += (applied.empty() ? "" : "; ") + a;
    fixed.explanation = (why.empty() ? applied : why) + (left_for_you.empty() ? "" : " " + left_for_you + ".");
    fixed.findings = applied;
    const auto id = add_version(l, std::move(fixed));
    check(p, run.item, id, roles);
    run.last = id;
    ++run.round_fixes;
    return true;
}
// The end of a round (its drawing, critique and fixes): the stop rules.
void end_round(project& p, auto_run& run)
{
    const auto& l = item_of(p, run.item);
    double round_best = std::numeric_limits<double>::infinity();
    for (const auto& v : l.versions)
        if (v.run == run.id && int(v.round) == run.drawings && v.score >= 0)
            round_best = std::min(round_best, v.score);
    if (round_best < run.round_start_best - 1e-9)
        run.stale_rounds = 0;
    else
        ++run.stale_rounds;
    run.round_start_best = std::min(run.round_start_best, round_best);
    if (run.drawings >= run.settings.rounds)
        run.stop = auto_stop::rounds;
    else if (run.stale_rounds >= 2)
        run.stop = auto_stop::no_improvement;
    else if (run.settings.until == auto_until::ask_each_round)
        run.stop = auto_stop::asking;
}
void present(project& p, auto_run& run)
{
    if (!run.item || p.find_layer(run.item) < 0)
        return;
    auto& l = item_of(p, run.item);
    const uint32_t best = best_auto_version(l, run.id);
    if (!best)
        return;
    run.best = best;
    uint32_t before = 0;
    bool first = true;
    for (auto& v : l.versions)
        if (v.run == run.id)
        {
            if (first)
                before = v.previous, first = false;
            if (v.id != best && v.state == review_state::pending)
            {
                v.state = review_state::answered;
                if (v.review.empty())
                    v.review = std::format("Auto: round {}, not chosen", v.round);
            }
        }
    auto* v = find_version(l, best);
    v->state = review_state::pending;
    v->previous = before;
    if (!run.sheets_note.empty() && !v->explanation.contains(run.sheets_note))
        v->explanation += (v->explanation.empty() ? "" : " ") + run.sheets_note;
    if (!run.left_for_you.empty() && !v->explanation.contains(run.left_for_you))
        v->explanation += (v->explanation.empty() ? "" : " ") + run.left_for_you + ".";
    set_current_version(l, best);
    rebuild_sheets(p);
    p.selected_layer = p.find_layer(run.item);
    p.step = work_step::colours;
    p.view = view_mode::assembled;
}
}  // namespace

std::string_view auto_until_name(auto_until u)
{
    return u == auto_until::good ? "Auto until good" : u == auto_until::ask_each_round ? "Ask after each round" : "Plan only";
}
std::string_view auto_stop_text(auto_stop s)
{
    switch (s)
    {
        case auto_stop::none:
            return "";
        case auto_stop::passed:
            return "the director passed it";
        case auto_stop::rounds:
            return "all rounds used";
        case auto_stop::no_improvement:
            return "no better for two rounds";
        case auto_stop::cancelled:
            return "cancelled";
        case auto_stop::failed:
            return "stopped";
        case auto_stop::plan_only:
            return "plan ready";
        case auto_stop::asking:
            return "round done";
        case auto_stop::arranged:
            return "arranged";
        case auto_stop::no_drawing:
            return "needs a drawing provider";
    }
    return "";
}

std::string plan_route(const std::string& plan_json)
{
    const auto root = parse(plan_json);
    return root ? str(detail::json_at(*root, {"route"})) : std::string();
}
std::string sheet_reduction_reason(const std::string& plan_json, int project_sheets)
{
    const auto root = parse(plan_json);
    if (!root)
        return {};
    const auto* sheets = detail::json_at(*root, {"sheets"});
    if (!sheets || sheets->kind != json_value::array || sheets->items.empty() || int(sheets->items.size()) >= project_sheets)
        return {};
    std::vector<std::string> texts;
    for (const auto* r = detail::json_at(*root, {"risks"}); r && r->kind == json_value::array;)
    {
        for (const auto& x : r->items)
            if (x.kind == json_value::string)
                texts.push_back(x.text);
        break;
    }
    texts.push_back(str(detail::json_at(*root, {"summary"})));
    // Confident: it names the sheets (planes, layers, colours) and gives a
    // reason for fewer: a limit, a trade, a simplification or the count.
    for (const auto& t : texts)
    {
        const auto s = lower(t);
        const bool about = std::ranges::any_of(std::array{"sheet", "plane", "layer", "colour", "color", "card"}, [&](const char* k) { return s.contains(k); });
        const bool reason = std::ranges::any_of(std::array{"fewer", "reduc", "only ", "limit", "enough", "simpl", "instead of", "trade", "merge", "drop"},
                                                [&](const char* k) { return s.contains(k); });
        if (about && reason && !t.empty())
            return t;
    }
    return {};
}
int auto_errors(const std::string& measurements_json)
{
    const auto root = parse(measurements_json);
    if (!root)
        return 0;
    const auto* f = detail::json_at(*root, {"floating"});
    return f && f->kind == json_value::scalar ? std::atoi(f->text.c_str()) : 0;
}
uint32_t best_auto_version(const layer& l, const std::string& run)
{
    const item_version* best = nullptr;
    const auto passed = [](const item_version& v) { return read_verdict(v.verdict).kind == "pass"; };
    for (const auto& v : l.versions)
    {
        if (v.run != run || v.score < 0)
            continue;
        if (!best || v.score < best->score - 1e-9)
            best = &v;
        else if (std::abs(v.score - best->score) <= 1e-9)
        {
            const bool a = passed(v), b = passed(*best);
            if (a != b ? a : v.round >= best->round)
                best = &v;
        }
    }
    return best ? best->id : 0;
}
std::string describe_marks(const std::string& measurements_json, const std::vector<std::string>& marks)
{
    const auto root = parse(measurements_json);
    if (!root || marks.empty())
        return {};
    double fw = 0, fh = 0;
    if (const auto* f = detail::json_at(*root, {"frame_mm"}); f && f->kind == json_value::array && f->items.size() == 2)
        fw = std::atof(f->items[0].text.c_str()), fh = std::atof(f->items[1].text.c_str());
    std::map<std::string, int> counts;  // "small floating piece near the lower left" → how many
    std::vector<std::string> order;
    const auto* findings = detail::json_at(*root, {"findings"});
    if (!findings || findings->kind != json_value::array)
        return {};
    const std::set<std::string> wanted(marks.begin(), marks.end());
    for (const auto& f : findings->items)
    {
        if (!wanted.contains(str(detail::json_at(f, {"id"}))))
            continue;
        const auto kind = str(detail::json_at(f, {"kind"}));
        double x = 0, y = 0, area = 0;
        if (const auto* at = detail::json_at(f, {"at_mm"}); at && at->kind == json_value::array && at->items.size() == 2)
            x = std::atof(at->items[0].text.c_str()), y = std::atof(at->items[1].text.c_str());
        if (const auto* a = detail::json_at(f, {"area_mm2"}))
            area = std::atof(a->text.c_str());
        std::string where;
        if (fw > 0 && fh > 0)
        {
            const auto v = y < fh / 3 ? "upper" : y > fh * 2 / 3 ? "lower" : "middle";
            const auto h = x < fw / 3 ? "left" : x > fw * 2 / 3 ? "right" : "centre";
            where = std::string(v) == "middle" && std::string(h) == "centre" ? "the centre" : std::format("the {} {}", v, h);
            if (std::string(v) == "middle" && std::string(h) != "centre")
                where = std::format("the {} side", h);
        }
        const auto what = kind == "thin" ? std::string("a thin part") : std::string(area < 10 ? "a small loose piece" : "a loose piece");
        const auto phrase = where.empty() ? what : what + " near " + where;
        if (!counts[phrase]++)
            order.push_back(phrase);
    }
    std::string out;
    for (const auto& phrase : order)
    {
        auto shown = phrase;
        if (counts[phrase] > 1)
        {
            shown = std::format("{} {}", counts[phrase], phrase.substr(phrase.find(' ') + 1));
            // "3 small loose pieces near the lower left"
            const auto at = shown.find(" near ");
            const auto noun_end = at == std::string::npos ? shown.size() : at;
            shown.insert(noun_end, "s");
        }
        out += (out.empty() ? "" : "; ") + shown;
    }
    return out;
}
std::string auto_ceiling(const auto_settings& s, bool can_draw)
{
    if (s.until == auto_until::plan_only)
        return "One reasoning call, about 1–2 min. Nothing is drawn.";
    if (!can_draw)
        return "Up to 2 reasoning calls, about 1–3 min. Nothing is drawn: Offcut arranges the picture itself.";
    const int rounds = std::clamp(s.rounds, 1, 5);
    // From the first real runs: a plan about 90 s, a drawing about 50 s, a critique about 16 s.
    const int low = std::max(1, int(std::floor((90 + 50 + 16) / 60.)));
    const int high = int(std::ceil((90 + rounds * (50 + 2 * 16)) / 60.));
    return std::format("Up to {} {}, {} reasoning calls, about {}–{} min.", rounds, rounds == 1 ? "drawing" : "drawings", 1 + 2 * rounds, low, high);
}
std::string plan_text(const std::string& plan_json)
{
    const auto root = parse(plan_json);
    if (!root)
        return plan_json;
    std::string out = str(detail::json_at(*root, {"summary"})) + "\n\nSheets, near to far:\n";
    if (const auto* s = detail::json_at(*root, {"sheets"}); s && s->kind == json_value::array)
        for (const auto& x : s->items)
            out += std::format("- {} ({}, {}): {}\n", str(detail::json_at(x, {"id"})), str(detail::json_at(x, {"role"})), str(detail::json_at(x, {"card"})),
                               str(detail::json_at(x, {"holds"})));
    out += "\nFeatures:\n";
    if (const auto* f = detail::json_at(*root, {"features"}); f && f->kind == json_value::array)
        for (const auto& x : f->items)
        {
            out += std::format("- {}: {}", str(detail::json_at(x, {"label"})), str(detail::json_at(x, {"keep"})));
            if (const auto* a = detail::json_at(x, {"arrangement"}); a && a->kind == json_value::object)
                out += std::format(", {} {}", str(detail::json_at(*a, {"kind"})), str(detail::json_at(*a, {"sheet"})));
            if (const auto* sup = detail::json_at(x, {"support"}); sup && sup->kind == json_value::object)
                out += std::format(", held by {}", str(detail::json_at(*sup, {"by"})));
            if (const auto simple = str(detail::json_at(x, {"simplify_to"})); !simple.empty())
                out += ": " + simple;
            out += "\n";
        }
    const auto list = [&](const char* title, const char* key)
    {
        if (const auto* a = detail::json_at(*root, {key}); a && a->kind == json_value::array && !a->items.empty())
        {
            out += std::string("\n") + title + ":\n";
            for (const auto& x : a->items)
                out += "- " + x.text + "\n";
        }
    };
    list("Scene directions", "scene_directions");
    list("Avoid", "avoid");
    list("Risks", "risks");
    return out;
}
raster auto_overlay(const std::vector<cut_layer>& cuts, const validation& v)
{
    auto image = render_cuts(cuts, {.floating = true});
    if (!image.valid())
        return image;
    // Tiny bitmap numerals: the measurement ids (m1, m2, …), floating first, then thin.
    constexpr unsigned digits[10][7] = {{14, 17, 17, 17, 17, 17, 14}, {4, 6, 4, 4, 4, 4, 14},      {14, 17, 16, 8, 4, 2, 31},  {15, 16, 16, 14, 16, 16, 15},
                                        {8, 12, 10, 9, 31, 8, 8},     {31, 1, 1, 15, 16, 16, 15},  {14, 1, 1, 15, 17, 17, 14}, {31, 16, 8, 4, 2, 2, 2},
                                        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 30, 16, 16, 14}};
    int id = 0;
    const auto paint = [&](const std::vector<piece_note>& pieces, std::array<uint8_t, 3> colour)
    {
        for (const auto& piece : pieces)
        {
            if (++id > 200)
                return;
            const auto label = std::to_string(id);
            const int width = int(label.size()) * 12 + 4, height = 18;
            const int ox = std::clamp(int(piece.x), 0, std::max(0, image.width - width)), oy = std::clamp(int(piece.y), 0, std::max(0, image.height - height));
            for (int y = 0; y < height && oy + y < image.height; ++y)
                for (int x = 0; x < width && ox + x < image.width; ++x)
                {
                    auto* pixel = image.rgba.data() + (size_t(oy + y) * image.width + ox + x) * 4;
                    const int digit = (x - 2) / 12, col = (x - 2) % 12 / 2, row = (y - 2) / 2;
                    const bool ink =
                        x >= 2 && y >= 2 && row < 7 && col < 5 && digit < int(label.size()) && ((digits[label[size_t(digit)] - '0'][row] >> col) & 1);
                    for (int c = 0; c < 3; ++c)
                        pixel[c] = ink ? 255 : colour[size_t(c)];
                    pixel[3] = 255;
                }
        }
    };
    paint(v.floating, {185, 30, 35});
    paint(v.thin, {165, 82, 0});
    return image;
}
const ai_account* drawing_account(const ai_accounts& accounts)
{
    const auto draws = [](const ai_account& a)
    {
        try
        {
            return find_ai_provider(a.provider).can_draw && !a.model.empty();
        }
        catch (const std::exception&)
        {
            return false;
        }
    };
    if (const auto* d = accounts.find(accounts.default_id); d && draws(*d))
        return d;
    for (const auto& a : accounts.entries)
        if (draws(a))
            return &a;
    return nullptr;
}
const ai_account* planning_account(const ai_accounts& accounts)
{
    const auto* a = accounts.plans_account();
    if (!a || a->reasoning_model.empty())
        return nullptr;
    try
    {
        return find_ai_provider(a->provider).can_plan ? a : nullptr;
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}
auto_route auto_route_for(const ai_accounts& accounts)
{
    const bool plans = planning_account(accounts) != nullptr, draws = drawing_account(accounts) != nullptr;
    return plans && draws ? auto_route::loop : plans ? auto_route::plan_and_arrange : draws ? auto_route::template_draw : auto_route::local;
}
director_endpoint director_endpoint_for(const ai_account& a)
{
    return {a.provider, a.base_url, a.reasoning_model, a.credential_target};
}

auto_run resume_auto_from(const project& p, uint32_t item, uint32_t version, const std::string& review)
{
    const int i = p.find_layer(item);
    if (i < 0)
        throw std::runtime_error("That item is no longer in the project.");
    const auto& l = p.layers[size_t(i)];
    const auto* v = find_version(l, version);
    if (!v || v->plan.empty())
        throw std::runtime_error("This version has no plan to review against.");
    auto_run run;
    run.id = v->run;
    run.item = item;
    run.last = version;
    run.plan = v->plan;
    run.compiled = validate_director_plan(v->plan, 32);
    for (const auto& x : l.versions)
        if (x.run == run.id)
        {
            run.drawings = std::max(run.drawings, int(x.round));
            if (x.score >= 0)
                run.round_start_best = std::min(run.round_start_best, x.score);
        }
    run.drawings = std::max(run.drawings, int(v->round));
    // A review resumes the loop for one round.
    run.settings.rounds = std::min(run.drawings + 1, 99);
    run.next = auto_run::step::critique;
    run.review = review;
    run.round_fixes = 0;
    if (!v->request.empty())
        run.folder = from_utf8(v->request).parent_path();
    return run;
}

void finish_auto(project& p, auto_run& run)
{
    if (run.stop == auto_stop::asking || run.stop == auto_stop::none)
        run.stop = auto_stop::rounds;
    present(p, run);
}

void run_auto(project& p, auto_run& run, const auto_roles& roles, const std::atomic_bool* cancel)
{
    if (!roles.plan || !roles.critique)
        throw std::runtime_error("Auto with AI needs a 'Plans and reviews with' entry.");
    run.settings.rounds = std::clamp(run.settings.rounds, 1, 99);
    run.stop = auto_stop::none;
    run.message.clear();
    if (run.folder.empty())
        throw std::runtime_error("Auto needs a folder for its requests.");
    std::filesystem::create_directories(run.folder);
    const auto can_draw = bool(roles.draw) && roles.illustrator.has_value();
    // Draw (or refine): the illustrator, then Check.
    const auto draw = [&](bool refine)
    {
        if (!can_draw)
        {
            run.stop = auto_stop::no_drawing;
            run.message = "The director asked for a drawing, and no provider draws. Choose one under Settings → AI providers → Draws with.";
            return;
        }
        report(roles, run, refine ? std::format("Refining {} of {}: {}", run.drawings + 1, run.settings.rounds, run.instruction)
                                  : std::format("Drawing {} of {}…", run.drawings + 1, run.settings.rounds));
        prepared_ai_upload u;
        if (refine)
        {
            u = prepare_refinement(p, run.item, run.last, ai_task::refine, run.instruction);
            u.refine_marks = run.marks;
        }
        else if (!run.item)
            u = prepare_ai_upload(p);
        else
            u = prepare_refinement(p, run.item, 0, ai_task::refine, {});  // a fresh drawing from the composition (replan)
        u.plan = run.plan;
        u.run = run.id;
        u.round = uint32_t(run.drawings + 1);
        if (!refine)
            u.words.instructions = run.settings.instructions;
        choose_ai_account(u, *roles.illustrator);
        cancelled(cancel);
        const auto result = roles.draw(u, run.folder, cancel);
        cancelled(cancel);
        report(roles, run, "Checking…");
        const int index = apply_ai_result(p, u, result);
        auto& l = p.layers[size_t(index)];
        run.item = l.id;
        auto* v = find_version(l, l.version);
        if (!refine && u.parent == 0 && u.item)
            v->task = "Replan";
        else if (!refine)
            v->task = "Auto";
        add_usage(v->director, run.pending_director);
        run.pending_director = {};
        ++run.drawings;
        run.round_fixes = 0;
        run.last = v->id;
        check(p, run.item, run.last, roles);
        run.next = auto_run::step::critique;
        report(roles, run, "Checking…", &p);
    };
    try
    {
        while (run.stop == auto_stop::none)
        {
            cancelled(cancel);
            switch (run.next)
            {
                case auto_run::step::measure_plan:
                {
                    // 1 Measure: the composition as Auto reduces it today.
                    report(roles, run, "Measuring…");
                    auto sheets = make_sheets(p.layers, p.options);
                    auto found = validate_sheets(sheets.layers, p.options, sheets.removed);
                    found.tones_found = sheets.tones_found;
                    found.tones_wanted = p.options.layers;
                    auto input = base_input(p, run);
                    input.measurements = measurements_json(found, p.options);
                    input.findings = describe_findings(found, p.options);
                    if (run.mode == auto_mode::arrange)
                        input.instructions = "Arrange this picture as it is, without drawing: use route \"local\" (palette mapping, holes, merges and "
                                             "sheet count)." +
                                             (input.instructions.empty() ? std::string() : " " + input.instructions);
                    const auto upload = prepare_ai_upload(p);
                    std::vector<ai_image> references(upload.images.begin() + 1, upload.images.end());
                    const auto overlay = auto_overlay(sheets.layers, found);
                    std::optional<ai_image> marked;
                    if (overlay.valid())
                        marked = ai_image{"issues.png", upload_png(overlay)};
                    input.images = director_image_order(false, upload.images.front(), std::move(references), std::move(marked));
                    // 2 Plan.
                    report(roles, run, "Planning…");
                    const auto reply = roles.plan(input, run.folder, cancel);
                    cancelled(cancel);
                    run.plan = reply.json;
                    run.compiled = reply.plan.json.empty() ? validate_director_plan(reply.json, std::max(1, p.options.layers)) : reply.plan;
                    add_usage(run.pending_director, usage_of(reply));
                    // Fewer sheets when the director says why; otherwise propose it.
                    const int planned = int(run.compiled.palette.size());
                    if (planned && planned < p.options.layers)
                    {
                        const auto reason = sheet_reduction_reason(run.plan, p.options.layers);
                        if (!reason.empty())
                        {
                            run.sheets_note = std::format("Sheets {} → {}: {}", p.options.layers, planned, reason);
                            p.options.layers = planned;
                        }
                        else
                            run.sheets_note = std::format("The plan uses {} of your {} sheets; set Sheets to {} to keep it.", planned, p.options.layers, planned);
                    }
                    report(roles, run, "Planned.");
                    if (run.settings.until == auto_until::plan_only)
                    {
                        run.stop = auto_stop::plan_only;
                        run.message = reply.explanation;
                        break;
                    }
                    run.next = run.mode == auto_mode::arrange || plan_route(run.plan) == "local" ? auto_run::step::arrange : auto_run::step::draw;
                    break;
                }
                case auto_run::step::arrange:
                {
                    // route "local": the picture as it is, snapped to the plan's palette.
                    report(roles, run, "Arranging…");
                    const auto palette = palette_text(run.compiled.palette);
                    const layer* selected = p.selected_layer >= 0 && p.selected_layer < int(p.layers.size()) ? &p.layers[size_t(p.selected_layer)] : nullptr;
                    if (selected && selected->type == layer_type::ai_illustration && selected->visible)
                    {
                        auto& l = p.layers[size_t(p.selected_layer)];
                        item_version v;
                        v.parent = l.version;
                        v.image = current_pixels(l);
                        v.task = "Arrange for me";
                        v.palette = palette;
                        v.plan = run.plan;
                        v.run = run.id;
                        v.round = 1;
                        v.style = p.ai_style;
                        v.notes = l.notes;
                        v.created = now_iso();
                        run.item = l.id;
                        run.last = add_version(l, std::move(v));
                    }
                    else
                    {
                        auto u = prepare_ai_upload(p);
                        u.plan = run.plan;
                        u.run = run.id;
                        u.round = 1;
                        assemble(u);
                        ai_result as_is;
                        as_is.image = u.composition;
                        const int index = apply_ai_result(p, u, as_is);
                        auto& l = p.layers[size_t(index)];
                        run.item = l.id;
                        run.last = l.version;
                        auto* v = find_version(l, l.version);
                        v->task = "Arrange for me";
                        v->prompt.clear();
                        v->illustrator = {};
                        v->model.clear();
                    }
                    auto* v = find_version(item_of(p, run.item), run.last);
                    add_usage(v->director, run.pending_director);
                    run.pending_director = {};
                    run.drawings = std::max(run.drawings, 1);
                    check(p, run.item, run.last, roles);
                    std::string ops = "[]";
                    if (const auto root = parse(run.plan))
                        if (const auto* o = detail::json_at(*root, {"local", "ops"}); o && o->kind == json_value::array)
                            ops = dump(o);
                    if (ops != "[]")
                    {
                        report(roles, run, "Fixing locally…");
                        local_fix(p, run, roles, ops, {});
                    }
                    find_version(item_of(p, run.item), run.last)->explanation = run.compiled.summary;
                    report(roles, run, "Checking…", &p);
                    if (run.mode == auto_mode::arrange)
                    {
                        run.stop = auto_stop::arranged;
                        break;
                    }
                    run.next = auto_run::step::critique;
                    break;
                }
                case auto_run::step::draw:
                case auto_run::step::refine:
                    if (run.drawings >= run.settings.rounds)
                    {
                        run.stop = auto_stop::rounds;
                        break;
                    }
                    draw(run.next == auto_run::step::refine);
                    break;
                case auto_run::step::critique:
                {
                    report(roles, run, "Reviewing…");
                    auto& l = item_of(p, run.item);
                    set_current_version(l, run.last);
                    const auto found = rebuild_sheets(p);
                    const auto* v = find_version(item_of(p, run.item), run.last);
                    auto input = base_input(p, run);
                    if (const auto palette = parse_palette(v->palette); !palette.empty())
                        input.sheets = int(palette.size());
                    input.plan = run.plan;
                    input.measurements = v->measurements;
                    input.findings = v->findings;
                    if (!run.review.empty())
                        input.instructions = run.review + (input.instructions.empty() ? "" : "\n" + input.instructions);
                    std::vector<ai_image> references;
                    const auto& item = item_of(p, run.item);
                    if (item.image.valid())
                        references.push_back({"original-composition.png", upload_png(item.image)});
                    const auto overlay = auto_overlay(p.cuts, found);
                    std::optional<ai_image> marked;
                    if (overlay.valid())
                        marked = ai_image{"issues.png", upload_png(overlay)};
                    input.images = director_image_order(true, {"drawing.png", upload_png(v->image)}, std::move(references), std::move(marked));
                    const auto reply = roles.critique(input, run.folder, cancel);
                    cancelled(cancel);
                    run.review.clear();
                    auto* judged = find_version(item_of(p, run.item), run.last);
                    judged->verdict = reply.json;
                    judged->explanation = reply.explanation;
                    add_usage(judged->director, usage_of(reply));
                    const auto verdict = read_verdict(reply.json);
                    const int errors = auto_errors(judged->measurements);
                    const auto measurements = judged->measurements;
                    const auto findings = judged->findings;
                    // 7 Fix.
                    if (verdict.kind == "pass")
                    {
                        if (!errors)
                        {
                            run.stop = auto_stop::passed;
                            break;
                        }
                        // Offcut decides: errors remain, so redraw what it found.
                        run.instruction = "Offcut still finds pieces that would fall out: " + findings +
                                          " Join each one to the shape behind or beside it, and keep everything else as it is.";
                        run.marks.clear();
                        run.next = auto_run::step::refine;
                        end_round(p, run);
                        break;
                    }
                    // Local ops come first, for fix_local and for a refine that includes them.
                    if (verdict.ops != "[]" && run.round_fixes < 1)
                    {
                        report(roles, run, std::format("Fixing {} here…", std::ranges::count(verdict.ops, '{') == 1 ? std::string("1 thing") : std::format("{} things", std::ranges::count(verdict.ops, '{'))));
                        if (local_fix(p, run, roles, verdict.ops, verdict.explain))
                        {
                            report(roles, run, "Checking…", &p);
                            if (verdict.kind == "fix_local")
                            {
                                run.next = auto_run::step::critique;  // check again, within the round
                                break;
                            }
                        }
                    }
                    if (verdict.kind == "replan")
                    {
                        if (verdict.patch != "[]")
                        {
                            const auto patched = roles.plan_patch ? roles.plan_patch(run.plan, verdict.patch) : apply_plan_patch(run.plan, verdict.patch);
                            run.compiled = validate_director_plan(patched, 32);
                            run.plan = patched;
                        }
                        run.next = auto_run::step::draw;
                    }
                    else
                    {
                        // refine, or a local fix that could not be applied (again): redraw what it said.
                        run.instruction = !verdict.instruction.empty() ? verdict.instruction
                                                                       : verdict.explain + " Keep everything else as it is.";
                        run.marks = describe_marks(measurements, verdict.marks);
                        run.next = auto_run::step::refine;
                    }
                    end_round(p, run);
                    break;
                }
                case auto_run::step::done:
                    run.stop = auto_stop::rounds;
                    break;
            }
        }
    }
    catch (const ai_cancelled&)
    {
        if (!run.item)
            throw;
        run.stop = auto_stop::cancelled;
        run.message = "Cancelled: the best round so far is shown.";
    }
    catch (const std::exception& e)
    {
        if (!run.item)
            throw;
        run.stop = auto_stop::failed;
        run.message = e.what();
    }
    if (run.stop == auto_stop::plan_only)
        return;
    if (run.stop == auto_stop::asking)
    {
        // The round's latest, for Continue or Stop.
        auto& l = item_of(p, run.item);
        set_current_version(l, run.last);
        rebuild_sheets(p);
        p.selected_layer = p.find_layer(run.item);
        return;
    }
    run.next = auto_run::step::done;
    present(p, run);
}
}  // namespace offcut
