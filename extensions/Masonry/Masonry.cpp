// Masonry nodes: Masonry Wall (stones in courses along footprint lines) and Masonry Building (a whole building from a
// spec), both ruined and aged by their inputs, plus Masonry Ruin and Masonry Age for any instances. Every tool and
// command builds a group around these nodes and reads results through the same cached build.

#include "Masonry.h"
#include "MasonryCore.h"
#include "MasonryTools.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Profile.h>

#include <algorithm>
#include <list>
#include <mutex>
#include <stdexcept>

using namespace lila;

namespace masonry
{
using json = nlohmann::json;

namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;
using Socket = gn::SocketType;

// clang-format off
const Input kWall[] = {
    {"bond", Input::String, 0, "running", 0, 0, 0, "Wall", "running, stack, english, flemish, header, gardenWall, herringbone, ashlar, rubble or dryStone."},
    {"height", Input::Float, 200.0, nullptr, 0, 1e7, 1000, "Wall", "Wall height above the ground, cm, where the footprint has no per-point wallHeight."},
    {"thickness", Input::Float, 21.5, nullptr, 1, 1e6, 200, "Wall", "Wall thickness at the base, cm. Under 1.5 stone depths the wall is one stone thick."},
    {"cornerAngle", Input::Float, 30.0, nullptr, 1, 179, 90, "Wall", "Turns sharper than this, degrees, are corners; gentler ones curve the wall."},
    {"batter", Input::Float, 0.0, nullptr, 0, 45, 10, "Wall", "Degrees each face leans in, so the wall thins upward."},
    {"bands", Input::String, 0, "", 0, 0, 0, "Wall", "String course heights above the lowest ground, cm: a JSON list such as [300, 600]."},
    {"cornice", Input::Int, 0, nullptr, 0, 16, 4, "Wall", "Top courses corbelled out, each further than the one below."},
    {"openings", Input::String, 0, "", 0, 0, 0, "Wall",
     "Doors, windows and niches as a JSON list: [{\"at\": cm along the footprint, \"width\", \"height\", \"sill\", \"arch\": "
     "lintel|round|pointed|segmental|flat, \"kind\": door|window|niche, \"name\"}]."},
    {"stoneLength", Input::Float, 21.5, nullptr, 1, 1e5, 100, "Stones", "Stone length along the face, cm."},
    {"stoneHeight", Input::Float, 6.5, nullptr, 0.5, 1e5, 50, "Stones", "Stone height, cm. Courses are fitted to the wall height."},
    {"stoneDepth", Input::Float, 10.25, nullptr, 0.5, 1e5, 50, "Stones", "Stone depth into the wall, cm; also a header's width on the face."},
    {"joint", Input::Float, 1.0, nullptr, 0, 1e4, 5, "Stones", "Mortar joint, cm."},
    {"recess", Input::Float, 0.6, nullptr, 0, 1e4, 3, "Stones", "How far the mortar sits behind the stone faces, cm."},
    {"region", Input::String, 0, "", 0, 0, 0, "Wall", "Region name of the wall (ruin, masonryInfo and masonryCheck use it); empty is wall_0. More footprints are region_1, region_2, ..."},
};
const Input kBuilding[] = {
    {"spec", Input::String, 0, "{}", 0, 0, 0, "Building", "The building as a JSON spec (masonrySpec reads it back; lila://masonry/spec-schema describes it)."},
};
const Input kLook[] = {
    {"bevel", Input::Float, 0.5, nullptr, 0, 1e4, 3, "Look", "Stone edge chamfer, cm."},
    {"wear", Input::Float, 0.5, nullptr, 0, 1, 1, "Look", "How knocked about stone corners are, 0 to 1."},
    {"jitter", Input::Float, 1.0, nullptr, 0, 1, 1, "Look", "How far stones wander and tilt inside their joints, 0 to 1."},
    {"protrude", Input::Float, 0.3, nullptr, 0, 1e4, 2, "Look", "Stones pushed out of the face by up to this, cm."},
    {"variants", Input::Int, 6, nullptr, 1, 64, 16, "Look", "How many differently worn versions of each stone size."},
    {"detail", Input::Int, 0, nullptr, 0, 2, 2, "Look", "0 worn, 1 clean chamfer, 2 plain boxes (fast preview)."},
    {"tile", Input::Float, 100.0, nullptr, 1, 1e7, 1000, "Look", "Cm of stone per UV unit of the tiling texture."},
    {"chips", Input::Float, 0.3, nullptr, 0, 1, 1, "Look", "Chips knocked out of stone edges and corners, 0 to 1; more on old, wet and damaged stones."},
    {"seed", Input::Int, 1, nullptr, 0, 2147483647, 0, "Look", "Another seed gives other wander, wear and cut positions."},
};
const Input kRuin[] = {
    {"left", Input::Float, 1.0, nullptr, 0, 1, 1, "Ruin", "Share of the stone left standing, 0 to 1: 1 intact, 0.75, 0.5, 0.25 a stump."},
    {"collapse", Input::Float, 0.0, nullptr, -360, 360, 360, "Ruin", "Direction it fell toward, degrees about Y from +X; it stands lowest there and rubble lies that way."},
    {"slope", Input::Float, 0.5, nullptr, 0, 1, 1, "Ruin", "How much lower it stands toward the collapse side, 0 to 1."},
    {"roughness", Input::Float, 0.35, nullptr, 0, 1, 1, "Ruin", "How ragged the broken top is, 0 to 1."},
    {"gaps", Input::Int, 0, nullptr, 0, 64, 8, "Ruin", "Breaches cut down toward the ground."},
    {"survivors", Input::Float, 0.1, nullptr, 0, 1, 1, "Ruin", "Share of the stones just above the break that hang on."},
    {"loose", Input::Float, 0.4, nullptr, 0, 1, 1, "Ruin", "Share of the top stones tipped or slid."},
    {"tip", Input::Float, 40.0, nullptr, 0, 89, 89, "Ruin", "Most a loose stone tips, degrees."},
    {"rubble", Input::Float, 0.6, nullptr, 0, 1, 1, "Ruin", "Share of the fallen stones lying at the foot; the rest are gone."},
    {"settle", Input::Bool, 0, nullptr, 0, 0, 0, "Ruin", "Drop the rubble with the rigid-body solver (slower) instead of only heaping it."},
    {"ruinRegions", Input::String, 0, "", 0, 0, 0, "Ruin", "Only these regions ruin, comma separated (north_wall, tower_1/door_1); empty is all."},
    {"ruinSeed", Input::Int, 1, nullptr, 0, 2147483647, 0, "Ruin", "Another seed breaks it differently."},
    {"structural", Input::Bool, 0, nullptr, 0, 0, 0, "Ruin", "Also drop what can't stand under the rigid-body solver: arches without keystones, overhangs (slower)."},
    {"ruinStrokes", Input::String, 0, "", 0, 0, 0, "Ruin", "Painted damage as JSON [{points, radius, strength}]; ruinPaint and the Ruin Brush add to it."},
};
const Input kAge[] = {
    {"age", Input::Float, 0.0, nullptr, 0, 1, 1, "Age", "How old it looks, 0 to 1: moss, wear and staining in the material."},
    {"moss", Input::Float, 0.5, nullptr, 0, 1, 1, "Age", "How much moss grows where it is wet and exposed, 0 to 1."},
    {"wetHeight", Input::Float, 0.0, nullptr, 0, 1e6, 500, "Age", "Damp band above the ground, cm; 0 is a fifth of the height."},
    {"plaster", Input::Float, 0.0, nullptr, 0, 1, 1, "Age", "How much render coat is left on the outer faces, 0 to 1; it peels high up and where damaged."},
    {"plasterThickness", Input::Float, 0.0, nullptr, 0, 1e4, 10, "Age", "Render coat thickness, cm; 0 is a third of a stone height."},
    {"ivy", Input::Float, 0.0, nullptr, 0, 1, 1, "Age", "How much ivy climbs from the ground, 0 to 1."},
    {"ivyHeight", Input::Float, 0.7, nullptr, 0, 1, 1, "Age", "How far up the ivy reaches, 0 to 1."},
    {"leaf", Input::Float, 0.0, nullptr, 0, 1e4, 50, "Age", "Ivy leaf size, cm; 0 is two thirds of a typical stone."},
};
// clang-format on

template <size_t N> void append(std::vector<const Input*>& out, const Input (&list)[N])
{
    for (const Input& in : list)
        out.push_back(&in);
}

gn::SocketDecl socketOf(const Input& in)
{
    gn::SocketDecl s;
    s.id = in.name;
    s.doc = in.doc;
    switch (in.type)
    {
    case Input::Float:
        s.type = Socket::Float;
        s.defaultValue = in.def;
        break;
    case Input::Int:
        s.type = Socket::Int;
        s.defaultValue = int64_t(in.def);
        break;
    case Input::Bool:
        s.type = Socket::Bool;
        s.defaultValue = in.def != 0.0;
        break;
    case Input::String:
        s.type = Socket::String;
        s.defaultValue = std::string(in.text ? in.text : "");
        break;
    }
    if (in.max > in.min && in.type != Input::String && in.type != Input::Bool)
    {
        s.min = in.min;
        s.max = in.max;
    }
    return s;
}

gn::SocketDecl geoSocket(std::string id, std::string doc) { return {.id = std::move(id), .type = Socket::Geometry, .doc = std::move(doc)}; }

json valuesOf(ExecContext& c, InputSet set)
{
    json v = json::object();
    for (const Input* in : inputsOf(set))
        switch (in->type)
        {
        case Input::Float: v[in->name] = typedValue(*in, c.f(in->name)); break;
        case Input::Int: v[in->name] = typedValue(*in, c.i(in->name)); break;
        case Input::Bool: v[in->name] = c.b(in->name); break;
        case Input::String: v[in->name] = c.s(in->name); break;
        }
    return v;
}

GeometryPtr stonesGeometry(const Built& b)
{
    auto inst = std::make_shared<gn::Instances>();
    for (const geo::Mesh& m : b.meshes)
        inst->references.push_back(gn::meshGeometry(m));
    for (size_t k = 0; k < b.stones.size(); ++k)
        inst->add(stoneMatrix(b.stones[k]), b.reference[k]);
    writeStoneAttrs(inst->attrs, b);
    Geometry stones;
    stones.instances = std::move(inst);
    return gn::makeGeometry(std::move(stones));
}

void outputBuilt(ExecContext& c, const Built& b)
{
    GeometryPtr stonesPtr = stonesGeometry(b);
    GeometryPtr corePtr = gn::meshGeometry(b.core);
    GeometryPtr dressPtr = gn::meshGeometry(b.dressing);
    c.out("stones", stonesPtr);
    c.out("core", corePtr);
    c.out("dressing", dressPtr);
    const GeometryPtr parts[] = {stonesPtr, corePtr, dressPtr};
    c.out("geometry", gn::makeGeometry(gn::join(parts)));
}

std::vector<gn::SocketDecl> socketsOf(InputSet set)
{
    std::vector<gn::SocketDecl> out;
    for (const Input* in : inputsOf(set))
        out.push_back(socketOf(*in));
    return out;
}

const std::vector<gn::SocketDecl> kBuiltOutputs = {geoSocket("geometry", "Stones, core, plaster and ivy together."), geoSocket("stones", "The stones, as instances."),
                                                   geoSocket("core", "The mortar core, a mesh."), geoSocket("dressing", "Plaster and ivy, a mesh.")};

const char* kAttrDoc = "Stones are instances sharing a few meshes, with attributes stone, wall, course, role (0 stretcher, 1 header, 2 bat, "
                       "3 closer, 4 filler, 5 voussoir, 6 keystone, 7 lintel, 8 sill, 9 jamb, 10 column, 11 step, 12 band, 13 slab, 14 rubble), "
                       "face (0 outer, 1 inner, 2 through), region, random, variant, size, height, age, damage, wet, moss and loose. Lengths are cm.";

gn::NodeDecl wallDecl()
{
    std::vector<gn::SocketDecl> ins = {geoSocket("footprint", "Lines the wall stands on: curves, or a mesh's loose edges (point float wallHeight sets the height per point). Y is up; each point's Y is the ground.")};
    for (gn::SocketDecl& s : socketsOf(InputSet::Wall))
        ins.push_back(std::move(s));
    return {
        .type = "MasonryWall",
        .label = "Masonry Wall",
        .category = "Masonry",
        .doc = std::string("Lays stones in courses along footprint lines and builds the mortar core they sit in. Corners turn by "
                           "alternating which wall runs through; courses step over sloping ground; openings get arches or lintels, "
                           "sills and jambs. The Ruin and Age inputs break and weather it. ") + kAttrDoc,
        .sockets = {.inputs = ins, .outputs = kBuiltOutputs},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE_N("Masonry Wall");
                const std::vector<Footprint> fps = footprintsOf(*c.geometry("footprint"));
                try
                {
                    outputBuilt(c, buildMasonry(fps, valuesOf(c, InputSet::Wall), false)->built);
                }
                catch (const std::runtime_error& e)
                {
                    throw gn::ExecError(std::string("Masonry Wall: ") + e.what());
                }
            },
    };
}

gn::NodeDecl buildingDecl()
{
    return {
        .type = "MasonryBuilding",
        .label = "Masonry Building",
        .category = "Masonry",
        .doc = std::string("A whole masonry building from a JSON spec: walls with openings, buttresses, bands and cornices, columns, "
                           "stairs, vaults, domes, arches and floors, each a named region. The Ruin and Age inputs break and weather it; "
                           "floors break into pieces that fall where nothing holds them. ") + kAttrDoc,
        .sockets = {.inputs = socketsOf(InputSet::Building), .outputs = kBuiltOutputs},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE_N("Masonry Building");
                try
                {
                    outputBuilt(c, buildMasonry({}, valuesOf(c, InputSet::Building), false)->built);
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(std::string("Masonry Building: ") + e.what());
                }
            },
    };
}

/** Instances again after a ruin or age pass: kept ones in order, their attributes copied, the new attributes set. */
std::shared_ptr<gn::Instances> rebuiltInstances(const gn::Instances& src, const std::vector<Stone>& stones, const std::vector<int>& from,
                                                const std::vector<glm::dvec3>& offset)
{
    auto out = std::make_shared<gn::Instances>();
    out->references = src.references;
    out->attrs = src.attrs;
    out->attrs.resize(0);
    out->attrs.resize(stones.size());
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const size_t s = size_t(from[k]);
        // Back from the stone's box to the instance's own origin and scale.
        const glm::dvec3 scale(glm::length(glm::dvec3(src.transforms[s][0])), glm::length(glm::dvec3(src.transforms[s][1])),
                               glm::length(glm::dvec3(src.transforms[s][2])));
        glm::dmat4 m = stoneMatrix(stones[k]);
        m = m * glm::dmat4(glm::dvec4(scale.x, 0, 0, 0), glm::dvec4(0, scale.y, 0, 0), glm::dvec4(0, 0, scale.z, 0), glm::dvec4(-offset[s] * scale, 1.0));
        out->add(m, src.reference[s]);
        out->attrs.copyFrom(src.attrs, k, s);
    }
    for (const char* name : {"age", "damage", "wet", "moss", "loose"})
        out->attrs.add(name, geo::AttrType::Float);
    std::span<float> age = out->attrs.get<float>("age"), damage = out->attrs.get<float>("damage"), wet = out->attrs.get<float>("wet"),
                     moss = out->attrs.get<float>("moss"), loose = out->attrs.get<float>("loose");
    for (size_t k = 0; k < stones.size(); ++k)
    {
        age[k] = stones[k].age;
        damage[k] = stones[k].damage;
        wet[k] = stones[k].wet;
        moss[k] = stones[k].moss;
        loose[k] = stones[k].loose ? 1.0f : 0.0f;
    }
    return out;
}

gn::NodeDecl ruinDecl()
{
    std::vector<gn::SocketDecl> ins = {geoSocket("geometry", "Instances to ruin: stones from a masonry node, or any instances.")};
    for (gn::SocketDecl& s : socketsOf(InputSet::Ruin))
        ins.push_back(std::move(s));
    return {
        .type = "MasonryRuin",
        .label = "Masonry Ruin",
        .category = "Masonry",
        .doc = "Ruins any instances as masonry: those above a ragged break fall (heaped at the foot as rubble, or gone), a few "
               "survive, nothing is left floating, and top ones tip. Writes damage and loose. Other components pass through.",
        .sockets = {.inputs = ins, .outputs = {geoSocket("geometry", "What is left, rubble included.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE_N("Masonry Ruin");
                GeometryPtr in = c.geometry("geometry");
                if (!in->instances || in->instances->size() == 0)
                {
                    c.out("geometry", in);
                    return;
                }
                std::vector<glm::dvec3> offset;
                std::vector<Stone> stones = stonesFromInstances(*in->instances, &offset);
                std::vector<int> from;
                ruinStones(stones, ruinFromValues(valuesOf(c, InputSet::Ruin)), 0.0, {}, &from);
                Geometry g = *in;
                g.instances = rebuiltInstances(*in->instances, stones, from, offset);
                c.out("geometry", gn::makeGeometry(std::move(g)));
            },
    };
}

gn::NodeDecl ageDecl()
{
    std::vector<gn::SocketDecl> ins = {geoSocket("geometry", "Instances to age: stones from a masonry node, or any instances.")};
    for (gn::SocketDecl& s : socketsOf(InputSet::Age))
        ins.push_back(std::move(s));
    return {
        .type = "MasonryAge",
        .label = "Masonry Age",
        .category = "Masonry",
        .doc = "Weathers any instances as masonry: writes age, wet (near the ground and on exposed tops) and moss (where wet and "
               "exposed) per instance, the values Unreal's masonry material reads. Damage already on the instances is kept.",
        .sockets = {.inputs = ins, .outputs = {geoSocket("geometry", "The same instances with age, wet and moss.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE_N("Masonry Age");
                GeometryPtr in = c.geometry("geometry");
                if (!in->instances || in->instances->size() == 0)
                {
                    c.out("geometry", in);
                    return;
                }
                std::vector<glm::dvec3> offset;
                std::vector<Stone> stones = stonesFromInstances(*in->instances, &offset);
                if (std::span<const float> d = in->instances->attrs.get<float>("damage"); d.size() == stones.size())
                    for (size_t k = 0; k < stones.size(); ++k)
                        stones[k].damage = d[k];
                ageStones(stones, ageFromValues(valuesOf(c, InputSet::Age)));
                std::vector<int> from(stones.size());
                for (size_t k = 0; k < from.size(); ++k)
                    from[k] = int(k);
                Geometry g = *in;
                g.instances = rebuiltInstances(*in->instances, stones, from, offset);
                c.out("geometry", gn::makeGeometry(std::move(g)));
            },
    };
}

gn::NodeDecl linksDecl()
{
    return {
        .type = "MasonryLinks",
        .label = "Masonry Links",
        .category = "Masonry",
        .doc = "Links for the Rigid Body Solver between masonry stones that touch, as two-point curves (pieceA, pieceB, type, strength, "
               "area): mortar strength times contact area, weakened by each stone's age. Grounded stones are tied to the world.",
        .sockets = {.inputs = {geoSocket("stones", "Stones as instances (a masonry node's stones output, or any)."),
                               socketOf({"strength", Input::Float, 30.0, nullptr, 0, 1e6, 200, "", "Mortar strength, N per cm2 of contact."}),
                               socketOf({"ageWeakens", Input::Float, 0.6, nullptr, 0, 1, 1, "", "How much of its strength the oldest mortar has lost, 0 to 1."}),
                               socketOf({"anchor", Input::Bool, 1, nullptr, 0, 0, 0, "", "Tie stones on the ground to the world."})},
                    .outputs = {geoSocket("links", "Links as two-point curves.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE_N("Masonry Links");
                GeometryPtr in = c.geometry("stones");
                gn::CurveSet curves;
                if (in->instances && in->instances->size() > 0)
                {
                    std::vector<glm::dvec3> offset;
                    std::vector<Stone> stones = stonesFromInstances(*in->instances, &offset);
                    double lowest = 1e300, typical = 0.0;
                    for (const Stone& s : stones)
                    {
                        lowest = std::min(lowest, s.center.y - halfExtentAabb(s).y);
                        typical += std::min({s.size.x, s.size.y, s.size.z});
                    }
                    typical /= double(stones.size());
                    const double pad = std::max(0.3, typical * 0.08);
                    for (Stone& s : stones)
                        s.grounded = c.b("anchor") && s.center.y - halfExtentAabb(s).y <= lowest + pad * 2.0;
                    std::span<const float> age = in->instances->attrs.get<float>("age");
                    const double strength = std::max(0.0, c.f("strength")), weaken = std::clamp(c.f("ageWeakens"), 0.0, 1.0);
                    std::vector<StoneLink> links = stoneLinks(stones, pad);
                    for (const StoneLink& l : links)
                    {
                        const glm::dvec3 pts[2] = {l.at, l.at};
                        curves.addCurve(pts, false);
                    }
                    curves.syncSizes();
                    std::span<int32_t> a = curves.curveAttrs.getOrAdd<int32_t>("pieceA");
                    std::span<int32_t> b = curves.curveAttrs.getOrAdd<int32_t>("pieceB");
                    std::span<int32_t> type = curves.curveAttrs.getOrAdd<int32_t>("type");
                    std::span<float> force = curves.curveAttrs.getOrAdd<float>("strength");
                    std::span<float> area = curves.curveAttrs.getOrAdd<float>("area");
                    for (size_t k = 0; k < links.size(); ++k)
                    {
                        const StoneLink& l = links[k];
                        auto ageOf = [&](int i) { return i >= 0 && size_t(i) < age.size() ? double(age[size_t(i)]) : 0.0; };
                        const double old = std::max(ageOf(l.a), ageOf(l.b));
                        a[k] = l.a;
                        b[k] = l.b;
                        type[k] = 0;
                        force[k] = float(strength * l.area * (1.0 - weaken * old));
                        area[k] = float(l.area);
                    }
                }
                c.out("links", gn::curveGeometry(std::move(curves)));
            },
    };
}

std::string footprintKey(std::span<const Footprint> fps)
{
    std::string key;
    for (const Footprint& f : fps)
    {
        key.append(reinterpret_cast<const char*>(f.points.data()), f.points.size() * sizeof(glm::dvec3));
        key.append(reinterpret_cast<const char*>(f.heights.data()), f.heights.size() * sizeof(double));
        key += f.closed ? "c|" : "o|";
    }
    return key;
}

struct CacheEntry
{
    std::string key;
    std::shared_ptr<const Result> result;
};
std::mutex gCacheLock;
std::list<CacheEntry> gCache;
constexpr size_t kCacheSize = 6;
} // namespace

std::vector<const Input*> inputsOf(InputSet set)
{
    std::vector<const Input*> out;
    switch (set)
    {
    case InputSet::Wall:
        append(out, kWall);
        append(out, kLook);
        append(out, kRuin);
        append(out, kAge);
        break;
    case InputSet::Building:
        append(out, kBuilding);
        append(out, kLook);
        append(out, kRuin);
        append(out, kAge);
        break;
    case InputSet::Ruin: append(out, kRuin); break;
    case InputSet::Age: append(out, kAge); break;
    }
    return out;
}

const Input* findInput(std::string_view name)
{
    for (InputSet set : {InputSet::Wall, InputSet::Building})
        for (const Input* in : inputsOf(set))
            if (name == in->name)
                return in;
    return nullptr;
}

json typedValue(const Input& in, const json& v)
{
    switch (in.type)
    {
    case Input::Float:
    {
        double d = v.is_number() ? v.get<double>() : (v.is_boolean() ? double(v.get<bool>()) : in.def);
        if (in.max > in.min)
            d = std::clamp(d, in.min, in.max);
        return d;
    }
    case Input::Int:
    {
        int64_t i = v.is_number() ? int64_t(std::llround(v.get<double>())) : int64_t(in.def);
        if (in.max > in.min)
            i = std::clamp<int64_t>(i, int64_t(in.min), int64_t(in.max));
        return i;
    }
    case Input::Bool: return v.is_boolean() ? v.get<bool>() : (v.is_number() ? v.get<double>() != 0.0 : in.def != 0.0);
    case Input::String:
        if (v.is_string())
            return v;
        if (v.is_null())
            return std::string(in.text ? in.text : "");
        return v.dump();
    }
    return v;
}

std::vector<Footprint> footprintsOf(const gn::Geometry& g)
{
    std::vector<Footprint> out;
    if (g.curves)
    {
        const gn::CurveSet ev = g.curves->evaluated();
        for (size_t k = 0; k < ev.numCurves(); ++k)
        {
            Footprint f;
            const auto pts = ev.curvePoints(k);
            f.points.assign(pts.begin(), pts.end());
            f.closed = ev.cyclic[k] != 0;
            if (f.points.size() >= 2)
                out.push_back(std::move(f));
        }
    }
    if (g.mesh)
        for (Footprint& f : footprintsFromEdges(*g.mesh))
            out.push_back(std::move(f));
    return out;
}

std::vector<Opening> openingsFromJson(const json& list)
{
    std::vector<Opening> out;
    if (list.is_null() || (list.is_string() && list.get<std::string>().empty()))
        return out;
    const json parsed = list.is_string() ? json::parse(list.get<std::string>(), nullptr, false) : list;
    if (!parsed.is_array())
        throw std::runtime_error("openings is a JSON list of {at, width, height, sill, arch, kind, name}");
    for (size_t k = 0; k < parsed.size(); ++k)
    {
        const json& o = parsed[k];
        if (!o.is_object() || !o.contains("at"))
            throw std::runtime_error("each opening is an object with at (cm along the footprint)");
        Opening op;
        const std::string kind = o.value("kind", std::string(o.value("sill", 0.0) > 0.0 ? "window" : "door"));
        if (kind != "door" && kind != "window" && kind != "niche")
            throw std::runtime_error("an opening's kind is door, window or niche");
        op.name = o.value("name", kind + "_" + std::to_string(k + 1));
        op.at = o.value("at", 0.0);
        op.width = o.value("width", 100.0);
        op.height = o.value("height", 200.0);
        op.sill = o.value("sill", kind == "window" ? 100.0 : 0.0);
        op.rise = o.value("rise", 0.0);
        op.ring = o.value("ring", 0.0);
        op.through = kind != "niche";
        op.sillStone = o.value("sillStone", true);
        if (o.contains("arch") && (!o["arch"].is_string() || !parseArch(o["arch"].get<std::string>(), &op.arch)))
            throw std::runtime_error("an opening's arch is lintel, round, pointed, segmental or flat");
        out.push_back(op);
    }
    return out;
}

BuildOptions optionsFromValues(const json& v)
{
    auto f = [&](const char* name) { return typedValue(*findInput(name), v.contains(name) ? v[name] : json()).get<double>(); };
    auto i = [&](const char* name) { return typedValue(*findInput(name), v.contains(name) ? v[name] : json()).get<int64_t>(); };
    BuildOptions o;
    const std::string bond = v.value("bond", std::string("running"));
    if (!parseBond(bond, &o.wall.bond))
    {
        std::string names;
        for (const std::string& n : bondNames())
            names += (names.empty() ? "" : ", ") + n;
        throw std::runtime_error("unknown bond '" + bond + "' (" + names + ")");
    }
    if (v.contains("height"))
    {
        o.wall.height = f("height");
        o.wall.thickness = f("thickness");
        o.wall.stone.length = f("stoneLength");
        o.wall.stone.height = f("stoneHeight");
        o.wall.stone.depth = f("stoneDepth");
        o.wall.stone.joint = f("joint");
        o.wall.recess = f("recess");
        o.wall.cornerAngle = f("cornerAngle");
        o.wall.batter = f("batter");
        o.wall.cornice = int(i("cornice"));
        const std::string bands = v.value("bands", std::string());
        if (!bands.empty())
        {
            const json list = json::parse(bands.front() == '[' ? bands : "[" + bands + "]", nullptr, false);
            if (!list.is_array())
                throw std::runtime_error("bands is a list of heights, cm");
            for (const json& b : list)
                if (b.is_number())
                    o.wall.bands.push_back(b.get<double>());
        }
        o.wall.openings = openingsFromJson(v.value("openings", json()));
    }
    o.wall.jitter = f("jitter");
    o.wall.protrude = f("protrude");
    o.wall.seed = uint32_t(i("seed"));
    o.bevel = f("bevel");
    o.wear = f("wear");
    o.variants = int(i("variants"));
    o.detail = Detail(i("detail"));
    o.tile = f("tile");
    o.chips = f("chips");
    return o;
}

RuinParams ruinFromValues(const json& v)
{
    auto f = [&](const char* name) { return typedValue(*findInput(name), v.contains(name) ? v[name] : json()).get<double>(); };
    auto i = [&](const char* name) { return typedValue(*findInput(name), v.contains(name) ? v[name] : json()).get<int64_t>(); };
    RuinParams r;
    r.left = f("left");
    r.collapse = f("collapse");
    r.slope = f("slope");
    r.roughness = f("roughness");
    r.gaps = int(i("gaps"));
    r.survivors = f("survivors");
    r.loose = f("loose");
    r.tip = f("tip");
    r.rubble = f("rubble");
    r.settle = typedValue(*findInput("settle"), v.contains("settle") ? v["settle"] : json()).get<bool>();
    r.seed = uint32_t(i("ruinSeed"));
    r.structural = typedValue(*findInput("structural"), v.contains("structural") ? v["structural"] : json()).get<bool>();
    r.strokes = strokesFromJson(v.value("ruinStrokes", json()));
    std::string regions = v.value("ruinRegions", std::string());
    size_t start = 0;
    while (start <= regions.size())
    {
        size_t end = regions.find(',', start);
        if (end == std::string::npos)
            end = regions.size();
        std::string part = regions.substr(start, end - start);
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        if (!part.empty())
            r.regions.push_back(part);
        start = end + 1;
    }
    return r;
}

AgeParams ageFromValues(const json& v)
{
    auto f = [&](const char* name) { return typedValue(*findInput(name), v.contains(name) ? v[name] : json()).get<double>(); };
    AgeParams a;
    a.age = f("age");
    a.moss = f("moss");
    a.wetHeight = f("wetHeight");
    a.plaster = f("plaster");
    a.plasterThickness = f("plasterThickness");
    a.ivy = f("ivy");
    a.ivyHeight = f("ivyHeight");
    a.leaf = f("leaf");
    a.seed = uint32_t(typedValue(*findInput("ruinSeed"), v.contains("ruinSeed") ? v["ruinSeed"] : json()).get<int64_t>());
    return a;
}

std::shared_ptr<const Result> buildMasonry(std::span<const Footprint> footprints, const json& values, bool lods)
{
    const std::string spec = values.value("spec", std::string());
    const bool building = values.contains("spec");
    const std::string key = (building ? std::string() : footprintKey(footprints)) + values.dump() + (lods ? "L" : "-");
    {
        std::lock_guard lock(gCacheLock);
        for (auto it = gCache.begin(); it != gCache.end(); ++it)
            if (it->key == key)
            {
                gCache.splice(gCache.begin(), gCache, it);
                return gCache.front().result;
            }
    }
    LILA_ZONE_N("Masonry build");
    const BuildOptions look = optionsFromValues(values);
    Plan plan;
    if (building)
    {
        const json parsed = json::parse(spec.empty() ? std::string("{}") : spec, nullptr, false);
        if (parsed.is_discarded())
            throw std::runtime_error("the spec is not valid JSON");
        plan = planBuilding(parsed, look);
    }
    else
        plan = planWalls(footprints, look, values.value("region", std::string()));
    auto result = std::make_shared<Result>();
    result->built = realizePlan(plan, ruinFromValues(values), ageFromValues(values), lods, &result->ruin);
    std::lock_guard lock(gCacheLock);
    gCache.push_front({key, result});
    if (gCache.size() > kCacheSize)
        gCache.pop_back();
    return result;
}

std::vector<Stone> stonesFromInstances(const gn::Instances& inst, std::vector<glm::dvec3>* offset)
{
    std::vector<geo::Bounds> refBounds(inst.references.size());
    for (size_t r = 0; r < inst.references.size(); ++r)
        if (const GeometryPtr& g = inst.references[r]; g && g->mesh)
            refBounds[r] = g->mesh->bounds();
    std::vector<Stone> out(inst.size());
    offset->assign(inst.size(), glm::dvec3(0.0));
    std::span<const int32_t> region = inst.attrs.get<int32_t>("region");
    std::span<const float> random = inst.attrs.get<float>("random");
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const glm::dmat4& m = inst.transforms[k];
        glm::dvec3 axis[3];
        glm::dvec3 scale;
        for (int a = 0; a < 3; ++a)
        {
            scale[a] = glm::length(glm::dvec3(m[a]));
            axis[a] = scale[a] > 1e-12 ? glm::dvec3(m[a]) / scale[a] : glm::dvec3(a == 0, a == 1, a == 2);
        }
        const int r = inst.reference[k];
        geo::Bounds b = r >= 0 && size_t(r) < refBounds.size() && refBounds[size_t(r)].valid() ? refBounds[size_t(r)] : geo::Bounds();
        if (!b.valid())
        {
            b.extend(glm::dvec3(-0.5));
            b.extend(glm::dvec3(0.5));
        }
        const glm::dvec3 mid = (b.min + b.max) * 0.5;
        Stone& s = out[k];
        s.frame = glm::dmat3(axis[0], axis[1], axis[2]);
        if (glm::determinant(s.frame) < 0.0)
            s.frame[2] = -s.frame[2];
        s.center = glm::dvec3(m * glm::dvec4(mid, 1.0));
        s.size = glm::max((b.max - b.min) * scale, glm::dvec3(1e-3));
        s.face = Face::Through;
        s.region = k < region.size() ? region[k] : 0;
        s.random = k < random.size() ? random[k] : float(unitRandom(k));
        (*offset)[k] = mid;
    }
    return out;
}

void writeStoneAttrs(geo::AttributeSet& attrs, const Built& b)
{
    // Every layer first: adding one may move the others, which would leave earlier spans dangling.
    for (const char* name : {"stone", "wall", "course", "role", "face", "variant", "region"})
        attrs.add(name, geo::AttrType::Int);
    for (const char* name : {"random", "height", "age", "damage", "wet", "moss", "loose"})
        attrs.add(name, geo::AttrType::Float);
    attrs.add("size", geo::AttrType::Float3);
    std::span<int32_t> stone = attrs.get<int32_t>("stone"), wall = attrs.get<int32_t>("wall"), course = attrs.get<int32_t>("course"),
                       role = attrs.get<int32_t>("role"), face = attrs.get<int32_t>("face"), variant = attrs.get<int32_t>("variant"),
                       region = attrs.get<int32_t>("region");
    std::span<float> random = attrs.get<float>("random"), height = attrs.get<float>("height"), age = attrs.get<float>("age"),
                     damage = attrs.get<float>("damage"), wet = attrs.get<float>("wet"), moss = attrs.get<float>("moss"), loose = attrs.get<float>("loose");
    std::span<glm::vec3> size = attrs.get<glm::vec3>("size");
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const Stone& s = b.stones[k];
        stone[k] = int32_t(k);
        wall[k] = s.wall;
        course[k] = s.course;
        role[k] = int32_t(s.role);
        face[k] = int32_t(s.face);
        variant[k] = b.variant[k];
        region[k] = s.region;
        random[k] = s.random;
        height[k] = k < b.heightFraction.size() ? b.heightFraction[k] : 0.0f;
        age[k] = s.age;
        damage[k] = s.damage;
        wet[k] = s.wet;
        moss[k] = s.moss;
        loose[k] = s.loose ? 1.0f : 0.0f;
        size[k] = glm::vec3(s.size);
    }
}

bool registerNodes(ext::Context& ctx)
{
    for (gn::NodeDecl d : {wallDecl(), buildingDecl(), ruinDecl(), ageDecl(), linksDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace masonry

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!masonry::registerNodes(ctx))
        return 1;
    masonry::registerCommands(ctx);
    masonry::registerUi(ctx);
    masonry::registerAgentDocs(ctx);
    return 0;
}
