#include "GenPlaceholders.h"

#include "Placeholders.h"
#include "Truss.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weathering::gen
{
namespace
{
constexpr double kPi = std::numbers::pi;

/** Every point moved by up to `amount` cm, the same way for the same seed. */
void jitter(Mesh& m, uint32_t seed, double amount)
{
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, m.position(p) + glm::dvec3(hash01(seed, p, 1) - 0.5, hash01(seed, p, 2) - 0.5, hash01(seed, p, 3) - 0.5) * 2.0 * amount);
}

/** Moves the mesh so its lowest point sits on y = 0. */
void standOnGround(Mesh& m)
{
    const Bounds b = boundsOf(m);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, m.position(p) - glm::dvec3(0, b.min.y, 0));
}

Mesh finish(Mesh m)
{
    standOnGround(m);
    boxUvs(m);
    return m;
}

/** A wavy sheet: `waves` across width, extruded along Z. */
Mesh corrugated(double width, double length, int waves, double amplitude, double thickness, int samplesPerWave)
{
    std::vector<glm::dvec2> profile;
    const int n = waves * samplesPerWave;
    for (int k = 0; k <= n; ++k)
    {
        const double x = -width * 0.5 + width * k / n;
        profile.push_back({x, amplitude * (1.0 + std::sin(2.0 * kPi * waves * k / n))});
    }
    for (int k = n; k >= 0; --k)
        profile.push_back({profile[size_t(k)].x, profile[size_t(k)].y - thickness});
    Mesh m;
    addExtrusion(m, profile, -length * 0.5, length * 0.5);
    return m;
}

/** A half-round shell (barrel tile, round ridge cap) across X, extruded along Z. */
Mesh arch(double radius, double thickness, double length, int segments)
{
    std::vector<glm::dvec2> profile;
    for (int k = 0; k <= segments; ++k)
    {
        const double a = kPi * k / segments;
        profile.push_back({std::cos(a) * radius, std::sin(a) * radius});
    }
    for (int k = segments; k >= 0; --k)
    {
        const double a = kPi * k / segments;
        profile.push_back({std::cos(a) * (radius - thickness), std::sin(a) * (radius - thickness)});
    }
    Mesh m;
    addExtrusion(m, profile, -length * 0.5, length * 0.5);
    return m;
}

/** A bar with chamfered long edges, w across X, h up Y, along Z. */
Mesh chamferBar(double w, double h, double length, double chamfer)
{
    const double c = std::min({chamfer, w * 0.45, h * 0.45});
    const std::vector<glm::dvec2> p = {{-w / 2 + c, 0}, {w / 2 - c, 0}, {w / 2, c}, {w / 2, h - c}, {w / 2 - c, h}, {-w / 2 + c, h}, {-w / 2, h - c}, {-w / 2, c}};
    Mesh m;
    addExtrusion(m, p, -length * 0.5, length * 0.5);
    return m;
}

/** A pane split into shards from a crack centre, each shard pulled in a little, thin along Z. */
Mesh shards(int count, double gap, uint32_t seed, double thickness)
{
    const glm::dvec2 c(-8.0 + 16.0 * hash01(seed, 1), -8.0 + 16.0 * hash01(seed, 2));
    std::vector<glm::dvec2> rim;
    for (int k = 0; k < count; ++k)
    {
        const double a = 2.0 * kPi * (k + 0.3 * hash01(seed, 3, uint32_t(k))) / count;
        glm::dvec2 d(std::cos(a), std::sin(a));
        const double t = 50.0 / std::max(std::abs(d.x), std::abs(d.y));
        rim.push_back(c + d * t);
    }
    Mesh m;
    for (int k = 0; k < count; ++k)
    {
        glm::dvec2 a = rim[size_t(k)], b = rim[size_t((k + 1) % count)];
        std::vector<glm::dvec2> poly = {c, glm::clamp(a, glm::dvec2(-50), glm::dvec2(50))};
        // Square corners between the two rim points keep the pane filled.
        for (const glm::dvec2 q : {glm::dvec2(50, 50), glm::dvec2(-50, 50), glm::dvec2(-50, -50), glm::dvec2(50, -50)})
        {
            const double aa = std::atan2(a.y - c.y, a.x - c.x), bb = std::atan2(b.y - c.y, b.x - c.x), qq = std::atan2(q.y - c.y, q.x - c.x);
            auto ccw = [](double from, double to) { return std::fmod(to - from + 4.0 * kPi, 2.0 * kPi); };
            if (ccw(aa, qq) < ccw(aa, bb))
                poly.push_back(q);
        }
        poly.push_back(glm::clamp(b, glm::dvec2(-50), glm::dvec2(50)));
        {
            // Corners in order round the crack centre.
            std::sort(poly.begin() + 1, poly.end(), [&](const glm::dvec2& x, const glm::dvec2& y) {
                auto ang = [&](const glm::dvec2& v) { return std::fmod(std::atan2(v.y - c.y, v.x - c.x) - std::atan2(a.y - c.y, a.x - c.x) + 4.0 * kPi, 2.0 * kPi); };
                return ang(x) < ang(y);
            });
        }
        glm::dvec2 centre(0.0);
        for (const glm::dvec2& p : poly)
            centre += p;
        centre /= double(poly.size());
        for (glm::dvec2& p : poly)
            p += (centre - p) * (gap / std::max(1.0, glm::length(centre - p)));
        addExtrusion(m, poly, -thickness * 0.5, thickness * 0.5);
    }
    return m;
}

/** Jagged teeth of glass left round the edge of a pane, 100 by 100, thin along Z. */
Mesh brokenPane(int teethPerSide, uint32_t seed, double thickness, double reach)
{
    Mesh m;
    for (int side = 0; side < 4; ++side)
        for (int k = 0; k < teethPerSide; ++k)
        {
            if (hash01(seed, uint32_t(side), uint32_t(k)) < 0.25)
                continue;
            const double t0 = -50.0 + 100.0 * k / teethPerSide, t1 = -50.0 + 100.0 * (k + 1) / teethPerSide;
            const double depth = reach * (0.3 + 0.7 * hash01(seed, uint32_t(side), uint32_t(k), 7));
            const double apex = t0 + (t1 - t0) * (0.2 + 0.6 * hash01(seed, uint32_t(side), uint32_t(k), 9));
            std::vector<glm::dvec2> tri = {{t0, -50}, {t1, -50}, {apex, -50 + depth}};
            const double turn = kPi * 0.5 * side;
            for (glm::dvec2& p : tri)
                p = {p.x * std::cos(turn) - p.y * std::sin(turn), p.x * std::sin(turn) + p.y * std::cos(turn)};
            addExtrusion(m, tri, -thickness * 0.5, thickness * 0.5);
        }
    return m;
}

glm::dmat4 alongX() { return glm::rotate(glm::dmat4(1.0), kPi / 2.0, glm::dvec3(0, 1, 0)); }

Mesh build(const std::string& r, bool toon)
{
    Mesh m;
    if (r == role::kRoofSheet)
        return finish(toon ? corrugated(100, 180, 3, 3.0, 1.5, 8) : corrugated(90, 200, 12, 1.4, 0.3, 8));
    if (r == role::kRidgeCap)
    {
        if (toon)
            return finish(arch(14, 3, 60, 6));
        const std::vector<glm::dvec2> v = {{-15, 0}, {-13.6, 0}, {0, 6.9}, {13.6, 0}, {15, 0}, {0, 8}};
        addExtrusion(m, v, -50, 50);
        return finish(std::move(m));
    }
    if (r == role::kRoofTile)
        return finish(toon ? chamferBar(30, 4, 30, 1.5) : arch(10, 1.2, 35, 8));
    if (r == role::kRoofPlank)
    {
        if (toon)
            return finish(chamferBar(24, 5, 150, 1.2));
        m = chamferBar(20, 2.5, 180, 0.3);
        jitter(m, 41, 0.35);
        return finish(std::move(m));
    }
    if (r == role::kFrame)
    {
        // Members run along X: the bar is built along Z, then turned.
        m = toon ? chamferBar(10, 10, 100, 2.0) : chamferBar(6, 8, 100, 0.4);
        Mesh out;
        out.append(m);
        for (Index p = 0; p < out.pointCapacity(); ++p)
            if (out.pointValid(p))
                out.setPosition(p, glm::dvec3(alongX() * glm::dvec4(out.position(p), 1.0)));
        return finish(std::move(out));
    }
    if (r == role::kSill)
    {
        if (toon)
            addExtrusion(m, std::vector<glm::dvec2>{{-11, 0}, {11, 0}, {11, 6}, {9, 8}, {-11, 8}}, -50, 50, alongX());
        else
        {
            addBox(m, {-50, 0, -10}, {50, 5, 10});
            addBox(m, {-50, -1.5, 8.5}, {50, 0.01, 10});
        }
        return finish(std::move(m));
    }
    if (r == role::kGlass)
    {
        addBox(m, {-50, -50, toon ? -0.5 : -0.3}, {50, 50, toon ? 0.5 : 0.3});
        setMaterial(m, toon ? "M_WeatheringGlassToon" : "M_WeatheringGlass");
        boxUvs(m);
        return m;
    }
    if (r == role::kGlassCracked)
    {
        m = shards(toon ? 4 : 7, toon ? 1.2 : 0.4, toon ? 5 : 11, toon ? 1.0 : 0.6);
        setMaterial(m, toon ? "M_WeatheringGlassToon" : "M_WeatheringGlass");
        boxUvs(m);
        return m;
    }
    if (r == role::kGlassBroken)
    {
        m = brokenPane(toon ? 3 : 6, toon ? 3 : 17, toon ? 1.0 : 0.6, toon ? 40.0 : 30.0);
        setMaterial(m, toon ? "M_WeatheringGlassToon" : "M_WeatheringGlass");
        boxUvs(m);
        return m;
    }
    if (r == role::kBoard)
    {
        if (toon)
            addExtrusion(m, std::vector<glm::dvec2>{{-60, 0}, {60, 0}, {56, 20}, {-58, 20}}, -2, 2);
        else
        {
            addBox(m, {-60, 0, -1.25}, {60, 15, 1.25});
            jitter(m, 23, 0.4);
        }
        return finish(std::move(m));
    }
    if (r == role::kInsulator)
    {
        if (toon)
        {
            addCylinder(m, 4, 0, 10, 8);
            addCylinder(m, 7, 0, 2.5, 8);
            addCylinder(m, 7, 7.5, 10, 8);
        }
        else
        {
            addCylinder(m, 1.5, 0, 15, 8);
            for (int k = 0; k < 3; ++k)
                addCylinder(m, 5, 2 + 4.5 * k, 3.2 + 4.5 * k, 12);
        }
        return finish(std::move(m));
    }
    if (r == role::kBracket)
    {
        const double t = toon ? 5.0 : 3.0;
        addBox(m, {0, -t / 2, -t / 2}, {25, t / 2, t / 2});
        addBox(m, {-1.5, -10, -6}, {0, 10, 6});
        addBox(m, {0, -10, -t / 3}, {12, -t / 2 + 0.01, t / 3});
        return finish(std::move(m));
    }
    if (r == role::kPole)
    {
        const double h = toon ? 700 : 900;
        addCylinder(m, toon ? 18 : 12, 0, h, toon ? 6 : 12);
        addBox(m, {toon ? -90.0 : -80.0, h - 60, toon ? -8.0 : -5.0}, {toon ? 90.0 : 80.0, h - (toon ? 44 : 50), toon ? 8.0 : 5.0});
        return finish(std::move(m));
    }
    if (r == role::kRubble)
    {
        if (toon)
            return finish(chamferBar(35, 25, 30, 6));
        addBox(m, {-15, 0, -12}, {15, 18, 12});
        jitter(m, 77, 4.0);
        return finish(std::move(m));
    }
    if (r == role::kPlank)
    {
        m = toon ? chamferBar(18, 5, 110, 1.2) : chamferBar(15, 3, 120, 0.3);
        if (!toon)
            jitter(m, 13, 0.3);
        return finish(std::move(m));
    }
    if (r == role::kBrick)
    {
        if (toon)
            return finish(chamferBar(12, 9, 24, 1.8));
        addBox(m, {-10.75, 0, -5.1}, {10.75, 6.5, 5.1});
        jitter(m, 5, 0.25);
        return finish(std::move(m));
    }
    if (r == role::kVent)
    {
        if (toon)
        {
            addCylinder(m, 12, 0, 40, 8);
            addCylinder(m, 20, 40, 48, 8);
        }
        else
        {
            addBox(m, {-20, 0, -15}, {20, 30, 15});
            addBox(m, {-24, 38, -19}, {24, 41, 19});
            for (int k = 0; k < 4; ++k)
                addBox(m, {(k & 1) ? 15.0 : -18.0, 29.9, (k & 2) ? 10.0 : -13.0}, {(k & 1) ? 18.0 : -15.0, 38.1, (k & 2) ? 13.0 : -10.0});
        }
        return finish(std::move(m));
    }
    if (r == role::kAcUnit)
    {
        if (toon)
        {
            addExtrusion(m, std::vector<glm::dvec2>{{-55, 0}, {55, 0}, {55, 75}, {45, 85}, {-45, 85}, {-55, 75}}, -37, 37);
            addCylinder(m, 30, 85, 90, 10);
        }
        else
        {
            addBox(m, {-50, 0, -35}, {50, 80, 35});
            addCylinder(m, 30, 80, 83, 20);
            addBox(m, {-48, 10, 35}, {48, 70, 36});
        }
        return finish(std::move(m));
    }
    if (r == role::kPipe)
    {
        const double rad = toon ? 10 : 6;
        addCylinder(m, rad, -100, 100, toon ? 6 : 12, glm::rotate(glm::dmat4(1.0), -kPi / 2.0, glm::dvec3(0, 0, 1)) * glm::translate(glm::dmat4(1.0), {0, 0, 0}));
        addCylinder(m, rad, -rad, 60, toon ? 6 : 12, glm::translate(glm::dmat4(1.0), {100, 0, 0}));
        addCylinder(m, rad * 1.3, -rad * 1.3, rad * 1.3, toon ? 6 : 12, glm::translate(glm::dmat4(1.0), {-100, 0, 0}));
        return finish(std::move(m));
    }
    if (r == role::kRailing)
    {
        const double p = toon ? 7 : 4, rail = toon ? 7 : 4;
        addBox(m, {-100, 0, -p / 2}, {-100 + p, 100, p / 2});
        addBox(m, {100 - p, 0, -p / 2}, {100, 100, p / 2});
        addBox(m, {-100, 100 - rail, -rail / 2}, {100, 100, rail / 2});
        addBox(m, {-100 + p, 50 - rail / 2, -rail / 3}, {100 - p, 50 + rail / 2, rail / 3});
        return finish(std::move(m));
    }
    if (r == role::kTower)
    {
        truss::Options o;
        if (toon)
        {
            o.chords = 3;
            o.width = 150;
            o.endWidth = 60;
            o.member = 12;
            o.segment = 150;
            o.bracing = truss::Bracing::Zigzag;
        }
        else
        {
            o.chords = 4;
            o.width = 120;
            o.endWidth = 40;
            o.member = 5;
            o.segment = 100;
        }
        const std::vector<std::vector<glm::dvec3>> line = {{{0, o.member, 0}, {0, toon ? 600.0 : 800.0, 0}}};
        return finish(truss::build(line, o).mesh);
    }
    return m;
}

struct RoleInfo
{
    const char* role;
    const char* collection;
    const char* name;
    const char* material;
    const char* body;
    const char* description;
};

// Rubble, plank and brick pieces for piles are base dressing's placeholders (BasePieces.cpp).
const RoleInfo kRoles[] = {
    {role::kRoofSheet, "Weathering/Roof", "CorrugatedSheet", "metal", "dynamic", "A corrugated roof sheet, longer side down the slope."},
    {role::kRidgeCap, "Weathering/Roof", "RidgeCap", "metal", "dynamic", "A ridge cap laid along a roof's ridge."},
    {role::kRoofTile, "Weathering/Roof", "RoofTile", "ceramic", "dynamic", "A roof tile."},
    {role::kRoofPlank, "Weathering/Roof", "RoofPlank", "wood", "dynamic", "A roof board, laid down the slope."},
    {role::kFrame, "Weathering/Window", "FrameMember", "wood", "dynamic", "A window frame member along X, fitted to each side of an opening."},
    {role::kSill, "Weathering/Window", "Sill", "stone", "static", "A window sill along X, standing out of the wall along Z."},
    {role::kGlass, "Weathering/Window", "Pane", "glass", "dynamic", "An intact pane, X across and Y up."},
    {role::kGlassCracked, "Weathering/Window", "PaneCracked", "glass", "dynamic", "A cracked pane in shards."},
    {role::kGlassBroken, "Weathering/Window", "PaneBroken", "glass", "dynamic", "Glass teeth left round a broken pane."},
    {role::kBoard, "Weathering/Window", "Board", "wood", "dynamic", "A board nailed across a window."},
    {role::kInsulator, "Weathering/Wires", "Insulator", "ceramic", "static", "An insulator a wire leaves from, on a pole top."},
    {role::kBracket, "Weathering/Wires", "WallBracket", "metal", "static", "A wall bracket, its arm along X out of the wall."},
    {role::kPole, "Weathering/Wires", "Pole", "wood", "static", "A utility pole with a crossarm."},
    {role::kVent, "Weathering/Rooftop", "Vent", "metal", "static", "A roof vent."},
    {role::kAcUnit, "Weathering/Rooftop", "AcUnit", "metal", "static", "An air conditioning unit."},
    {role::kPipe, "Weathering/Rooftop", "Pipe", "metal", "static", "A pipe run lying on a roof, turning up at one end."},
    {role::kRailing, "Weathering/Rooftop", "Railing", "metal", "static", "A railing segment along X."},
    {role::kTower, "Weathering/Rooftop", "LatticeTower", "metal", "static", "A lattice tower (Curve to Truss)."},
};
} // namespace

Mesh placeholderMesh(const std::string& r, bool toon) { return build(r, toon); }

std::vector<std::string> generatorRoles()
{
    std::vector<std::string> out;
    for (const RoleInfo& i : kRoles)
        out.emplace_back(i.role);
    return out;
}

void addGeneratorPlaceholders()
{
    for (const RoleInfo& i : kRoles)
        for (bool toon : {false, true})
        {
            Placeholder p;
            p.collection = i.collection;
            p.name = std::string(i.name) + (toon ? "Toon" : "");
            p.role = i.role;
            p.style = toon ? "toon" : "realistic";
            p.tags = {"generator"};
            p.description = std::string(i.description) + (toon ? " Toon: chunky, clean shapes." : "");
            p.physicsRole = i.body;
            p.physicsMaterial = i.material;
            p.physicsShape = "hull";
            const std::string r = i.role;
            p.build = [r, toon] { return build(r, toon); };
            addPlaceholder(std::move(p));
        }
}
} // namespace weathering::gen
