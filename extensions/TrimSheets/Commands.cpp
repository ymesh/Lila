// trimSheet* make, inspect, edit, generate, bake, import, export and send sheets; trim* map faces onto a sheet's trims
// (one trim, automatic hotspotting, shifting between strips, edge chamfers).

#include "TrimSheets.h"
#include "Check.h"
#include "Generator.h"
#include "Strips.h"
#include "TrimBake.h"

#include <lila/Bake.h>
#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/TextureGraph.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Uv.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <sstream>

using namespace lila;

namespace trims
{
namespace
{
using scene::Graph;
using scene::Node;
using AT = ArgType;
using Ids = scene::components::Ids;

constexpr const char* kPrefs = "trimSheets";

std::string presetChoices()
{
    std::string s;
    for (const PresetInfo& p : presets())
        s += (s.empty() ? "" : ", ") + p.id;
    return s;
}

std::vector<std::string> presetIds()
{
    std::vector<std::string> out;
    for (const PresetInfo& p : presets())
        out.push_back(p.id);
    return out;
}

FlagDef sheetFlag() { return {"sheet", "sh", AT::String, nullptr, "The .lilatrim file; the current sheet (trimSheetCurrent) when not given."}; }
FlagDef uvSetFlag() { return {"uvSet", "", AT::String, "uv0", "UV set to write."}; }

/** The sheet= flag, else the first positional (when the command takes the sheet there), else the current sheet. */
std::filesystem::path loadedPath(const Args& a, const char* cmd, std::string_view flag = "sheet", bool positional = true)
{
    std::string p;
    if (a.has(std::string(flag)))
        p = a.get<std::string>(std::string(flag));
    else if (positional && flag == "sheet" && !a.positional.empty() && a.positional[0].is_string())
        p = a.positional[0].get<std::string>();
    else
        p = currentSheet();
    if (p.empty())
        throw CommandError(std::string(cmd) + ": no sheet; give sheet= or make one with trimSheetCreate");
    // Relative paths are as scenes store them: against the workspace, then the scene's folder.
    const std::filesystem::path f = paths::resolvePath(p);
    if (!std::filesystem::exists(f))
        throw CommandError(std::string(cmd) + ": no trim sheet at " + f.generic_string());
    return f;
}

Sheet loadOr(const std::filesystem::path& f, const char* cmd)
{
    try
    {
        return load(f);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** A trim by index or name. */
int trimIndex(const Sheet& s, const json& v, const char* cmd)
{
    if (v.is_number_integer())
    {
        const int i = v.get<int>();
        if (i < 0 || size_t(i) >= s.trims.size())
            throw CommandError(std::format("{}: the sheet has {} trims, no trim {}", cmd, s.trims.size(), i));
        return i;
    }
    if (v.is_string())
        for (size_t i = 0; i < s.trims.size(); ++i)
            if (s.trims[i].name == v.get<std::string>())
                return int(i);
    throw CommandError(std::string(cmd) + ": no trim " + v.dump());
}

json trimSummary(const Sheet& s, size_t i)
{
    const Trim& t = s.trims[i];
    json j = t.toJson();
    j["index"] = int64_t(i);
    const glm::dvec2 w = s.worldSize(t);
    j["worldSizeCm"] = {w.x, w.y};
    const glm::dvec2 lo = s.uvMin(t), hi = s.uvMax(t);
    j["uv"] = {{"min", {lo.x, lo.y}}, {"max", {hi.x, hi.y}}};
    return j;
}

json sheetSummary(const Sheet& s, const std::filesystem::path& f)
{
    json trimsJ = json::array();
    for (size_t i = 0; i < s.trims.size(); ++i)
        trimsJ.push_back(trimSummary(s, i));
    const glm::dvec2 ext = s.worldExtent();
    json textures = json::object();
    for (const std::string& m : s.maps)
    {
        const std::filesystem::path p = mapFile(s, f, m);
        if (std::filesystem::exists(p))
            textures[m] = p.generic_string();
    }
    return {{"file", f.generic_string()},
            {"name", s.name},
            {"size", {s.width, s.height}},
            {"pxPerM", s.pxPerM},
            {"worldSizeCm", {ext.x, ext.y}},
            {"preset", s.preset},
            {"material", materialName(s)},
            {"textures", textures},
            {"baked", !textures.empty()},
            {"warnings", validate(s)},
            {"palettes", [&] {
                 json p = json::array({"Default"});
                 for (const Palette& pal : s.palettes)
                     p.push_back(pal.name);
                 return p;
             }()},
            {"trims", trimsJ}};
}

/** The rotation that fits an island onto one trim best. */
bool bestRotation(const uv::IslandMeasure& m, const uv::HotspotTrim& t)
{
    if (!t.allowRotate || m.width <= 0.0 || m.height <= 0.0)
        return false;
    switch (t.kind)
    {
    // A strip fixes one side: turn only when the other side fits it clearly better, so world up survives where it can.
    case uv::TrimKind::StripU:
        return std::abs(std::log2(m.width / std::max(t.worldSize.y, 1e-9))) + 0.25 < std::abs(std::log2(m.height / std::max(t.worldSize.y, 1e-9)));
    case uv::TrimKind::StripV:
        return std::abs(std::log2(m.height / std::max(t.worldSize.x, 1e-9))) + 0.25 < std::abs(std::log2(m.width / std::max(t.worldSize.x, 1e-9)));
    default:
    {
        const double ta = t.worldSize.x / std::max(t.worldSize.y, 1e-9);
        return std::abs(std::log2((m.height / m.width) / ta)) < std::abs(std::log2((m.width / m.height) / ta));
    }
    }
}

/**
 * Writes the trim index (+1, 0 for none) on the faces and points them at a palette's material slot: the one given, else
 * the sheet palette each face already uses, else the sheet's own.
 */
void tagFaces(geo::Mesh& m, std::span<const geo::Index> faces, int trim, const Sheet& s, const std::optional<std::string>& palette)
{
    std::span<int32_t> attr = m.faceAttrs().getOrAdd<int32_t>(kFaceTrim);
    const std::vector<std::string> sheetSlots = paletteMaterials(s);
    std::span<const int32_t> slots = std::as_const(m).faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::map<std::string, std::vector<uint32_t>> bySlot;
    for (geo::Index f : faces)
        if (m.faceValid(f))
        {
            attr[f] = trim + 1;
            std::string slot = palette ? materialName(s, *palette) : materialName(s);
            if (!palette && f < slots.size())
                if (std::string now = scene::materials::slotName(m, slots[f]); std::ranges::find(sheetSlots, now) != sheetSlots.end())
                    slot = std::move(now);
            bySlot[slot].push_back(f);
        }
    for (const auto& [slot, ids] : bySlot)
        scene::materials::assign(m, slot, ids);
}

FlagDef paletteFlag() { return {"palette", "pal", AT::String, nullptr, "Palette the faces use (trimPaletteList); each face keeps its palette when not given."}; }

/** The palette= flag checked against the sheet: nullopt when not given, "" for the default palette. */
std::optional<std::string> paletteArg(const Sheet& s, const Args& a, const char* cmd)
{
    if (!a.has("palette"))
        return std::nullopt;
    const std::string p = a.get<std::string>("palette");
    if (p.empty() || p == "Default")
        return std::string();
    if (!s.palette(p))
    {
        std::string names = "Default";
        for (const Palette& pal : s.palettes)
            names += ", " + pal.name;
        throw CommandError(std::format("{}: {} has no palette {}; it has {}", cmd, s.name, p, names));
    }
    return p;
}

/** Splits the faces by angle, unwraps them and returns their islands, ready for measureChart. */
std::vector<uv::Chart> freshIslands(geo::Mesh& m, std::span<const geo::Index> faces, const std::string& set, double angle)
{
    if (angle > 0.0)
        uv::markSeamsByAngle(m, faces, angle);
    uv::UnwrapFacesOptions uo;
    uo.keepPlacement = false;
    uo.pack = false;
    uv::unwrapFaces(m, faces, set, uo);
    uv::ChartOptions co;
    co.uvSet = set;
    return uv::buildCharts(m, faces, co);
}

std::vector<geo::Index> facesOrAll(const geo::Mesh& m, const Ids& ids)
{
    return uv::liveFaces(m, std::span<const geo::Index>(ids.data(), ids.size()));
}

/** Marks the mesh node with the sheet it is mapped with, so sends find it. */
void tagNode(const std::string& node, const std::filesystem::path& sheet)
{
    try
    {
        ops::execute("getAttr", {{"args", {node + "." + kNodeSheet}}});
    }
    catch (const std::exception&)
    {
        ops::execute("addAttr", {{"args", {node}}, {"longName", kNodeSheet}, {"dataType", "string"}, {"doc", "Trim sheet the faces are mapped with."}});
    }
    ops::execute("setAttr", {{"args", {node + "." + kNodeSheet, paths::storedPath(sheet)}}});
}

std::filesystem::path bakeRecord(const Sheet& s, const std::filesystem::path& f);

/** cm from the baked height's 0 to its 1, from the bake record; 0 without one. */
double bakedHeightDepth(const Sheet& s, const std::filesystem::path& f)
{
    std::ifstream in(bakeRecord(s, f));
    const json r = json::parse(in, nullptr, false);
    if (r.is_object())
        for (const json& o : r.value("outputs", json::array()))
            if (o.value("map", std::string()) == "height")
                if (const json meta = o.value("meta", json()); meta.is_object())
                    return 2.0 * meta.value("heightRange", 0.0);
    return 0.0;
}

/** The Lila material a palette's faces use (the sheet's own for ""), made or updated with the baked maps. */
void ensureMaterial(const Sheet& s, const std::filesystem::path& f, const std::string& palette = {})
{
    Graph& g = Graph::current();
    const std::string name = materialName(s, palette);
    json call = {{"args", {name}}};
    const std::pair<const char*, const char*> maps[] = {{"normal", "normalMap"}, {"ao", "ambientOcclusionMap"}, {"baseColor", "baseColorMap"}};
    json sets = json::object();
    for (const auto& [map, attr] : maps)
        if (const std::filesystem::path p = mapFile(s, f, map); (s.decal || std::string_view(map) != "baseColor") && std::filesystem::exists(p))
            sets[attr] = p.generic_string();
    // With a detail normal the viewport rebuilds the relief's normals from the height, as the Unreal master does.
    const std::filesystem::path detail = mapFile(s, f, "detailNormal"), height = mapFile(s, f, "height");
    if (const double depth = bakedHeightDepth(s, f); !s.decal && depth > 0.0 && std::filesystem::exists(detail) && std::filesystem::exists(height))
    {
        const glm::dvec2 ext = s.worldExtent();
        sets["normalMap"] = detail.generic_string();
        sets["heightMap"] = height.generic_string();
        sets["normalFromHeight"] = true;
        sets["heightDepth"] = depth;
        sets["heightUvWidth"] = ext.x;
        sets["heightUvHeight"] = ext.y;
    }
    // A decal's base colour carries its opacity in alpha.
    if (s.decal)
        call["blendMode"] = "translucent";
    if (!scene::materials::find(g, name))
    {
        // A palette's Tint and BaseRoughness show in Lila's viewport until its texture set paints it.
        const json& vectors = palette.empty() ? s.vectors : s.palette(palette)->vectors;
        const json& scalars = palette.empty() ? s.scalars : s.palette(palette)->scalars;
        call["baseColor"] = vectors.contains("Tint") ? vectors["Tint"] : s.decal ? json({1.0, 1.0, 1.0}) : json({0.45, 0.45, 0.45});
        call["roughness"] = scalars.value("BaseRoughness", 0.5);
        ops::execute("createMaterial", call);
    }
    for (const auto& [attr, value] : sets.items())
        ops::execute("setAttr", {{"args", {name + "." + attr, value}}});
}

/** ensureMaterial for every palette. */
void ensureMaterials(const Sheet& s, const std::filesystem::path& f)
{
    ensureMaterial(s, f);
    for (const Palette& p : s.palettes)
        ensureMaterial(s, f, p.name);
}

/** Sheet trims ordered top to bottom then left to right, among the same class (strips or rectangles) as trim i. */
std::vector<int> neighbourOrder(const Sheet& s, int i)
{
    auto isStrip = [&](int k) {
        const uv::TrimKind kind = s.trims[size_t(k)].kind;
        return kind == uv::TrimKind::StripU || kind == uv::TrimKind::StripV;
    };
    std::vector<int> out;
    for (int k = 0; k < int(s.trims.size()); ++k)
        if (isStrip(k) == isStrip(i) && !s.trims[size_t(k)].reserved)
            out.push_back(k);
    std::ranges::stable_sort(out, [&](int a, int b) {
        const Trim &ta = s.trims[size_t(a)], &tb = s.trims[size_t(b)];
        return ta.min.y != tb.min.y ? ta.min.y < tb.min.y : ta.min.x < tb.min.x;
    });
    return out;
}

uv::HotspotOptions hotspotOptions(const Args& a)
{
    uv::HotspotOptions o;
    o.splitAngle = a.get<double>("splitAngle", o.splitAngle);
    const std::string orient = a.get<std::string>("orient", "worldUp");
    o.orient = orient == "asIs" ? uv::HotspotOrient::AsIs : orient == "longestU" ? uv::HotspotOrient::LongestU : uv::HotspotOrient::WorldUp;
    o.tag = a.get<std::string>("tag", "");
    o.seed = uint32_t(a.get<int64_t>("seed", 0));
    o.cycle = int(a.get<int64_t>("cycle", 0));
    o.randomOffset = a.get<bool>("randomOffset", true);
    o.randomFlip = a.get<bool>("randomFlip", false);
    o.randomMirror = a.get<bool>("randomMirror", false);
    o.maxSizeError = a.get<double>("maxSizeError", o.maxSizeError);
    o.aspectWeight = a.get<double>("aspectWeight", o.aspectWeight);
    o.sizeWeight = a.get<double>("sizeWeight", o.sizeWeight);
    o.normalWeight = a.get<double>("normalWeight", o.normalWeight);
    o.allowRotate = a.get<bool>("allowRotate", true);
    o.sliceOversized = a.get<bool>("sliceOversized", true);
    return o;
}

std::vector<FlagDef> hotspotFlags()
{
    return {sheetFlag(),
            uvSetFlag(),
            {"splitAngle", "sa", AT::Float, 30.0, "Faces meeting sharper than this, degrees, become separate islands; 0 keeps the seams.", {}, {}, 0.0, 180.0},
            {"orient", "o", AT::String, "worldUp", "worldUp (up runs up the trim), longestU or asIs.", {"worldUp", "longestU", "asIs"}},
            {"tag", "tg", AT::String, "", "Only trims with this tag (edge, radial, flat or your own); unique and flat trims need one."},
            {"seed", "s", AT::Int, int64_t(0), "Variation seed: offsets along strips, picks among equal trims."},
            {"cycle", "c", AT::Int, int64_t(0), "Take the n-th best trim instead of the best: run again with cycle+1 to try the next."},
            {"randomOffset", "ro", AT::Bool, true, "Slide islands along tiling strips so repeats don't line up."},
            {"randomFlip", "rf", AT::Bool, false, "Turn islands 180 degrees at random where the trim allows it."},
            {"randomMirror", "rm", AT::Bool, false, "Mirror islands at random."},
            {"allowRotate", "ar", AT::Bool, true, "Let islands turn 90 degrees to fit."},
            {"sliceOversized", "so", AT::Bool, true,
             "Cut islands taller than every strip into courses at the tallest strip's height, so walls read as rows of boards or panels at true scale."},
            {"maxSizeError", "mse", AT::Float, 1.0, "Islands off a trim's world size by more than this many doublings count as poor matches.", {}, {}, 0.0, 8.0},
            {"aspectWeight", "aw", AT::Float, 1.0, "How much shape matters.", {}, {}, 0.0, 10.0},
            {"sizeWeight", "sw", AT::Float, 1.0, "How much world size matters.", {}, {}, 0.0, 10.0},
            {"normalWeight", "nw", AT::Float, 0.5, "How much facing matters, for trims with a preferred direction.", {}, {}, 0.0, 10.0}};
}

/** Where a sheet keeps a copy of its high meshes, so it bakes the same in any scene. */
std::filesystem::path highFile(const Sheet& s, const std::filesystem::path& f) { return textureFolder(s, f) / (s.name + "_high.obj"); }

/**
 * The sheet's high meshes in sheet space: from the scene when they are all there (world space, which trimSheetGenerate
 * lays out as sheet space), saved beside the textures for next time; else the copy saved by the last bake that had them.
 */
std::vector<geo::Mesh> highMeshes(const Sheet& s, const std::filesystem::path& f, const char* cmd)
{
    std::vector<geo::Mesh> out;
    if (s.high.empty())
        return out;
    Graph& g = Graph::current();
    std::vector<std::string> missing;
    for (const std::string& name : s.high)
    {
        Node* n = g.find(name);
        if (!n || !n->type().isA("mesh"))
        {
            missing.push_back(name);
            continue;
        }
        geo::Mesh m = *std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
        const glm::dmat4 world = std::get<glm::dmat4>(g.value({n->id(), scene::Transform::kWorldMatrix, -1}));
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                m.setPosition(p, glm::dvec3(world * glm::dvec4(m.position(p), 1.0)));
        out.push_back(std::move(m));
    }
    const std::filesystem::path saved = highFile(s, f);
    if (missing.empty())
    {
        geo::Mesh all;
        for (const geo::Mesh& m : out)
            all.append(m);
        std::filesystem::create_directories(saved.parent_path());
        if (!geo::writeObjFile(saved.string(), all))
            log::warn("{}: couldn't save the high meshes to {}", cmd, saved.generic_string());
        return out;
    }
    geo::ObjReadOptions ro;
    ro.splitObjects = false;
    const geo::ObjReadResult r = std::filesystem::exists(saved) ? geo::readObjFile(saved.string(), ro) : geo::ObjReadResult{};
    if (r.objects.empty())
        throw CommandError(std::string(cmd) + ": the sheet's high meshes (" + missing.front() + "...) aren't in the scene and none were saved; "
                           "open the scene that has them, or trimSheetBake(high=[]) to bake without them");
    out.clear();
    for (const geo::ObjObject& o : r.objects)
        out.push_back(o.mesh);
    return out;
}

/** The bake result recorded beside the textures, for sends. */
std::filesystem::path bakeRecord(const Sheet& s, const std::filesystem::path& f) { return textureFolder(s, f) / (s.name + "_bake.json"); }

void syncSheetSets(const Sheet& s, const std::filesystem::path& f);

json runBake(const Sheet& s, const std::filesystem::path& f, const SheetBakeOptions& opts, const char* cmd)
{
    SheetBakeOptions o = opts;
    if (o.high.empty())
        o.high = highMeshes(s, f, cmd);
    json r;
    try
    {
        r = bakeSheet(s, f, o);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": baking failed: " + e.what());
    }
    if (o.only.empty())
    {
        std::ofstream out(bakeRecord(s, f));
        out << r.dump(2);
    }
    ensureMaterials(s, f);
    syncSheetSets(s, f);
    return r;
}

/** Layer at the bottom of a sheet's texture set carrying its baked normal and AO under the smart materials. */
constexpr const char* kBakeLayer = "Sheet Bake";
/** Used when neither the call nor the sheet names a smart material. */
constexpr const char* kDefaultSmartMaterial = "Trim Painted Metal";

/** The texture set painting a palette's material: TS_<name without T_>[_<palette>]. */
std::string textureSetName(const Sheet& s, const std::string& palette = {}) { return "TS_" + materialName(s, palette).substr(2); }

bool hasTextureSet(const std::string& set)
{
    const json sets = ops::execute("texSets", json::object());
    return std::ranges::find(sets, json(set)) != sets.end();
}

/** A bake project named after the sheet and pointed at its texture folder: texture sets read the sheet's bake through it. */
std::string ensureBakeProject(const Sheet& s, const std::filesystem::path& f, const char* cmd)
{
    const std::string folder = paths::storedPath(textureFolder(s, f));
    if (Node* n = Graph::current().find(s.name))
    {
        if (n->type().name != "bakeProject")
            throw CommandError(std::string(cmd) + ": " + s.name + " is taken by a node that isn't a bake project");
        ops::execute("setBakeOutput", {{"project", s.name}, {"folder", folder}});
    }
    else
        ops::execute("createBakeProject", {{"name", s.name}, {"folder", folder}});
    return s.name;
}

json bakeLayerGraph()
{
    return {{"name", "sheetBake"},
            {"typeVersion", 1},
            {"inputs", json::array()},
            {"outputs", {{{"name", "normal"}, {"type", "color"}}, {{"name", "ao"}, {"type", "gray"}}}},
            {"nodes",
             {{{"name", "GroupInput"}, {"type", "GroupInput"}, {"position", {-400, 0}}},
              {{"name", "Normal"}, {"type", "trimSheetNormal"}, {"position", {-200, 0}}},
              {{"name", "AO"}, {"type", "meshAo"}, {"position", {-200, 150}}},
              {{"name", "GroupOutput"}, {"type", "GroupOutput"}, {"position", {200, 0}}}}},
            {"links", {{"Normal", "out", "GroupOutput", "normal"}, {"AO", "out", "GroupOutput", "ao"}}}};
}

/**
 * With a detail normal baked, the set shows and exports its normal rebuilt from the sheet's baked height, as the Unreal
 * master draws it; without one, as painted.
 */
void syncHeightNormals(const Sheet& s, const std::filesystem::path& f, const std::string& set)
{
    const double depth = bakedHeightDepth(s, f);
    const bool on = depth > 0.0 && std::filesystem::exists(mapFile(s, f, "detailNormal"));
    const glm::dvec2 ext = s.worldExtent();
    json want = {{"normalFromHeight", on}};
    if (on)
        want.update({{"heightSource", "mesh"}, {"heightDepth", depth}, {"uvWidth", ext.x}, {"uvHeight", ext.y}});
    const json have = ops::execute("texGraphInfo", {{"args", {set}}}).value("settings", json::object());
    for (const auto& [k, v] : want.items())
        if (!have.contains(k) || have[k] != v)
        {
            ops::execute("texSetSettings", {{"args", {set, want}}});
            return;
        }
}

/** syncHeightNormals for the texture sets the sheet's palettes already have. */
void syncSheetSets(const Sheet& s, const std::filesystem::path& f)
{
    std::vector<std::string> palettes = {""};
    for (const Palette& p : s.palettes)
        palettes.push_back(p.name);
    for (const std::string& p : palettes)
        if (const std::string set = textureSetName(s, p); hasTextureSet(set))
            syncHeightNormals(s, f, set);
}

/**
 * A palette's texture set (the sheet's own for ""), bound to its material and reading the sheet's shared bake, made with a
 * Sheet Bake layer when missing. Bakes the sheet first when it has no textures.
 */
std::string ensureTextureSet(const Sheet& s, const std::filesystem::path& f, const char* cmd, const std::string& palette = {})
{
    if (!std::filesystem::exists(bakeRecord(s, f)))
        runBake(s, f, {}, cmd);
    else
        ensureMaterials(s, f);
    const std::string project = ensureBakeProject(s, f, cmd);
    const std::string set = textureSetName(s, palette);
    if (!hasTextureSet(set))
    {
        ops::execute("texCreateSet", {{"args", {set}},
                                      {"settings", {{"width", s.width}, {"height", s.height}, {"bakeProject", project}}},
                                      {"materials", {materialName(s, palette)}}});
        ops::execute("texAddLayer", {{"args", {set, "procedural"}}, {"name", kBakeLayer}, {"index", 0}, {"graph", bakeLayerGraph()}});
    }
    syncHeightNormals(s, f, set);
    return set;
}

/** A new mesh node holding m (replacing one of that name), its faces on a palette's material. Returns its path. */
std::string addTrimMesh(const Sheet& s, const std::filesystem::path& f, const std::string& name, geo::Mesh m, const std::string& palette = {})
{
    std::vector<uint32_t> faces;
    for (geo::Index fi = 0; fi < m.faceCapacity(); ++fi)
        if (m.faceValid(fi))
            faces.push_back(fi);
    scene::materials::assign(m, materialName(s, palette), faces);
    Graph& g = Graph::current();
    if (Node* old = g.find(name))
        ops::execute("delete", {{"args", {old->path()}}});
    const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
    Node* n = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    tagNode(n->path(), f);
    return n->path();
}

/** The trim= flag as one index, else the best strip for the width among trims tagged `tag` (or all strips). */
int stripTrimArg(const Sheet& s, const Args& a, double width, std::string_view tag, const char* cmd)
{
    if (a.has("trim"))
    {
        const json t = a.flags["trim"];
        if (t.is_string())
            if (const int tagged = bestStripTrim(s, width, t.get<std::string>()); tagged >= 0)
                return tagged;
        return trimIndex(s, t, cmd);
    }
    int i = tag.empty() ? -1 : bestStripTrim(s, width, tag);
    if (i < 0)
        i = bestStripTrim(s, width, "");
    if (i < 0)
        throw CommandError(std::string(cmd) + ": the sheet has no tiling strips");
    return i;
}

/** An axis-aligned box from lo to hi, cm, appended to m. */
void addBox(geo::Mesh& m, glm::dvec3 lo, glm::dvec3 hi)
{
    const glm::dvec3 pts[] = {{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
                              {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}};
    const geo::Index sizes[] = {4, 4, 4, 4, 4, 4};
    const geo::Index idx[] = {0, 1, 2, 3, 5, 4, 7, 6, 3, 2, 6, 7, 1, 0, 4, 5, 1, 5, 6, 2, 4, 0, 3, 7};
    m.append(geo::Mesh::fromPolygons(pts, sizes, idx));
}

/** A small blockout that exercises every kind of match: a wall, a beam, steps, a pillar, a plinth and a slab, laid along X. */
geo::Mesh testBlockout()
{
    geo::Mesh m;
    addBox(m, {0, 0, -20}, {400, 250, 0});
    addBox(m, {0, 0, 0}, {400, 12, 3});
    addBox(m, {480, 0, -25}, {780, 25, 0});
    for (int i = 0; i < 3; ++i)
        addBox(m, {860, 20.0 * i, -30.0 * (3 - i)}, {1010, 20.0 * (i + 1), 0});
    addBox(m, {1090, 0, -40}, {1130, 300, 0});
    addBox(m, {1210, 0, -100}, {1310, 10, 0});
    return m;
}

/** The default of a generator's input, from its definition (a graph generator's group inputs, else its ports). */
json generatorDefault(const std::string& type, const std::string& input)
{
    const lila::tex::NodeDef* def = lila::tex::findNode(type);
    if (!def)
        return nullptr;
    if (def->graph.is_object())
        for (const json& in : def->graph.value("inputs", json::array()))
            if (in.value("name", std::string()) == input)
                return in.value("default", json());
    if (const lila::tex::PortDecl* port = def->input(input))
        return port->defaultValue;
    return nullptr;
}

/** A generator's inputs that set a pattern's frequency over uv: scale, noiseScale, chipScale... */
std::vector<std::string> scaleInputs(const std::string& type)
{
    std::vector<std::string> out;
    const lila::tex::NodeDef* def = lila::tex::findNode(type);
    if (!def)
        return out;
    auto take = [&](const std::string& name) {
        if (name == "scale" || name.ends_with("Scale"))
            out.push_back(name);
    };
    if (def->graph.is_object())
        for (const json& in : def->graph.value("inputs", json::array()))
            take(in.value("name", std::string()));
    for (const lila::tex::PortDecl& in : def->inputs)
        take(in.id);
    return out;
}

/**
 * Smart materials size their generator patterns for one mesh, whose uvs cover about a metre. A sheet's uvs cover its whole
 * world width, so each generator in the layer (and its children) gets its scales multiplied by that width in metres: chips,
 * breakup and grime come out the size they would on a single prop.
 */
void scaleGenerators(const std::string& set, const json& layer, double factor)
{
    for (const json& m : layer.value("masks", json::array()))
    {
        if (m.value("kind", std::string()) != "generator")
            continue;
        const std::string type = m.value("type", std::string());
        json values = m.value("values", json::object());
        json patch = json::object();
        for (const std::string& name : scaleInputs(type))
        {
            const json base = values.contains(name) ? values[name] : generatorDefault(type, name);
            if (base.is_number())
                patch[name] = base.get<double>() * factor;
        }
        if (!patch.empty())
            ops::execute("texSetMask", {{"args", {set, layer.value("id", std::string()), m.value("id", std::string()), {{"values", patch}}}}});
    }
    for (const json& child : layer.value("children", json::array()))
        scaleGenerators(set, child, factor);
}

/** Runs of consecutive indices, as Trim Mask's first and last. */
std::vector<std::pair<size_t, size_t>> trimRuns(std::span<const size_t> sorted)
{
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i : sorted)
        if (!out.empty() && out.back().second + 1 == i)
            out.back().second = i;
        else
            out.push_back({i, i});
    return out;
}

/** The name= flag, else the first positional. */
std::string nameArg(const Args& a)
{
    if (a.has("name"))
        return a.get<std::string>("name");
    return !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
}

/** Merges the scalars=, vectors= and switches= flags into a palette's parameters; a null value removes one. */
void mergeParams(json& scalars, json& vectors, json& switches, const Args& a, const char* cmd)
{
    for (auto [flag, into] : {std::pair{"scalars", &scalars}, std::pair{"vectors", &vectors}, std::pair{"switches", &switches}})
    {
        if (!a.has(flag))
            continue;
        const json v = a.flags[flag];
        if (!v.is_object())
            throw CommandError(std::format("{}: {} is a dict of parameter names", cmd, flag));
        for (const auto& [k, x] : v.items())
        {
            const bool ok = x.is_null() || (std::string_view(flag) == "scalars" && x.is_number()) || (std::string_view(flag) == "switches" && x.is_boolean()) ||
                            (std::string_view(flag) == "vectors" && x.is_array() && x.size() >= 3);
            if (!ok)
                throw CommandError(std::format("{}: {}['{}'] should be {}", cmd, flag, k,
                                               std::string_view(flag) == "scalars" ? "a number" : std::string_view(flag) == "switches" ? "True or False" : "[r, g, b]"));
        }
        into->merge_patch(v);
    }
}

json paletteSummary(const Sheet& s)
{
    json out = json::array();
    auto entry = [&](const std::string& name, const std::string& material, const json& scalars, const json& vectors, const json& switches) {
        const std::string key = name == "Default" ? std::string() : name;
        const std::string set = textureSetName(s, key);
        out.push_back({{"name", name},
                       {"material", material},
                       {"slot", materialName(s, key)},
                       {"textureSet", set},
                       {"textured", hasTextureSet(set)},
                       {"instance", "MI_" + materialName(s, key).substr(2)},
                       {"scalars", scalars},
                       {"vectors", vectors},
                       {"switches", switches}});
    };
    entry("Default", s.material, s.scalars, s.vectors, s.switches);
    for (const Palette& p : s.palettes)
        entry(p.name, p.material.empty() ? s.material : p.material, p.scalars, p.vectors, p.switches);
    return out;
}

/** The sheet a node is mapped with (its trimSheet attribute, resolved), empty when none. */
std::filesystem::path sheetOf(const Node& n)
{
    Graph& g = Graph::current();
    try
    {
        const scene::PlugValue v = g.value(g.plug(n.path() + "." + kNodeSheet));
        if (const std::string* s = std::get_if<std::string>(&v); s && !s->empty())
            return paths::resolvePath(*s);
    }
    catch (const std::exception&)
    {
    }
    return {};
}

/** Re-runs the live hotspot modifiers mapping onto the sheet at f, after the file changed under them. */
void touchLive(const std::filesystem::path& f)
{
    Graph& g = Graph::current();
    for (const scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || n->type().name != kLiveModifier)
            continue;
        std::string p = n->as<std::string>(n->type().attrIndex("sheet"));
        if (p.empty())
            p = currentSheet();
        if (p.empty() || paths::resolvePath(p).lexically_normal() != f.lexically_normal())
            continue;
        const int rev = n->type().attrIndex("sheetRevision");
        g.setValue({id, rev, -1}, n->as<int64_t>(rev) + 1);
    }
}

/** Mesh nodes mapped onto the sheet at f (their trimSheet attribute names it), with their evaluated meshes. */
std::vector<std::pair<std::string, scene::MeshPtr>> sheetMeshes(const std::filesystem::path& f)
{
    std::vector<std::pair<std::string, scene::MeshPtr>> out;
    Graph& g = Graph::current();
    for (const scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (sheetOf(*n).lexically_normal() != f.lexically_normal())
            continue;
        if (const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1})))
            out.push_back({n->path(), m});
    }
    return out;
}

/**
 * A palette's exported normal (DirectX) as detail over the sheet's baked height, written beside it as <name>_detail: the
 * painted normal carries the bake's normal, which the master rebuilds from the height instead. Returns the output for it.
 */
json paintedDetail(const json& normalOut, const json& heightOut, glm::dvec2 extent, double depth)
{
    const std::filesystem::path file = paths::fromUtf8(normalOut.value("file", std::string()));
    const image::Image normal = image::read(file);
    const image::Image height = image::read(paths::fromUtf8(heightOut.value("file", std::string())));
    if (normal.width <= 0 || height.width <= 0)
        return normalOut;
    const std::filesystem::path out = file.parent_path() / (file.stem().string() + "_detail" + file.extension().string());
    image::write(out, bake::detailNormals(normal, height, {depth / extent.x, depth / extent.y}, 45.0, false), normalOut.value("bits", 8));
    json o = normalOut;
    o["file"] = paths::toUtf8(out);
    return o;
}

/** What trimSheetSend imports and makes: the texture files, and one master instance per palette. */
struct SendPlan
{
    json textures = json::array();
    json instances = json::array();
};

/**
 * The sheet's baked maps shared by every palette, each palette's painted maps (exported from its texture set when it has
 * one) and its instance: name, slot, textures, scalars, vectors and switches, as LilaLink's trimMaterials takes them.
 */
SendPlan sendPlan(const Sheet& s, const std::filesystem::path& f, const json& baked, const std::string& folder)
{
    SendPlan plan;
    auto asset = [&](const json& o) {
        const std::string name = link::textureAssetName(o.value("file", std::string()), o.value("udim", 1001));
        return folder + "/Textures/" + name + "." + name;
    };
    const std::map<std::string, std::string> params = {{"normal", "SheetNormalMap"},     {"ao", "SheetAOMap"},
                                                       {"curvature", "SheetCurvatureMap"}, {"cavity", "SheetCavityMap"},
                                                       {"height", "SheetHeightMap"},     {"detailNormal", "SheetDetailNormal"}};
    json shared = json::object();
    json height;
    for (const json& o : baked.value("outputs", json::array()))
        if (const auto param = params.find(o.value("map", std::string())); param != params.end())
        {
            shared[param->second] = asset(o);
            plan.textures.push_back(o);
            if (param->first == "height")
                height = o;
        }
    const glm::dvec2 ext = s.worldExtent();
    // With a detail normal and the height it was split from, the master rebuilds the relief's normals from the height.
    const json heightMeta = height.is_object() ? height.value("meta", json()) : json();
    const double depth = heightMeta.is_object() ? 2.0 * heightMeta.value("heightRange", 0.0) : 0.0;
    const bool canRebuild = shared.contains("SheetDetailNormal") && depth > 0.0;
    auto instance = [&](const std::string& palette, const json& scalars, const json& vectors, const json& switches) {
        json textures = shared;
        textures.erase("SheetDetailNormal");
        const bool rebuild = canRebuild && (!switches.is_object() || switches.value("UseHeightNormals", true));
        if (rebuild)
            textures["SheetNormalMap"] = shared["SheetDetailNormal"];
        bool painted = false;
        // A textured palette sends its painted maps too; its normal carries the layers' height over the bake.
        if (const std::string set = textureSetName(s, palette); hasTextureSet(set))
        {
            const std::filesystem::path out = textureFolder(s, f) / "Painted" / (palette.empty() ? std::string("Default") : palette);
            const json exported = ops::execute("texExport", {{"args", {set}}, {"preset", "unreal"}, {"folder", out.generic_string()}, {"incremental", true}});
            const std::map<std::string, std::string> paintParams = {{"baseColor", "SheetBaseColorMap"}, {"orm", "SheetORMMap"}, {"normal", "SheetNormalMap"}};
            int found = 0;
            for (const json& o : exported.value("outputs", json::array()))
                if (const auto param = paintParams.find(o.value("map", std::string())); param != paintParams.end())
                {
                    found += param->first != "normal";
                    const json sent = param->first == "normal" && rebuild ? paintedDetail(o, height, ext, depth) : o;
                    textures[param->second] = asset(sent);
                    plan.textures.push_back(sent);
                }
            painted = found == 2;
        }
        json sc = {{"SheetWidthCm", ext.x}, {"SheetHeightCm", ext.y}};
        if (rebuild)
            sc["HeightDepthCm"] = depth;
        sc.merge_patch(scalars);
        json sw = {{"UsePaintedMaps", painted}, {"UseHeightNormals", rebuild}};
        sw.merge_patch(switches);
        plan.instances.push_back({{"name", "MI_" + materialName(s, palette).substr(2)},
                                  {"slot", materialName(s, palette)},
                                  {"palette", palette.empty() ? std::string("Default") : palette},
                                  {"textures", textures},
                                  {"scalars", sc},
                                  {"vectors", vectors},
                                  {"switches", sw}});
    };
    instance("", s.scalars, s.vectors, s.switches);
    for (const Palette& p : s.palettes)
        instance(p.name, p.scalars, p.vectors, p.switches);
    return plan;
}

/** The decal= flag or first positional: a .lilatrim, or a decal's name under <workspace>/TrimSheets/Decals. */
std::filesystem::path decalPath(const Args& a, const char* cmd)
{
    std::string p = a.has("decal") ? a.get<std::string>("decal") : !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
    if (p.empty())
        throw CommandError(std::string(cmd) + ": give the decal (its name or .lilatrim)");
    std::filesystem::path f = paths::resolvePath(p);
    if (!std::filesystem::exists(f))
        f = defaultSheetFolder() / "Decals" / (p + kExtension);
    if (!std::filesystem::exists(f))
        throw CommandError(std::string(cmd) + ": no decal " + p + "; make it with trimDecalCreate");
    return f;
}

/** The decal a placed quad shows (its trimDecal attribute, resolved), empty for other nodes. */
std::filesystem::path decalOf(const Node& n)
{
    Graph& g = Graph::current();
    try
    {
        const scene::PlugValue v = g.value(g.plug(n.path() + "." + kNodeDecal));
        if (const std::string* s = std::get_if<std::string>(&v); s && !s->empty())
            return paths::resolvePath(*s);
    }
    catch (const std::exception&)
    {
    }
    return {};
}

/** Where a placed decal quad sits in the world: its centre, facing, the way its texture's top points and its size, cm. */
struct DecalFrame
{
    glm::dvec3 centre{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 up{0.0, 0.0, -1.0};
    glm::dvec2 size{0.0};
};

/** The frame of the quad's first face, from its corners and uvs in world space. Nullopt without uvs or with a degenerate face. */
std::optional<DecalFrame> decalFrame(const geo::Mesh& m, const glm::dmat4& world)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    if (uv.empty())
        return std::nullopt;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<glm::dvec3> p;
        std::vector<glm::dvec2> t;
        m.forEachFaceCorner(f, [&](geo::Index k) {
            p.push_back(glm::dvec3(world * glm::dvec4(m.position(m.cornerPoint(k)), 1.0)));
            t.push_back(glm::dvec2(uv[k]));
        });
        if (p.size() < 3)
            return std::nullopt;
        // dP/du and dP/dv from the first triangle: [e1 e2] = [dPdu dPdv] [duv1 duv2].
        const glm::dvec2 d1 = t[1] - t[0], d2 = t[2] - t[0];
        const double det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12)
            return std::nullopt;
        const glm::dvec3 e1 = p[1] - p[0], e2 = p[2] - p[0];
        const glm::dvec3 dPdu = (e1 * d2.y - e2 * d1.y) / det, dPdv = (e2 * d1.x - e1 * d2.x) / det;
        DecalFrame out;
        glm::dvec2 lo(1e300), hi(-1e300);
        for (size_t i = 0; i < p.size(); ++i)
        {
            out.centre += p[i];
            lo = glm::min(lo, t[i]);
            hi = glm::max(hi, t[i]);
        }
        out.centre /= double(p.size());
        out.normal = glm::normalize(glm::cross(dPdu, dPdv));
        out.up = glm::normalize(dPdv);
        out.size = {glm::length(dPdu) * (hi.x - lo.x), glm::length(dPdv) * (hi.y - lo.y)};
        return out;
    }
    return std::nullopt;
}
} // namespace

std::string materialName(const Sheet& s)
{
    std::string base = s.name;
    if (base.starts_with("T_"))
        base = base.substr(2);
    return "M_" + base;
}

std::string materialName(const Sheet& s, std::string_view palette)
{
    return palette.empty() ? materialName(s) : materialName(s) + "_" + std::string(palette);
}

std::vector<std::string> paletteMaterials(const Sheet& s)
{
    std::vector<std::string> out = {materialName(s)};
    for (const Palette& p : s.palettes)
        out.push_back(materialName(s, p.name));
    return out;
}

std::string currentSheet() { return PrefStore::get().value<std::string>(kPrefs, "/current", std::string()); }

void setCurrentSheet(const std::string& path) { PrefStore::get().set(kPrefs, "/current", path); }

std::filesystem::path defaultSheetFolder()
{
    if (Workspace::get().isOpen())
        return Workspace::get().root() / "TrimSheets";
    return link::cacheDir().parent_path() / "TrimSheets";
}

void registerCommands(ext::Context& ctx)
{
    // A scene with meshes mapped onto a sheet makes that sheet current when it opens, so its palette and tools are ready.
    ctx.subscribe(events::Event::SceneOpened, [](const events::EventData&) {
        Graph& g = Graph::current();
        for (const scene::NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                try
                {
                    const json v = ops::execute("getAttr", {{"args", {n->path() + "." + kNodeSheet}}});
                    if (const std::filesystem::path f = v.is_string() ? paths::resolvePath(v.get<std::string>()) : std::filesystem::path();
                        !f.empty() && std::filesystem::exists(f))
                    {
                        setCurrentSheet(f.generic_string());
                        return;
                    }
                }
                catch (const std::exception&)
                {
                }
    });

    ctx.registerCommand({
        .name = "trimSheetCreate",
        .doc = "Make a trim sheet and bake it: the layout and every trim's profile go in a .lilatrim file, the generated high is "
               "baked onto a flat plane into normal, AO, curvature, height and cavity maps plus an exact trim ID map, and a Lila "
               "material M_<name> shows them. Without a preset the sheet is blank (width x height, no trims, or full-width raised "
               "strips of the heights in strips=); add trims with trimSheetEdit(add=...). An existing file is kept (its settings "
               "may have been changed) and only baked when its textures are missing, so calling this again is cheap. Presets: " +
               presetChoices() + ". The new sheet becomes current. Returns the sheet summary (trims with world sizes, textures, "
               "warnings) and the bake result.",
        .flags = {{"preset", "p", AT::String, "blank", "Layout and profiles to start from; blank starts empty.",
                   [] {
                       std::vector<std::string> ids = presetIds();
                       ids.insert(ids.begin(), "blank");
                       return ids;
                   }()},
                  {"height", "h", AT::Int, int64_t(0), "Texture height of a blank sheet, px (0: square).", {}, {}, 0.0, 16384.0},
                  {"strips", "st", AT::List, nullptr, "Blank sheet: full-width raised strips of these heights, px, top to bottom."},
                  {"width", "w", AT::Int, int64_t(0), "Texture width, px (0: the preset's, 2048 for a blank sheet).", {}, {}, 0.0, 16384.0},
                  {"file", "f", AT::String, nullptr, "Where to save; default <workspace>/TrimSheets/<name>.lilatrim."},
                  {"name", "n", AT::String, nullptr, "Sheet name (texture prefix); default the preset's, T_<Preset>."},
                  {"pxPerM", "", AT::Float, nullptr, "Texels per metre: sets every trim's world size.", {}, {}, 1.0},
                  {"bake", "b", AT::Bool, true, "Bake when the textures are missing."},
                  {"overwrite", "", AT::Bool, false, "Replace an existing file with the preset."},
                  {"backend", "", AT::String, "auto", "Bake backend: auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Sheet s;
            const std::string presetId = a.get<std::string>("preset", "blank");
            if (presetId == "blank")
            {
                const int w = int(a.get<int64_t>("width", 0)) > 0 ? int(a.get<int64_t>("width", 0)) : 2048;
                const int h = int(a.get<int64_t>("height", 0)) > 0 ? int(a.get<int64_t>("height", 0)) : w;
                std::vector<int> heights;
                if (a.has("strips"))
                    for (const json& v : a.flags["strips"])
                    {
                        if (!v.is_number() || v.get<double>() < 1.0)
                            throw CommandError("trimSheetCreate: strips are heights in px, like [256, 128, 64]");
                        heights.push_back(int(v.get<double>()));
                    }
                s = stripLadder("T_TrimSheet", w, h, heights);
            }
            else
                try
                {
                    s = preset(presetId, int(a.get<int64_t>("width", 0)));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("trimSheetCreate: ") + e.what());
                }
            if (a.has("name"))
                s.name = a.get<std::string>("name");
            if (a.has("pxPerM"))
                s.pxPerM = std::max(1.0, a.get<double>("pxPerM"));
            std::filesystem::path f = a.has("file") ? std::filesystem::path(a.get<std::string>("file")) : defaultSheetFolder() / (s.name + kExtension);
            if (f.extension().empty())
                f += kExtension;
            const bool existed = std::filesystem::exists(f);
            if (existed && !a.get<bool>("overwrite", false))
                s = loadOr(f, "trimSheetCreate");
            else
                save(s, f);
            setCurrentSheet(f.generic_string());

            json out = sheetSummary(s, f);
            out["created"] = !existed || a.get<bool>("overwrite", false);
            if (a.get<bool>("bake", true) && !s.trims.empty() && (!out["baked"].get<bool>() || out["created"].get<bool>()))
            {
                SheetBakeOptions o;
                o.backend = a.get<std::string>("backend", "auto");
                out["bake"] = runBake(s, f, o, "trimSheetCreate");
                out["textures"] = sheetSummary(s, f)["textures"];
                out["baked"] = true;
            }
            return out;
        },
        .examples = {"trimSheetCreate(preset='sciFiHull')", "trimSheetCreate(preset='woodTrims', width=1024, pxPerM=1024)['trims'][0]",
                     "trimSheetCreate(name='T_Hall', width=2048, height=1024, pxPerM=128, strips=[512, 256, 128, 64, 64])"},
    });

    ctx.registerCommand({
        .name = "trimSheetCurrent",
        .doc = "The current trim sheet, which trim tools use when no sheet= is given; give a .lilatrim to make it current.",
        .positionalDoc = "sheet to make current (optional)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.positional.empty() && a.positional[0].is_string())
            {
                const std::filesystem::path f = loadedPath(a, "trimSheetCurrent");
                setCurrentSheet(f.generic_string());
            }
            return currentSheet();
        },
    });

    ctx.registerCommand({
        .name = "trimSheetInfo",
        .doc = "What a trim sheet holds: size, texels per metre, world size, each trim (index, name, kind, pixel rect, uv rect, world "
               "size in cm, tags, profile), which textures are baked, and layout warnings (overlaps, borders off the mip grid, "
               "mixed bevel widths).",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetInfo");
            return sheetSummary(loadOr(f, "trimSheetInfo"), f);
        },
        .examples = {"[t['name'] for t in trimSheetInfo()['trims']]"},
    });

    ctx.registerCommand({
        .name = "trimSheetEdit",
        .doc = "Change a trim sheet and save it. values= merges into the trim given by trim= (index or name), or into the sheet "
               "itself without trim=: e.g. {'profile': {'style': 'groove', 'bevelPx': 6}}, {'tags': ['edge']}, {'pxPerM': 1024}. "
               "add= appends a trim ({'name', 'min': [x, y], 'max': [x, y], 'kind', 'profile'}) and remove= deletes one. Profile "
               "styles: flat, raised, recessed, doubleBevel, step, halfPipe, groove, ribs, rivets, bolts, vents, tread, planks. "
               "Kinds: stripU, stripV, rect, radial, unique, flat. Rebake afterwards (trimSheetBake, trims= for just the changed "
               "ones). Returns the sheet summary.",
        .flags = {sheetFlag(),
                  {"trim", "t", AT::Any, nullptr, "Trim index or name to change."},
                  {"values", "v", AT::Any, nullptr, "Fields to merge."},
                  {"add", "", AT::Any, nullptr, "A trim to append."},
                  {"remove", "", AT::Any, nullptr, "Trim index or name to delete."},
                  {"moveBorder", "", AT::Any, nullptr,
                   "Drag a border: {axis: 'x' or 'y', at: px of the line, along: px on it, to: px}. Moves it for the whole strip or row of "
                   "trims on the line, never thinner than a pixel."},
                  {"split", "", AT::Any, nullptr, "Split a trim in two: {trim, axis: 'x' (a vertical cut) or 'y', at: px}."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetEdit");
            Sheet s = loadOr(f, "trimSheetEdit");
            if (a.has("values"))
            {
                const json v = a.flags["values"];
                if (!v.is_object())
                    throw CommandError("trimSheetEdit: values is a dict of fields");
                if (a.has("trim"))
                {
                    const int i = trimIndex(s, a.flags["trim"], "trimSheetEdit");
                    json t = s.trims[size_t(i)].toJson();
                    t.merge_patch(v);
                    s.trims[size_t(i)] = Trim::fromJson(t);
                }
                else
                {
                    json j = s.toJson();
                    j.merge_patch(v);
                    s = Sheet::fromJson(j);
                }
            }
            if (a.has("add"))
                s.trims.push_back(Trim::fromJson(a.flags["add"]));
            if (a.has("remove"))
                s.trims.erase(s.trims.begin() + trimIndex(s, a.flags["remove"], "trimSheetEdit"));
            json changed = json::array();
            if (a.has("moveBorder"))
            {
                const json m = a.flags["moveBorder"];
                if (!m.is_object() || !m.contains("at") || !m.contains("to"))
                    throw CommandError("trimSheetEdit: moveBorder needs at and to");
                const std::vector<size_t> moved = moveBorder(s, m.value("axis", std::string("y")) == "x" ? 0 : 1, m["at"].get<int>(),
                                                             m.value("along", 0), m["to"].get<int>());
                if (moved.empty())
                    throw CommandError("trimSheetEdit: no border at " + m["at"].dump());
                changed = moved;
            }
            if (a.has("split"))
            {
                const json sp = a.flags["split"];
                if (!sp.is_object() || !sp.contains("trim") || !sp.contains("at"))
                    throw CommandError("trimSheetEdit: split needs trim and at");
                const int i = trimIndex(s, sp["trim"], "trimSheetEdit");
                if (!splitTrim(s, size_t(i), sp.value("axis", std::string("y")) == "x" ? 0 : 1, sp["at"].get<int>()))
                    throw CommandError("trimSheetEdit: " + sp["at"].dump() + " isn't inside trim " + std::to_string(i));
                changed = {i, i + 1};
            }
            save(s, f);
            touchLive(f);
            json out = sheetSummary(s, f);
            if (!changed.empty())
                out["changed"] = changed;
            return out;
        },
        .examples = {"trimSheetEdit(trim='Plate128', values={'profile': {'style': 'rivets', 'spacingPx': 48}})",
                     "trimSheetEdit(add={'name': 'Cap', 'min': [0, 1792], 'max': [256, 2048], 'kind': 'radial', 'profile': {'style': 'halfPipe'}})"},
    });

    ctx.registerCommand({
        .name = "trimSheetGenerate",
        .doc = "Put a sheet's generated high and low in the scene as <name>_High and <name>_Low (replacing earlier ones), to look at "
               "or to sculpt on before baking from them yourself. The plane lies on XZ facing +Y, one texel per pxPerM. Returns both "
               "names and the high's face count.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(), {"tile", "", AT::Bool, false, "Repeat tiling strips one sheet width to each side, as the bake does."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetGenerate");
            const Sheet s = loadOr(f, "trimSheetGenerate");
            GenerateOptions go;
            go.tile = a.get<bool>("tile", false);
            Generated gen = generate(s, go);
            Graph& g = Graph::current();
            json out = json::object();
            for (auto [suffix, mesh] : {std::pair<const char*, geo::Mesh*>{"_High", &gen.high}, {"_Low", &gen.low}})
            {
                const std::string name = s.name + suffix;
                if (Node* old = g.find(name))
                    ops::execute("delete", {{"args", {old->path()}}});
                json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
                const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                Node* n = g.find(path);
                g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(*mesh))));
                out[std::string(suffix).substr(1) == "High" ? "high" : "low"] = path;
            }
            out["highFaces"] = int64_t(gen.high.numFaces());
            out["reliefCm"] = {gen.above, gen.below};
            return out;
        },
    });

    ctx.registerCommand({
        .name = "trimSheetBake",
        .doc = "Bake a trim sheet: its generated high onto the flat low, tiling strips wrapped so the textures tile, maps per the "
               "sheet's maps list, into its texture folder as T_<name>_<map>.png, plus an exact trim ID map. trims= rebakes just "
               "those trims into the existing textures (after trimSheetEdit). Updates the Lila material M_<name>. Returns the "
               "Baker's result, the files, seamError (0 is seamless across the U wrap) and seconds.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"trims", "t", AT::List, nullptr, "Trim indices or names to rebake; all when not given."},
                  {"backend", "", AT::String, "auto", "auto, gpu or cpu.", {"auto", "gpu", "cpu"}},
                  {"high", "hi", AT::List, nullptr,
                   "Scene meshes to bake as extra high poly detail, laid on the sheet as trimSheetGenerate shows it; remembered by the "
                   "sheet for later bakes. An empty list goes back to the generated relief alone."},
                  {"highReplaces", "hr", AT::Bool, nullptr, "The high meshes are the whole high poly, instead of adding to the generated one."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetBake");
            Sheet s = loadOr(f, "trimSheetBake");
            if (a.has("high") || a.has("highReplaces"))
            {
                if (a.has("high"))
                    s.high = a.flags["high"].get<std::vector<std::string>>();
                s.highReplaces = a.get<bool>("highReplaces", s.highReplaces);
                save(s, f);
            }
            SheetBakeOptions o;
            o.backend = a.get<std::string>("backend", "auto");
            if (a.has("trims"))
                for (const json& t : a.flags["trims"])
                    o.only.push_back(size_t(trimIndex(s, t, "trimSheetBake")));
            return runBake(s, f, o, "trimSheetBake");
        },
        .examples = {"trimSheetBake()", "trimSheetBake(trims=['Plate128'])"},
    });

    ctx.registerCommand({
        .name = "trimSheetImport",
        .doc = "Make a trim sheet from another tool's layout: a Valve .rect (needs width and height), an SVG of rects, an ID map "
               "image (one trim per flat colour), ZenUV trims (its JSON, as Arcbrush copies it, or a ZenUV trim preset .py, read "
               "and never run; or that JSON pasted as text=), or a mesh's uv islands (source is a mesh name). Full-width rows "
               "become tiling strips, the rest hotspot rectangles, all with a raised bevel profile to change with trimSheetEdit. "
               "Saves to file= (default beside the source, or the sheet folder for text) and makes it current. Returns the sheet "
               "summary.",
        .positionalDoc = "source file or mesh",
        .flags = {{"file", "f", AT::String, nullptr, "Where to save the .lilatrim."},
                  {"text", "t", AT::String, nullptr, "ZenUV trim JSON instead of a source, such as Arcbrush's copied trim data."},
                  {"width", "w", AT::Int, int64_t(2048), "Sheet width, px, for .rect and mesh sources.", {}, {}, 16.0, 16384.0},
                  {"height", "h", AT::Int, int64_t(2048), "Sheet height, px, for .rect and mesh sources.", {}, {}, 16.0, 16384.0},
                  {"name", "n", AT::String, nullptr, "Sheet name; default T_<source name>."},
                  {"pxPerM", "", AT::Float, 512.0, "Texels per metre.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const int w = int(a.get<int64_t>("width", 2048)), h = int(a.get<int64_t>("height", 2048));
            if (a.positional.empty() && !a.has("text"))
                throw CommandError("trimSheetImport: give a .rect, .svg, .json, .py or image file, a mesh, or text=");
            const std::string src = a.positional.empty() ? std::string("pasted trim data") : a.positional[0].get<std::string>();
            Sheet s;
            std::filesystem::path out;
            Graph& g = Graph::current();
            if (a.positional.empty())
            {
                s = fromZenUv(a.get<std::string>("text"), w, h);
                s.name = "T_Imported";
                out = defaultSheetFolder() / ("T_Imported" + std::string(kExtension));
            }
            else if (Node* n = g.find(src); n && n->type().isA("mesh"))
            {
                g.evaluate();
                const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
                if (!m)
                    throw CommandError("trimSheetImport: " + src + " has no mesh");
                s = fromUvs(*m, "uv0", w, h);
                out = defaultSheetFolder() / ("T_" + n->name() + kExtension);
                s.name = "T_" + n->name();
            }
            else
            {
                const std::filesystem::path p(src);
                std::string ext = p.extension().string();
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                std::ifstream in(p, std::ios::binary);
                if (!in)
                    throw CommandError("trimSheetImport: can't read " + src);
                std::stringstream text;
                text << in.rdbuf();
                if (ext == ".rect")
                    s = fromRect(text.str(), w, h);
                else if (ext == ".svg")
                    s = fromSvg(text.str());
                else if (ext == ".json" || ext == ".txt" || ext == ".py")
                    s = fromZenUv(text.str(), w, h);
                else
                {
                    std::string err;
                    const image::Image img = image::read(p, &err);
                    if (img.width == 0)
                        throw CommandError("trimSheetImport: " + src + ": " + err);
                    s = fromIdMap(img);
                }
                s.name = "T_" + p.stem().string();
                out = p.parent_path() / (p.stem().string() + kExtension);
            }
            if (s.trims.empty())
                throw CommandError("trimSheetImport: found no trims in " + src);
            if (a.has("name"))
                s.name = a.get<std::string>("name");
            s.pxPerM = std::max(1.0, a.get<double>("pxPerM", 512.0));
            if (a.has("file"))
                out = a.get<std::string>("file");
            save(s, out);
            setCurrentSheet(out.generic_string());
            return sheetSummary(s, out);
        },
        .examples = {"trimSheetImport('C:/maps/metal_trims.rect', width=1024, height=1024)", "trimSheetImport('C:/maps/hotspots_id.png')",
                     "trimSheetImport('C:/maps/Crate_TrimPreset.py')"},
    });

    ctx.registerCommand({
        .name = "trimSheetExport",
        .doc = "Write a trim sheet's layout for another tool: rect (Valve .rect for Source 2 and Hammer), svg, idmap (a png with "
               "one flat colour per trim) or json. Returns the file written.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"format", "fmt", AT::String, "rect", "rect, svg, idmap or json.", {"rect", "svg", "idmap", "json"}},
                  {"file", "f", AT::String, nullptr, "Output file; default beside the sheet."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetExport");
            const Sheet s = loadOr(f, "trimSheetExport");
            const std::string fmt = a.get<std::string>("format", "rect");
            const char* ext = fmt == "svg" ? ".svg" : fmt == "idmap" ? ".png" : fmt == "json" ? ".json" : ".rect";
            const std::filesystem::path out = a.has("file") ? std::filesystem::path(a.get<std::string>("file")) : f.parent_path() / (f.stem().string() + ext);
            if (fmt == "idmap")
                image::write(out, idMap(s), 8);
            else
            {
                std::ofstream o(out, std::ios::binary);
                if (!o)
                    throw CommandError("trimSheetExport: can't write " + out.generic_string());
                o << (fmt == "svg" ? toSvg(s) : fmt == "json" ? s.toJson().dump(2) : toRect(s));
            }
            return out.generic_string();
        },
    });

    ctx.registerCommand({
        .name = "trimApply",
        .doc = "Map the selected faces (whole selected meshes in object mode) onto one trim: each island is split by angle, "
               "rectified, turned to suit the trim and fitted to it (stretched to a strip's height, or fitted inside a "
               "rectangle). Along a strip, a run spans a whole number of sheet widths, so copies laid end to "
               "end tile, and runs on round corners. Faces get the sheet's material and remember their trim (face int 'trim', index + 1). "
               "Returns per mesh the islands placed.",
        .flags = [] {
            std::vector<FlagDef> f = {sheetFlag(),
                                      uvSetFlag(),
                                      {"trim", "t", AT::Any, nullptr, "Trim index or name."},
                                      {"rotate", "r", AT::String, "auto", "auto (whichever fits), on or off.", {"auto", "on", "off"}},
                                      {"splitAngle", "sa", AT::Float, 30.0, "Split islands where faces meet sharper than this, degrees.", {}, {}, 0.0, 180.0},
                                      {"orient", "o", AT::String, "worldUp", "worldUp, longestU or asIs.", {"worldUp", "longestU", "asIs"}},
                                      {"seed", "s", AT::Int, int64_t(0), "Offset along strips."},
                                      paletteFlag(),
                                      {"randomOffset", "ro", AT::Bool, false,
                                       "Slide islands to a random place along tiling strips; off starts each run on the sheet's edge and spans whole sheets, so copies tile."}};
            return f;
        }(),
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimApply");
            const Sheet s = loadOr(f, "trimApply");
            if (!a.has("trim"))
                throw CommandError("trimApply: give trim= (index or name)");
            const int ti = trimIndex(s, a.flags["trim"], "trimApply");
            const std::optional<std::string> palette = paletteArg(s, a, "trimApply");
            const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
            const std::string set = a.get<std::string>("uvSet", "uv0");
            const std::string rotate = a.get<std::string>("rotate", "auto");
            uv::HotspotOptions o = hotspotOptions(a);
            o.randomOffset = a.get<bool>("randomOffset", false);
            std::vector<std::string> nodes;
            json out = uv::editUvMeshes("Apply Trim", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node& node) -> json {
                const std::vector<geo::Index> faces = facesOrAll(m, ids);
                std::vector<uv::Chart> charts = freshIslands(m, faces, set, o.splitAngle);
                for (uv::Chart& ch : charts)
                {
                    const uv::IslandMeasure ms = uv::measureChart(ch, o);
                    const bool rot = rotate == "on" || (rotate == "auto" && bestRotation(ms, trims[size_t(ti)]));
                    const uint64_t key = ch.faces.empty() ? 0 : ch.faces.front();
                    uv::placeChart(ch, trims[size_t(ti)], rot, o, uv::hotspotRandom(o.seed, key, 1), uv::hotspotRandom(o.seed, key, 2),
                                   uv::hotspotRandom(o.seed, key, 3));
                }
                const std::vector<int> trimOf(charts.size(), ti);
                uv::chainStripCharts(charts, trimOf, trims, !o.randomOffset);
                uv::writeCharts(m, charts, set);
                tagFaces(m, faces, ti, s, palette);
                nodes.push_back(node.path());
                return {{"islands", int64_t(charts.size())}, {"trim", s.trims[size_t(ti)].name}};
            });
            for (const std::string& n : nodes)
                tagNode(n, f);
            return out;
        },
        .examples = {"trimApply(trim='Plate128')", "trimApply(trim=3, rotate='on')"},
    });

    ctx.registerCommand({
        .name = "trimAuto",
        .doc = "Hotspot the selected faces (whole selected meshes in object mode) onto a trim sheet in one go: islands split by "
               "angle and rectified, measured in world size from 3D, matched to the trim whose shape and size fit best (strips "
               "for long islands, rectangles for panels, round trims for discs), then placed with seeded variation. Same seed, "
               "same result; cycle=n takes the n-th best match. showError paints a colour set trimError (green good, yellow loose, "
               "red poor or unmatched). Faces get the sheet's material and remember their trim. Returns per mesh: islands, "
               "matched, poor, failed, and each island's trim and size error.",
        .flags = [] {
            std::vector<FlagDef> f = hotspotFlags();
            f.push_back(paletteFlag());
            f.push_back({"showError", "se", AT::Bool, false, "Paint match quality into the colour set trimError."});
            return f;
        }(),
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimAuto");
            const Sheet s = loadOr(f, "trimAuto");
            const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
            const std::string set = a.get<std::string>("uvSet", "uv0");
            const uv::HotspotOptions o = hotspotOptions(a);
            const std::optional<std::string> palette = paletteArg(s, a, "trimAuto");
            const bool showError = a.get<bool>("showError", false);
            std::vector<std::string> nodes;
            json out = uv::editUvMeshes("Auto Trim", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node& node) -> json {
                const std::vector<geo::Index> faces = facesOrAll(m, ids);
                const uv::HotspotResult r = uv::hotspot(m, faces, trims, set, o);
                json islands = json::array();
                if (showError)
                    geo::colors::add(m, "trimError", glm::vec4(1.0f));
                for (const uv::HotspotIsland& is : r.islands)
                {
                    tagFaces(m, is.faces, is.trim, s, palette);
                    islands.push_back({{"faces", int64_t(is.faces.size())},
                                       {"trim", is.trim >= 0 ? json(s.trims[size_t(is.trim)].name) : json(nullptr)},
                                       {"sizeError", is.sizeError},
                                       {"rotated", is.rotated},
                                       {"poor", is.poor},
                                       {"sizeCm", {is.measure.width, is.measure.height}}});
                    if (showError)
                    {
                        const glm::vec4 c = is.trim < 0 || is.poor ? glm::vec4(0.9f, 0.15f, 0.1f, 1.0f)
                                            : is.sizeError > o.maxSizeError * 0.5 ? glm::vec4(0.95f, 0.8f, 0.1f, 1.0f)
                                                                                  : glm::vec4(0.2f, 0.8f, 0.3f, 1.0f);
                        std::span<glm::vec4> col = geo::colors::values(m, "trimError");
                        for (geo::Index fc : is.faces)
                            m.forEachFaceCorner(fc, [&](geo::Index k) { col[k] = c; });
                    }
                }
                nodes.push_back(node.path());
                return {{"islands", int64_t(r.islands.size())},
                        {"matched", int64_t(r.matched)},
                        {"poor", int64_t(r.poor)},
                        {"failed", int64_t(r.failed)},
                        {"seamsAdded", int64_t(r.seamsAdded)},
                        {"details", islands}};
            });
            for (const std::string& n : nodes)
                tagNode(n, f);
            return out;
        },
        .examples = {"trimAuto()", "trimAuto(seed=3, randomFlip=True, showError=True)", "trimAuto(tag='edge', orient='longestU')"},
    });

    ctx.registerCommand({
        .name = "trimLive",
        .doc = "Keep the selected meshes trimmed while they change: adds a trimHotspot modifier to each one's stack, which "
               "hotspots the mesh under it onto the sheet whenever that mesh changes (a blockout edited, a generator's "
               "parameters, a modifier lower down) and when the sheet's layout is edited. Same matching and variation as "
               "trimAuto. Remove it, or apply the stack, to freeze the mapping. Returns {mesh: modifier}.",
        .flags = [] {
            std::vector<FlagDef> f = {sheetFlag(),
                                      uvSetFlag(),
                                      {"splitAngle", "sa", AT::Float, 30.0, "Faces meeting sharper than this, degrees, become separate islands.", {}, {}, 0.0, 180.0},
                                      {"orient", "o", AT::String, "worldUp", "worldUp, longestU or asIs.", {"worldUp", "longestU", "asIs"}},
                                      {"tag", "tg", AT::String, "", "Only trims with this tag."},
                                      {"seed", "s", AT::Int, int64_t(0), "Variation seed."},
                                      {"randomOffset", "ro", AT::Bool, true, "Slide islands along tiling strips."},
                                      {"randomFlip", "rf", AT::Bool, false, "Turn islands 180 degrees at random where the trim allows."},
                                      {"sliceOversized", "so", AT::Bool, true, "Cut islands taller than every strip into courses."},
                                      paletteFlag()};
            return f;
        }(),
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimLive", "sheet", false);
            const Sheet s = loadOr(f, "trimLive");
            const std::string palette = paletteArg(s, a, "trimLive").value_or(std::string());
            const std::string orient = a.get<std::string>("orient", "worldUp");
            const json params = {{"sheet", paths::storedPath(f)},
                                 {"uvSet", a.get<std::string>("uvSet", "uv0")},
                                 {"splitAngle", a.get<double>("splitAngle", 30.0)},
                                 {"orient", orient == "longestU" ? 1 : orient == "asIs" ? 2 : 0},
                                 {"tag", a.get<std::string>("tag", "")},
                                 {"seed", a.get<int64_t>("seed", 0)},
                                 {"randomOffset", a.get<bool>("randomOffset", true)},
                                 {"randomFlip", a.get<bool>("randomFlip", false)},
                                 {"sliceOversized", a.get<bool>("sliceOversized", true)}};
            Graph& g = Graph::current();
            std::vector<std::string> meshes;
            for (scene::NodeId id : g.selection())
                if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    meshes.push_back(n->path());
            if (meshes.empty())
                throw CommandError("trimLive: select the meshes to keep trimmed");
            json out = json::object();
            for (const std::string& m : meshes)
            {
                out[m] = ops::execute("addModifier", {{"args", {m}}, {"kind", "trimHotspot"}, {"params", params}});
                // After the sheet: params apply in name order, and the palette is checked against the sheet.
                if (!palette.empty())
                    ops::execute("setAttr", {{"args", {out[m].get<std::string>() + ".palette", palette}}});
                tagNode(m, f);
            }
            ensureMaterial(s, f, palette);
            return out;
        },
        .examples = {"trimLive()", "trimLive(seed=2, palette='Rust')"},
    });

    ctx.registerCommand({
        .name = "trimShift",
        .doc = "Move the selected faces' islands to the next trim up or down the sheet (Insomniac's shift UVs): strips step to the "
               "neighbouring strip, rectangles to the neighbouring rectangle, keeping how each island was turned. Faces must "
               "have been mapped by trimApply or trimAuto. Returns per mesh how many islands moved and where.",
        .flags = {sheetFlag(), uvSetFlag(), {"steps", "n", AT::Int, int64_t(1), "Trims to move; negative goes up the sheet.", {}, {}, -64.0, 64.0}},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimShift");
            const Sheet s = loadOr(f, "trimShift");
            const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
            const std::string set = a.get<std::string>("uvSet", "uv0");
            const int steps = int(a.get<int64_t>("steps", 1));
            uv::HotspotOptions o;
            o.orient = uv::HotspotOrient::AsIs;
            o.randomOffset = false;
            return uv::editUvMeshes("Shift Trim", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node&) -> json {
                const std::vector<geo::Index> faces = facesOrAll(m, ids);
                std::span<const int32_t> attr = std::as_const(m).faceAttrs().get<int32_t>(kFaceTrim);
                if (attr.empty())
                    throw CommandError("trimShift: these faces are not mapped to a trim; use trimApply or trimAuto first");
                std::map<int, std::vector<geo::Index>> byTrim;
                for (geo::Index fc : faces)
                    if (attr[fc] > 0 && size_t(attr[fc]) <= s.trims.size())
                        byTrim[attr[fc] - 1].push_back(fc);
                json moved = json::array();
                for (const auto& [from, group] : byTrim)
                {
                    const std::vector<int> order = neighbourOrder(s, from);
                    const auto at = std::ranges::find(order, from);
                    const int pos = std::clamp(int(at - order.begin()) + steps, 0, int(order.size()) - 1);
                    const int to = order[size_t(pos)];
                    uv::ChartOptions co;
                    co.source = uv::IslandSource::Uvs;
                    co.uvSet = set;
                    std::vector<uv::Chart> charts = uv::buildCharts(m, group, co);
                    for (uv::Chart& ch : charts)
                    {
                        const glm::dvec2 before = ch.uvMax() - ch.uvMin();
                        uv::measureChart(ch, o);
                        const glm::dvec2 after = ch.uvMax() - ch.uvMin();
                        // Measuring rectifies along the longer side; keep the turn the island had on its old trim.
                        const bool turned = (before.x >= before.y) != (after.x >= after.y);
                        uv::placeChart(ch, trims[size_t(to)], turned, o, 0.0, 1.0, 1.0);
                    }
                    uv::writeCharts(m, charts, set);
                    tagFaces(m, group, to, s, std::nullopt);
                    moved.push_back({{"from", s.trims[size_t(from)].name}, {"to", s.trims[size_t(to)].name}, {"islands", int64_t(charts.size())}});
                }
                return {{"moved", moved}};
            });
        },
        .examples = {"trimShift(steps=1)", "trimShift(steps=-2)"},
    });

    ctx.registerCommand({
        .name = "trimCheck",
        .doc = "Check trim-mapped meshes against their sheets, island by island: texel density (px per metre) against the sheet's "
               "or targetPxPerM, the trim each maps to and how far it runs outside it, orientation (quarter turns from world up, "
               "skew, mirroring the trim doesn't allow), stretch, trims crossed (a sheet seam through the island), the mip its "
               "trim stays clean to, the trim's smallest feature in screen px at distance= for a screenHeight= display "
               "(readability), and neighbours showing the same trim at the same offset (repetition). Checks the selected meshes, "
               "or every mesh mapped onto a sheet when none is selected. showIssues paints a colour set trimCheck on the selected "
               "meshes (red issues, green clean). Returns per mesh {sheet, islands, withIssues, ok, pxPerM {min, max}, issues "
               "{kind: islands}, details} plus totals; details lists the islands with issues (all=True lists every one).",
        .flags = {sheetFlag(),
                  uvSetFlag(),
                  {"targetPxPerM", "tpm", AT::Float, nullptr, "Texel density islands should have; the sheet's when not given.", {}, {}, 0.001},
                  {"densityTolerance", "dt", AT::Float, 0.25, "Allowed density error in doublings (0.25 is about 19%).", {}, {}, 0.0, 4.0},
                  {"maxStretch", "ms", AT::Float, 0.1, "Allowed stretch: larger uv scale over smaller, minus one.", {}, {}, 0.0, 10.0},
                  {"maxSkew", "mk", AT::Float, 3.0, "Allowed skew off a quarter turn, degrees.", {}, {}, 0.0, 45.0},
                  {"distance", "d", AT::Float, 1000.0, "Gameplay viewing distance, cm.", {}, {}, 1.0},
                  {"screenHeight", "sh", AT::Float, 1080.0, "Screen height, px.", {}, {}, 1.0},
                  {"fov", "", AT::Float, 60.0, "Vertical field of view, degrees.", {}, {}, 1.0, 170.0},
                  {"minFeaturePx", "mf", AT::Float, 2.0, "Smallest feature a trim may show on screen, px.", {}, {}, 0.0},
                  {"all", "", AT::Bool, false, "List every island, not only those with issues."},
                  {"showIssues", "si", AT::Bool, false, "Paint the selected meshes' colour set trimCheck: red islands with issues, green clean."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            CheckOptions o;
            o.uvSet = a.get<std::string>("uvSet", "uv0");
            o.targetPxPerM = a.get<double>("targetPxPerM", 0.0);
            o.densityTolerance = a.get<double>("densityTolerance", o.densityTolerance);
            o.maxStretch = a.get<double>("maxStretch", o.maxStretch);
            o.maxSkew = a.get<double>("maxSkew", o.maxSkew);
            o.distanceCm = a.get<double>("distance", o.distanceCm);
            o.screenHeightPx = a.get<double>("screenHeight", o.screenHeightPx);
            o.fovDeg = a.get<double>("fov", o.fovDeg);
            o.minFeaturePx = a.get<double>("minFeaturePx", o.minFeaturePx);
            const bool all = a.get<bool>("all", false);
            const std::optional<std::filesystem::path> forced = a.has("sheet") ? std::optional(loadedPath(a, "trimCheck", "sheet", false)) : std::nullopt;

            Graph& g = Graph::current();
            std::vector<scene::NodeId> targets;
            for (scene::NodeId id : g.selection())
                if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    targets.push_back(id);
            if (targets.empty())
                for (scene::NodeId id : g.nodes())
                    if (Node* n = g.node(id); n && n->type().isA("mesh") && !sheetOf(*n).empty())
                        targets.push_back(id);
            if (targets.empty())
                throw CommandError("trimCheck: no meshes mapped onto a trim sheet; select some or map them with trimApply or trimAuto");

            std::map<std::filesystem::path, Sheet> sheets;
            auto sheetFor = [&](const Node& n) -> std::pair<std::filesystem::path, const Sheet*> {
                std::filesystem::path f = forced ? *forced : sheetOf(n);
                if (f.empty() && !currentSheet().empty())
                    f = paths::resolvePath(currentSheet());
                if (f.empty() || !std::filesystem::exists(f))
                    return {f, nullptr};
                auto it = sheets.find(f);
                if (it == sheets.end())
                    it = sheets.emplace(f, loadOr(f, "trimCheck")).first;
                return {f, &it->second};
            };
            json meshes = json::object();
            std::map<std::string, int> totals;
            int64_t islands = 0, bad = 0;
            std::map<scene::NodeId, std::vector<std::vector<geo::Index>>> flagged, clean;
            for (scene::NodeId id : targets)
            {
                Node& n = *g.node(id);
                const auto [f, sheet] = sheetFor(n);
                if (!sheet)
                {
                    meshes[n.path()] = {{"error", "no trim sheet; map it or give sheet="}};
                    continue;
                }
                CheckOptions mo = o;
                mo.world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
                const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1}));
                if (!m)
                    continue;
                const MeshCheck r = checkMesh(*m, *sheet, mo);
                json j = r.toJson(*sheet, all);
                j["sheet"] = f.generic_string();
                meshes[n.path()] = j;
                islands += int64_t(r.islands.size());
                for (const IslandCheck& is : r.islands)
                {
                    bad += !is.issues.empty();
                    (is.issues.empty() ? clean : flagged)[id].push_back(is.faces);
                }
                for (const auto& [k, v] : r.issueCounts())
                    totals[k] += v;
            }
            if (a.get<bool>("showIssues", false))
                uv::editUvMeshes("Show Trim Issues", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids&, Node& n) -> json {
                    geo::colors::add(m, "trimCheck", glm::vec4(1.0f));
                    std::span<glm::vec4> col = geo::colors::values(m, "trimCheck");
                    for (auto [groups, c] : {std::pair{&flagged, glm::vec4(0.9f, 0.15f, 0.1f, 1.0f)}, std::pair{&clean, glm::vec4(0.2f, 0.8f, 0.3f, 1.0f)}})
                        if (const auto it = groups->find(n.id()); it != groups->end())
                            for (const std::vector<geo::Index>& faces : it->second)
                                for (geo::Index fc : faces)
                                    if (m.faceValid(fc))
                                        m.forEachFaceCorner(fc, [&](geo::Index k) { col[k] = c; });
                    return true;
                });
            return {{"meshes", meshes}, {"islands", islands}, {"withIssues", bad}, {"ok", bad == 0}, {"issues", totals}};
        },
        .examples = {"trimCheck()['issues']", "trimCheck(distance=3000, screenHeight=2160, all=True)", "trimCheck(targetPxPerM=256, showIssues=True)"},
    });

    ctx.registerCommand({
        .name = "trimEdges",
        .doc = "Trim the selected edges with a bevel strip: each edge gets a one-segment chamfer of width= cm, the chamfer faces are "
               "hotspotted onto the sheet's edge-tagged trims (long side along U, so the bevel's crest runs along the chamfer), and "
               "the meshes get face-weighted normals so their big faces stay flat. Returns the bevel, the trim result and the "
               "chamfer faces.",
        .flags = {sheetFlag(),
                  uvSetFlag(),
                  {"width", "w", AT::Float, 1.0, "Chamfer width, cm.", {}, {}, 0.01},
                  {"tag", "tg", AT::String, "edge", "Trims to use for the chamfers."},
                  {"weightedNormals", "wn", AT::Bool, true, "Set face-weighted normals on the meshes afterwards."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimEdges");
            if (scene::components::mode() != scene::ComponentMode::Edge)
                throw CommandError("trimEdges: select edges");
            json out = json::object();
            out["bevel"] = ops::execute("polyBevel", {{"offset", a.get<double>("width", 1.0)}, {"segments", 1}, {"smoothingAngle", 180.0}});
            out["trim"] = ops::execute("trimAuto", {{"sheet", f.generic_string()},
                                                    {"uvSet", a.get<std::string>("uvSet", "uv0")},
                                                    {"tag", a.get<std::string>("tag", "edge")},
                                                    {"orient", "longestU"},
                                                    {"splitAngle", 0.0}});
            if (a.get<bool>("weightedNormals", true))
                out["normals"] = uv::editUvMeshes("Weighted Normals", scene::ComponentMode::Face, [](geo::Mesh& m, const Ids&, Node&) -> json {
                    geo::applyWeightedNormals(m);
                    return true;
                });
            return out;
        },
        .examples = {"trimEdges(width=1.5)"},
    });

    ctx.registerCommand({
        .name = "trimSheetTest",
        .doc = "Put a test blockout in the scene, <name>_Test, and auto trim it onto the sheet: a wall with a skirting, a beam, steps, a "
               "pillar and a slab, sized like a real level, so you see how the sheet reads before mapping your own meshes. Replaces "
               "an earlier one. Returns trimAuto's result.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(), {"seed", "s", AT::Int, int64_t(0), "Variation seed for the auto trim."}},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetTest");
            const Sheet s = loadOr(f, "trimSheetTest");
            const std::string path = addTrimMesh(s, f, s.name + "_Test", testBlockout());
            if (scene::components::mode() != scene::ComponentMode::Object)
                scene::components::setMode(scene::ComponentMode::Object);
            ops::execute("select", {{"args", {path}}});
            json out = ops::execute("trimAuto", {{"sheet", f.generic_string()}, {"seed", a.get<int64_t>("seed", 0)}});
            out["mesh"] = path;
            return out;
        },
        .examples = {"trimSheetTest()"},
    });

    ctx.registerCommand({
        .name = "trimAlongCurve",
        .doc = "Lay a trim along curves: a strip of quads following each curve, width= cm across, on a tiling trim that repeats along U "
               "at the sheet's density (so it never stretches; loops repeat it a whole number of times), with head and tail caps from "
               "the sheet's cap-tagged trim. Flat strips lie on the ground (kerbs, floor edging, road lines), wall strips stand up "
               "(skirting, cornices, trim bands). Makes <curve>_Trim meshes with the sheet's material. Returns [{curve, mesh, trim, "
               "cap, length}].",
        .positionalDoc = "curves (default the selected ones)",
        .flags = {sheetFlag(),
                  {"trim", "t", AT::Any, nullptr, "Trim index, name or tag; the strip whose world height is closest to width= when not given."},
                  {"width", "w", AT::Float, nullptr, "Strip width, cm; the trim's world height when not given.", {}, {}, 0.01},
                  {"orient", "o", AT::String, "flat", "flat (lies on the ground) or wall (stands up).", {"flat", "wall"}},
                  {"offset", "of", AT::Float, 0.0, "Lift along the strip's normal, cm."},
                  {"caps", "cp", AT::Bool, true, "Cap the ends with the sheet's cap-tagged trim when it has one."},
                  {"seed", "s", AT::Int, int64_t(0), "Slides the pattern along tiling trims."},
                  paletteFlag()},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimAlongCurve", "sheet", false);
            const Sheet s = loadOr(f, "trimAlongCurve");
            Graph& g = Graph::current();
            std::vector<std::string> curves;
            for (const json& p : a.positional)
                if (p.is_string())
                    curves.push_back(p.get<std::string>());
            if (curves.empty())
                for (scene::NodeId id : g.selection())
                    if (Node* n = g.node(id))
                        curves.push_back(n->path());
            const StripOrient orient = a.get<std::string>("orient", "flat") == "wall" ? StripOrient::Wall : StripOrient::Flat;
            json out = json::array();
            for (const std::string& c : curves)
            {
                json data;
                try
                {
                    data = ops::execute("curveData", {{"args", {c}}, {"worldSpace", true}});
                }
                catch (const std::exception&)
                {
                    if (!a.positional.empty())
                        throw CommandError("trimAlongCurve: " + c + " is not a curve");
                    continue;
                }
                double width = a.get<double>("width", 0.0);
                const int trim = stripTrimArg(s, a, width > 0.0 ? width : 20.0, "", "trimAlongCurve");
                if (width <= 0.0)
                    width = s.worldSize(s.trims[size_t(trim)]).y;
                geo::Mesh all;
                double length = 0.0;
                for (const json& sp : data)
                {
                    curve::Polyline pl;
                    try
                    {
                        pl = curve::tessellate(curve::splineFromJson(sp));
                    }
                    catch (const std::exception& e)
                    {
                        throw CommandError("trimAlongCurve: " + c + ": " + e.what());
                    }
                    StripOptions o;
                    o.trim = size_t(trim);
                    o.closed = pl.closed;
                    o.cap = a.get<bool>("caps", true) ? capTrim(s) : -1;
                    o.uOffset = double(uint32_t(a.get<int64_t>("seed", 0)) * 2654435761u % 1000u) / 1000.0;
                    const geo::Mesh m = stripMesh(s, stationsAlong(pl.points, pl.closed, width, orient, a.get<double>("offset", 0.0)), o);
                    all.append(m);
                    length += pl.length;
                }
                if (all.numFaces() == 0)
                    continue;
                const std::string leaf = c.substr(c.find_last_of('|') + 1);
                out.push_back({{"curve", c},
                               {"mesh", addTrimMesh(s, f, leaf + "_Trim", std::move(all), paletteArg(s, a, "trimAlongCurve").value_or(std::string()))},
                               {"trim", trim},
                               {"cap", a.get<bool>("caps", true) ? capTrim(s) : -1},
                               {"length", length}});
            }
            if (out.empty())
                throw CommandError("trimAlongCurve: select or name curves");
            return out;
        },
        .examples = {"trimAlongCurve('curve1', width=15)", "trimAlongCurve(orient='wall', trim='Plate128')"},
    });

    ctx.registerCommand({
        .name = "trimDecal",
        .doc = "Trim decals over the selected edges: a strip wrapping each run of edges, half its width on each face beside them, "
               "floating offset= cm off the surface and mapped onto an edge trim with its crest on the edge, as <mesh>_Decal with "
               "the sheet's material. cut=True carves it into the mesh instead (Trim Cut): a groove depth= cm deep whose floor and "
               "walls carry the trim. Returns [{mesh, decal or cut, chains, trim}].",
        .flags = {sheetFlag(),
                  {"trim", "t", AT::Any, nullptr, "Trim index, name or tag; the edge-tagged strip closest to width= when not given."},
                  {"width", "w", AT::Float, 2.0, "Strip width across the edge, cm.", {}, {}, 0.01},
                  {"offset", "of", AT::Float, 0.05, "Float off the surface, cm, so the decal doesn't fight it.", {}, {}, 0.0},
                  {"cut", "c", AT::Bool, false, "Carve the strip into the mesh as a groove instead of floating it."},
                  {"depth", "d", AT::Float, 0.5, "Groove depth for cut, cm.", {}, {}, 0.001},
                  paletteFlag()},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimDecal");
            const Sheet s = loadOr(f, "trimDecal");
            const double width = a.get<double>("width", 2.0);
            const int trim = stripTrimArg(s, a, width, "edge", "trimDecal");
            const bool cut = a.get<bool>("cut", false);
            Graph& g = Graph::current();
            json out = json::array();
            for (const auto& [id, edges] : scene::components::selected(scene::ComponentMode::Edge))
            {
                Node* n = g.node(id);
                if (!n || edges.empty())
                    continue;
                const scene::MeshPtr mesh = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1}));
                if (!mesh)
                    continue;
                const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
                const glm::dmat3 normals = glm::transpose(glm::inverse(glm::dmat3(world)));
                const std::vector<geo::Index> ids(edges.begin(), edges.end());
                const std::vector<EdgeChain> chains = edgeChains(*mesh, ids);
                geo::Mesh all;
                for (const EdgeChain& chain : chains)
                {
                    std::vector<Station> st = stationsOverEdges(*mesh, chain, width, cut ? 0.0 : a.get<double>("offset", 0.05));
                    for (Station& x : st)
                    {
                        for (glm::dvec3& p : x.across)
                            p = glm::dvec3(world * glm::dvec4(p, 1.0));
                        x.normal = glm::normalize(normals * x.normal);
                    }
                    StripOptions o;
                    o.trim = size_t(trim);
                    o.closed = chain.closed;
                    o.cutDepth = cut ? a.get<double>("depth", 0.5) : 0.0;
                    all.append(stripMesh(s, st, o));
                }
                if (all.numFaces() == 0)
                    continue;
                const std::string leaf = n->name();
                const std::string made =
                    addTrimMesh(s, f, leaf + (cut ? "_TrimCut" : "_Decal"), std::move(all), paletteArg(s, a, "trimDecal").value_or(std::string()));
                json r = {{"mesh", n->path()}, {"chains", chains.size()}, {"trim", trim}};
                if (cut)
                {
                    ops::execute("polyBoolean", {{"args", {n->path(), made}}, {"operation", "difference"}});
                    r["cut"] = true;
                }
                else
                    r["decal"] = made;
                out.push_back(r);
            }
            if (out.empty())
                throw CommandError("trimDecal: select edges");
            return out;
        },
        .examples = {"trimDecal(width=3)", "trimDecal(cut=True, depth=0.4)"},
    });

    ctx.registerCommand({
        .name = "trimSheetTexture",
        .doc = "Texture a trim sheet with a smart material. The sheet's bake becomes the mesh maps of its texture set TS_<name>, so "
               "edge wear, cavity dirt and chips land on the bevels, and the set paints M_<name> live on every mesh using the sheet. "
               "The first call makes the set with a Sheet Bake layer (the baked normal and AO) and bakes the sheet if it has no "
               "textures; each call adds the smart material on top, kept to trims= (indices, names or tags) by Trim Mask masks when "
               "given. palette= textures that palette's own set TS_<name>_<palette> (painting M_<name>_<palette>) over the same bake. "
               "Returns {set, project, layer, material, palette, trims, maps}.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"palette", "pal", AT::String, nullptr, "Palette to texture (trimPaletteList); the sheet's default palette when not given."},
                  {"material", "m", AT::String, nullptr, "Smart material name or .lilasmart; the palette's (else the sheet's) own when not given."},
                  {"trims", "t", AT::Any, nullptr, "Only on these trims: an index, name or tag, or a list of them."},
                  {"name", "n", AT::String, nullptr, "Layer name; the smart material's when not given."},
                  {"replace", "r", AT::Bool, false, "First remove every layer but Sheet Bake, so the sheet starts over."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetTexture");
            Sheet s = loadOr(f, "trimSheetTexture");
            std::vector<size_t> only;
            if (a.has("trims"))
                try
                {
                    only = trimsMatching(s, a.flags["trims"]);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("trimSheetTexture: ") + e.what());
                }
            const std::string palette = paletteArg(s, a, "trimSheetTexture").value_or(std::string());
            const std::string set = ensureTextureSet(s, f, "trimSheetTexture", palette);
            if (a.get<bool>("replace", false))
                for (const json& l : ops::execute("texLayerStack", {{"args", {set}}}).value("layers", json::array()))
                    if (l.value("name", std::string()) != kBakeLayer)
                        ops::execute("texRemoveLayer", {{"args", {set, l.value("id", std::string())}}});

            const std::string own = palette.empty() || s.palette(palette)->material.empty() ? s.material : s.palette(palette)->material;
            const std::string material = a.get<std::string>("material", own.empty() ? std::string(kDefaultSmartMaterial) : own);
            json apply = {{"args", {set, material}}, {"addMaps", false}};
            if (a.has("name"))
                apply["name"] = a.get<std::string>("name");
            const json applied = ops::execute("texApplySmartMaterial", apply);
            const std::string layer = applied.value("layer", std::string());
            const double metres = s.worldExtent().x / 100.0;
            if (metres > 1.0)
                for (const json& l : ops::execute("texLayerStack", {{"args", {set}}}).value("layers", json::array()))
                    if (l.value("id", std::string()) == layer)
                        scaleGenerators(set, l, metres);
            // Smart materials can read maps the sheet doesn't bake yet (dust reads the object normal): bake them too.
            bake::registerBuiltinMaps();
            bool more = false;
            for (const json& m : applied.value("maps", json::array()))
                if (const std::string id = tex::bakeMapName(m.get<std::string>()); bake::findMap(id) && std::ranges::find(s.maps, id) == s.maps.end())
                {
                    s.maps.push_back(id);
                    more = true;
                }
            if (more)
            {
                save(s, f);
                runBake(s, f, {}, "trimSheetTexture");
            }
            const auto runs = trimRuns(only);
            for (size_t k = 0; k < runs.size(); ++k)
                ops::execute("texAddMask", {{"args", {set, layer, "generator"}},
                                            {"type", "trimMask"},
                                            {"name", std::format("Trims {}-{}", runs[k].first, runs[k].second)},
                                            {"values", {{"first", runs[k].first}, {"last", runs[k].second}}},
                                            {"mode", k == 0 ? "normal" : "lighten"}});
            if (std::string& kept = palette.empty() ? s.material : s.palette(palette)->material; kept != material && only.empty())
            {
                kept = material;
                save(s, f);
            }
            return {{"set", set},
                    {"project", s.name},
                    {"layer", layer},
                    {"material", material},
                    {"palette", palette},
                    {"trims", only},
                    {"maps", applied.value("maps", json::array())}};
        },
        .examples = {"trimSheetTexture()", "trimSheetTexture(material='Rusty Iron', trims=['edge'])"},
    });

    ctx.registerCommand({
        .name = "trimSheetPaint",
        .doc = "Get a trim sheet ready to paint by hand: its texture set (as trimSheetTexture makes it) and a flat <name>_Sheet plane "
               "with the sheet's material, selected, for the texture paint tool or the UV editor. Strokes land on the sheet's shared "
               "texels, so every mesh using the sheet shows them. palette= paints that palette's set on a <name>_<palette>_Sheet "
               "plane. Returns {set, plane}.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(), {"palette", "pal", AT::String, nullptr, "Palette to paint; the default palette when not given."}},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetPaint");
            const Sheet s = loadOr(f, "trimSheetPaint");
            const std::string palette = paletteArg(s, a, "trimSheetPaint").value_or(std::string());
            const std::string set = ensureTextureSet(s, f, "trimSheetPaint", palette);
            Graph& g = Graph::current();
            const std::string name = s.name + (palette.empty() ? "" : "_" + palette) + "_Sheet";
            Node* n = g.find(name);
            if (!n)
            {
                geo::Mesh plane = lowPlane(s);
                std::vector<geo::Index> faces;
                for (geo::Index fi = 0; fi < plane.faceCapacity(); ++fi)
                    if (plane.faceValid(fi))
                        faces.push_back(fi);
                tagFaces(plane, faces, -1, s, palette);
                const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
                n = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
                g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(plane))));
            }
            ops::execute("select", {{"args", {n->path()}}});
            return {{"set", set}, {"plane", n->path()}};
        },
    });

    ctx.registerCommand({
        .name = "trimPaletteList",
        .doc = "The palettes of a trim sheet: one layout and bake, several material families. Each entry has its name (Default "
               "is the sheet itself), smart material, Lila material slot, texture set (and whether it exists yet), Unreal instance "
               "name, and the instance parameters it sets (scalars, vectors, switches).",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimPaletteList");
            return paletteSummary(loadOr(f, "trimPaletteList"));
        },
        .examples = {"[p['name'] for p in trimPaletteList()]"},
    });

    ctx.registerCommand({
        .name = "trimPaletteAdd",
        .doc = "Add a palette to a trim sheet: another material family on the same layout and baked maps, e.g. rusted iron and "
               "painted steel on one industrial sheet, or sandstone and basalt on one stone sheet. It gets the Lila material "
               "M_<sheet>_<name>; texture it with trimSheetTexture(palette=...), map faces to it with palette= on trimApply, "
               "trimAuto or trimPaletteAssign, and trimSheetSend makes MI_<sheet>_<name> for it in Unreal. scalars, vectors and "
               "switches are MF_LilaTrim parameters for that instance (Tint, EdgeTint, BaseRoughness, Metallic, EdgeWear, "
               "UseDetail...). Returns the palette list.",
        .positionalDoc = "palette name",
        .flags = {sheetFlag(),
                  {"name", "n", AT::String, nullptr, "Palette name: letters, digits and underscores."},
                  {"material", "m", AT::String, nullptr, "Smart material its texture set starts from; the sheet's when not given."},
                  {"copyFrom", "cf", AT::String, nullptr, "Palette whose material and parameters to start from (Default is the sheet's)."},
                  {"scalars", "", AT::Any, nullptr, "{parameter: number} for its Unreal instance."},
                  {"vectors", "", AT::Any, nullptr, "{parameter: [r, g, b]} for its Unreal instance."},
                  {"switches", "", AT::Any, nullptr, "{parameter: bool} for its Unreal instance."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimPaletteAdd", "sheet", false);
            Sheet s = loadOr(f, "trimPaletteAdd");
            const std::string name = nameArg(a);
            if (name.empty() || name == "Default" || !std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
                throw CommandError("trimPaletteAdd: give a palette name of letters, digits and underscores (not Default)");
            if (s.palette(name))
                throw CommandError("trimPaletteAdd: " + s.name + " already has a palette " + name + "; change it with trimPaletteEdit");
            Palette p;
            if (a.has("copyFrom"))
            {
                const std::string from = a.get<std::string>("copyFrom");
                if (from.empty() || from == "Default")
                {
                    p.material = s.material;
                    p.scalars = s.scalars;
                    p.vectors = s.vectors;
                    p.switches = s.switches;
                }
                else if (const Palette* src = s.palette(from))
                    p = *src;
                else
                    throw CommandError("trimPaletteAdd: no palette " + from + " to copy");
            }
            p.name = name;
            p.material = a.get<std::string>("material", p.material);
            mergeParams(p.scalars, p.vectors, p.switches, a, "trimPaletteAdd");
            s.palettes.push_back(std::move(p));
            save(s, f);
            ensureMaterial(s, f, name);
            return paletteSummary(s);
        },
        .examples = {"trimPaletteAdd('Rust', material='Trim Rusty Iron', vectors={'Tint': [0.35, 0.2, 0.12]}, scalars={'EdgeWear': 2.0})",
                     "trimPaletteAdd('Basalt', copyFrom='Default', vectors={'Tint': [0.12, 0.12, 0.13]})"},
    });

    ctx.registerCommand({
        .name = "trimPaletteEdit",
        .doc = "Change a palette: material= sets its smart material (retexture with trimSheetTexture(palette=..., replace=True)); "
               "scalars, vectors and switches merge into its Unreal parameters, a None value removing one. Default edits the "
               "sheet's own palette. Returns the palette list.",
        .positionalDoc = "palette name",
        .flags = {sheetFlag(),
                  {"name", "n", AT::String, nullptr, "Palette to change; Default is the sheet's own."},
                  {"material", "m", AT::String, nullptr, "Smart material."},
                  {"scalars", "", AT::Any, nullptr, "{parameter: number or None}."},
                  {"vectors", "", AT::Any, nullptr, "{parameter: [r, g, b] or None}."},
                  {"switches", "", AT::Any, nullptr, "{parameter: bool or None}."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimPaletteEdit", "sheet", false);
            Sheet s = loadOr(f, "trimPaletteEdit");
            const std::string name = nameArg(a);
            if (name.empty())
                throw CommandError("trimPaletteEdit: give the palette's name (Default for the sheet's own)");
            if (name == "Default")
            {
                s.material = a.get<std::string>("material", s.material);
                mergeParams(s.scalars, s.vectors, s.switches, a, "trimPaletteEdit");
            }
            else if (Palette* p = s.palette(name))
            {
                p->material = a.get<std::string>("material", p->material);
                mergeParams(p->scalars, p->vectors, p->switches, a, "trimPaletteEdit");
            }
            else
                throw CommandError("trimPaletteEdit: " + s.name + " has no palette " + name);
            save(s, f);
            return paletteSummary(s);
        },
        .examples = {"trimPaletteEdit('Rust', scalars={'EdgeWear': 0.5})"},
    });

    ctx.registerCommand({
        .name = "trimPaletteRemove",
        .doc = "Remove a palette from a trim sheet. Its Lila material and texture set stay in the scene; meshes with faces still on "
               "it are listed in stillUsedBy, to move with trimPaletteAssign(palette='Default', fromPalette=...). Returns {palettes, "
               "stillUsedBy}.",
        .positionalDoc = "palette name",
        .flags = {sheetFlag(), {"name", "n", AT::String, nullptr, "Palette to remove."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimPaletteRemove", "sheet", false);
            Sheet s = loadOr(f, "trimPaletteRemove");
            const std::string name = nameArg(a);
            const auto it = std::ranges::find_if(s.palettes, [&](const Palette& p) { return p.name == name; });
            if (it == s.palettes.end())
                throw CommandError("trimPaletteRemove: " + s.name + " has no palette " + name);
            const std::string slot = materialName(s, name);
            s.palettes.erase(it);
            save(s, f);
            json used = json::array();
            for (const auto& [node, mesh] : sheetMeshes(f))
                for (const auto& [n, count] : scene::materials::slotUsage(*mesh))
                    if (n == slot && count > 0)
                        used.push_back({{"mesh", node}, {"faces", count}});
            return {{"palettes", paletteSummary(s)}, {"stillUsedBy", used}};
        },
    });

    ctx.registerCommand({
        .name = "trimPaletteAssign",
        .doc = "Put the selected faces (whole selected meshes in object mode) on a palette of their trim sheet, keeping their uvs: "
               "only faces mapped onto the sheet change. fromPalette= limits it to faces on that palette. Returns per mesh the "
               "faces moved.",
        .flags = {sheetFlag(),
                  {"palette", "pal", AT::String, nullptr, "Palette to put the faces on; Default is the sheet's own."},
                  {"fromPalette", "fp", AT::String, nullptr, "Only faces now on this palette."}},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimPaletteAssign", "sheet", false);
            const Sheet s = loadOr(f, "trimPaletteAssign");
            if (!a.has("palette"))
                throw CommandError("trimPaletteAssign: give palette=");
            const std::string palette = paletteArg(s, a, "trimPaletteAssign").value_or(std::string());
            std::optional<std::string> fromSlot;
            if (a.has("fromPalette"))
            {
                const std::string from = a.get<std::string>("fromPalette");
                if (!from.empty() && from != "Default" && !s.palette(from))
                    throw CommandError("trimPaletteAssign: " + s.name + " has no palette " + from);
                fromSlot = materialName(s, from == "Default" ? std::string() : from);
            }
            const std::vector<std::string> slots = paletteMaterials(s);
            ensureMaterial(s, f, palette);
            return uv::editUvMeshes("Assign Trim Palette", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node&) -> json {
                std::span<const int32_t> mat = std::as_const(m).faceAttrs().get<int32_t>(geo::attr::kMaterial);
                std::span<const int32_t> trim = std::as_const(m).faceAttrs().get<int32_t>(kFaceTrim);
                std::vector<uint32_t> faces;
                for (geo::Index fc : facesOrAll(m, ids))
                {
                    const std::string now = fc < mat.size() ? scene::materials::slotName(m, mat[fc]) : std::string();
                    const bool onSheet = std::ranges::find(slots, now) != slots.end() || (fc < trim.size() && trim[fc] > 0);
                    if (onSheet && (!fromSlot || now == *fromSlot))
                        faces.push_back(fc);
                }
                if (!faces.empty())
                    scene::materials::assign(m, materialName(s, palette), faces);
                return {{"faces", int64_t(faces.size())}, {"material", materialName(s, palette)}};
            });
        },
        .examples = {"trimPaletteAssign(palette='Rust')", "trimPaletteAssign(palette='Default', fromPalette='Rust')"},
    });

    ctx.registerCommand({
        .name = "trimSheetSend",
        .doc = "Send a trim sheet to Unreal, in any order with its meshes: the meshes using the sheet are mapped (new ones under "
               "<folder>/<name>/Meshes/SM_<mesh>) and sent first, the baked textures go to <folder>/<name>/Textures, the trim master "
               "M_LilaTrim is generated (or rebuilt with regenerateMaster), each palette becomes an instance (MI_<name> for the "
               "default, MI_<name>_<palette> for the others) with the shared maps plus that palette's painted maps and parameters, "
               "and every mesh gets its palettes' instances on the matching slots. meshes='mapped' sends no meshes and fails "
               "listing the ones Unreal doesn't have. Returns {instances, assigned, meshes: {sent, newlyMapped}, missing, "
               "textures, materials, errors}; wait=False queues it; dryRun=True returns the plan without connecting.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"folder", "f", AT::String, "/Game/Lila/Trims", "Content folder; the sheet's assets go in <folder>/<name>."},
                  {"regenerateMaster", "", AT::Bool, false, "Rebuild M_LilaTrim's graph from this version of Lila, discarding edits to it."},
                  {"meshes", "me", AT::String, "send", "send: map and send the sheet's meshes first; mapped: only assign to meshes already in Unreal.",
                   {"send", "mapped"}},
                  {"wait", "w", AT::Bool, true, "Block until Unreal has imported everything."},
                  {"dryRun", "", AT::Bool, false, "Return the plan (textures, instances, meshes) without connecting to Unreal."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetSend");
            const Sheet s = loadOr(f, "trimSheetSend");
            std::ifstream rec(bakeRecord(s, f));
            if (!rec)
                throw CommandError("trimSheetSend: " + s.name + " is not baked yet; run trimSheetBake");
            json baked;
            rec >> baked;
            const bool dryRun = a.get<bool>("dryRun", false);
            const std::string meshMode = a.get<std::string>("meshes", "send");
            const std::string folder = a.get<std::string>("folder", "/Game/Lila/Trims") + "/" + s.name;

            // Meshes: the ones Unreal has (mapped) and the ones it still needs.
            json mapped = json::array(), toMap = json::array();
            std::vector<std::string> nodes;
            for (const auto& [node, mesh] : sheetMeshes(f))
            {
                nodes.push_back(node);
                if (const json m = link::mappingOf(node); m.is_object() && m.contains("contentPath"))
                    mapped.push_back({{"node", node}, {"contentPath", m["contentPath"]}});
                else
                {
                    const std::string leaf = node.substr(node.find_last_of('|') + 1);
                    toMap.push_back({{"node", node}, {"contentPath", folder + "/Meshes/SM_" + leaf}});
                }
            }
            if (meshMode == "mapped" && !toMap.empty())
            {
                std::string list;
                for (const json& m : toMap)
                    list += (list.empty() ? "" : ", ") + m["node"].get<std::string>();
                throw CommandError("trimSheetSend: these meshes use " + s.name + " but aren't in Unreal: " + list +
                                   "; send them first (unrealMap + unrealSync) or use meshes='send'");
            }

            const SendPlan plan = sendPlan(s, f, baked, folder);
            json meshPaths = json::array();
            for (const json& m : mapped)
                meshPaths.push_back(m["contentPath"]);
            if (meshMode == "send")
                for (const json& m : toMap)
                    meshPaths.push_back(m["contentPath"]);
            json request = {{"folder", folder + "/Materials"},
                            {"regenerate", a.get<bool>("regenerateMaster", false)},
                            {"meshes", meshPaths},
                            {"instances", plan.instances}};
            const json importParams = link::textureImportParams({{"outputs", plan.textures}}, {{"folder", folder + "/Textures"}, {"materialMode", "none"}});
            json meshReport = {{"sent", meshMode == "send" ? json(nodes) : json::array()}, {"newlyMapped", meshMode == "send" ? toMap : json::array()}};
            if (dryRun)
                return {{"request", request}, {"textures", plan.textures}, {"meshes", meshReport}, {"mapped", mapped}};

            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("trimSheetSend: not connected to Unreal: ") + e.what());
            }
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::ranges::find(features, json("trimMaterials")) == features.end())
                throw CommandError("trimSheetSend: the LilaLink plugin in this editor is too old for trim sheets; rebuild it from Lila's ue/LilaLink");
            if (meshMode == "send")
            {
                for (const json& m : toMap)
                    ops::execute("unrealMap", {{"args", {m["node"]}}, {"contentPath", m["contentPath"]}, {"kind", "static"}});
                // Queued on the link's worker ahead of the material job, so the meshes exist when their slots are set.
                link::syncMappings(true, nodes);
            }

            auto send = [importParams, request, meshReport]() -> json {
                std::shared_ptr<link::RpcClient> client = link::Link::get().client();
                if (!client)
                    throw std::runtime_error("not connected to Unreal");
                json errors = json::array();
                link::RpcResult t = client->call("importTextures", importParams, link::importCallOptions());
                if (!t.ok)
                    errors.push_back("textures: " + t.error);
                link::RpcResult m = client->call("trimMaterials", request, link::importCallOptions());
                if (!m.ok)
                    errors.push_back("materials: " + m.error);
                json missing = json::array();
                if (m.ok)
                    for (const json& w : m.result.value("warnings", json::array()))
                        if (const std::string text = w.is_string() ? w.get<std::string>() : std::string(); text.starts_with("no static mesh "))
                            missing.push_back(text.substr(15));
                return {{"instances", m.ok ? m.result.value("instances", json::array()) : json::array()},
                        {"assigned", m.ok ? m.result.value("assigned", json::array()) : json::array()},
                        {"meshes", meshReport},
                        {"missing", missing},
                        {"textures", t.ok ? t.result : json()},
                        {"materials", m.ok ? m.result : json()},
                        {"errors", errors}};
            };
            if (a.get<bool>("wait", true))
            {
                try
                {
                    link::Link::get().waitIdle();
                    return send();
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("trimSheetSend: ") + e.what());
                }
            }
            const std::string name = s.name;
            link::Link::get().post([send, name] {
                json done;
                try
                {
                    done = send();
                }
                catch (const std::exception& e)
                {
                    done = {{"errors", json::array({e.what()})}};
                }
                mainq::post([done, name] {
                    for (const json& e : done.value("errors", json::array()))
                        log::error("trimSheetSend {}: {}", name, e.get<std::string>());
                    for (const json& m : done.value("missing", json::array()))
                        log::warn("trimSheetSend {}: Unreal has no mesh {}", name, m.get<std::string>());
                    log::info("trimSheetSend {}: {} slots assigned", name, done.value("assigned", json::array()).size());
                });
            });
            return {{"queued", true}, {"meshes", meshReport}};
        },
        .examples = {"trimSheetSend()", "trimSheetSend(dryRun=True)['request']['instances']", "trimSheetSend(meshes='mapped')"},
    });

    ctx.registerCommand({
        .name = "trimDecalCreate",
        .doc = "Make a unique decal for what trims can't carry: an inscription, a key border, damage, or relief from your own meshes "
               "(high=), baked at its true size into normal, AO and height maps plus an opacity mask (where the relief stands off "
               "the flat surround, faded at the edges) and a base colour. The decal is a .lilatrim holding one trim (decal: true), "
               "so trimSheetEdit, trimSheetBake, trimSheetTexture and trimSheetPaint work on it: texture it with a smart material or "
               "paint it by hand. Place it with trimDecalPlace; trimDecalSend makes Unreal deferred decals of the placements. "
               "Returns the decal's summary and its bake.",
        .positionalDoc = "decal name",
        .flags = {{"name", "n", AT::String, nullptr, "Decal name: letters, digits and underscores."},
                  {"kind", "k", AT::String, "inscription", "inscription (carved glyphs), border (key pattern), damage (chips and pits) or relief (only high=).",
                   {"inscription", "border", "damage", "relief"}},
                  {"style", "st", AT::String, nullptr, "A profile style instead of the kind's (glyphs, meander, chipped, fluting...)."},
                  {"width", "w", AT::Float, 200.0, "World width, cm.", {}, {}, 0.1},
                  {"height", "h", AT::Float, 50.0, "World height, cm.", {}, {}, 0.1},
                  {"resolution", "r", AT::Int, int64_t(1024), "Texture width, px; the height follows the aspect.", {}, {}, 16.0, 8192.0},
                  {"depth", "d", AT::Float, nullptr, "Relief depth, cm; the kind's when not given.", {}, {}, 0.001},
                  {"seed", "s", AT::Int, int64_t(0), "Pattern seed: other glyphs, other damage."},
                  {"high", "hi", AT::List, nullptr, "Scene meshes baked as the decal's relief, laid on it as trimSheetGenerate shows a sheet."},
                  {"file", "f", AT::String, nullptr, "Where to save; default <workspace>/TrimSheets/Decals/<name>.lilatrim."},
                  {"overwrite", "", AT::Bool, false, "Replace an existing decal file."},
                  {"bake", "b", AT::Bool, true, "Bake it now."},
                  {"backend", "", AT::String, "auto", "Bake backend: auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string name = nameArg(a);
            if (name.empty() || !std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
                throw CommandError("trimDecalCreate: give a decal name of letters, digits and underscores");
            const std::string kind = a.get<std::string>("kind", "inscription");
            if (kind == "relief" && !a.has("high"))
                throw CommandError("trimDecalCreate: a relief decal bakes the meshes given in high=");
            const double w = a.get<double>("width", 200.0), h = a.get<double>("height", 50.0);
            const int resolution = int(a.get<int64_t>("resolution", 1024));
            std::filesystem::path f = a.has("file") ? std::filesystem::path(a.get<std::string>("file")) : defaultSheetFolder() / "Decals" / (name + kExtension);
            if (f.extension().empty())
                f += kExtension;
            if (std::filesystem::exists(f) && !a.get<bool>("overwrite", false))
                throw CommandError("trimDecalCreate: " + f.generic_string() + " exists; edit it with trimSheetEdit or pass overwrite=True");

            Sheet s;
            s.decal = true;
            s.name = "T_" + name + "Decal";
            s.width = resolution;
            s.height = std::max(16, int(std::lround(resolution * h / w)));
            s.pxPerM = resolution / (w / 100.0);
            s.padding = 2;
            s.mipLevel = 0;
            s.supersampling = 4;
            s.maps = {"normal", "ao", "height"};
            Trim t;
            t.name = name;
            t.min = {0, 0};
            t.max = {s.width, s.height};
            t.kind = uv::TrimKind::Unique;
            t.fit = uv::TrimFit::Fill;
            t.insetPx = 0.0;
            t.allowRotate = false;
            t.tags = {"decal"};
            const double cmToPx = s.pxPerM / 100.0;
            const Style byKind = kind == "border" ? Style::Meander : kind == "damage" ? Style::Chipped : kind == "relief" ? Style::Flat : Style::Glyphs;
            t.profile.style = byKind;
            if (a.has("style"))
            {
                const std::optional<Style> st = styleFromName(a.get<std::string>("style"));
                if (!st)
                    throw CommandError("trimDecalCreate: no style " + a.get<std::string>("style"));
                t.profile.style = *st;
            }
            // No border bevel: the decal's surround stays flat so only its relief shows.
            t.profile.bevelPx = 0.0;
            t.profile.detailPx = a.get<double>("depth", kind == "damage" ? h * 0.05 : h * 0.04) * cmToPx;
            t.profile.spacingPx = (kind == "damage" ? h * 0.12 : h * 0.5) * cmToPx;
            t.profile.seed = uint32_t(a.get<int64_t>("seed", 0));
            s.trims.push_back(t);
            if (a.has("high"))
                s.high = a.flags["high"].get<std::vector<std::string>>();
            save(s, f);
            json out = sheetSummary(s, f);
            if (a.get<bool>("bake", true))
            {
                SheetBakeOptions o;
                o.backend = a.get<std::string>("backend", "auto");
                o.tile = false;
                out["bake"] = runBake(s, f, o, "trimDecalCreate");
                out["textures"] = sheetSummary(s, f)["textures"];
                out["baked"] = true;
            }
            return out;
        },
        .examples = {"trimDecalCreate('TempleWords', kind='inscription', width=1200, height=180, seed=3)",
                     "trimDecalCreate('Breach', kind='damage', width=300, height=300)"},
    });

    ctx.registerCommand({
        .name = "trimDecalPlace",
        .doc = "Place a unique decal (trimDecalCreate) in the scene: a quad of the decal's world size centred at position, facing "
               "normal with its top toward up, floating offset cm off the surface, with the decal's translucent material so it "
               "previews in the viewport. Move, turn and scale it like any mesh; trimDecalSend sends it to Unreal as a deferred "
               "decal projecting depth cm into the surface. Returns the node.",
        .positionalDoc = "decal file or name",
        .flags = {{"decal", "dc", AT::String, nullptr, "The decal's .lilatrim, or its name under <workspace>/TrimSheets/Decals."},
                  {"position", "p", AT::List, nullptr, "Centre, cm; the origin when not given."},
                  {"normal", "nm", AT::List, nullptr, "The way it faces; +Y (a floor) when not given."},
                  {"up", "u", AT::List, nullptr, "The way its top points; world up, or -Z for a floor, when not given."},
                  {"depth", "d", AT::Float, 20.0, "How far the Unreal decal projects into the surface, cm.", {}, {}, 0.1},
                  {"offset", "of", AT::Float, 0.05, "Lift off the surface in Lila's viewport, cm.", {}, {}, 0.0},
                  {"name", "n", AT::String, nullptr, "Node name; DC_<decal> when not given."}},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = decalPath(a, "trimDecalPlace");
            const Sheet s = loadOr(f, "trimDecalPlace");
            if (!s.decal)
                throw CommandError("trimDecalPlace: " + f.generic_string() + " is a trim sheet, not a decal; make one with trimDecalCreate");
            auto vec3 = [&](const char* flag, glm::dvec3 fallback) {
                if (!a.has(flag))
                    return fallback;
                const json v = a.flags[flag];
                if (!v.is_array() || v.size() != 3)
                    throw CommandError(std::format("trimDecalPlace: {} is [x, y, z]", flag));
                return glm::dvec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
            };
            const glm::dvec3 at = vec3("position", glm::dvec3(0.0));
            const glm::dvec3 n = glm::normalize(vec3("normal", {0.0, 1.0, 0.0}));
            glm::dvec3 up = vec3("up", std::abs(n.y) > 0.9 ? glm::dvec3(0.0, 0.0, -1.0) : glm::dvec3(0.0, 1.0, 0.0));
            up -= n * glm::dot(up, n);
            if (glm::length(up) < 1e-6)
                throw CommandError("trimDecalPlace: up runs along normal");
            up = glm::normalize(up);
            const glm::dvec3 right = glm::cross(up, n);
            const glm::dvec2 size = s.worldExtent();
            const glm::dvec3 c = at + n * a.get<double>("offset", 0.05);
            const glm::dvec3 pts[] = {c - right * size.x * 0.5 - up * size.y * 0.5, c + right * size.x * 0.5 - up * size.y * 0.5,
                                      c + right * size.x * 0.5 + up * size.y * 0.5, c - right * size.x * 0.5 + up * size.y * 0.5};
            const geo::Index sizes[] = {4};
            const geo::Index idx[] = {0, 1, 2, 3};
            geo::Mesh quad = geo::Mesh::fromPolygons(pts, sizes, idx);
            std::span<glm::vec2> uv = quad.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            const glm::vec2 corners[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            quad.forEachFaceCorner(0, [&](geo::Index k) { uv[k] = corners[quad.cornerPoint(k)]; });
            scene::materials::assign(quad, materialName(s), {0});
            ensureMaterial(s, f);

            const std::string nodeName = a.get<std::string>("name", "DC_" + s.trims.front().name);
            const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", nodeName}});
            Graph& g = Graph::current();
            Node* node = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
            g.setValue({node->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(quad))));
            ops::execute("addAttr", {{"args", {node->path()}}, {"longName", kNodeDecal}, {"dataType", "string"}, {"doc", "Unique decal this quad places."}});
            ops::execute("setAttr", {{"args", {node->path() + "." + kNodeDecal, paths::storedPath(f)}}});
            ops::execute("addAttr", {{"args", {node->path()}}, {"longName", kNodeDecalDepth}, {"attributeType", "float"}, {"defaultValue", 20.0},
                                     {"doc", "How far the Unreal decal projects into the surface, cm."}});
            ops::execute("setAttr", {{"args", {node->path() + "." + kNodeDecalDepth, a.get<double>("depth", 20.0)}}});
            return node->path();
        },
        .examples = {"trimDecalPlace('TempleWords', position=[0, 400, -2500], normal=[0, 0, 1])"},
    });

    ctx.registerCommand({
        .name = "trimDecalSend",
        .doc = "Send placed unique decals to Unreal as deferred decals: each decal's maps (its painted texture set's base colour "
               "and normal when it has one, else the baked ones, and its opacity mask) go to <folder>/Textures, the decal "
               "master M_LilaDecal on MF_LilaDecal is generated (or rebuilt with regenerateMaster), each decal gets an instance "
               "MI_<decal>Decal, and each placement becomes a decal actor at the quad's place, size and facing, projecting its "
               "decalDepth into the surface. Sending again moves the same actors. Returns what Unreal made; dryRun=True returns "
               "the plan without connecting.",
        .positionalDoc = "decal nodes (default every placed decal)",
        .flags = {{"folder", "f", AT::String, "/Game/Lila/Decals", "Content folder for the textures and instances."},
                  {"regenerateMaster", "", AT::Bool, false, "Rebuild M_LilaDecal's graph from this version of Lila."},
                  {"wait", "w", AT::Bool, true, "Block until Unreal has placed everything."},
                  {"dryRun", "", AT::Bool, false, "Return the plan without connecting to Unreal."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<scene::NodeId> nodes;
            for (const json& p : a.positional)
                if (p.is_string())
                {
                    Node* n = g.find(p.get<std::string>());
                    if (!n || decalOf(*n).empty())
                        throw CommandError("trimDecalSend: " + p.get<std::string>() + " is not a placed decal (trimDecalPlace)");
                    nodes.push_back(n->id());
                }
            if (nodes.empty())
                for (scene::NodeId id : g.nodes())
                    if (Node* n = g.node(id); n && n->type().isA("mesh") && !decalOf(*n).empty())
                        nodes.push_back(id);
            if (nodes.empty())
                throw CommandError("trimDecalSend: no decals placed; use trimDecalPlace");

            const std::string root = a.get<std::string>("folder", "/Game/Lila/Decals");
            json textures = json::array(), decals = json::array();
            std::map<std::filesystem::path, json> instances;
            for (scene::NodeId id : nodes)
            {
                Node& n = *g.node(id);
                const std::filesystem::path f = decalOf(n);
                const Sheet s = loadOr(f, "trimDecalSend");
                if (s.trims.empty())
                    throw CommandError("trimDecalSend: " + f.generic_string() + " has no trim to send");
                const std::string folder = root;
                if (!instances.contains(f))
                {
                    std::ifstream rec(bakeRecord(s, f));
                    if (!rec)
                        throw CommandError("trimDecalSend: " + s.name + " is not baked yet; run trimSheetBake(sheet=...)");
                    json baked;
                    rec >> baked;
                    auto asset = [&](const json& o) {
                        const std::string name = link::textureAssetName(o.value("file", std::string()), o.value("udim", 1001));
                        return folder + "/Textures/" + name + "." + name;
                    };
                    std::map<std::string, json> byMap;
                    for (const json& o : baked.value("outputs", json::array()))
                        byMap[o.value("map", std::string())] = o;
                    if (const std::string set = textureSetName(s); hasTextureSet(set))
                    {
                        const json exported = ops::execute("texExport", {{"args", {set}},
                                                                         {"preset", "unreal"},
                                                                         {"folder", (textureFolder(s, f) / "Painted").generic_string()},
                                                                         {"incremental", true}});
                        for (const json& o : exported.value("outputs", json::array()))
                            if (const std::string m = o.value("map", std::string()); m == "baseColor" || m == "normal")
                                byMap[m] = o;
                    }
                    json tex = json::object();
                    for (const auto& [map, param] : {std::pair{"baseColor", "DecalBaseColorMap"}, std::pair{"normal", "DecalNormalMap"}, std::pair{"opacity", "DecalOpacityMap"}})
                        if (const auto it = byMap.find(map); it != byMap.end())
                        {
                            tex[param] = asset(it->second);
                            textures.push_back(it->second);
                        }
                    instances[f] = {{"name", "MI_" + materialName(s).substr(2)}, {"textures", tex}, {"scalars", s.scalars}, {"vectors", s.vectors}};
                }
                const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1}));
                const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
                const std::optional<DecalFrame> frame = m ? decalFrame(*m, world) : std::nullopt;
                if (!frame)
                    throw CommandError("trimDecalSend: " + n.path() + " is no longer a decal quad with uvs");
                const std::string* link = n.type().attrs.size() > size_t(scene::Transform::kLinkId) ? std::get_if<std::string>(&n.value(scene::Transform::kLinkId)) : nullptr;
                double depth = 20.0;
                try
                {
                    depth = std::get<double>(g.value(g.plug(n.path() + "." + kNodeDecalDepth)));
                }
                catch (const std::exception&)
                {
                }
                auto ue = [](const glm::dvec3& v) {
                    const math::Vector u = math::toUnreal(math::Vector(v));
                    return json::array({u.x, u.y, u.z});
                };
                decals.push_back({{"id", link && !link->empty() ? *link : n.fullPath()},
                                  {"name", n.name()},
                                  {"decal", s.trims.front().name},
                                  {"instance", instances[f]},
                                  {"location", ue(frame->centre)},
                                  {"forward", ue(-frame->normal)},
                                  {"up", ue(frame->up)},
                                  {"size", {depth, frame->size.x, frame->size.y}}});
            }
            const json importParams = link::textureImportParams({{"outputs", textures}}, {{"folder", root + "/Textures"}, {"materialMode", "none"}});
            const json request = {{"folder", root + "/Materials"}, {"regenerate", a.get<bool>("regenerateMaster", false)}, {"decals", decals}};
            if (a.get<bool>("dryRun", false))
                return {{"request", request}, {"textures", textures}};

            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("trimDecalSend: not connected to Unreal: ") + e.what());
            }
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::ranges::find(features, json("trimDecals")) == features.end())
                throw CommandError("trimDecalSend: the LilaLink plugin in this editor is too old for decals; rebuild it from Lila's ue/LilaLink");
            auto send = [importParams, request]() -> json {
                std::shared_ptr<link::RpcClient> client = link::Link::get().client();
                if (!client)
                    throw std::runtime_error("not connected to Unreal");
                json errors = json::array();
                link::RpcResult t = client->call("importTextures", importParams, link::importCallOptions());
                if (!t.ok)
                    errors.push_back("textures: " + t.error);
                link::RpcResult d = client->call("trimDecals", request, link::importCallOptions());
                if (!d.ok)
                    errors.push_back("decals: " + d.error);
                return {{"textures", t.ok ? t.result : json()}, {"decals", d.ok ? d.result : json()}, {"errors", errors}};
            };
            if (a.get<bool>("wait", true))
                try
                {
                    link::Link::get().waitIdle();
                    return send();
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("trimDecalSend: ") + e.what());
                }
            link::Link::get().post([send] {
                json done;
                try
                {
                    done = send();
                }
                catch (const std::exception& e)
                {
                    done = {{"errors", json::array({e.what()})}};
                }
                mainq::post([done] {
                    for (const json& e : done.value("errors", json::array()))
                        log::error("trimDecalSend: {}", e.get<std::string>());
                });
            });
            return {{"queued", true}, {"decals", decals.size()}};
        },
        .examples = {"trimDecalSend()", "trimDecalSend(dryRun=True)['request']['decals'][0]['size']"},
    });
}
} // namespace trims
