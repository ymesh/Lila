// The starter library: props for industrial sites, ruins, starfighter facilities and roadsides, built from code into the user's
// config folder the first time the library is used. Nothing binary ships; the meshes are boxes, cylinders and hulls
// written as OBJ beside their asset.json.

#include "Library.h"

#include <RigidScene.h>

#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace stagehand::library
{
namespace
{
/** Bumped whenever an asset changes, so existing starter folders are rebuilt. */
constexpr int kStarterVersion = 3;
constexpr double kPi = 3.14159265358979323846;

/** Polygons built up piece by piece, written as one OBJ object. */
struct Builder
{
    std::vector<glm::dvec3> points;
    std::vector<std::vector<int>> faces;

    int point(const glm::dvec3& p)
    {
        points.push_back(p);
        return int(points.size()) - 1;
    }

    /** An axis-aligned box. */
    void box(const glm::dvec3& lo, const glm::dvec3& hi)
    {
        const int b = int(points.size());
        for (int k = 0; k < 8; ++k)
            point({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        for (const auto& q : {std::array{0, 2, 3, 1}, std::array{4, 5, 7, 6}, std::array{0, 1, 5, 4}, std::array{2, 6, 7, 3}, std::array{0, 4, 6, 2},
                              std::array{1, 3, 7, 5}})
            faces.push_back({b + q[0], b + q[1], b + q[2], b + q[3]});
    }

    /** A capped cylinder (or a tube with inner > 0) from a to b. */
    void cylinder(const glm::dvec3& a, const glm::dvec3& b, double radius, int sides, double inner = 0.0)
    {
        const glm::dvec3 axis = glm::normalize(b - a);
        const glm::dvec3 side = std::abs(axis.y) < 0.9 ? glm::normalize(glm::cross(axis, glm::dvec3(0, 1, 0))) : glm::dvec3(1, 0, 0);
        const glm::dvec3 up = glm::cross(side, axis);
        auto ring = [&](const glm::dvec3& c, double r) {
            std::vector<int> out;
            for (int k = 0; k < sides; ++k)
            {
                const double t = 2.0 * kPi * k / sides;
                out.push_back(point(c + (side * std::cos(t) + up * std::sin(t)) * r));
            }
            return out;
        };
        const std::vector<int> oa = ring(a, radius), ob = ring(b, radius);
        for (int k = 0; k < sides; ++k)
            faces.push_back({oa[k], oa[(k + 1) % sides], ob[(k + 1) % sides], ob[k]});
        if (inner <= 0.0)
        {
            faces.push_back(std::vector<int>(oa.rbegin(), oa.rend()));
            faces.push_back(ob);
            return;
        }
        const std::vector<int> ia = ring(a, inner), ib = ring(b, inner);
        for (int k = 0; k < sides; ++k)
        {
            faces.push_back({ia[(k + 1) % sides], ia[k], ib[k], ib[(k + 1) % sides]});
            faces.push_back({oa[(k + 1) % sides], oa[k], ia[k], ia[(k + 1) % sides]});
            faces.push_back({ob[k], ob[(k + 1) % sides], ib[(k + 1) % sides], ib[k]});
        }
    }

    /** The convex hull of the points. */
    void hull(const std::vector<glm::dvec3>& pts)
    {
        lila::physics::Shape s;
        s.kind = lila::physics::Shape::Kind::Hull;
        s.points = std::make_shared<const std::vector<glm::dvec3>>(pts);
        for (const lila::physics::ConvexMesh& m : lila::physics::convexMeshes(s))
        {
            const int b = int(points.size());
            points.insert(points.end(), m.points.begin(), m.points.end());
            for (const std::vector<int>& f : m.faces)
            {
                std::vector<int> face;
                for (int i : f)
                    face.push_back(b + i);
                faces.push_back(face);
            }
        }
    }

    /** An outline in XZ (counter-clockwise seen from above) raised from y0 to y1. */
    void prism(const std::vector<glm::dvec2>& outline, double y0, double y1)
    {
        const int n = int(outline.size());
        const int b = int(points.size());
        for (const glm::dvec2& p : outline)
            point({p.x, y0, p.y});
        for (const glm::dvec2& p : outline)
            point({p.x, y1, p.y});
        std::vector<int> bottom, top;
        for (int k = 0; k < n; ++k)
        {
            bottom.push_back(b + n - 1 - k);
            top.push_back(b + n + k);
            faces.push_back({b + k, b + (k + 1) % n, b + n + (k + 1) % n, b + n + k});
        }
        faces.push_back(top);
        faces.push_back(bottom);
    }

    glm::dvec3 size() const
    {
        glm::dvec3 lo(1e300), hi(-1e300);
        for (const glm::dvec3& p : points)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        return points.empty() ? glm::dvec3(0.0) : hi - lo;
    }

    /** OBJ text with a uv per corner, projected along each face's main axis at a metre a tile. */
    std::string obj(const std::string& name) const
    {
        std::ostringstream o;
        o.precision(9);
        o << "# Lila starter library: " << name << "\no " << name << "\n";
        for (const glm::dvec3& p : points)
            o << "v " << p.x << ' ' << p.y << ' ' << p.z << "\n";
        size_t vt = 0;
        std::ostringstream f;
        for (const std::vector<int>& face : faces)
        {
            glm::dvec3 n(0.0);
            for (size_t k = 0; k < face.size(); ++k)
                n += glm::cross(points[size_t(face[k])], points[size_t(face[(k + 1) % face.size()])]);
            const glm::dvec3 a = glm::abs(n);
            const int axis = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
            f << "f";
            for (int i : face)
            {
                const glm::dvec3& p = points[size_t(i)];
                const glm::dvec2 uv = axis == 0 ? glm::dvec2(p.z, p.y) : axis == 1 ? glm::dvec2(p.x, p.z) : glm::dvec2(p.x, p.y);
                o << "vt " << uv.x / 100.0 << ' ' << uv.y / 100.0 << "\n";
                f << ' ' << i + 1 << '/' << ++vt;
            }
            f << "\n";
        }
        o << f.str();
        return o.str();
    }
};

/** Deterministic noise in [0, 1). */
struct Rng
{
    uint32_t s;
    double operator()()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return double(s % 1000003u) / 1000003.0;
    }
};

/** An irregular rock: a hull of points on a squashed ellipsoid, flat-ish underneath, resting on y = 0. */
std::vector<glm::dvec3> rockPoints(double size, uint32_t seed, int count = 18)
{
    Rng r{seed * 2654435761u + 7u};
    const glm::dvec3 radii = glm::dvec3(1.0, 0.55 + 0.25 * r(), 0.7 + 0.3 * r()) * size * 0.5;
    std::vector<glm::dvec3> pts;
    for (int k = 0; k < count; ++k)
    {
        const double u = r() * 2.0 - 1.0, t = r() * 2.0 * kPi;
        const double s = std::sqrt(std::max(0.0, 1.0 - u * u));
        glm::dvec3 d(s * std::cos(t), u, s * std::sin(t));
        d *= 0.75 + 0.25 * r();
        pts.push_back(d * radii);
    }
    double low = 1e300;
    for (const glm::dvec3& p : pts)
        low = std::min(low, p.y);
    for (glm::dvec3& p : pts)
        p.y -= low;
    return pts;
}

struct Starter
{
    std::string collection, name, description, material, shape, role = "dynamic";
    std::vector<std::string> tags;
    std::function<void(Builder&)> build;
    int maxParts = 0;
};

std::vector<Starter> starters()
{
    std::vector<Starter> out;
    auto add = [&](Starter s) { out.push_back(std::move(s)); };

    // Industrial.
    add({"Industrial", "Crate", "Wooden crate with corner battens, 80 cm.", "wood", "box", "dynamic", {"crate", "box", "wood", "storage", "industrial"},
         [](Builder& b) {
             b.box({-38, 0, -38}, {38, 80, 38});
             for (int k = 0; k < 4; ++k)
             {
                 const double x = k & 1 ? 34 : -40, z = k & 2 ? 34 : -40;
                 b.box({x, 0, z}, {x + 6, 80, z + 6});
             }
         }});
    add({"Industrial", "Barrel", "Steel drum, 200 litre.", "metal", "auto", "dynamic", {"barrel", "drum", "metal", "oil", "industrial"},
         [](Builder& b) {
             b.cylinder({0, 0, 0}, {0, 88, 0}, 29, 20);
             b.cylinder({0, 28, 0}, {0, 31, 0}, 30, 20);
             b.cylinder({0, 58, 0}, {0, 61, 0}, 30, 20);
         }});
    add({"Industrial", "Pallet", "Euro pallet, 120 by 80 cm.", "wood", "box", "dynamic", {"pallet", "wood", "industrial", "warehouse"},
         [](Builder& b) {
             for (double z : {-40.0, -5.0, 30.0})
                 b.box({-60, 0, z}, {60, 2.2, z + 10});
             for (double x : {-60.0, -5.0, 50.0})
                 for (double z : {-40.0, -5.0, 30.0})
                     b.box({x, 2.2, z}, {x + 10, 10, z + 10});
             for (double z : {-40.0, -5.0, 30.0})
                 b.box({-60, 10, z}, {60, 12.2, z + 10});
             for (int k = 0; k < 5; ++k)
                 b.box({-60, 12.2, -40 + k * 17.5}, {60, 14.4, -30 + k * 17.5});
         }});
    add({"Industrial", "Beam", "Square timber beam, 4 m.", "wood", "box", "dynamic", {"beam", "timber", "wood", "construction"},
         [](Builder& b) { b.box({-200, 0, -10}, {200, 20, 10}); }});
    add({"Industrial", "IBeam", "Steel I-beam, 3 m, 30 cm deep.", "metal", "decompose", "dynamic", {"ibeam", "girder", "steel", "metal", "construction", "industrial"},
         [](Builder& b) {
             b.box({-150, 0, -8}, {150, 2, 8});
             b.box({-150, 2, -1}, {150, 28, 1});
             b.box({-150, 28, -8}, {150, 30, 8});
         }});
    add({"Industrial", "PipeSegment", "Steel pipe, 3 m, 60 cm across.", "metal", "decompose", "dynamic", {"pipe", "tube", "metal", "industrial", "plumbing"},
         [](Builder& b) { b.cylinder({-150, 30, 0}, {150, 30, 0}, 30, 20, 28); }, 12});
    add({"Industrial", "Panel", "Steel sheet panel, 2 by 1 m.", "metal", "box", "dynamic", {"panel", "sheet", "plate", "metal", "industrial"},
         [](Builder& b) { b.box({-100, 0, -50}, {100, 1, 50}); }});
    add({"Industrial", "CableSpool", "Wooden cable drum lying on its side, 120 cm.", "wood", "decompose", "dynamic", {"spool", "drum", "cable", "wood", "industrial"},
         [](Builder& b) {
             b.cylinder({-30, 60, 0}, {-26, 60, 0}, 60, 24);
             b.cylinder({26, 60, 0}, {30, 60, 0}, 60, 24);
             b.cylinder({-26, 60, 0}, {26, 60, 0}, 25, 20);
         }});
    add({"Industrial", "ScaffoldTube", "Scaffold tube, 3 m.", "metal", "capsule", "dynamic", {"scaffold", "tube", "pole", "metal", "construction"},
         [](Builder& b) { b.cylinder({-150, 2.4, 0}, {150, 2.4, 0}, 2.4, 10); }});
    add({"Industrial", "ScaffoldBoard", "Scaffold board, 3 m.", "wood", "box", "dynamic", {"scaffold", "board", "plank", "wood", "construction"},
         [](Builder& b) { b.box({-150, 0, -11.25}, {150, 3.8, 11.25}); }});
    add({"Industrial", "ScaffoldFrame", "Scaffold frame, 2 m tall, 1 m wide.", "metal", "decompose", "dynamic", {"scaffold", "frame", "metal", "construction"},
         [](Builder& b) {
             for (double x : {-50.0, 50.0})
                 b.cylinder({x, 0, 0}, {x, 200, 0}, 2.4, 10);
             for (double y : {20.0, 100.0, 190.0})
                 b.cylinder({-47.6, y, 0}, {47.6, y, 0}, 2.1, 10);
         }});

    // Debris: stone chunks in four size classes, three of each.
    const std::pair<const char*, double> classes[] = {{"S", 12.0}, {"M", 35.0}, {"L", 90.0}, {"XL", 220.0}};
    const char* words[] = {"small", "medium", "large", "huge"};
    for (int c = 0; c < 4; ++c)
        for (int v = 1; v <= 3; ++v)
        {
            const double size = classes[c].second * (0.85 + 0.15 * v);
            const uint32_t seed = uint32_t(c * 10 + v);
            add({"Debris", std::format("StoneChunk{}{}", classes[c].first, v), std::format("Broken stone, about {:.0f} cm.", size), "stone", "hull",
                 "dynamic", {"stone", "chunk", "rock", "debris", "rubble", words[c]},
                 [size, seed](Builder& b) { b.hull(rockPoints(size, seed)); }});
        }
    add({"Debris", "RubblePile", "Heap of broken stone, 2 m across.", "stone", "mesh", "static", {"rubble", "heap", "pile", "debris", "stone", "ruin"},
         [](Builder& b) {
             Rng r{4242u};
             for (int k = 0; k < 14; ++k)
             {
                 const double a = r() * 2.0 * kPi, d = std::sqrt(r()) * 80.0;
                 const double size = 30.0 + 50.0 * r() * (1.0 - d / 100.0);
                 std::vector<glm::dvec3> pts = rockPoints(size, 100u + uint32_t(k));
                 const glm::dvec3 at(std::cos(a) * d, std::max(0.0, 40.0 - d * 0.45) * r(), std::sin(a) * d);
                 for (glm::dvec3& p : pts)
                     p += at;
                 b.hull(pts);
             }
         }});
    add({"Debris", "BrokenSlab", "Concrete floor slab broken along one edge, 2 m.", "stone", "hull", "dynamic", {"slab", "concrete", "floor", "debris", "ruin"},
         [](Builder& b) {
             std::vector<glm::dvec2> outline = {{-100, -75}, {100, -75}};
             Rng r{77u};
             for (int k = 0; k <= 8; ++k)
                 outline.push_back({100.0 - k * 25.0, 75.0 - r() * 40.0});
             b.prism(outline, 0, 20);
             for (double x : {-60.0, -20.0, 20.0, 60.0})
                 b.cylinder({x, 10, 20}, {x + 4, 12, 90}, 0.8, 6);
         }});
    add({"Debris", "Rebar", "Straight reinforcing bar, 2 m.", "metal", "capsule", "dynamic", {"rebar", "bar", "steel", "metal", "debris", "construction"},
         [](Builder& b) { b.cylinder({-100, 0.8, 0}, {100, 0.8, 0}, 0.8, 8); }});
    add({"Debris", "RebarBent", "Reinforcing bar bent at a right angle.", "metal", "decompose", "dynamic", {"rebar", "bent", "steel", "metal", "debris"},
         [](Builder& b) {
             b.cylinder({-80, 0.8, 0}, {0, 0.8, 0}, 0.8, 8);
             b.cylinder({0, 0.8, 0}, {0, 0.8, 60}, 0.8, 8);
         }});

    // Road: terrainRoadBuild stands these along roads, +X along the road and arms reaching over it along +Z.
    add({"Road", "Delineator", "Roadside marker post with a reflector, 100 cm.", "plastic", "box", "static", {"delineator", "marker", "post", "road"},
         [](Builder& b) {
             b.box({-6, 0, -6}, {6, 100, 6});
             b.box({-5, 80, 6}, {5, 92, 7});
         }});
    add({"Road", "StreetLight", "Street light, 8 m, with an arm over the road.", "metal", "decompose", "static", {"streetlight", "light", "lamp", "pole", "road"},
         [](Builder& b) {
             b.cylinder({0, 0, 0}, {0, 800, 0}, 10, 12);
             b.cylinder({0, 790, 0}, {0, 790, 180}, 5, 8);
             b.box({-15, 775, 170}, {15, 790, 230});
         }, 4});
    add({"Road", "EdgeLight", "Runway edge light on a frangible stem, 35 cm.", "metal", "auto", "static", {"edgelight", "runway", "light", "airfield"},
         [](Builder& b) {
             b.cylinder({0, 0, 0}, {0, 25, 0}, 4, 8);
             b.cylinder({0, 25, 0}, {0, 35, 0}, 12, 12);
         }});
    add({"Road", "CatenaryMast", "Rail catenary mast, 6.5 m, with a cantilever over the track.", "metal", "decompose", "static", {"mast", "catenary", "rail", "pole"},
         [](Builder& b) {
             b.box({-12, 0, -12}, {12, 650, 12});
             b.cylinder({0, 600, 0}, {0, 600, 260}, 4, 8);
             b.cylinder({0, 520, 0}, {0, 600, 200}, 3, 8);
         }, 4});

    // Facility.
    add({"Facility", "Locker", "Steel locker, 180 cm.", "metal", "box", "dynamic", {"locker", "cabinet", "metal", "facility", "hangar"},
         [](Builder& b) {
             b.box({-30, 0, -25}, {30, 180, 25});
             b.box({20, 95, 25}, {23, 115, 27});
         }});
    add({"Facility", "Console", "Control console with a sloped panel.", "metal", "hull", "dynamic", {"console", "terminal", "control", "facility", "sci-fi", "starfighter"},
         [](Builder& b) {
             b.box({-60, 0, -35}, {60, 80, 35});
             b.hull({{-60, 80, -35}, {60, 80, -35}, {-60, 80, 35}, {60, 80, 35}, {-60, 115, -35}, {60, 115, -35}, {-60, 88, 35}, {60, 88, 35}});
         }});
    add({"Facility", "SupplyCrate", "Hard-shell supply crate with corner bumpers, 120 cm.", "plastic", "box", "dynamic",
         {"crate", "cargo", "supply", "container", "facility", "sci-fi", "starfighter"},
         [](Builder& b) {
             b.box({-58, 0, -38}, {58, 78, 38});
             for (int k = 0; k < 8; ++k)
             {
                 const double x = k & 1 ? 50 : -60, y = k & 2 ? 70 : 0, z = k & 4 ? 30 : -40;
                 b.box({x, y, z}, {x + 10, y + 10, z + 10});
             }
         }});
    add({"Facility", "CableTray", "Ladder cable tray, 3 m.", "metal", "decompose", "dynamic", {"cable", "tray", "ladder", "metal", "facility", "hangar"},
         [](Builder& b) {
             for (double z : {-16.0, 15.0})
                 b.box({-150, 0, z}, {150, 10, z + 1});
             for (int k = 0; k < 10; ++k)
             {
                 const double x = -140.0 + k * 31.0;
                 b.box({x, 0, -15}, {x + 3, 1.5, 15});
             }
         }, 12});
    return out;
}

fs::path markerOf(const fs::path& dir) { return dir / "starter.json"; }

bool current(const fs::path& dir)
{
    try
    {
        std::ifstream f(markerOf(dir));
        if (!f)
            return false;
        return json::parse(f).value("version", 0) == kStarterVersion;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace

fs::path starterDir() { return lila::PrefStore::get().userDir() / "starter-library"; }

bool starterEnabled() { return lila::PrefStore::get().value<bool>("stagehand", "/library/starter", true); }

json buildStarter(bool force)
{
    LILA_ZONE();

    const fs::path dir = starterDir();
    json ids = json::array();
    if (!force && current(dir))
    {
        for (const Starter& s : starters())
            ids.push_back(s.collection + "/" + s.name);
        return {{"dir", dir.generic_string()}, {"built", false}, {"assets", ids}};
    }
    std::error_code ec;
    for (const Starter& s : starters())
    {
        Builder b;
        s.build(b);
        const fs::path folder = dir / s.collection / s.name;
        fs::create_directories(folder, ec);
        std::ofstream(folder / (s.name + ".obj"), std::ios::binary) << b.obj(s.name);
        // The old drawing may show the old model; a missing one is drawn again the next time the library shows.
        fs::remove(folder / "thumb.png", ec);
        const glm::dvec3 size = b.size();
        json physics = {{"role", s.role}, {"material", s.material}, {"shape", s.shape}};
        if (s.maxParts > 0)
            physics["maxParts"] = s.maxParts;
        const json meta = {{"name", s.name},
                           {"model", s.name + ".obj"},
                           {"tags", s.tags},
                           {"description", s.description},
                           {"size", json::array({size.x, size.y, size.z})},
                           {"physics", physics},
                           {"starter", true}};
        std::ofstream(folder / "asset.json") << meta.dump(2);
        ids.push_back(s.collection + "/" + s.name);
    }
    std::ofstream(markerOf(dir)) << json{{"version", kStarterVersion}}.dump(2);
    lila::log::info("Library: starter props written to {}", dir.generic_string());
    return {{"dir", dir.generic_string()}, {"built", true}, {"assets", ids}};
}
} // namespace stagehand::library
