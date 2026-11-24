#include "director_cli.h"
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include "chatgpt_auth.h"
#include "director.h"
#include "director_helpers.h"
#include "export.h"
#include "image_io.h"
#include "settings.h"
#include "transform.h"
#include "versions.h"
namespace offcut
{
namespace
{
std::vector<uint8_t> upload_png(const raster& image)
{
    const double scale = std::min(1., 1024. / std::max(image.width, image.height));
    return encode_png(scale == 1 ? image
                                 : fit_raster(image, std::max(1, int(std::lround(image.width * scale))), std::max(1, int(std::lround(image.height * scale)))));
}
void write(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary);
    out << text;
    out.close();
    if (!out)
        throw std::runtime_error("Could not write director output.");
}
std::vector<ai_image> plan_references(const std::filesystem::path& plan_path, const std::string& json)
{
    // Use only uniquely matching saved request provenance. A standalone plan
    // has no source image: do not present the drawing again as its reference.
    std::vector<std::filesystem::path> matches;
    const auto consider = [&](const std::filesystem::path& folder)
    {
        const auto plan = folder / "plan.json", image = folder / "image-01.png";
        if (!std::filesystem::is_regular_file(plan) || !std::filesystem::is_regular_file(image) || std::filesystem::file_size(plan) > 4 * 1024 * 1024)
            return;
        std::ifstream in(plan, std::ios::binary);
        const std::string saved{std::istreambuf_iterator<char>(in), {}};
        if (saved == json)
            matches.push_back(image);
    };
    consider(plan_path.parent_path());
    if (matches.empty())
        for (const auto& entry : std::filesystem::directory_iterator(plan_path.parent_path()))
            if (entry.is_directory() && utf8(entry.path().filename()).starts_with("director-request-"))
                consider(entry.path());
    if (matches.size() != 1)
        return {};
    return {{"original-composition.png", upload_png(read_image(matches.front()).image)}};
}
raster issue_overlay(const sheet_result& sheets, const validation& v)
{
    auto image = render_cuts(sheets.layers, {.floating = true});
    if (!image.valid())
        return image;
    // Tiny bitmap numerals refer to m1, m2, ... in measurements.json.
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
}  // namespace
int director_command(int argc, char** argv)
{
    const bool signin = argc >= 2 && std::string_view(argv[1]) == "--chatgpt-sign-in";
    const bool director = argc >= 2 && std::string_view(argv[1]) == "--director";
    if (!signin && !director)
        return -1;
    try
    {
        if (signin)
        {
            if (argc != 3)
                throw std::runtime_error("Usage: offcut --chatgpt-sign-in <entry-id>");
            auto accounts = configured_ai_accounts();
            const std::string id = argv[2];
            auto* current = accounts.find(id);
            ai_account account;
            if (current)
            {
                if (current->provider != "chatgpt")
                    throw std::runtime_error("This entry belongs to another provider. Choose a new entry ID.");
                account = *current;
            }
            else
            {
                account.id = id;
                account.provider = "chatgpt";
                account.name = "ChatGPT";
                account.base_url = "https://api.openai.com/v1";
                account.credential_target = ai_credential_target(id);
            }
            if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
                throw std::runtime_error("Use letters, numbers, '-' or '_' for the entry ID.");
            if (account.credential_target.empty())
                account.credential_target = ai_credential_target(id);
            std::atomic_bool cancel{false};
            // Listener/browser/token work runs on a worker, just as the UI calls it.
            auto work = std::async(std::launch::async, [&] { return chatgpt_sign_in(account.credential_target, &cancel); });
            const auto identity = work.get();
            account.name = identity.label;
            // Save the signed-in entry before discovery, so a temporary model
            // catalog outage never strands an otherwise usable registration.
            if (current)
                *current = account;
            else
                accounts.entries.push_back(account);
            if (accounts.plans_id.empty())
                accounts.plans_id = id;
            save_ai_accounts(accounts);
            try
            {
                account.models =
                    list_ai_models(find_ai_provider("chatgpt"), account.base_url, chatgpt_access_token(account.credential_target, &cancel), &cancel);
                if (account.reasoning_model.empty() && !account.models.empty())
                    account.reasoning_model = account.models.front();
                *accounts.find(id) = account;
                save_ai_accounts(accounts);
            }
            catch (const std::exception& e)
            {
                std::cerr << "Signed in, but model discovery needs another try: " << e.what() << '\n';
                return 1;
            }
            std::cout << "Signed in as " << identity.label << ". Entry: " << id << "; reasoning model: " << account.reasoning_model << '\n';
            return 0;
        }
        if (argc < 6 || argc > 7)
            throw std::runtime_error("Usage: offcut --director plan|critique <entry-id|default> <image> <out-dir> [instructions|plan.json]");
        const std::string mode = argv[2];
        if (mode != "plan" && mode != "critique")
            throw std::runtime_error("Director command must be plan or critique.");
        const auto accounts = configured_ai_accounts();
        const auto* chosen = std::string_view(argv[3]) == "default" ? accounts.plans_account() : accounts.find(argv[3]);
        if (!chosen || chosen->reasoning_model.empty())
            throw std::runtime_error("Choose a 'Plans and reviews with' entry and reasoning model in Settings → AI providers.");
        const auto output = from_utf8(argv[5]);
        std::filesystem::create_directories(output);
        project p;
        p.name = "Director " + mode;
        p.layers = {read_image(from_utf8(argv[4]))};
        p.options = new_project_options(p.layers.front().image);
        p.options.layers = 4;
        for (auto& l : p.layers)
            l.transform = place_image(l, p.options, p.options.placement, p.options.touch_edges);
        p.assign_layer_ids();
        director_input input;
        input.frame_width_mm = p.options.width_mm;
        input.frame_height_mm = p.options.height_mm;
        input.min_width_mm = p.options.bridge_mm;
        input.border_widths_mm = borders(p.options);
        input.layer_gap_mm = p.options.layer_gap_mm;
        input.style = p.ai_style;
        input.notes = ai_notes(p);
        std::vector<ai_image> references;
        std::vector<std::array<uint8_t, 3>> palette;
        if (argc == 7)
        {
            if (mode == "plan")
                input.instructions = argv[6];
            else
            {
                std::ifstream plan(from_utf8(argv[6]), std::ios::binary);
                if (!plan)
                    throw std::runtime_error("Could not read plan.json.");
                input.plan = {std::istreambuf_iterator<char>(plan), {}};
                const auto compiled = validate_director_plan(input.plan, 32);
                palette = compiled.palette;
                p.options.layers = int(palette.size());
                references = plan_references(std::filesystem::absolute(from_utf8(argv[6])), input.plan);
            }
        }
        input.sheets = p.options.layers;
        const sheet_result sheets =
            mode == "critique" ? sheet_result(layers_from_illustration(p.layers.front().image, p.options, palette)) : make_sheets(p.layers, p.options);
        auto findings = validate_sheets(sheets.layers, p.options, sheets.removed);
        findings.tones_found = sheets.tones_found;
        findings.tones_wanted = p.options.layers;
        input.measurements = measurements_json(findings, p.options);
        input.findings = describe_findings(findings, p.options);
        if (mode == "plan")
            references.push_back({"reference.png", upload_png(p.layers.front().image)});
        const auto primary = mode == "critique" ? ai_image{"drawing.png", upload_png(p.layers.front().image)}
                                                : ai_image{"composition.png", upload_png(flatten(p.layers, p.options))};
        const auto overlay = issue_overlay(sheets, findings);
        std::optional<ai_image> marked;
        if (overlay.valid())
            marked = ai_image{"issues.png", upload_png(overlay)};
        input.images = director_image_order(mode == "critique", primary, std::move(references), std::move(marked));
        write(output / "measurements.json", input.measurements);
        write(output / "findings.txt", input.findings);
        const director_endpoint endpoint{chosen->provider, chosen->base_url, chosen->reasoning_model, chosen->credential_target};
        std::atomic_bool cancel{false};
        auto work = std::async(
            std::launch::async,
            [&] { return mode == "plan" ? director_plan(endpoint, input, utf8(output), &cancel) : director_critique(endpoint, input, utf8(output), &cancel); });
        const auto reply = work.get();
        write(output / (mode == "plan" ? "plan.json" : "verdict.json"), reply.json);
        write(output / "summary.txt", reply.explanation + "\nRequest: " + reply.request + "\nSeconds: " + std::to_string(reply.seconds) + "\nInput tokens: " +
                                          std::to_string(reply.input_tokens) + "\nOutput tokens: " + std::to_string(reply.output_tokens) + "\n");
        std::cout << reply.explanation << "\nRequest: " << reply.request << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
}  // namespace offcut
