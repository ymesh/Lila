#include "Placeholders.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/MeshOps.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

namespace weathering
{
namespace
{
constexpr int kVersion = 5;

std::vector<Placeholder>& registry()
{
    static std::vector<Placeholder> list;
    return list;
}

fs::path libraryDir() { return PrefStore::get().userDir() / "weathering-library"; }

fs::path markerOf(const fs::path& dir) { return dir / "placeholders.json"; }

/** What a built folder must match: the version and every builder's id, so a new builder triggers a rebuild. */
json signature()
{
    json ids = json::array();
    for (const Placeholder& p : registry())
        ids.push_back(p.collection + "/" + p.name);
    return {{"version", kVersion}, {"assets", ids}};
}

bool current(const fs::path& dir)
{
    try
    {
        std::ifstream f(markerOf(dir));
        return f && json::parse(f) == signature();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

glm::dvec3 sizeOf(const geo::Mesh& m)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    return lo.x > hi.x ? glm::dvec3(0.0) : hi - lo;
}

/** A readable stand-in colour per role, brighter and more saturated in toon, so placeholders tell apart at a glance. */
glm::vec3 roleColor(const std::string& role, bool toon)
{
    static const std::vector<std::pair<std::string, glm::vec3>> colors = {
        {"rubble", {0.24f, 0.22f, 0.2f}},  {"rock", {0.2f, 0.19f, 0.18f}},   {"brick", {0.45f, 0.17f, 0.1f}},
        {"plank", {0.36f, 0.24f, 0.14f}},  {"beam", {0.3f, 0.2f, 0.12f}},     {"charred", {0.05f, 0.045f, 0.04f}},
        {"plant", {0.17f, 0.3f, 0.08f}},   {"grass", {0.22f, 0.36f, 0.1f}},   {"moss", {0.14f, 0.26f, 0.06f}},
        {"bush", {0.12f, 0.24f, 0.07f}},   {"pebble", {0.22f, 0.21f, 0.2f}},  {"litter", {0.75f, 0.72f, 0.66f}},
        {"leaf", {0.45f, 0.25f, 0.08f}},   {"ivy-leaf", {0.1f, 0.25f, 0.06f}}, {"vine-leaf", {0.16f, 0.3f, 0.08f}},
        {"ivy-flower", {0.8f, 0.78f, 0.7f}}, {"moss-tuft", {0.14f, 0.26f, 0.06f}}, {"roof-sheet", {0.35f, 0.33f, 0.3f}},
        {"ridge-cap", {0.3f, 0.29f, 0.27f}}, {"roof-tile", {0.48f, 0.2f, 0.12f}}, {"roof-plank", {0.32f, 0.22f, 0.13f}},
        {"window-frame", {0.5f, 0.47f, 0.42f}}, {"window-sill", {0.55f, 0.53f, 0.5f}}, {"window-board", {0.34f, 0.24f, 0.15f}},
        {"wire-insulator", {0.6f, 0.62f, 0.6f}}, {"wire-bracket", {0.2f, 0.2f, 0.21f}}, {"pole", {0.3f, 0.22f, 0.15f}},
        {"roof-vent", {0.5f, 0.5f, 0.52f}}, {"ac-unit", {0.62f, 0.62f, 0.6f}}, {"roof-pipe", {0.35f, 0.36f, 0.38f}},
        {"railing", {0.25f, 0.25f, 0.26f}}, {"tower", {0.45f, 0.45f, 0.47f}},
    };
    glm::vec3 c(0.5f);
    for (const auto& [r, v] : colors)
        if (r == role)
            c = v;
    if (!toon)
        return c;
    // Toon: lifted and pushed away from grey.
    const float grey = (c.r + c.g + c.b) / 3.0f;
    return glm::clamp(glm::vec3(grey) + (c - glm::vec3(grey)) * 1.6f + 0.12f, glm::vec3(0.0f), glm::vec3(1.0f));
}

void addRoot()
{
    try
    {
        ops::execute("libraryRoot", {{"args", {libraryDir().generic_string()}}, {"tier", "placeholder"}});
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, std::string("Weathering: placeholder library not added: ") + e.what());
    }
}
} // namespace

void addPlaceholder(Placeholder p)
{
    for (Placeholder& x : registry())
        if (x.collection == p.collection && x.name == p.name)
        {
            x = std::move(p);
            return;
        }
    registry().push_back(std::move(p));
}

const std::vector<Placeholder>& placeholders() { return registry(); }

json buildPlaceholders(bool force)
{
    LILA_ZONE();

    const fs::path dir = libraryDir();
    json ids = json::array();
    for (const Placeholder& p : registry())
        ids.push_back(p.collection + "/" + p.name);
    if (!force && current(dir))
        return {{"dir", dir.generic_string()}, {"built", false}, {"assets", ids}};
    std::error_code ec;
    fs::remove_all(dir, ec);
    for (const Placeholder& p : registry())
    {
        const geo::Mesh m = p.build();
        const fs::path folder = dir / p.collection / p.name;
        fs::create_directories(folder, ec);
        const glm::vec3 c = p.color.r >= 0.0f ? p.color : roleColor(p.role, p.style == "toon");
        // One material per role and style, shared by its pieces, under a prefix tools can leave out of material lists.
        std::string material = kPieceMaterialPrefix;
        bool upper = true;
        for (char ch : p.role)
        {
            if (!std::isalnum(static_cast<unsigned char>(ch)))
            {
                upper = true;
                continue;
            }
            material += upper ? char(std::toupper(static_cast<unsigned char>(ch))) : ch;
            upper = false;
        }
        if (p.style == "toon")
            material += "Toon";
        std::ofstream(folder / (p.name + ".mtl"), std::ios::binary)
            << "newmtl " << material << "\nKd " << std::pow(c.r, 1.0f / 2.2f) << " " << std::pow(c.g, 1.0f / 2.2f) << " " << std::pow(c.b, 1.0f / 2.2f)
            << "\nKs 0 0 0\nNs 10\n";
        // usemtl goes after the object line: a new object starts with no material.
        std::string obj = geo::writeObj(m);
        const size_t objectLine = obj.find("\no ");
        const size_t afterObject = objectLine == std::string::npos ? 0 : obj.find('\n', objectLine + 1) + 1;
        obj.insert(afterObject, "usemtl " + material + "\n");
        std::ofstream(folder / (p.name + ".obj"), std::ios::binary) << "mtllib " << p.name << ".mtl\n" << obj;
        json tags = json::array({"weathering", "placeholder", p.role, p.style});
        for (const std::string& t : p.tags)
            tags.push_back(t);
        const glm::dvec3 size = sizeOf(m);
        const json meta = {{"name", p.name},
                           {"model", p.name + ".obj"},
                           {"tags", tags},
                           {"description", p.description},
                           {"size", json::array({size.x, size.y, size.z})},
                           {"physics", {{"role", p.physicsRole}, {"material", p.physicsMaterial}, {"shape", p.physicsShape}}}};
        std::ofstream(folder / "asset.json") << meta.dump(2);
    }
    std::ofstream(markerOf(dir)) << signature().dump(2);
    addRoot();
    try
    {
        ops::execute("libraryReload", json::object());
    }
    catch (const std::exception&)
    {
    }
    log::info("Weathering: placeholder pieces written to {}", dir.generic_string());
    // Preset thumbnails show placeholders: drawn again with the new ones next time the library shows them.
    try
    {
        json names = json::array();
        for (const json& p : ops::execute("arrangementPresets", json::object()))
            if (p.value("source", std::string()) == "Weathering")
                names.push_back(p.value("name", std::string()));
        if (!names.empty())
            ops::execute("arrangementThumbnails", {{"args", names}, {"clear", true}});
    }
    catch (const std::exception&)
    {
    }
    return {{"dir", dir.generic_string()}, {"built", true}, {"assets", ids}};
}

void registerPlaceholderCommands(ext::Context& ctx)
{
    addRoot();
    // Built at load (only when missing or out of date), so presets run from Stagehand's library find them by tag.
    buildPlaceholders(false);
    ctx.registerCommand({
        .name = "weatheringPlaceholders",
        .doc = "Write Weathering's placeholder pieces (rubble, planks, plants, leaves, roof sheets, tiles, frames, insulators, "
               "rooftop units, in realistic and toon versions) into the library's placeholder tier, where every preset finds them "
               "until you tag your own assets with the same roles. Built on first use; force=True rebuilds. Returns dir, built "
               "and assets (ids).",
        .flags = {{"force", "f", ArgType::Bool, false, "Rebuild even when they are up to date."}},
        .undoable = false,
        .run = [](const lila::Args& a) -> json { return buildPlaceholders(a.get<bool>("force", false)); },
        .examples = {"weatheringPlaceholders()", "[a['id'] for a in librarySearch(tags=['weathering', 'rubble'])]"},
    });
}
} // namespace weathering
