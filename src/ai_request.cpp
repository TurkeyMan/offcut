// The AI loop's network half (docs/ux-design.md section 8): sending a
// prepared request and decoding the reply. Split from ai_workflow.cpp, which
// is in offcut_core for the tests, because decoding images needs Rapport.
#include <fstream>
#include <iomanip>
#include <sstream>
#include "ai_workflow.h"
#include "export.h"
#include "image_io.h"
namespace offcut
{
ai_result request_ai_image(const prepared_ai_upload& upload, const std::string& key, const std::filesystem::path& parent, const std::atomic_bool* cancel)
{
    if (cancel && cancel->load())
        throw ai_cancelled();
    validate_ai_endpoint(upload.endpoint);
    // The request exactly as uploaded, written only once sending is
    // authorised. No key enters these files.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    for (const auto& i : upload.images)
        files.emplace_back(i.filename, i.png);
    ai_result result;
    result.folder = write_ai_request(parent, upload.prompt, files);
    const auto reply = edit_ai_image(upload.endpoint, upload.prompt, upload.images, key, cancel);
    if (cancel && cancel->load())
        throw ai_cancelled();
    const auto response = result.folder / ("response" + ai_image_extension(reply.png));
    {
        std::ofstream out(response, std::ios::binary);
        out.write(reinterpret_cast<const char*>(reply.png.data()), std::streamsize(reply.png.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Could not save the generated image.");
    }
    result.image = read_image(response).image;
    result.seconds = reply.seconds;
    result.usage = reply.usage_json;
    std::ostringstream metadata;
    metadata << "{\n  \"model\": " << ai_json_quote(redact_ai_secret(upload.endpoint.model, key))
             << ",\n  \"endpoint\": " << ai_json_quote(redact_ai_secret(upload.endpoint.base_url, key)) << ",\n  \"seconds\": " << std::fixed
             << std::setprecision(3) << reply.seconds << ",\n  \"usage\": " << reply.usage_json << "\n}\n";
    std::ofstream out(result.folder / "response.json", std::ios::binary);
    out << metadata.str();
    out.close();
    if (!out)
        throw std::runtime_error("Could not save AI response metadata.");
    return result;
}
std::filesystem::path run_ai_roundtrip(project& p, const prepared_ai_upload& upload, const std::string& key, const std::filesystem::path& parent,
                                       const std::atomic_bool* cancel)
{
    const auto result = request_ai_image(upload, key, parent, cancel);
    if (cancel && cancel->load())
        throw ai_cancelled();
    apply_ai_result(p, upload, result);
    p.activity += " Saved request and response to " + utf8(result.folder) + ".";
    return result.folder;
}
}  // namespace offcut
