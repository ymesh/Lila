// Interiors in the scene: buildingMakeEnterable and buildingMakeExterior, room layout edits (kind, split, merge, move a
// wall, swap, doors), the character profile the navigation checks use, room presets and furnishing, and regeneration with
// the building. Each room is a plain mesh of its own under the interior's group, so it is modelled like any mesh; a room
// mesh whose content no longer matches the hash it was generated with is hand-edited and is kept as it is when the
// interior regenerates.

#include "BuildingsScene.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Slots.h>
#include <lila/Transform.h>

#include <lila/Deformer.h>


#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace lila;
using lila::Args;
using lila::CommandError;
using T = lila::ArgType;
namespace fs = std::filesystem;

namespace buildings
{
namespace
{
constexpr const char* kProfileKey = "buildings.characterProfile";

scene::Graph& IG() { return scene::Graph::current(); }

std::string hexOf(const std::string& id) { return id.rfind("building:", 0) == 0 ? id.substr(9) : id; }

struct CacheEntry
{
    std::shared_ptr<const Result> result;
    std::string key;
    std::shared_ptr<const Interior> interior;
};
std::mutex gCacheMutex;
std::unordered_map<std::string, CacheEntry> gCache;

/** The interior edits of a building as its edits hold them. */
json interiorEditsOf(const json& edits) { return edits.is_object() ? edits.value("interior", json::object()) : json::object(); }

/** A content hash of a mesh as the scene has it (points, face sizes, materials) and of the node's local matrix. */
std::string meshHash(const geo::Mesh* m, const glm::dmat4& local)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int k = 0; k < 8; ++k)
        {
            h ^= (v >> (k * 8)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    if (m)
    {
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                const glm::dvec3 q = m->position(p);
                mix(uint64_t(int64_t(std::llround(q.x * 10.0))));
                mix(uint64_t(int64_t(std::llround(q.y * 10.0))));
                mix(uint64_t(int64_t(std::llround(q.z * 10.0))));
            }
        for (geo::Index f = 0; f < m->faceCapacity(); ++f)
            if (m->faceValid(f))
                mix(m->faceSize(f));
        mix(m->numFaces());
    }
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            mix(uint64_t(int64_t(std::llround(local[c][r] * 1000.0))));
    return hex64(h);
}

/** A part's soup as a mesh: points welded so it models like any mesh, edges hard where faces turn. */
geo::Mesh partMesh(const Soup& s)
{
    std::vector<std::string> names;
    for (int k = 0; k < IntCount; ++k)
        names.push_back(intMatName(k));
    std::unordered_map<uint64_t, geo::Index> weld;
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> idx(s.pts.size()), sizes(s.sizes.begin(), s.sizes.end());
    for (size_t k = 0; k < s.pts.size(); ++k)
    {
        const glm::dvec3& p = s.pts[k];
        const uint64_t key = (uint64_t(int64_t(std::llround(p.x * 20.0)) & 0x1fffff) << 42) | (uint64_t(int64_t(std::llround(p.y * 20.0)) & 0x1fffff) << 21) |
                             (uint64_t(int64_t(std::llround(p.z * 20.0)) & 0x1fffff));
        auto [it, fresh] = weld.emplace(key, geo::Index(pts.size()));
        if (fresh)
            pts.push_back(p);
        idx[k] = it->second;
    }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    if (m.numFaces() != s.sizes.size())
        return toMesh(s, names); // faces the welded mesh refused: unwelded, every face its own
    m.materialNames() = names;
    auto& fa = m.faceAttrs();
    fa.add(geo::attr::kMaterial, geo::AttrType::Int);
    auto mat = fa.get<int32_t>(geo::attr::kMaterial);
    auto& ca = m.cornerAttrs();
    ca.add(geo::attr::uvName(0), geo::AttrType::Float2);
    auto uv = ca.get<glm::vec2>(geo::attr::uvName(0));
    size_t off = 0;
    for (geo::Index f = 0; f < geo::Index(s.sizes.size()); ++f)
    {
        mat[f] = s.mat[f];
        size_t k = 0;
        for (geo::Index c : m.faceCorners(f))
            uv[c] = s.uvs[off + k++];
        off += s.sizes[f];
    }
    geo::setHardEdgesByAngle(m, 30.0);
    return m;
}

int ensureInteriorMaterials()
{
    scene::Graph& g = IG();
    int made = 0;
    for (int k = 0; k < IntCount; ++k)
    {
        if (scene::materials::find(g, intMatName(k)))
            continue;
        const glm::vec3 c = intMatColour(k);
        ops::execute("createMaterial", {{"args", {intMatName(k)}}, {"baseColor", {c.r, c.g, c.b}}, {"roughness", k == IntFloorTile || k == IntWallTile ? 0.3 : 0.8}});
        ++made;
    }
    return made;
}

scene::Node* findInteriorGroup(const std::string& id)
{
    scene::Graph& g = IG();
    for (scene::NodeId n : g.nodes())
        if (scene::Node* node = g.node(n); node && node->type().attrIndex(kInteriorMark) >= 0)
            if (nodeAttrString(node->path() + "." + kInteriorMark) == id)
                return node;
    return nullptr;
}

std::vector<scene::Node*> childrenOf(scene::Node* n)
{
    std::vector<scene::Node*> out;
    scene::Graph& g = IG();
    for (scene::NodeId c : g.children(n->id()))
        if (scene::Node* cn = g.node(c))
            out.push_back(cn);
    return out;
}

glm::dmat4 localMatrix(scene::Node* n)
{
    if (n->type().isA("transform"))
        return n->as<glm::dmat4>(scene::Transform::kMatrix);
    return glm::dmat4(1.0);
}

std::string currentHash(scene::Node* n)
{
    std::shared_ptr<const geo::Mesh> m = lila::slots::combinedMesh(IG(), n->id());
    return meshHash(m.get(), localMatrix(n));
}

json roomJson(const Room& r)
{
    return {{"id", r.id},
            {"kind", r.kind},
            {"floor", r.floor},
            {"area", std::round(r.area * 10.0) / 10.0},
            {"reachable", r.reachable},
            {"entry", r.entry},
            {"user", r.user},
            {"unit", r.unit},
            {"label", {r.label.x, r.y, r.label.y}},
            {"doors", r.doors}};
}

json doorJson(const InteriorDoor& d)
{
    json j = {{"id", d.id}, {"a", d.a}, {"b", d.b}, {"width", d.width}, {"height", d.height}, {"floor", d.floor}, {"user", d.user},
              {"origin", {d.origin.x, d.origin.y, d.origin.z}}, {"normal", {d.normal.x, d.normal.y, d.normal.z}}};
    if (d.open)
        j["open"] = true;
    if (d.up)
        j["up"] = true;
    return j;
}

json stairJson(const Stair& s)
{
    return {{"id", s.id}, {"kind", s.kind}, {"floor", s.floor}, {"risers", s.risers}, {"riser", std::round(s.riser * 10.0) / 10.0}, {"going", std::round(s.going * 10.0) / 10.0},
            {"slope", std::round(s.slope * 10.0) / 10.0}, {"width", s.width}, {"headroom", std::round(s.headroom)}};
}

std::optional<lila::slots::Slot> slotOf(const Module& m, const std::string& owner, const std::vector<std::string>& regions)
{
    lila::slots::Slot s;
    s.id = lila::slots::makeId(owner, m.path);
    s.type = m.type;
    s.owner = owner;
    s.regions = regions;
    s.tags = {"interior"};
    if (m.type.rfind("furnishing.", 0) == 0)
        s.tags.push_back("furnishing");
    if (!m.data.is_null())
        s.payload.data = m.data;
    s.transform = m.frame();
    return s;
}

/** Writes an interior into the scene: the group, a mesh per room and core, the slots. Returns what was written. */
json writeInterior(const std::string& id, bool reset)
{
    LILA_ZONE();
    std::shared_ptr<const Interior> in = interiorOf(id);
    if (!in)
        throw CommandError("no building " + id + " to make an interior for");
    auto loc = locateBuilding(id);
    scene::Graph& g = IG();
    scene::Node* mesh = loc ? g.find(loc->mesh) : nullptr;
    if (!mesh)
        throw CommandError("no building " + id);
    const scene::NodeId parent = g.parent(mesh->id());
    scene::Node* group = findInteriorGroup(id);
    const std::string base = safeNodeName("Interior_" + hexOf(id));
    if (!group)
    {
        group = g.createNode(*scene::types::find("transform"), base, parent, true);
        markNodeAttr(group->path(), kInteriorMark, id);
    }
    ensureInteriorMaterials();
    const std::string groupPath = group->path();
    const scene::NodeId groupId = group->id();
    std::map<std::string, scene::Node*> existing;
    for (scene::Node* c : childrenOf(group))
        if (c->type().attrIndex(kPartMark) >= 0)
            existing[nodeAttrString(c->path() + "." + kPartMark)] = c;
    json kept = json::array(), orphans = json::array();
    int made = 0, updated = 0, same = 0;
    std::set<std::string> seen;
    for (const InteriorPart& part : in->parts)
    {
        if (part.soup.faces() == 0)
            continue;
        seen.insert(part.id);
        geo::Mesh m = partMesh(part.soup);
        const std::string h = meshHash(&m, glm::dmat4(1.0));
        auto it = existing.find(part.id);
        if (it != existing.end())
        {
            scene::Node* n = it->second;
            const std::string stored = nodeAttrString(n->path() + "." + kPartHash), cur = currentHash(n);
            if (!reset && cur != stored)
            {
                kept.push_back(part.id);
                continue;
            }
            if (cur == h && stored == h)
            {
                ++same;
                continue;
            }
            g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            markNodeAttr(n->path(), kPartHash, h);
            ++updated;
            continue;
        }
        std::string name = part.id;
        for (char& ch : name)
            if (!std::isalnum(static_cast<unsigned char>(ch)))
                ch = '_';
        scene::Node* n = g.createNode(*scene::types::find("mesh"), base + "_" + name, groupId, true);
        g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        markNodeAttr(n->path(), kPartMark, part.id);
        markNodeAttr(n->path(), kPartHash, h);
        ++made;
    }
    for (auto& [pid, n] : existing)
    {
        if (seen.count(pid))
            continue;
        if (!reset && currentHash(n) != nodeAttrString(n->path() + "." + kPartHash))
        {
            orphans.push_back(pid);
            continue;
        }
        ops::execute("delete", {{"args", {n->path()}}});
    }
    // Slots: interior doors, lamps, furnishing, lifts.
    const std::string owner = "interior:" + hexOf(id);
    std::vector<std::string> regions;
    if (loc)
        regions = loc->pub->specs[size_t(loc->index)].regions;
    regions.push_back("building:" + hexOf(id));
    std::vector<lila::slots::Slot> list;
    for (const Module& m : in->slots)
        if (auto s = slotOf(m, owner, regions))
            list.push_back(std::move(*s));
    const json slotStats = lila::slots::writeSet(g, base + "_Slots", groupId, list, "buildingMakeEnterable");
    json out = {{"building", id},
                {"group", g.node(groupId) ? g.node(groupId)->path() : groupPath},
                {"program", in->program},
                {"programFrom", in->programFrom},
                {"rooms", in->rooms.size()},
                {"doors", in->doors.size()},
                {"stairs", in->stairs.size()},
                {"slots", list.size()},
                {"entries", in->entries},
                {"meshesMade", made},
                {"meshesUpdated", updated},
                {"meshesUnchanged", same},
                {"keptEdited", kept},
                {"orphanParts", orphans}};
    int unreachable = 0;
    for (const Room& r : in->rooms)
        unreachable += r.reachable ? 0 : 1;
    out["unreachable"] = unreachable;
    if (!in->warnings.empty())
        out["warnings"] = in->warnings;
    if (!in->notes.empty())
        out["notes"] = in->notes;
    if (!in->orphanEdits.empty())
        out["orphanEdits"] = in->orphanEdits;
    if (slotStats.contains("conflicts") && !slotStats["conflicts"].empty())
        out["slotConflicts"] = slotStats["conflicts"];
    return out;
}

/**
 * The buildings programs=, landmarks= and styles= pick among the named ones (every building when none is named), in id
 * order, at most limit= of them. A building is picked when any given filter matches it.
 */
std::vector<std::string> pickEnterable(const Args& a, const char* cmd)
{
    IG().evaluate();
    // Only buildings named outright narrow the pick; the selection does not.
    const bool naming = !a.positional.empty() || a.has("building") || a.has("polygon");
    const std::vector<std::string> named = naming ? buildingTargets(a, cmd, true) : std::vector<std::string>{};
    const std::set<std::string> within(named.begin(), named.end());
    auto listOf = [&](const char* flag) {
        std::set<std::string> out;
        if (a.has(flag))
            for (const json& v : a.flags.at(flag).is_array() ? a.flags.at(flag) : json::array({a.flags.at(flag)}))
                if (v.is_string())
                    out.insert(v.get<std::string>());
        return out;
    };
    const std::set<std::string> programs = listOf("programs"), styles = listOf("styles");
    const bool landmarks = a.get<bool>("landmarks", false);
    std::set<std::string> picked;
    for (const auto& p : allPublished())
        for (size_t i = 0; i < p->specs.size() && IG().node(scene::NodeId(p->modifier)); ++i)
        {
            const Spec& s = p->specs[i];
            if (!within.empty() && !within.contains(s.id))
                continue;
            if (programs.contains(s.program) || programs.contains(s.use) || styles.contains(p->results[i]->style) || (landmarks && !s.landmark.empty()))
                picked.insert(s.id);
        }
    std::vector<std::string> out(picked.begin(), picked.end());
    if (const int64_t limit = a.get<int64_t>("limit", 0); limit > 0 && int64_t(out.size()) > limit)
        out.resize(size_t(limit));
    return out;
}

/** A ground-floor street bay to turn into the entry when no door opens into a room. */
std::optional<std::string> entryBay(const Result& r)
{
    double best = 1e300;
    std::optional<std::string> out;
    for (const Facade& f : r.facades)
    {
        if (f.side == 1 || f.courtyard || f.tier != 0)
            continue;
        const Vec2 mid = 0.5 * (f.a + f.b);
        for (const Bay& b : r.bays)
        {
            if (b.floor != 0 || b.path.rfind(f.path + "/", 0) != 0 || b.kind == "arcade")
                continue;
            const Vec3 c = 0.5 * (b.corners[0] + b.corners[1]);
            const double d = glm::length(Vec2(c.x, c.z) - mid) + (f.side == 0 ? 0.0 : 5000.0);
            if (d < best)
            {
                best = d;
                out = b.path;
            }
        }
    }
    return out;
}

void setProfile(const CharacterProfile& p) { ops::execute("sceneData", {{"args", {kProfileKey}}, {"value", p.toJson()}}); }

json interiorSummary(const std::string& id, const Interior& in, bool full)
{
    json rooms = json::array(), doors = json::array(), stairs = json::array();
    for (const Room& r : in.rooms)
        rooms.push_back(roomJson(r));
    for (const InteriorDoor& d : in.doors)
        doors.push_back(doorJson(d));
    for (const Stair& s : in.stairs)
        stairs.push_back(stairJson(s));
    json out = {{"building", id}, {"program", in.program}, {"programFrom", in.programFrom}, {"generic", in.generic}, {"rooms", rooms},
                {"doors", doors}, {"stairs", stairs}, {"entries", in.entries}, {"group", interiorGroupOf(id)}};
    if (full)
    {
        json slotTypes = json::object();
        for (const Module& m : in.slots)
            slotTypes[m.type] = slotTypes.value(m.type, 0) + 1;
        out["slotTypes"] = slotTypes;
        json parts = json::array();
        if (scene::Node* grp = findInteriorGroup(id))
            for (scene::Node* c : childrenOf(grp))
                if (c->type().attrIndex(kPartMark) >= 0)
                    parts.push_back({{"part", nodeAttrString(c->path() + "." + kPartMark)},
                                     {"mesh", c->path()},
                                     {"edited", currentHash(c) != nodeAttrString(c->path() + "." + kPartHash)}});
        out["parts"] = parts;
    }
    if (!in.warnings.empty())
        out["warnings"] = in.warnings;
    if (!in.notes.empty())
        out["notes"] = in.notes;
    if (!in.orphanEdits.empty())
        out["orphanEdits"] = in.orphanEdits;
    return out;
}

fs::path userRoomPresetFile() { return lila::PrefStore::get().userDir() / "building-rooms.json"; }

void loadUserRoomPresets()
{
    std::ifstream f(userRoomPresetFile());
    if (!f)
    {
        setUserRoomPresets(json::object());
        return;
    }
    try
    {
        setUserRoomPresets(json::parse(f));
    }
    catch (const std::exception& e)
    {
        lila::log::warn("Buildings: {} is not JSON ({}); the shipped room presets stand", userRoomPresetFile().generic_string(), e.what());
        setUserRoomPresets(json::object());
    }
}

/** Room presets as Set Dressing arrangement presets ("Room: Bedroom"), so any floor can be furnished from the library panel. */
void ensureRoomArrangements()
{
    static bool done = false;
    if (done || !ops::find("arrangementRegister"))
        return;
    done = true;
    // Held: items() of the temporary roomPresets() returns would dangle.
    const json presets = roomPresets();
    for (const auto& [kind, preset] : presets.items())
    {
        std::string label = kind;
        label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
        json steps = json::array({{{"command", "roomFurnish"}, {"args", {"$floor"}}, {"flags", {{"kind", kind}}}}});
        steps.push_back({{"command", "scatterProps"},
                         {"args", {"$floor"}},
                         {"flags", {{"items", "$clutter"}, {"density", 0.4}, {"realize", true}, {"settle", true}, {"name", "Clutter"}}},
                         {"needs", {"clutter"}}});
        // A thumbnail of its own, so the library never stages the preset in the open scene to draw one.
        static const std::map<std::string, std::string> kIcons = {
            {"bedroom", "bed"}, {"living", "sofa"}, {"salon", "armchair"}, {"kitchen", "tools-kitchen-2"}, {"bathroom", "bath"},
            {"toilet", "toilet-paper"}, {"sales", "building-store"}, {"storage", "box"}, {"backroom", "box"}, {"office", "briefcase"},
            {"openfloor", "building-warehouse"}, {"workshop", "tool"}, {"bar", "beer"}, {"nave", "building-church"}, {"cell", "lock"},
            {"briefing", "users"}, {"armory", "shield"}, {"locker", "hanger"}, {"study", "notebook"}, {"dining", "table"}};
        const auto ic = kIcons.find(kind);
        const std::string thumb = (lila::paths::resourcesDir() / "icons" / "tabler" / ((ic == kIcons.end() ? std::string("layout-board") : ic->second) + ".svg")).generic_string();
        const json spec = {{"category", "Rooms"},
                           {"thumbnail", thumb},
                           {"description", "Furnishing for a " + kind + ": the room preset's pieces as slots on the floor, clutter from the library if given."},
                           {"slots",
                            {{"floor", {{"kind", "mesh"}, {"doc", "The room's floor (or the room mesh)."}}},
                             {"clutter", {{"kind", "items"}, {"doc", "Small things strewn about."}, {"optional", true}}}}},
                           {"steps", steps}};
        try
        {
            ops::execute("arrangementRegister", {{"args", {"Room: " + label}}, {"spec", spec}, {"source", "Buildings"}});
        }
        catch (const std::exception& e)
        {
            lila::log::warn("Buildings: room preset {} not offered to Set Dressing ({})", kind, e.what());
        }
    }
}

const std::vector<std::string> kAxes = {"auto", "u", "v"};
} // namespace

// Public ---------------------------------------------------------------------------------------------------------------------

CharacterProfile characterProfile()
{
    try
    {
        return CharacterProfile::fromJson(ops::execute("sceneData", {{"args", {kProfileKey}}}));
    }
    catch (const std::exception&)
    {
        return {};
    }
}

std::vector<std::string> enterableBuildings()
{
    std::vector<std::string> out;
    IG().evaluate();
    for (const auto& p : allPublished())
        for (size_t i = 0; i < p->results.size(); ++i)
            if (p->results[i]->enterable)
                out.push_back(p->specs[i].id);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string interiorGroupOf(const std::string& id)
{
    scene::Node* n = findInteriorGroup(id);
    return n ? n->path() : std::string();
}

/**
 * Set Dressing clutter strewn on every room's floor of a building's interior (realized, settled by physics when asked),
 * one group per room named <room>_Clutter, replacing what an earlier call strewed there.
 */
json scatterClutter(const std::string& id, const json& items, double density, bool settle, const char* cmd)
{
    json out = {{"rooms", 0}, {"placed", 0}};
    if (!ops::find("scatterProps"))
    {
        out["note"] = "Set Dressing is off: no clutter strewn";
        return out;
    }
    scene::Graph& g = IG();
    scene::Node* grp = g.find(interiorGroupOf(id));
    if (!grp)
        return out;
    std::vector<std::string> rooms;
    for (scene::NodeId c : g.children(grp->id()))
        if (scene::Node* n = g.node(c); n && n->type().attrIndex(kPartMark) >= 0)
        {
            const std::string part = nodeAttrString(n->path() + "." + kPartMark);
            if (!part.ends_with("/ceiling") && !part.starts_with("core"))
                rooms.push_back(n->path());
        }
    json errors = json::array();
    int placed = 0;
    for (const std::string& room : rooms)
    {
        const std::string name = safeNodeName(room.substr(room.rfind('|') + 1) + "_Clutter");
        if (scene::Node* old = g.find(name))
            ops::execute("delete", {{"args", {old->path()}}});
        try
        {
            const json r = ops::execute("scatterProps", {{"args", {room}}, {"items", items}, {"density", density}, {"maxCount", 40},
                                                         {"realize", true}, {"settle", settle}, {"name", name},
                                                         // The shell around the room would count as standing on its floor.
                                                         {"avoid", false}});
            placed += int(r.value("count", r.value("nodes", json::array()).size()));
        }
        catch (const std::exception& e)
        {
            errors.push_back(room + ": " + e.what());
        }
    }
    out["rooms"] = rooms.size();
    out["placed"] = placed;
    if (!errors.empty())
    {
        out["errors"] = errors;
        lila::log::warn("{}: clutter not strewn in {} room(s), first: {}", cmd, errors.size(), errors[0].get<std::string>());
    }
    return out;
}

std::shared_ptr<const Interior> interiorOf(const std::string& id)
{
    auto loc = locateBuilding(id);
    if (!loc)
        return nullptr;
    const Spec& spec = loc->pub->specs[size_t(loc->index)];
    std::shared_ptr<const Result> res = loc->pub->results[size_t(loc->index)];
    const json edits = buildingEditsOf(id);
    const json ie = interiorEditsOf(edits);
    const CharacterProfile prof = characterProfile();
    const std::string key = ie.dump() + "|" + prof.toJson().dump();
    {
        std::lock_guard lock(gCacheMutex);
        auto it = gCache.find(id);
        if (it != gCache.end() && it->second.result == res && it->second.key == key)
            return it->second.interior;
    }
    std::shared_ptr<const Style> st = findStyle(res->style);
    if (!st)
        st = nearestStyle(res->style);
    if (!st)
        return nullptr;
    std::shared_ptr<const Interior> in = generateInterior(spec, *res, *st, ie, prof);
    std::lock_guard lock(gCacheMutex);
    gCache[id] = {res, key, in};
    return in;
}

json refreshInteriors(const std::vector<std::string>& ids)
{
    json out = json::array();
    std::vector<std::string> list = ids;
    if (list.empty())
        list = enterableBuildings();
    for (const std::string& id : list)
    {
        if (!findInteriorGroup(id))
            continue;
        auto loc = locateBuilding(id);
        if (!loc || !loc->pub->results[size_t(loc->index)]->enterable)
            continue;
        try
        {
            out.push_back(writeInterior(id, false));
        }
        catch (const std::exception& e)
        {
            out.push_back({{"building", id}, {"error", e.what()}});
        }
    }
    return out;
}

std::optional<RoomHit> pickRoom(Vec3 origin, Vec3 dir, int floor, const std::string& building)
{
    std::optional<RoomHit> best;
    double bestT = 1e300;
    const std::vector<std::string> ids = building.empty() ? enterableBuildings() : std::vector<std::string>{building};
    for (const std::string& id : ids)
    {
        if (!findInteriorGroup(id))
            continue;
        std::shared_ptr<const Interior> in = interiorOf(id);
        if (!in)
            continue;
        for (const FloorGrid& fg : in->grids)
        {
            if (floor >= 0 && fg.floor != floor)
                continue;
            if (std::abs(dir.y) < 1e-9)
                continue;
            const double t = (fg.y + 1.0 - origin.y) / dir.y;
            if (t <= 0.0 || t >= bestT)
                continue;
            const Vec3 p = origin + dir * t;
            const Vec2 d = Vec2(p.x, p.z) - fg.origin;
            const int i = int(std::floor((glm::dot(d, fg.ax) - fg.u0) / fg.cell)), j = int(std::floor((glm::dot(d, fg.az) - fg.v0) / fg.cell));
            if (i < 0 || j < 0 || i >= fg.nx || j >= fg.nz)
                continue;
            const int r = fg.room[size_t(j * fg.nx + i)];
            if (r < 0 || size_t(r) >= in->rooms.size())
                continue;
            const Room& room = in->rooms[size_t(r)];
            bestT = t;
            best = RoomHit{id, room.id, room.kind, room.floor, p, room.rects, room.y};
        }
    }
    return best;
}

json interiorIssues(const std::vector<std::string>& ids, const CharacterProfile& p)
{
    json issues = json::array();
    json counts = {{"roomUnreachable", 0}, {"stairSteep", 0}, {"stairLow", 0}, {"doorNarrow", 0}, {"doorLow", 0}, {"noEntry", 0}, {"doorNoOpening", 0},
                   {"interiorMissing", 0}};
    scene::Graph& g = IG();
    for (const std::string& id : ids)
    {
        scene::Node* grp = findInteriorGroup(id);
        if (!grp)
        {
            issues.push_back({{"kind", "interiorMissing"}, {"building", id}, {"what", "marked enterable but has no interior (buildingMakeEnterable)"}});
            counts["interiorMissing"] = counts["interiorMissing"].get<int>() + 1;
            continue;
        }
        // The interior as built (to the scene's profile), held to the one asked for.
        std::shared_ptr<const Interior> in = interiorOf(id);
        if (!in)
            continue;
        const json c = checkInterior(*in, p);
        for (const json& i : c["issues"])
            issues.push_back(i);
        for (auto& [k, v] : c["counts"].items())
            counts[k] = counts.value(k, 0) + v.get<int>();
        // Doors whose wall the room meshes (as the scene has them, hand edits included) close.
        geo::Mesh world;
        std::vector<std::shared_ptr<const geo::Mesh>> keep;
        for (scene::Node* part : childrenOf(grp))
        {
            if (part->type().attrIndex(kPartMark) < 0)
                continue;
            std::shared_ptr<const geo::Mesh> m = lila::slots::combinedMesh(g, part->id());
            if (!m || m->numFaces() == 0)
                continue;
            const glm::dmat4 wm = part->type().isA("transform") ? part->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
            geo::Mesh mm = *m;
            for (geo::Index q = 0; q < mm.pointCapacity(); ++q)
                if (mm.pointValid(q))
                    mm.setPosition(q, glm::dvec3(wm * glm::dvec4(mm.position(q), 1.0)));
            keep.push_back(std::make_shared<geo::Mesh>(std::move(mm)));
        }
        std::vector<geo::MeshBvh> bvhs(keep.size());
        for (size_t k = 0; k < keep.size(); ++k)
            bvhs[k].build(*keep[k]);
        auto blockedAt = [&](const Vec3& mid, const Vec3& normal) {
            const Vec3 from = mid - normal * 40.0;
            for (const geo::MeshBvh& b : bvhs)
            {
                geo::BvhHit hit;
                if (b.raycast(from, normal, hit) && hit.t < 80.0)
                    return true;
            }
            return false;
        };
        for (const InteriorDoor& d : in->doors)
        {
            const Vec3 mid = d.origin + Vec3(0.0, std::min(100.0, d.height * 0.5), 0.0);
            // An open archway onto a compact stair has the flights' spine behind its middle: any clear lane through it will do.
            std::vector<double> lanes = {0.0};
            if (d.open)
                lanes = {0.0, -0.3, 0.3, -0.15, 0.15};
            const Vec3 across = glm::normalize(glm::cross(Vec3(0.0, 1.0, 0.0), d.normal));
            bool blocked = true;
            for (double f : lanes)
                if (!blockedAt(mid + across * (f * d.width), d.normal))
                {
                    blocked = false;
                    break;
                }
            if (blocked)
            {
                issues.push_back({{"kind", "doorNoOpening"}, {"building", id}, {"id", d.id}, {"what", d.id + ": the wall has no opening for this door"},
                                  {"at", {d.origin.x, d.origin.y, d.origin.z}}});
                counts["doorNoOpening"] = counts["doorNoOpening"].get<int>() + 1;
            }
        }
    }
    int total = 0;
    for (auto& [k, v] : counts.items())
        total += v.get<int>();
    return {{"issues", issues}, {"counts", counts}, {"ok", total == 0}};
}

void registerInterior(ext::Context& ctx)
{
    loadUserRoomPresets();
    // Slot types interiors use.
    auto type = [&](std::string id, std::string label, Vec3 fp, lila::slots::Attach at, std::string ph, std::vector<std::string> tags, std::string doc) {
        if (lila::slots::types().find(id))
            return;
        lila::slots::registerType(ctx, {.id = std::move(id), .label = std::move(label), .tags = std::move(tags), .footprint = fp, .attach = at,
                                        .placeholder = std::move(ph), .doc = std::move(doc)});
    };
    using A = lila::slots::Attach;
    type("door.interior", "Interior door", {90, 210, 12}, A::Wall, "door", {"opening", "interior"}, "A door between two rooms; origin at the sill centre on the wall's middle, +Z toward the second room.");
    type("door.interior.double", "Interior double door", {160, 210, 12}, A::Wall, "door", {"opening", "interior"}, "A double door between public rooms.");
    type("lamp.ceiling", "Ceiling lamp", {40, 30, 40}, A::Ceiling, "lamp", {"lighting", "interior"}, "A light fitting at a room's ceiling; +Y points down.");
    type("lift", "Lift", {160, 230, 160}, A::Ground, "box", {"interior"}, "A lift car at the bottom of its shaft.");
    type("furnishing", "Furnishing", {100, 100, 60}, A::Ground, "box", {"interior", "furnishing"}, "Furniture and fittings in a room; +Z faces into the room.");
    for (const auto& [id, fp] : furnishingTypes())
    {
        std::string label = id.substr(id.find('.') + 1);
        label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
        const bool round = id == "furnishing.stool" || id == "furnishing.toilet";
        type(id, label, fp, A::Ground, round ? "cylinder" : "box", {"interior", "furnishing"}, "Furnishing from a room preset.");
    }

    ctx.registerCommand({
        .name = "buildingMakeEnterable",
        .doc = "Give buildings real interiors from their own grammar: floor plates, a stair core (with a lift on taller buildings), "
               "rooms split by program (shop, house, apartment, office, hotel, warehouse, police, mansion, bar, garage, church; generic "
               "rooms when the program is none of them, and the result says so), partitions with door slots, the facade's windows "
               "and doors cut through the rooms' walls, floors, ceilings, skirting, ceiling lamp slots and furnishing slots from "
               "the room presets. Each room is a mesh of its own under Interior_<id> (model it freely; hand-edited rooms are kept "
               "when the interior regenerates), the slots go in Interior_<id>_Slots. The exterior doors that open into rooms are "
               "tagged 'entry' (a slot rule on that tag gives them your door Blueprint). The building is pinned against regeneration; "
               "rules and overrides still swap its slots. When no door "
               "opens into a room, entry=True paints one on the street. clutter= strews library items on every room's floor with Set "
               "Dressing, settled by physics. Returns per building: rooms, doors, stairs, slots, entries (and clutter).",
        .positionalDoc = "building ids or building meshes (default the selection)",
        .flags = {{"building", "b", T::Any, nullptr, "Building id(s)."},
                  {"program", "pr", T::String, nullptr, "Interior program instead of the one from the building's use.", interiorPrograms()},
                  {"furnish", "fu", T::Bool, true, "Furnishing slots from the room presets."},
                  {"pin", "pn", T::Bool, true, "Pin the building so regeneration leaves it as it is."},
                  {"entry", "en", T::Bool, true, "Paint a door on the street when none opens into a room."},
                  {"reset", "rs", T::Bool, false, "Regenerate hand-edited room meshes too."},
                  {"clutter", "cl", T::List, nullptr, "Library items Set Dressing strews on every room's floor (realized), replacing earlier clutter."},
                  {"clutterDensity", "cd", T::Float, 0.4, "Clutter per square metre.", {}, {}, 0.0, 10.0},
                  {"settle", "st", T::Bool, true, "Drop the clutter with physics so it comes to rest."},
                  {"programs", "prs", T::List, nullptr, "Pick the buildings whose program or use is one of these (police, church, civic, shop, ...)."},
                  {"landmarks", "lmk", T::Bool, false, "Pick the landmark buildings (cathedral, palace, market...)."},
                  {"styles", "sty", T::List, nullptr, "Pick the buildings in these styles."},
                  {"limit", "lim", T::Int, int64_t(0), "At most this many of the picked buildings, in id order; 0 for all.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "buildingMakeEnterable";
            const bool picking = a.has("programs") || a.has("styles") || a.get<bool>("landmarks", false);
            const std::vector<std::string> ids = picking ? pickEnterable(a, cmd) : buildingTargets(a, cmd);
            if (ids.empty())
                return {{"buildings", json::array()}, {"count", 0}, {"note", "no building matched programs=, landmarks= or styles="}};
            ensureRoomArrangements();
            changeBuildingEdits(ids, [&](const std::string&, json& e) {
                e["enterable"] = true;
                json ie = interiorEditsOf(e);
                if (a.has("program"))
                    ie["program"] = a.get<std::string>("program");
                if (a.has("furnish"))
                {
                    if (a.get<bool>("furnish"))
                        ie.erase("furnish");
                    else
                        ie["furnish"] = false;
                }
                if (ie.empty())
                    e.erase("interior");
                else
                    e["interior"] = ie;
            }, cmd);
            IG().evaluate();
            ensureBuildingMaterials({});
            json results = json::array();
            for (const std::string& id : ids)
            {
                std::shared_ptr<const Interior> in = interiorOf(id);
                json added;
                if (in && in->entries.empty() && a.get<bool>("entry", true))
                    if (auto loc = locateBuilding(id))
                        if (auto bay = entryBay(*loc->pub->results[size_t(loc->index)]))
                        {
                            const bool shop = isBusiness(loc->pub->specs[size_t(loc->index)]);
                            changeBuildingEdits({id}, [&](const std::string&, json& e) { e["bays"][*bay] = shop ? "shopdoor" : "door"; }, cmd);
                            IG().evaluate();
                            added = *bay;
                        }
                json r = writeInterior(id, a.get<bool>("reset", false));
                if (!added.is_null())
                    r["entryAdded"] = added;
                if (a.get<bool>("pin", true) && !lila::slots::isPinned("owner:" + id))
                    r["pinned"] = lila::slots::pin(IG(), "owner:" + id, true);
                if (a.has("clutter"))
                    r["clutter"] = scatterClutter(id, a.flags["clutter"], a.get<double>("clutterDensity", 0.4), a.get<bool>("settle", true), cmd);
                results.push_back(r);
            }
            return {{"buildings", results}, {"count", results.size()}};
        },
        .examples = {"buildingMakeEnterable('building:1a2b')", "buildingMakeEnterable('building:1a2b', program='bar')",
                     "buildingMakeEnterable(['building:1a2b', 'building:3c4d'], furnish=False)",
                     "buildingMakeEnterable(programs=['police', 'church'], landmarks=True, limit=10)"},
    });

    ctx.registerCommand({
        .name = "buildingMakeExterior",
        .doc = "Take buildings' interiors away again: their groups (room meshes, hand edits included, and slots) are deleted, "
               "the building is unpinned and its windows go back to fake interiors. The room layout edits stay with the building "
               "for the next buildingMakeEnterable unless clear=True. Undo brings everything back.",
        .positionalDoc = "building ids or building meshes (default the selection)",
        .flags = {{"building", "b", T::Any, nullptr, "Building id(s)."},
                  {"clear", "cl", T::Bool, false, "Drop the room layout edits too."},
                  {"unpin", "up", T::Bool, true, "Unpin the building."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "buildingMakeExterior";
            const std::vector<std::string> ids = buildingTargets(a, cmd);
            int groups = 0, edited = 0;
            for (const std::string& id : ids)
                if (scene::Node* grp = findInteriorGroup(id))
                {
                    for (scene::Node* c : childrenOf(grp))
                        if (c->type().attrIndex(kPartMark) >= 0 && currentHash(c) != nodeAttrString(c->path() + "." + kPartHash))
                            ++edited;
                    ops::execute("delete", {{"args", {grp->path()}}});
                    ++groups;
                }
            changeBuildingEdits(ids, [&](const std::string&, json& e) {
                e.erase("enterable");
                if (a.get<bool>("clear", false))
                    e.erase("interior");
            }, cmd);
            int unpinned = 0;
            if (a.get<bool>("unpin", true))
                for (const std::string& id : ids)
                    if (lila::slots::unpin("owner:" + id))
                        ++unpinned;
            IG().evaluate();
            // A style that is enterable by default needs saying no to.
            std::vector<std::string> still;
            for (const std::string& id : ids)
                if (auto loc = locateBuilding(id); loc && loc->pub->results[size_t(loc->index)]->enterable)
                    still.push_back(id);
            if (!still.empty())
            {
                changeBuildingEdits(still, [](const std::string&, json& e) { e["enterable"] = false; }, cmd);
                IG().evaluate();
            }
            json out = {{"buildings", ids}, {"groupsDeleted", groups}, {"unpinned", unpinned}};
            if (edited > 0)
                out["editedRoomsDeleted"] = edited;
            return out;
        },
        .examples = {"buildingMakeExterior('building:1a2b')", "buildingMakeExterior('building:1a2b', clear=True)"},
    });

    ctx.registerCommand({
        .name = "buildingInterior",
        .doc = "A building's interior: program and where it came from, rooms (id, kind, floor, area m2, reachable, entry, label point, "
               "doors), doors (id, the two rooms, width, height, where), stairs (risers, riser, going, slope, width, headroom), "
               "entries (the exterior doors into rooms), slot counts by type, room meshes and whether each is hand-edited, warnings, "
               "notes and layout edits that no longer find their room. check=True adds buildingCheck's interior issues. programs=True "
               "lists the programs, room kinds and room presets instead.",
        .positionalDoc = "building id",
        .flags = {{"building", "b", T::String, nullptr, "Building id."},
                  {"check", "ck", T::Bool, false, "Add the interior's issues."},
                  {"programs", "pg", T::Bool, false, "List programs, room kinds and room presets."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("programs", false))
                return {{"programs", interiorPrograms()}, {"roomKinds", roomKinds()}, {"presets", roomPresets()}, {"userPresets", userRoomPresetFile().generic_string()}};
            std::string id = a.get<std::string>("building", "");
            if (id.empty() && !a.positional.empty() && a.positional[0].is_string())
                id = a.positional[0].get<std::string>();
            if (id.empty())
            {
                const std::vector<std::string> ids = buildingTargets(a, "buildingInterior");
                id = ids.front();
            }
            std::shared_ptr<const Interior> in = interiorOf(id);
            if (!in)
                throw CommandError("buildingInterior: no building " + id);
            json out = interiorSummary(id, *in, true);
            auto loc = locateBuilding(id);
            out["enterable"] = loc && loc->pub->results[size_t(loc->index)]->enterable;
            out["pinned"] = lila::slots::isPinned("owner:" + id);
            out["edits"] = interiorEditsOf(buildingEditsOf(id));
            if (a.get<bool>("check", false))
                out["check"] = interiorIssues({id}, characterProfile());
            return out;
        },
        .examples = {"buildingInterior('building:1a2b')['rooms']", "buildingInterior(programs=True)['roomKinds']"},
    });

    ctx.registerCommand({
        .name = "buildingRoom",
        .doc = "Edit a building's room layout, kept by room id and replayed on every regeneration: kind= gives a room another kind "
               "(its furnishing follows); split= cuts it in two (auto across its longer side, or u, v along the building's axes) at= "
               "of the way, the new room <room>-split taking newKind=; merge= takes another room into it (the wall between goes); "
               "grow= with by= moves the wall it shares with that room by cm (negative shrinks it); swap= trades kinds with another "
               "room; program= changes the whole interior's program; reset=True drops the room's edits (or every layout edit without "
               "room=). Doors are re-solved after a split, merge or move. Returns the rooms of the interior.",
        .positionalDoc = "building id",
        .flags = {{"building", "b", T::String, nullptr, "Building id."},
                  {"room", "rm", T::String, nullptr, "Room id (buildingInterior lists them, e.g. l0/sales)."},
                  {"kind", "k", T::String, nullptr, "New kind.", roomKinds()},
                  {"split", "sp", T::String, nullptr, "Split along: auto, u or v.", kAxes},
                  {"at", "at", T::Float, 0.5, "Where the split falls, 0 to 1.", {}, {}, 0.05, 0.95},
                  {"newKind", "nk", T::String, nullptr, "Kind of the split-off room.", roomKinds()},
                  {"merge", "mg", T::String, nullptr, "Room id taken into this one."},
                  {"grow", "gw", T::String, nullptr, "Neighbour room the shared wall moves into."},
                  {"by", "by", T::Float, 60.0, "cm the wall moves (grow=).", {}, {}, -2000.0, 2000.0},
                  {"swap", "sw", T::String, nullptr, "Room id to trade kinds with."},
                  {"program", "pr", T::String, nullptr, "Interior program.", interiorPrograms()},
                  {"reset", "rs", T::Bool, false, "Drop the room's layout edits, or every one without room=."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "buildingRoom";
            std::string id = a.get<std::string>("building", "");
            if (id.empty() && !a.positional.empty() && a.positional[0].is_string())
                id = a.positional[0].get<std::string>();
            if (id.empty())
                throw CommandError(std::string(cmd) + ": name the building (building id)");
            std::shared_ptr<const Interior> in = interiorOf(id);
            if (!in)
                throw CommandError(std::string(cmd) + ": no building " + id);
            const std::string room = a.get<std::string>("room", "");
            auto need = [&](const std::string& r, const char* what) {
                if (!in->room(r))
                    throw CommandError(std::string(cmd) + ": " + what + " " + r + " is not a room of " + id + " (buildingInterior lists them)");
            };
            const bool reset = a.get<bool>("reset", false);
            if (!room.empty() && !reset)
                need(room, "room");
            if ((a.has("kind") || a.has("split") || a.has("merge") || a.has("grow") || a.has("swap")) && room.empty())
                throw CommandError(std::string(cmd) + ": give room=");
            if (a.has("kind") && std::find(roomKinds().begin(), roomKinds().end(), a.get<std::string>("kind")) == roomKinds().end())
                throw CommandError(std::string(cmd) + ": kind is one of the room kinds (buildingInterior(programs=True) lists them)");
            std::vector<std::string> changes;
            changeBuildingEdits({id}, [&](const std::string&, json& e) {
                InteriorEdits ie = InteriorEdits::fromJson(interiorEditsOf(e));
                if (reset)
                {
                    if (room.empty())
                        ie = InteriorEdits{};
                    else
                    {
                        ie.kinds.erase(room);
                        ie.splits.erase(room);
                        std::erase_if(ie.merges, [&](const auto& m) { return m.first == room || m.second == room; });
                        std::erase_if(ie.moves, [&](const auto& m) { return m.room == room || m.toward == room; });
                    }
                    changes.push_back("reset");
                }
                if (a.has("program"))
                {
                    ie.program = a.get<std::string>("program");
                    changes.push_back("program");
                }
                if (a.has("kind"))
                {
                    ie.kinds[room] = a.get<std::string>("kind");
                    changes.push_back("kind");
                }
                if (a.has("split"))
                {
                    InteriorEdits::Split s;
                    s.axis = a.get<std::string>("split");
                    s.at = a.get<double>("at", 0.5);
                    s.kind = a.get<std::string>("newKind", "");
                    ie.splits[room] = s;
                    changes.push_back("split");
                }
                if (a.has("merge"))
                {
                    need(a.get<std::string>("merge"), "merge");
                    ie.merges.push_back({room, a.get<std::string>("merge")});
                    changes.push_back("merge");
                }
                if (a.has("grow"))
                {
                    need(a.get<std::string>("grow"), "grow");
                    ie.moves.push_back({room, a.get<std::string>("grow"), a.get<double>("by", 60.0)});
                    changes.push_back("move");
                }
                if (a.has("swap"))
                {
                    const std::string other = a.get<std::string>("swap");
                    need(other, "swap");
                    const std::string k1 = in->room(room)->kind, k2 = in->room(other)->kind;
                    ie.kinds[room] = k2;
                    ie.kinds[other] = k1;
                    changes.push_back("swap");
                }
                const json j = ie.toJson();
                if (j.empty())
                    e.erase("interior");
                else
                    e["interior"] = j;
            }, cmd);
            if (changes.empty())
                throw CommandError(std::string(cmd) + ": nothing to change (kind=, split=, merge=, grow=, swap=, program= or reset=True)");
            IG().evaluate();
            json out = findInteriorGroup(id) ? writeInterior(id, false) : json{{"building", id}};
            std::shared_ptr<const Interior> now = interiorOf(id);
            out["changes"] = changes;
            if (now)
            {
                json rooms = json::array();
                for (const Room& r : now->rooms)
                    rooms.push_back(roomJson(r));
                out["roomList"] = rooms;
            }
            return out;
        },
        .examples = {"buildingRoom('building:1a2b', room='l0/backroom', kind='kitchen')", "buildingRoom('building:1a2b', room='l0/sales', split='auto', newKind='office')",
                     "buildingRoom('building:1a2b', room='l0/sales', grow='l0/backroom', by=80)", "buildingRoom('building:1a2b', room='l1/bedroom', merge='l1/bedroom2')"},
    });

    ctx.registerCommand({
        .name = "buildingDoor",
        .doc = "Add or remove interior doors, kept by id with the building's edits: add=[room, room] puts a door in the wall the two "
               "rooms share (at= a point to put it nearest, width= cm), remove= takes a door away by id (buildingInterior lists them; "
               "a room left without a way in shows in buildingCheck). Cores are named core0@l2 (core 0 on floor 2). Returns the doors.",
        .positionalDoc = "building id",
        .flags = {{"building", "b", T::String, nullptr, "Building id."},
                  {"add", "ad", T::List, nullptr, "The two rooms the door joins."},
                  {"at", "at", T::Vec3, nullptr, "A point the door goes nearest."},
                  {"width", "w", T::Float, nullptr, "Door width, cm.", {}, {}, 50.0, 600.0},
                  {"remove", "rm", T::String, nullptr, "Door id to take away."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "buildingDoor";
            std::string id = a.get<std::string>("building", "");
            if (id.empty() && !a.positional.empty() && a.positional[0].is_string())
                id = a.positional[0].get<std::string>();
            if (id.empty())
                throw CommandError(std::string(cmd) + ": name the building (building id)");
            std::shared_ptr<const Interior> in = interiorOf(id);
            if (!in)
                throw CommandError(std::string(cmd) + ": no building " + id);
            if (!a.has("add") && !a.has("remove"))
                throw CommandError(std::string(cmd) + ": give add=[room, room] or remove=door id");
            if (a.has("add") && (!a.flags["add"].is_array() || a.flags["add"].size() != 2))
                throw CommandError(std::string(cmd) + ": add is [room, room]");
            changeBuildingEdits({id}, [&](const std::string&, json& e) {
                InteriorEdits ie = InteriorEdits::fromJson(interiorEditsOf(e));
                if (a.has("add"))
                {
                    InteriorEdits::DoorAdd d;
                    d.a = a.flags["add"][0].get<std::string>();
                    d.b = a.flags["add"][1].get<std::string>();
                    for (const std::string& r : {d.a, d.b})
                        if (!in->room(r) && r.rfind("core", 0) != 0)
                            throw CommandError(std::string(cmd) + ": " + r + " is not a room of " + id);
                    if (a.has("at"))
                        d.at = Vec2(a.flags["at"][0].get<double>(), a.flags["at"][2].get<double>());
                    d.width = a.get<double>("width", 0.0);
                    ie.addDoors.push_back(d);
                }
                if (a.has("remove"))
                {
                    const std::string door = a.get<std::string>("remove");
                    const auto it = std::find_if(in->doors.begin(), in->doors.end(), [&](const InteriorDoor& d) { return d.id == door; });
                    if (it == in->doors.end())
                        throw CommandError(std::string(cmd) + ": no door " + door + " (buildingInterior lists them)");
                    if (it->user)
                    {
                        const auto jt = std::find_if(ie.addDoors.begin(), ie.addDoors.end(), [&](const InteriorEdits::DoorAdd& d) {
                            return (d.a == it->a && d.b == it->b) || (d.a == it->b && d.b == it->a);
                        });
                        if (jt != ie.addDoors.end())
                            ie.addDoors.erase(jt);
                    }
                    else
                        ie.removeDoors.insert(door);
                }
                const json j = ie.toJson();
                if (j.empty())
                    e.erase("interior");
                else
                    e["interior"] = j;
            }, cmd);
            IG().evaluate();
            json out = findInteriorGroup(id) ? writeInterior(id, false) : json{{"building", id}};
            if (std::shared_ptr<const Interior> now = interiorOf(id))
            {
                json doors = json::array();
                for (const InteriorDoor& d : now->doors)
                    doors.push_back(doorJson(d));
                out["doorList"] = doors;
                if (!now->warnings.empty())
                    out["warnings"] = now->warnings;
            }
            return out;
        },
        .examples = {"buildingDoor('building:1a2b', add=['l0/sales', 'l0/toilet'])", "buildingDoor('building:1a2b', remove='l0/door/backroom+sales')"},
    });

    ctx.registerCommand({
        .name = "buildingCharacterProfile",
        .doc = "The character interiors are checked against (saved with the scene): capsule height and radius, the highest step it "
               "walks up and the steepest slope, Unreal's default character unless set. Stairs are built to it (risers no higher than "
               "18 cm or the step, slopes no steeper), doors are at least its width plus clearance. Interiors regenerate when it "
               "changes. Returns the profile.",
        .flags = {{"height", "h", T::Float, nullptr, "Capsule height, cm.", {}, {}, 30.0, 1000.0},
                  {"radius", "r", T::Float, nullptr, "Capsule radius, cm.", {}, {}, 5.0, 300.0},
                  {"maxStep", "ms", T::Float, nullptr, "Highest step, cm.", {}, {}, 1.0, 200.0},
                  {"maxSlope", "sl", T::Float, nullptr, "Steepest walkable slope, degrees.", {}, {}, 5.0, 85.0},
                  {"reset", "rs", T::Bool, false, "Back to Unreal's default character."}},
        .run = [](const Args& a) -> json {
            CharacterProfile p = a.get<bool>("reset", false) ? CharacterProfile{} : characterProfile();
            bool changed = a.get<bool>("reset", false);
            for (const char* k : {"height", "radius", "maxStep", "maxSlope"})
                if (a.has(k))
                {
                    json j = p.toJson();
                    j[k] = a.get<double>(k);
                    p = CharacterProfile::fromJson(j);
                    changed = true;
                }
            json out = p.toJson();
            if (changed)
            {
                setProfile(p);
                out["interiors"] = refreshInteriors({});
            }
            return out;
        },
        .examples = {"buildingCharacterProfile()", "buildingCharacterProfile(height=190, radius=40, maxStep=30)"},
    });

    ctx.registerCommand({
        .name = "buildingRoomPick",
        .doc = "The room whose floor a ray meets first (origin=, direction=), on floor= only when given: building, room id, kind, "
               "floor and the point. Empty when it meets none.",
        .flags = {{"origin", "o", T::Vec3, nullptr, "Ray origin."},
                  {"direction", "d", T::Vec3, nullptr, "Ray direction."},
                  {"floor", "fl", T::Int, int64_t(-1), "Only this floor."},
                  {"building", "b", T::String, "", "Only this building."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.has("origin") || !a.has("direction"))
                throw CommandError("buildingRoomPick: give origin= and direction=");
            auto vec = [&](const char* f) { return Vec3(a.flags[f][0].get<double>(), a.flags[f][1].get<double>(), a.flags[f][2].get<double>()); };
            const Vec3 d = vec("direction");
            if (glm::length(d) < 1e-9)
                throw CommandError("buildingRoomPick: direction is zero");
            const auto h = pickRoom(vec("origin"), glm::normalize(d), int(a.get<int64_t>("floor", -1)), a.get<std::string>("building", ""));
            if (!h)
                return json::object();
            return {{"building", h->building}, {"room", h->room}, {"kind", h->kind}, {"floor", h->floor}, {"point", {h->point.x, h->point.y, h->point.z}}};
        },
    });

    ctx.registerCommand({
        .name = "roomFurnish",
        .doc = "Furnish a floor as a room kind from the room presets (the pieces Buildings puts in its rooms: beds, counters, "
               "shelves, pews...): slots on the floor's upward faces, in <floor>_Furnishing, each a furnishing.* slot a rule or "
               "the library swaps for a real piece. The Set Dressing presets 'Room: <Kind>' run this. Returns the slot set.",
        .positionalDoc = "floor mesh",
        .flags = {{"kind", "k", T::String, "living", "Room kind.", roomKinds()}, {"seed", "s", T::Int, int64_t(1), "Another seed, another layout."}},
        .run = [](const Args& a) -> json {
            const std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("roomFurnish: give the floor mesh");
            scene::Graph& g = IG();
            scene::Node* n = g.find(objs[0]);
            if (!n)
                throw CommandError("roomFurnish: no node " + objs[0]);
            std::shared_ptr<const geo::Mesh> m = lila::slots::combinedMesh(g, n->id());
            if (!m || m->numFaces() == 0)
                throw CommandError("roomFurnish: " + objs[0] + " has no faces");
            const glm::dmat4 wm = n->type().isA("transform") ? n->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
            Vec2 lo(1e300), hi(-1e300);
            double ySum = 0.0, yMin = 1e300;
            int cnt = 0;
            for (geo::Index f = 0; f < m->faceCapacity(); ++f)
            {
                if (!m->faceValid(f))
                    continue;
                std::vector<Vec3> pts;
                for (geo::Index c : m->faceCorners(f))
                    pts.push_back(Vec3(wm * glm::dvec4(m->position(m->cornerPoint(c)), 1.0)));
                if (pts.size() < 3)
                    continue;
                const Vec3 nrm = glm::cross(pts[1] - pts[0], pts[2] - pts[0]);
                if (glm::length(nrm) < 1e-9 || glm::normalize(nrm).y < 0.7)
                    continue;
                for (const Vec3& p : pts)
                {
                    lo = glm::min(lo, Vec2(p.x, p.z));
                    hi = glm::max(hi, Vec2(p.x, p.z));
                    yMin = std::min(yMin, p.y);
                    ySum += p.y;
                    ++cnt;
                }
            }
            if (cnt == 0)
                throw CommandError("roomFurnish: " + objs[0] + " has no upward faces to stand things on");
            const double y = yMin;
            const std::string kind = a.get<std::string>("kind", "living");
            const std::vector<std::array<Vec2, 4>> rects = {{Vec2(lo.x, lo.y), Vec2(hi.x, lo.y), Vec2(hi.x, hi.y), Vec2(lo.x, hi.y)}};
            const std::string owner = "room:" + n->name();
            std::vector<Module> mods = furnishArea(kind, rects, y, y + 270.0, {}, kind, uint64_t(a.get<int64_t>("seed", 1)));
            std::vector<lila::slots::Slot> list;
            for (const Module& md : mods)
                if (auto s = slotOf(md, owner, {}))
                    list.push_back(std::move(*s));
            const json st = lila::slots::writeSet(g, safeNodeName(n->name() + "_Furnishing"), g.parent(n->id()), list, "roomFurnish");
            return {{"set", st.value("set", std::string())}, {"slots", list.size()}, {"kind", kind}, {"y", ySum / cnt}};
        },
        .examples = {"roomFurnish('Floor', kind='bedroom')"},
    });

    ctx.registerCommand({
        .name = "buildingRoomPresets",
        .doc = "Room presets: what furnishing each room kind gets (pieces of furnishing.* types placed against a wall, in a corner, "
               "in the middle, in rows or a grid, a minimum room area each, the ceiling lamp and a Set Dressing arrangement for "
               "clutter). save= merges your presets by kind into your user file and regenerates interiors; reload=True reads it "
               "again. Returns the presets in use.",
        .flags = {{"save", "sv", T::Any, nullptr, "{kind: {pieces: [{type, place, minArea}], light, arrangement}}."},
                  {"reload", "rl", T::Bool, false, "Read the user file again."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("save"))
            {
                if (!a.flags["save"].is_object())
                    throw CommandError("buildingRoomPresets: save is {kind: preset}");
                json user = json::object();
                if (std::ifstream f(userRoomPresetFile()); f)
                    try
                    {
                        user = json::parse(f);
                    }
                    catch (const std::exception&)
                    {
                    }
                for (auto& [k, v] : a.flags["save"].items())
                {
                    if (!v.is_object() || !v.contains("pieces") || !v["pieces"].is_array())
                        throw CommandError("buildingRoomPresets: preset " + k + " needs pieces: [...]");
                    for (const json& p : v["pieces"])
                        if (!p.is_object() || !furnishingTypes().count(p.value("type", std::string())))
                            throw CommandError("buildingRoomPresets: " + k + " names a type that is not a furnishing type");
                    user[k] = v;
                }
                std::error_code ec;
                fs::create_directories(userRoomPresetFile().parent_path(), ec);
                std::ofstream out(userRoomPresetFile());
                if (!out)
                    throw CommandError("buildingRoomPresets: can't write " + userRoomPresetFile().generic_string());
                out << user.dump(2);
                out.close();
                setUserRoomPresets(user);
                {
                    std::lock_guard lock(gCacheMutex);
                    gCache.clear();
                }
                refreshInteriors({});
            }
            else if (a.get<bool>("reload", false))
            {
                loadUserRoomPresets();
                std::lock_guard lock(gCacheMutex);
                gCache.clear();
            }
            ensureRoomArrangements();
            return {{"presets", roomPresets()}, {"file", userRoomPresetFile().generic_string()}, {"types", [] {
                         json t = json::array();
                         for (const auto& [k, v] : furnishingTypes())
                             t.push_back(k);
                         return t;
                     }()}};
        },
    });
}
} // namespace buildings
