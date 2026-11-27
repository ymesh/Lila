// The Buildings geometry node: building specs and edits in, the merged shell and every module as an instance out,
// each module published as a slot. Buildings are generated in parallel and cached by what went into them, so an edit
// to one building of a cell regenerates only that one.

#include "BuildingsScene.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Command.h>
#include <lila/Events.h>
#include <lila/FileBytes.h>
#include <lila/SceneData.h>
#include <lila/Release.h>
#include <lila/SceneFile.h>
#include <lila/Slots.h>
#include <lila/Tasks.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <zstd.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Result has members named near and far.
#undef near
#undef far
#endif

namespace buildings
{
using lila::gn::ExecContext;
using lila::gn::GeometryPtr;
namespace slots = lila::slots;

namespace
{
struct Cached
{
    std::shared_ptr<const Result> result;
    uint64_t tick = 0;
};
std::mutex gGenMutex;
std::unordered_map<uint64_t, Cached> gGen;
uint64_t gTick = 0;
constexpr size_t kCacheMax = 30000;
/** Buildings the last cache read brought in, and half as many again: a big world read whole stays until its cells use it. */
size_t gCacheFloor = 0;

std::mutex gPubMutex;
/** The building Edit Rooms cuts away: its shell and modules are left out of its mesh. */
std::string gCutawayId;
/** What modifiers set to lod auto show, by camera distance (the viewport sets it); near until it has. */
std::unordered_map<uint64_t, std::string> gViewLod;
std::map<uint64_t, std::shared_ptr<const Published>> gPub;

std::mutex gGeoMutex;
std::unordered_map<std::string, GeometryPtr> gGeo;

struct ParsedSpecs
{
    uint64_t hash = 0;
    std::vector<Spec> specs;
    std::vector<uint64_t> hashes;
};
std::mutex gParseMutex;
std::map<uint64_t, std::shared_ptr<const ParsedSpecs>> gParsed; // by owner

std::shared_ptr<const ParsedSpecs> parseSpecs(uint64_t owner, const std::string& text)
{
    const uint64_t h = fnv(text);
    {
        std::lock_guard lock(gParseMutex);
        auto it = gParsed.find(owner);
        if (it != gParsed.end() && it->second->hash == h)
            return it->second;
    }
    auto p = std::make_shared<ParsedSpecs>();
    p->hash = h;
    json j;
    try
    {
        j = text.empty() ? json::array() : json::parse(text);
    }
    catch (const std::exception& e)
    {
        throw lila::gn::ExecError(std::string("Buildings: spec is not JSON: ") + e.what());
    }
    if (!j.is_array())
        throw lila::gn::ExecError("Buildings: spec is a list of buildings");
    p->specs.reserve(j.size());
    for (const json& e : j)
    {
        p->specs.push_back(Spec::fromJson(e));
        p->hashes.push_back(fnv(e.dump()));
    }
    std::lock_guard lock(gParseMutex);
    gParsed[owner] = p;
    return p;
}

GeometryPtr emptyRef()
{
    static const GeometryPtr g = lila::gn::makeGeometry(lila::gn::Geometry{});
    return g;
}

GeometryPtr placeholderRef(const Module& m)
{
    const Vec3 q = placeholderSize(m.size);
    const std::string key = m.kind + "/" + std::to_string(int(q.x)) + "/" + std::to_string(int(q.y)) + "/" + std::to_string(int(q.z)) + "/" +
                            std::to_string(m.variant);
    {
        std::lock_guard lock(gGeoMutex);
        auto it = gGeo.find(key);
        if (it != gGeo.end())
            return it->second;
    }
    const Soup& s = placeholderSoup(m.kind, m.size, m.variant);
    std::vector<std::string> names;
    for (int k = 0; k < MatCount; ++k)
        names.push_back(matName(k));
    GeometryPtr g = lila::gn::meshGeometry(toMesh(s, names));
    std::lock_guard lock(gGeoMutex);
    gGeo.emplace(key, g);
    return g;
}

std::string styleIdFor(const Spec& s, const json& edit)
{
    if (edit.is_object() && edit.contains("style") && edit["style"].is_string())
        return edit["style"].get<std::string>();
    return s.style.empty() ? styleFor("", s.program, s.use) : s.style;
}

uint64_t styleHash(const Style& s)
{
    return fnv(s.toJson().dump());
}

bool setsVisual(const slots::Payload& p)
{
    return !p.item.empty() || !p.pool.empty() || !p.poolTag.empty() || !p.mesh.empty() || !p.placeholder.empty() || (p.hidden && *p.hidden);
}

void exec(ExecContext& c)
{
    LILA_ZONE();
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t owner = c.env.owner;
    const bool autoLod = c.s("lod") == "auto";
    std::string lod = c.s("lod").empty() ? "near" : c.s("lod");
    if (autoLod)
    {
        std::lock_guard lock(gPubMutex);
        const auto it = gViewLod.find(owner);
        lod = it == gViewLod.end() ? "near" : it->second;
    }
    const std::string cell = c.s("cell");
    const bool withModules = c.b("modules");
    std::shared_ptr<const ParsedSpecs> parsed;
    {
        LILA_ZONE_N("buildings.parse");
        parsed = parseSpecs(owner, c.s("spec"));
    }
    json edits = json::object();
    if (const std::string e = c.s("edits"); !e.empty())
    {
        try
        {
            edits = json::parse(e);
        }
        catch (const std::exception& ex)
        {
            throw lila::gn::ExecError(std::string("Buildings: edits are not JSON: ") + ex.what());
        }
    }

    auto pub = std::make_shared<Published>();
    pub->modifier = owner;
    pub->cell = cell;
    pub->lod = lod;
    pub->autoLod = autoLod;
    for (const Spec& s : parsed->specs)
        for (const Vec2& p : s.footprint)
        {
            pub->lo = glm::min(pub->lo, Vec3(p.x, s.ground, p.y));
            pub->hi = glm::max(pub->hi, Vec3(p.x, s.ground, p.y));
        }
    pub->specs = parsed->specs;
    const size_t n = parsed->specs.size();
    pub->results.resize(n);

    // Styles and cache keys.
    std::map<std::string, std::pair<std::shared_ptr<const Style>, uint64_t>> styles;
    std::vector<std::shared_ptr<const Style>> styleOf(n);
    std::vector<Edits> editOf(n);
    std::vector<uint64_t> keys(n);
    std::map<std::string, std::string> missingStyles;
    for (size_t i = 0; i < n; ++i)
    {
        const Spec& s = parsed->specs[i];
        const json e = edits.value(s.id, json::object());
        std::string id = styleIdFor(s, e);
        auto it = styles.find(id);
        if (it == styles.end())
        {
            std::shared_ptr<const Style> st = findStyle(id);
            if (!st)
            {
                st = nearestStyle(id);
                if (st)
                    missingStyles[id] = st->id;
            }
            if (!st)
                throw lila::gn::ExecError("Buildings: no styles are loaded (the Buildings resources are missing)");
            it = styles.emplace(id, std::make_pair(st, styleHash(*st))).first;
        }
        styleOf[i] = it->second.first;
        editOf[i] = Edits::fromJson(e);
        keys[i] = parsed->hashes[i] ^ (it->second.second * 31) ^ (fnv(e.dump()) * 1099511628211ull);
        pub->byId[s.id] = int(i);
    }
    pub->keys = keys;
    for (const auto& [m, used] : missingStyles)
        pub->warnings.push_back("style '" + m + "' is not in the library; '" + used + "' stands in (add " + m +
                                ".json to the workspace's BuildingStyles folder)");

    // Generate what the cache lacks, in parallel.
    std::vector<size_t> todo;
    {
        std::lock_guard lock(gGenMutex);
        ++gTick;
        for (size_t i = 0; i < n; ++i)
        {
            auto it = gGen.find(keys[i]);
            if (it != gGen.end())
            {
                pub->results[i] = it->second.result;
                it->second.tick = gTick;
            }
            else
                todo.push_back(i);
        }
    }
    {
        LILA_ZONE_N("buildings.generate");
        tbb::parallel_for(tbb::blocked_range<size_t>(0, todo.size()), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t k = r.begin(); k < r.end(); ++k)
            {
                const size_t i = todo[k];
                pub->results[i] = generate(parsed->specs[i], *styleOf[i], editOf[i]);
            }
        });
    }
    pub->generated = int(todo.size());
    {
        std::lock_guard lock(gGenMutex);
        for (size_t i : todo)
            gGen[keys[i]] = {pub->results[i], gTick};
        if (gGen.size() > std::max(kCacheMax, gCacheFloor))
        {
            std::vector<std::pair<uint64_t, uint64_t>> ages;
            for (auto& [k, v] : gGen)
                ages.push_back({v.tick, k});
            std::sort(ages.begin(), ages.end());
            for (size_t k = 0; k < ages.size() / 3; ++k)
                gGen.erase(ages[k].second);
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (const std::string& w : pub->results[i]->warnings)
            pub->warnings.push_back(parsed->specs[i].id + ": " + w);

    // Slots for every module, and which shell modules a rule or override replaced.
    const slots::StatePtr st = slots::state();
    const bool anyChange = !st->rules.empty() || !st->overrides.empty();
    std::vector<std::string> rulePatterns;
    for (const slots::Rule& r : st->rules)
        if (r.enabled)
            rulePatterns.push_back(r.type);

    std::string cutaway;
    {
        std::lock_guard lock(gPubMutex);
        cutaway = gCutawayId;
    }
    Soup shell;
    std::vector<int32_t> faceBuilding;
    std::vector<std::string> materialNames;
    std::unordered_map<std::string, int32_t> materialIndex;
    auto materialId = [&](const std::string& name) {
        auto it = materialIndex.find(name);
        if (it != materialIndex.end())
            return it->second;
        const int32_t id = int32_t(materialNames.size());
        materialNames.push_back(name);
        materialIndex.emplace(name, id);
        return id;
    };
    auto inst = std::make_shared<lila::gn::Instances>();
    // The building's palette on its modules, which are shared placeholder meshes across buildings: two tones each.
    std::vector<glm::vec4> moduleColors, moduleColors2;
    std::unordered_map<std::string, int> refIndex;
    slots::InstanceSlots spec;
    spec.owner = "buildings:" + cell;
    std::vector<slots::Slot> slotList;
    {
        LILA_ZONE_N("buildings.assemble");
        size_t total = 0;
        for (size_t i = 0; i < n; ++i)
            total += pub->results[i]->modules.size();
        slotList.reserve(total);
        shell.reserve(4096);
        for (size_t i = 0; i < n; ++i)
        {
            const Result& r = *pub->results[i];
            const Spec& sp = parsed->specs[i];
            const bool cut = !cutaway.empty() && r.id == cutaway;
            std::vector<std::string> regions = sp.regions;
            regions.push_back("style:" + r.style);
            if (!cell.empty())
                regions.push_back("cell:" + cell);
            std::vector<std::string> tags = {"building"};
            if (!sp.program.empty())
                tags.push_back(sp.program);
            if (r.enterable)
                tags.push_back("enterable");
            std::vector<char> dropped(r.modules.size(), 0);
            for (size_t k = 0; k < r.modules.size(); ++k)
            {
                const Module& m = r.modules[k];
                slots::Slot s;
                s.id = slots::makeId(r.id, m.path);
                s.type = m.type;
                s.owner = r.id;
                s.regions = regions;
                s.tags = tags;
                if (!m.data.is_null())
                {
                    s.payload.data = m.data;
                    // The entry doors of enterable buildings: a rule on tag "entry" gives them the game's door class.
                    if (m.data.contains("entry"))
                        s.tags.push_back("entry");
                }
                s.transform = m.frame();
                if (m.shell && anyChange)
                {
                    bool check = st->overrides.count(s.id) > 0;
                    for (size_t p = 0; p < rulePatterns.size() && !check; ++p)
                        check = slots::typeMatches(rulePatterns[p], m.type);
                    if (check && setsVisual(slots::resolve(s, *st).payload))
                        dropped[k] = 1;
                }
                if (lod == "near" && withModules && !cut)
                {
                    int ref = 0;
                    if (m.shell)
                    {
                        auto it = refIndex.find("");
                        if (it == refIndex.end())
                        {
                            ref = int(inst->references.size());
                            inst->references.push_back(emptyRef());
                            refIndex.emplace("", ref);
                        }
                        else
                            ref = it->second;
                    }
                    else
                    {
                        const Vec3 q = placeholderSize(m.size);
                        const std::string key = m.kind + "/" + std::to_string(int(q.x)) + "/" + std::to_string(int(q.y)) + "/" +
                                                std::to_string(int(q.z)) + "/" + std::to_string(m.variant);
                        auto it = refIndex.find(key);
                        if (it == refIndex.end())
                        {
                            ref = int(inst->references.size());
                            inst->references.push_back(placeholderRef(m));
                            refIndex.emplace(key, ref);
                        }
                        else
                            ref = it->second;
                    }
                    inst->add(s.transform, ref);
                    const std::array<glm::vec4, 2> tones = m.shell ? std::array<glm::vec4, 2>{} : moduleTones(*styleOf[i], r.palette, m.kind);
                    moduleColors.push_back(tones[0]);
                    moduleColors2.push_back(tones[1]);
                }
                slotList.push_back(std::move(s));
            }

            if (cut)
                continue;
            // Shell faces, materials by palette.
            const Soup& src = lod == "near" ? r.shell : lod == "hlod" ? r.hlod : r.far;
            int32_t mats[MatCount];
            for (int k = 0; k < MatCount; ++k)
                mats[k] = materialId(materialFor(r, k));
            const int32_t bayBase = 0;
            (void)bayBase;
            for (size_t f = 0, off = 0; f < src.sizes.size(); off += src.sizes[f], ++f)
            {
                const int32_t o = src.owner[f];
                if (lod == "near" && o >= 0 && size_t(o) < dropped.size() && dropped[size_t(o)])
                    continue;
                const int32_t m = mats[std::clamp(src.mat[f], 0, int(MatCount) - 1)];
                shell.poly(src.pts.data() + off, src.uvs.data() + off, src.sizes[f], m, src.trim[f], o, src.part[f], src.bay[f], src.colour[f]);
                faceBuilding.push_back(int32_t(i));
            }
        }
    }

    lila::gn::Geometry g;
    {
        LILA_ZONE_N("buildings.mesh");
        // toMesh writes material ids as given; the soup holds node material indices already.
        lila::geo::Mesh mesh = toMesh(shell, materialNames);
        if (mesh.numFaces() == faceBuilding.size())
        {
            auto& fa = mesh.faceAttrs();
            fa.add("bldIndex", lila::geo::AttrType::Int);
            auto bi = fa.get<int32_t>("bldIndex");
            std::copy(faceBuilding.begin(), faceBuilding.end(), bi.begin());
        }
        pub->faces = mesh.numFaces();
        g.mesh = std::make_shared<const lila::geo::Mesh>(std::move(mesh));
    }
    pub->slots = slotList.size();
    GeometryPtr out;
    if (lod == "near" && withModules)
    {
        LILA_ZONE_N("buildings.slots");
        std::span<glm::vec4> colors = inst->attrs.getOrAdd<glm::vec4>(lila::gn::kInstanceColorAttr);
        std::copy(moduleColors.begin(), moduleColors.end(), colors.begin());
        std::span<glm::vec4> colors2 = inst->attrs.getOrAdd<glm::vec4>(lila::gn::kInstanceColor2Attr);
        std::copy(moduleColors2.begin(), moduleColors2.end(), colors2.begin());
        g.instances = inst;
        g.keepInstances = !c.b("realize");
        if (slotList.size() == inst->size())
            out = slots::applySlots(lila::gn::makeGeometry(std::move(g)), lila::scene::NodeId(owner), std::move(slotList));
        else
        {
            spec.slots = std::move(slotList);
            out = slots::applyToInstances(lila::gn::makeGeometry(std::move(g)), lila::scene::NodeId(owner), spec);
        }
    }
    else
    {
        if (owner != 0)
            slots::publish(lila::scene::NodeId(owner), std::move(slotList));
        out = lila::gn::makeGeometry(std::move(g));
    }
    pub->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard lock(gPubMutex);
        gPub[owner] = pub;
    }
    c.out("geometry", out);
}
} // namespace

std::string materialFor(const Result& r, int mat)
{
    switch (mat)
    {
    case MatRender:
        return r.palette.renderIndex >= 0 ? "Bld_" + r.style + "_render" + std::to_string(r.palette.renderIndex) : "Bld_c" + colourHex(r.palette.render).substr(1);
    case MatTrim:
        return r.palette.trimIndex >= 0 ? "Bld_" + r.style + "_trim" + std::to_string(r.palette.trimIndex) : "Bld_c" + colourHex(r.palette.trim).substr(1);
    case MatRoof:
        return r.palette.roofIndex >= 0 ? "Bld_" + r.style + "_roof" + std::to_string(r.palette.roofIndex) : "Bld_c" + colourHex(r.palette.roof).substr(1);
    case MatBase:
        return "Bld_" + r.style + "_base";
    default:
        return matName(mat);
    }
}

std::vector<std::pair<std::string, glm::vec3>> styleMaterials(const Style& s)
{
    std::vector<std::pair<std::string, glm::vec3>> out;
    for (size_t k = 0; k < s.renders.size(); ++k)
        out.push_back({"Bld_" + s.id + "_render" + std::to_string(k), s.renders[k]});
    for (size_t k = 0; k < s.trims.size(); ++k)
        out.push_back({"Bld_" + s.id + "_trim" + std::to_string(k), s.trims[k]});
    for (size_t k = 0; k < s.roofs.size(); ++k)
        out.push_back({"Bld_" + s.id + "_roof" + std::to_string(k), s.roofs[k]});
    out.push_back({"Bld_" + s.id + "_base", s.bases.empty() ? glm::vec3(0.35f) : s.bases.front()});
    return out;
}

std::shared_ptr<const Published> published(uint64_t modifier)
{
    std::lock_guard lock(gPubMutex);
    auto it = gPub.find(modifier);
    return it == gPub.end() ? nullptr : it->second;
}

void allPublished(std::vector<std::shared_ptr<const Published>>& out)
{
    out.clear();
    const lila::scene::Graph& g = lila::scene::Graph::current();
    std::vector<std::shared_ptr<const Published>> gone;
    {
        std::lock_guard lock(gPubMutex);
        out.reserve(gPub.size());
        for (auto it = gPub.begin(); it != gPub.end();)
            if (!g.node(lila::scene::NodeId(it->first)))
            {
                gone.push_back(std::move(it->second));
                it = gPub.erase(it);
            }
            else
                out.push_back((it++)->second);
    }
    if (!gone.empty())
    {
        std::lock_guard lock(gParseMutex);
        std::erase_if(gParsed, [&](const auto& e) { return !g.node(lila::scene::NodeId(e.first)); });
    }
    lila::release::laterEach(std::move(gone));
}

std::vector<std::shared_ptr<const Published>> allPublished()
{
    std::vector<std::shared_ptr<const Published>> out;
    allPublished(out);
    return out;
}

void forgetPublished(uint64_t modifier)
{
    std::lock_guard lock(gPubMutex);
    gPub.erase(modifier);
}

bool setViewLod(uint64_t modifier, const std::string& lod)
{
    std::lock_guard lock(gPubMutex);
    std::string& was = gViewLod[modifier];
    if (was == lod)
        return false;
    was = lod;
    return true;
}

bool setCutawayBuilding(const std::string& id)
{
    std::lock_guard lock(gPubMutex);
    if (gCutawayId == id)
        return false;
    gCutawayId = id;
    return true;
}

void clearGenerationCache()
{
    std::lock_guard lock(gGenMutex);
    gGen.clear();
}

namespace
{
constexpr char kCacheMagic[8] = {'L', 'I', 'L', 'A', 'B', 'L', 'D', '2'};
/** Fast enough that a world's buildings compress in under a second on a few cores, a third or less of the raw size. */
constexpr int kCacheLevel = 3;
/** No building comes near this: a frame claiming more is damaged. */
constexpr unsigned long long kMaxBuildingBytes = 1ull << 30;

/** The Buildings module on disk (file, size, time): a cache from any other build of the grammar is not trusted. */
uint64_t generatorId()
{
    static const uint64_t id = [] {
        std::string who = "buildings";
#ifdef _WIN32
        HMODULE self = nullptr;
        wchar_t file[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&generatorId), &self) &&
            GetModuleFileNameW(self, file, MAX_PATH))
        {
            std::error_code ec;
            const std::filesystem::path p(file);
            who += "|" + p.filename().string() + "|" + std::to_string(std::filesystem::file_size(p, ec)) + "|" +
                   std::to_string(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
        }
#endif
        return fnv(who);
    }();
    return id;
}

std::mutex gDiskMutex;
/** By cache file: the hash of the keys it holds, so a save that changed no building writes nothing. */
std::map<std::filesystem::path, uint64_t> gDiskHash;

void put(std::string& out, uint64_t v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); }
uint64_t get(std::string_view in, size_t at)
{
    uint64_t v = 0;
    std::memcpy(&v, in.data() + at, sizeof v);
    return v;
}

using Buildings = std::map<uint64_t, std::shared_ptr<const Result>>;

std::string compressBlob(const std::string& raw)
{
    std::string out(ZSTD_compressBound(raw.size()), '\0');
    const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), kCacheLevel);
    out.resize(ZSTD_isError(n) ? 0 : n);
    return out;
}

/** The building in one compressed entry; null when the entry is damaged. */
std::shared_ptr<const Result> decodeBlob(std::string_view blob)
{
    const unsigned long long size = ZSTD_getFrameContentSize(blob.data(), blob.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > kMaxBuildingBytes)
        return nullptr;
    std::string raw(size_t(size), '\0');
    const size_t n = ZSTD_decompress(raw.data(), raw.size(), blob.data(), blob.size());
    if (ZSTD_isError(n) || n != raw.size())
        return nullptr;
    return decodeResult(raw);
}

/** Every building the scene's cells show, by key. */
Buildings sceneBuildings()
{
    Buildings all;
    for (const std::shared_ptr<const Published>& p : allPublished())
        for (size_t i = 0; i < p->keys.size() && i < p->results.size(); ++i)
            if (p->results[i])
                all.emplace(p->keys[i], p->results[i]);
    return all;
}

uint64_t keysHash(const std::vector<uint64_t>& keys)
{
    uint64_t h = generatorId();
    for (uint64_t k : keys)
        h = (h ^ k) * 1099511628211ull;
    return h;
}

uint64_t keysHash(const Buildings& all)
{
    std::vector<uint64_t> keys;
    keys.reserve(all.size());
    for (const auto& [k, r] : all)
        keys.push_back(k);
    return keysHash(keys);
}

/** True when the cache file already holds exactly these buildings. */
bool unchanged(const std::filesystem::path& file, uint64_t hash)
{
    std::lock_guard lock(gDiskMutex);
    std::error_code ec;
    const auto it = gDiskHash.find(file);
    return it != gDiskHash.end() && it->second == hash && std::filesystem::exists(file, ec);
}

/** One write at a time: the save hook's background write and buildingCache write= go through it in turn. */
std::mutex gWriteMutex;

long long writeBuildings(const std::filesystem::path& file, const Buildings& all, uint64_t hash)
{
    LILA_ZONE();
    std::lock_guard writing(gWriteMutex);
    if (all.empty() || unchanged(file, hash))
        return -1;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<uint64_t, std::shared_ptr<const Result>>> list(all.begin(), all.end());
    std::vector<std::string> blobs(list.size());
    std::atomic<uint64_t> raw{0};
    tbb::parallel_for(tbb::blocked_range<size_t>(0, list.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            const std::string bytes = encodeResult(*list[i].second);
            raw += bytes.size();
            blobs[i] = compressBlob(bytes);
        }
    });
    std::string head(kCacheMagic, sizeof kCacheMagic);
    put(head, generatorId());
    put(head, list.size());
    uint64_t offset = head.size() + list.size() * 24;
    for (size_t i = 0; i < list.size(); ++i)
    {
        put(head, list[i].first);
        put(head, offset);
        put(head, blobs[i].size());
        offset += blobs[i].size();
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.generic_string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(head.data(), std::streamsize(head.size()));
        for (const std::string& b : blobs)
            out.write(b.data(), std::streamsize(b.size()));
        if (!out)
        {
            lila::log::warn("Buildings: the building cache {} could not be written", file.generic_string());
            return 0;
        }
    }
    if (const std::error_code renamed = lila::renameRetrying(tmp, file))
    {
        lila::log::warn("Buildings: the building cache {} could not be written: {}", file.generic_string(), renamed.message());
        return 0;
    }
    {
        std::lock_guard lock(gDiskMutex);
        gDiskHash[file] = hash;
    }
    lila::release::laterEach(std::move(blobs));
    lila::log::info("Buildings: {} buildings cached in {} ({:.0f} MB, {:.0f} MB uncompressed, {:.1f} s)", list.size(), file.generic_string(),
                    double(offset) / 1048576.0, double(raw.load()) / 1048576.0,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return (long long)list.size();
}
} // namespace

std::filesystem::path cacheFile(const std::filesystem::path& scene)
{
    return scene.parent_path() / (scene.stem().string() + "_cache") / "buildings.bin";
}

long long writeCache(const std::filesystem::path& scene)
{
    const std::filesystem::path file = cacheFile(scene);
    const Buildings all = sceneBuildings();
    return writeBuildings(file, all, keysHash(all));
}

size_t readCache(const std::filesystem::path& scene)
{
    LILA_ZONE();
    const std::filesystem::path file = cacheFile(scene);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return 0;
    const auto t0 = std::chrono::steady_clock::now();
    // The header and index first: a scene reopened in the same session has every building in memory already.
    std::ifstream stream(file, std::ios::binary);
    const uint64_t fileSize = std::filesystem::file_size(file, ec);
    std::string bytes(24, '\0');
    stream.read(bytes.data(), 24);
    if (!stream || ec || std::memcmp(bytes.data(), kCacheMagic, sizeof kCacheMagic) != 0)
    {
        lila::log::info("Buildings: {} is not a building cache; the buildings regenerate", file.generic_string());
        return 0;
    }
    if (get(bytes, 8) != generatorId())
    {
        lila::log::info("Buildings: the building cache {} was made by another build of Buildings; the buildings regenerate", file.generic_string());
        return 0;
    }
    const uint64_t n = get(bytes, 16);
    if (n > (fileSize - 24) / 24)
    {
        lila::log::info("Buildings: the building cache {} is damaged; the buildings regenerate", file.generic_string());
        return 0;
    }
    bytes.resize(size_t(24 + n * 24));
    stream.read(bytes.data() + 24, std::streamsize(n * 24));
    std::vector<uint64_t> keys(n);
    std::vector<std::shared_ptr<const Result>> results(n);
    std::vector<char> have(n, 0);
    size_t missing = 0;
    {
        std::lock_guard lock(gGenMutex);
        for (size_t i = 0; i < n; ++i)
        {
            keys[i] = get(bytes, 24 + i * 24);
            have[i] = gGen.count(keys[i]) ? 1 : 0;
            missing += !have[i];
        }
    }
    if (missing > 0)
    {
        bytes.resize(size_t(fileSize));
        stream.read(bytes.data() + 24 + n * 24, std::streamsize(fileSize - 24 - n * 24));
    }
    if (!stream)
    {
        lila::log::info("Buildings: the building cache {} could not be read; the buildings regenerate", file.generic_string());
        return 0;
    }
    const std::string_view in(bytes);
    std::atomic<size_t> damaged{0};
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            if (have[i])
                continue;
            const uint64_t at = get(in, 24 + i * 24 + 8), size = get(in, 24 + i * 24 + 16);
            if (at > in.size() || size > in.size() - at || !(results[i] = decodeBlob(in.substr(size_t(at), size_t(size)))))
                ++damaged;
        }
    });
    size_t read = 0;
    {
        std::lock_guard lock(gGenMutex);
        ++gTick;
        for (size_t i = 0; i < n; ++i)
            if (results[i])
            {
                gGen[keys[i]] = {std::move(results[i]), gTick};
                ++read;
            }
        gCacheFloor = std::max(gCacheFloor, size_t(n) + size_t(n) / 2);
    }
    if (damaged > 0)
        lila::log::info("Buildings: {} damaged entries in the building cache {}; those buildings regenerate", damaged.load(), file.generic_string());
    std::sort(keys.begin(), keys.end());
    {
        std::lock_guard lock(gDiskMutex);
        gDiskHash[file] = damaged > 0 ? 0 : keysHash(keys);
    }
    lila::release::later(std::move(bytes));
    lila::log::info("Buildings: {} buildings read from the building cache, {} already in memory ({:.1f} s)", read, n - missing,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return read;
}

namespace
{
/** The cache write that follows a save. Its code is this library's, so it is joined before Buildings unloads. */
std::mutex gWriterMutex;
/** Joins on destruction too: a host that never unloads Buildings tears it down with the statics. */
struct Writer : std::thread
{
    using std::thread::operator=;
    ~Writer()
    {
        if (joinable())
            join();
    }
} gWriter;

void joinWriter()
{
    std::lock_guard lock(gWriterMutex);
    if (gWriter.joinable())
        gWriter.join();
}
} // namespace

void registerCache(lila::ext::Context& ctx)
{
    using T = lila::ArgType;
    ctx.track("building cache writer", [] { joinWriter(); });
    ctx.registerCommand({
        .name = "buildingCache",
        .doc = "The building cache beside the saved scene (<scene>_cache/buildings.bin): every generated building, written after a "
               "save that changed buildings and read when the scene opens, so reopening skips generating buildings whose spec, "
               "style and edits are unchanged; the rest regenerate. With no flags, reports the file. write= writes it now for the "
               "current scene (waits for a write in progress). clearMemory= forgets the generated buildings held in memory, so "
               "the next evaluation reads them from the cache or generates them again.",
        .flags = {{"write", "w", T::Bool, false, "Write the cache for the current scene now."},
                  {"clearMemory", "cm", T::Bool, false, "Forget the generated buildings held in memory."}},
        .undoable = false,
        .run = [](const lila::Args& a) -> json {
            const std::filesystem::path scene = lila::io::currentPath();
            json out = json::object();
            if (a.get<bool>("clearMemory", false))
            {
                std::vector<std::shared_ptr<const Result>> dropped;
                {
                    std::lock_guard lock(gGenMutex);
                    out["cleared"] = gGen.size();
                    for (auto& [k, v] : gGen)
                        dropped.push_back(std::move(v.result));
                    gGen.clear();
                    gCacheFloor = 0;
                }
                lila::release::laterEach(std::move(dropped));
            }
            if (scene.empty())
            {
                if (a.get<bool>("write", false))
                    throw lila::CommandError("buildingCache: the scene is not saved yet, so it has no cache");
                return out;
            }
            if (a.get<bool>("write", false))
                out["written"] = writeCache(scene);
            const std::filesystem::path file = cacheFile(scene);
            std::error_code ec;
            out["file"] = file.generic_string();
            out["exists"] = std::filesystem::exists(file, ec);
            out["bytes"] = out["exists"].get<bool>() ? uint64_t(std::filesystem::file_size(file, ec)) : 0;
            return out;
        },
    });

    lila::scene::data::onLoaded([] {
        if (const std::filesystem::path& p = lila::io::loadingPath(); !p.empty())
            readCache(p);
    });
    // A replaced scene's cells are gone: what they published goes now, not when something next lists the buildings.
    for (const lila::events::Event e : {lila::events::Event::SceneNew, lila::events::Event::SceneOpened})
        lila::events::subscribe(e, [](const lila::events::EventData&) { allPublished(); }, "Buildings");
    lila::events::subscribe(
        lila::events::Event::SceneSaved,
        [](const lila::events::EventData& e) {
            const std::filesystem::path file = cacheFile(e.path);
            Buildings all = sceneBuildings();
            const uint64_t hash = keysHash(all);
            if (all.empty() || unchanged(file, hash))
                return;
            // The results are immutable and shared, so encoding them can run while the user carries on.
            std::lock_guard lock(gWriterMutex);
            // Two writes of one file never overlap.
            if (gWriter.joinable())
                gWriter.join();
            gWriter = std::thread([file, all = std::move(all), hash] { writeBuildings(file, all, hash); });
        },
        "Buildings");
}

void registerNode(lila::ext::Context& ctx)
{
    using Socket = lila::gn::SocketType;
    lila::gn::NodeDecl d;
    d.type = kNodeType;
    d.label = "Buildings";
    d.category = "Buildings";
    d.doc = "Procedural buildings from specs (Settlement envelopes, footprints): a merged shell and every module (windows, doors, "
            "shopfronts, signs, balconies, tanks...) as an instance, each published as a slot with a stable id. Edits are kept by "
            "building id. lod near draws shell and modules, hlod a simplified proxy, far flat-roof massing.";
    d.sockets.inputs = {
        {.id = "spec", .type = Socket::String, .defaultValue = "[]", .doc = "Buildings as a JSON list (buildingCreate writes it).", .editor = "multiline"},
        {.id = "edits", .type = Socket::String, .defaultValue = "{}", .doc = "Edits by building id (buildingEdit writes them).", .editor = "multiline"},
        {.id = "lod", .type = Socket::String, .defaultValue = "near", .doc = "near, hlod or far."},
        {.id = "cell", .type = Socket::String, .defaultValue = "", .doc = "The cell these buildings are in (x_z), or a building id when standalone."},
        {.id = "modules", .type = Socket::Bool, .defaultValue = true, .doc = "Draw the modules (near only)."},
        {.id = "realize",
         .type = Socket::Bool,
         .defaultValue = false,
         .doc = "Flatten the modules into the cell mesh, for exporters that take one flat mesh. Off, the mesh holds the shell and "
                "the modules stay instances: drawn instanced and sent to Unreal as instances, at a fraction of the memory."},
    };
    d.sockets.outputs = {{.id = "geometry", .type = Socket::Geometry, .doc = "Shell mesh and module instances."}};
    d.exec = exec;
    if (!lila::gn::registerDecl(ctx, std::move(d)))
        lila::log::warn("Buildings: the Buildings geometry node could not be registered");
}
} // namespace buildings
