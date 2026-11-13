#include "io/Samples.h"

#include "core/Paths.h"
#include "io/SceneFile.h"
#include "ops/Command.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <format>
#include <fstream>

namespace lila::io::samples
{
using json = nlohmann::json;

fs::path samplesDir()
{
    return paths::resourcesDir() / "samples";
}

std::vector<Sample> list(const fs::path& dir)
{
    std::vector<Sample> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_directory(ec))
            continue;
        std::ifstream in(e.path() / "sample.json");
        const json j = json::parse(in, nullptr, false);
        if (!j.is_object() || !j.contains("scene"))
            continue;
        Sample s;
        s.id = paths::toUtf8(e.path().filename());
        s.name = j.value("name", s.id);
        s.archive = j.value("archive", s.id);
        s.description = j.value("description", "");
        s.url = j.value("url", "");
        for (const json& f : j.value("files", json::array()))
            if (f.is_object() && f.contains("url") && f.contains("path"))
                s.files.push_back({f["url"].get<std::string>(), f["path"].get<std::string>()});
        s.size = j.value("size", std::uint64_t(0));
        s.diskSize = j.value("diskSize", std::uint64_t(0));
        s.sha256 = j.value("sha256", "");
        s.license = j.value("license", "");
        s.licenseUrl = j.value("licenseUrl", "");
        s.credit = j.value("credit", "");
        s.source = j.value("source", "");
        s.scene = j.value("scene", "");
        s.camera = j.value("camera", "");
        s.featured = j.value("featured", false);
        s.after = j.value("after", "");
        s.converter = j.value("converter", "");
        s.folder = e.path();
        if (fs::is_regular_file(e.path() / "thumb.png", ec))
            s.thumbnail = e.path() / "thumb.png";
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const Sample& a, const Sample& b) { return a.featured != b.featured ? a.featured : a.id < b.id; });
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (out[i].after.empty())
            continue;
        auto target = std::find_if(out.begin(), out.end(), [&](const Sample& t) { return t.id == out[i].after; });
        if (target == out.end() || target == out.begin() + std::ptrdiff_t(i))
            continue;
        Sample moved = std::move(out[i]);
        out.erase(out.begin() + std::ptrdiff_t(i));
        target = std::find_if(out.begin(), out.end(), [&](const Sample& t) { return t.id == moved.after; });
        const std::size_t at = std::size_t(target - out.begin()) + 1;
        out.insert(out.begin() + std::ptrdiff_t(at), std::move(moved));
        if (at <= i)
            continue; // moved earlier: the next item now sits at i + 1
        --i; // moved later: the item that slid into i is unvisited
    }
    return out;
}

const Sample* find(const std::vector<Sample>& all, const std::string& idOrName)
{
    auto it = std::find_if(all.begin(), all.end(), [&](const Sample& s) { return s.id == idOrName || s.name == idOrName; });
    return it == all.end() ? nullptr : &*it;
}

fs::path installDir(const Sample& s)
{
    return paths::userConfigDir() / "samples" / paths::fromUtf8(s.archive);
}

bool unpacked(const Sample& s)
{
    // The download is removed only once unpacking finished, and a failed unpack removes the folder.
    std::error_code ec;
    const fs::path dir = installDir(s);
    return !fs::exists(dir / "download.zip", ec) && fs::is_directory(dir / "source", ec) && !fs::is_empty(dir / "source", ec);
}

bool bundled(const Sample& s)
{
    return s.url.empty() && s.files.empty();
}

fs::path scenePath(const Sample& s)
{
    return (bundled(s) ? s.folder : installDir(s)) / paths::fromUtf8(s.scene);
}

std::map<std::string, std::string>& converterVersions()
{
    static std::map<std::string, std::string> versions;
    return versions;
}

fs::path stampPath(const Sample& s)
{
    fs::path p = scenePath(s);
    return p += ".setup";
}

/** "<converter> <version>" of the importer that would set the sample up now; empty when it has none. */
std::string currentStamp(const Sample& s)
{
    if (s.converter.empty())
        return {};
    auto it = converterVersions().find(s.converter);
    return s.converter + " " + (it == converterVersions().end() ? std::string("0") : it->second);
}

bool installed(const Sample& s)
{
    std::error_code ec;
    if (!fs::is_regular_file(scenePath(s), ec))
        return false;
    const std::string want = currentStamp(s);
    if (want.empty())
        return true;
    std::ifstream f(stampPath(s));
    std::string have;
    std::getline(f, have);
    return have == want;
}

void setConverterVersion(const std::string& converter, const std::string& version)
{
    converterVersions()[converter] = version;
}

void stamp(const Sample& s)
{
    if (const std::string st = currentStamp(s); !st.empty())
        std::ofstream(stampPath(s)) << st << '\n';
}

std::string formatSize(std::uint64_t bytes)
{
    const double b = double(bytes);
    if (b >= 1e9)
        return std::format("{:.1f} GB", b / 1e9);
    if (b >= 1e6)
        return std::format("{:.0f} MB", b / 1e6);
    return std::format("{:.0f} KB", std::max(b / 1e3, 1.0));
}

void open(const Sample& s)
{
    const fs::path scene = scenePath(s);
    loadVersion(scene, scene, false, s.name);
    markClean();
}

void registerCommands()
{
    ops::registerCommand({
        .name = "samples",
        .doc = "Sample scenes: shipped with Lila, or third-party ones it downloads on request: {id, name, installed, path, "
               "size, license, credit, source}. Download them from the Welcome screen's Sample Scenes.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Sample& s : list())
                out.push_back({{"id", s.id},
                               {"name", s.name},
                               {"installed", installed(s)},
                               {"path", paths::toUtf8(scenePath(s))},
                               {"size", s.size},
                               {"license", s.license},
                               {"credit", s.credit},
                               {"source", s.source}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "openSample",
        .doc = "Open a downloaded sample scene by id or name (from samples) as an untitled scene. Not undoable.",
        .positionalDoc = "name",
        .flags = {{"force", "f", ArgType::Bool, false, "Discard unsaved changes in the current scene."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("openSample: give one name");
            if (io::modified() && !a.get<bool>("force"))
                throw CommandError("openSample: the scene has unsaved changes; save it or pass force=True");
            const std::vector<Sample> all = list();
            const Sample* s = find(all, a.positional[0].get<std::string>());
            if (!s)
                throw CommandError("openSample: no sample named " + a.positional[0].get<std::string>());
            if (!installed(*s))
                throw CommandError("openSample: " + s->name + " is not downloaded; get it from the Welcome screen's Sample Scenes");
            try
            {
                open(*s);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return paths::toUtf8(scenePath(*s));
        },
    });
}
} // namespace lila::io::samples
