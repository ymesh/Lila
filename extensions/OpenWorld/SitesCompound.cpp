// Site kits behind walls: the walled mansion compound and the private island compound.

#include "SitesKit.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openworld::sites
{
namespace
{

/**
 * Walls round x in [-w/2, w/2], z in [z0, z0 + d] with the gate in the front (z0) wall, the mansion toward the back,
 * pool behind it, helipad and garage beside it, a palm drive from the gate. Returns the mansion's footprint.
 */
Ring walledCompound(Kit& k, double z0, bool guard, bool cameras)
{
    const Spec& s = k.spec;
    const double W = s.f("compoundWidth"), D = s.f("compoundDepth"), wallH = s.f("wallHeight");
    const double x0 = -W * 0.5, x1 = W * 0.5, z1 = z0 + D;
    std::vector<Ring> taken;

    k.ground("pad", "terrainStampPad", {{"footprint", pts(rect(x0 - 200.0, z0 - 200.0, x1 + 200.0, z1 + 200.0))}, {"mode", "flatten"}, {"falloff", 1500.0},
                                        {"masks", {"cityFootprint"}}});
    {
        Part& p = k.part("compound", "yard", rect(x0, z0, x1, z1), Part::Needs::Land);
        p.maxRelief = 250.0;
        p.groundOp = "pad";
    }
    k.alongRing("wall", "wall.perimeter", rect(x0, z0, x1, z1), true, 300.0, {{W * 0.5 - 300.0, W * 0.5 + 300.0}}, wallH / 300.0);
    SlotOut& gate = k.slot("gate", "gate.main", {0.0, z0}, 0.0);
    gate.scale = {600.0 / 500.0, wallH / 350.0, 1.0};
    if (cameras)
    {
        const P2 corners[4] = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
        for (int c = 0; c < 4; ++c)
            k.slot("camera/corner" + std::to_string(c), "camera.security", corners[c], facing(glm::normalize(corners[c])), wallH + 20.0);
        for (int side : {-1, 1})
            k.slot(std::string("camera/gate") + (side < 0 ? "w" : "e"), "camera.security", {side * 400.0, z0}, facing({0.0, -1.0}), wallH + 20.0);
    }

    if (guard)
    {
        const Ring g = rect(700.0, z0 + 300.0, 1600.0, z0 + 1100.0);
        EnvelopeOut& e = k.envelope("guardHouse", g, "mansion", "guardhouse", "residential", 1, 300.0, "flat");
        e.front = {-1.0, 0.0};
        e.entrances = {{700.0, z0 + 700.0}};
        taken.push_back(g);
    }

    // The mansion toward the back, its front to the gate.
    const int floors = s.i("mansionFloors");
    const double mw = std::min(W * 0.45, 3400.0), md = std::min(D * 0.32, 2200.0);
    const double mz = z0 + D * 0.6;
    const Ring m = rect(-mw * 0.5, mz - md * 0.5, mw * 0.5, mz + md * 0.5);
    const double r = k.rnd(31);
    const std::string roof = r < 0.3 ? "dome" : r < 0.75 ? "hip" : "flat";
    EnvelopeOut& mansion = k.envelope("mansion", m, "mansion", "mansion", "residential", floors, floors * 380.0, roof);
    mansion.front = {0.0, -1.0};
    mansion.entrances = {{0.0, mz - md * 0.5}};
    mansion.name = s.name;
    mansion.extra = {{"columns", true}, {"domes", roof == "dome"}, {"balconies", floors > 1}};
    taken.push_back(m);

    const Ring garage = rect(x0 + 600.0, z0 + D * 0.22, x0 + 600.0 + std::min(1800.0, W * 0.2), z0 + D * 0.22 + 900.0);
    if (!ringsOverlap(garage, m))
    {
        EnvelopeOut& g = k.envelope("garage", garage, "mansion", "garage", "residential", 1, 320.0, "flat");
        g.front = {1.0, 0.0};
        g.entrances = {{garage[1].x, (garage[0].y + garage[2].y) * 0.5}};
        taken.push_back(garage);
    }

    if (s.b("pool"))
    {
        const double pz = mz + md * 0.5 + 900.0;
        const double pl = std::min(1000.0, W * 0.3), pd = 500.0;
        if (pz + pd * 0.5 < z1 - 200.0)
        {
            SlotOut& p = k.slot("pool", "pool.swimming", {0.0, pz - pd * 0.5}, 0.0);
            p.scale = {pl / 1000.0, 1.0, 1.0};
        }
        else
        {
            // No room behind: beside the mansion instead, and say so.
            const double px = mw * 0.5 + 300.0 + pl * 0.5;
            if (px + pl * 0.5 < x1 - 200.0)
            {
                k.slot("pool", "pool.swimming", {px, mz - pd * 0.5}, 0.0).scale = {pl / 1000.0, 1.0, 1.0};
                k.L.notes.push_back("the pool went beside the mansion: no room behind it");
            }
            else
                k.L.notes.push_back("no room for the pool: deepen or widen the compound");
        }
    }
    if (s.b("helipad"))
    {
        const P2 at(x1 - 1300.0, z0 + D * 0.28);
        const Ring pad = rect(at.x - 1000.0, at.y - 1000.0, at.x + 1000.0, at.y + 1000.0);
        bool clash = at.x - 1000.0 < x0 + 200.0 || at.y - 1000.0 < z0 + 200.0;
        for (const Ring& o : taken)
            clash = clash || ringsOverlap(o, pad);
        if (!clash)
            k.slot("helipad", "helipad", at);
        else
            k.L.notes.push_back("no room for the helipad inside the walls: widen the compound");
    }
    for (int i = 0; z0 + 700.0 + i * 900.0 < mz - md * 0.5 - 300.0; ++i)
        for (int side : {-1, 1})
            k.slot("drive/palm/" + std::string(side < 0 ? "w" : "e") + std::to_string(i), "tree.palm", {side * 380.0, z0 + 700.0 + i * 900.0});
    k.ground("drive", "terrainStampRegion",
             {{"footprint", pts(rect(-300.0, z0, 300.0, mz - md * 0.5))}, {"mode", "blend"}, {"strength", 0.0}, {"falloff", 0.0}, {"masks", {"paved"}}});
    return m;
}
} // namespace

void buildCompound(Kit& k)
{
    const Spec& s = k.spec;
    const double W = s.f("compoundWidth"), D = s.f("compoundDepth");
    k.L.boundary = rect(-W * 0.5 - 300.0, -1500.0, W * 0.5 + 300.0, D + 300.0);
    walledCompound(k, 0.0, s.b("guardHouse"), true);
    k.ground("access", "terrainAddRoad",
             {{"kind", "road"}, {"profile", "lane"}, {"layer", "Sites"}, {"path", json::array({pt({0.0, 200.0}), pt({0.0, -12000.0})})}});
}

void buildIsland(Kit& k)
{
    const Spec& s = k.spec;
    const double R = s.f("islandRadius"), W = s.f("compoundWidth"), D = s.f("compoundDepth"), dock = s.f("dockLength");
    if (D * 0.5 + 1500.0 > R)
        throw std::runtime_error("the compound reaches past the shore: make islandRadius more than half compoundDepth plus 1500");
    k.L.boundary = rect(-std::max(R, W * 0.5 + 500.0), -R - dock - 800.0, std::max(R, W * 0.5 + 500.0), std::max(R, D * 0.5 + 500.0));
    if (s.b("raiseIsland"))
    {
        k.ground("island", "terrainStampRegion",
                 {{"footprint", pts(circle({0.0, 0.0}, R, 32))}, {"mode", "raise"}, {"heightFromWater", 300.0}, {"falloff", 3000.0}, {"masks", {"beach"}}});
    }
    {
        Part& isl = k.part("island", "island", circle({0.0, 0.0}, R * 0.7, 24), Part::Needs::Land);
        isl.maxSlope = 30.0;
    }
    walledCompound(k, -D * 0.5, false, true);
    for (EnvelopeOut& e : k.L.envelopes)
        if (e.path == "mansion")
            e.name = k.name("mansion", lila::names::Kind::Business, "mansion");

    // A path from the gate down to the dock, the dock out from the shore, boats along it, a boathouse at its foot.
    const double shore = -R;
    const int path = k.along("path", "path", {0.0, -D * 0.5 - 100.0}, {0.0, shore + 800.0}, 300.0);
    for (size_t n = k.L.slots.size() - size_t(path); n < k.L.slots.size(); ++n)
        k.L.slots[n].scale.z = 200.0 / 250.0;
    const int deck = k.along("dock/deck", "dock.pier", {0.0, shore + 800.0}, {0.0, shore - dock}, 500.0, 150.0, Level::Water);
    for (size_t n = k.L.slots.size() - size_t(deck); n < k.L.slots.size(); ++n)
        k.L.slots[n].scale.z = 250.0 / 300.0;
    for (int b = 0; b < s.i("boats"); ++b)
    {
        const double side = b % 2 ? -1.0 : 1.0;
        const double z = shore - dock + 800.0 + (b / 2) * 1100.0;
        if (z > shore - 300.0)
        {
            k.L.notes.push_back("only " + std::to_string(b) + " boats fit along the dock: lengthen it");
            break;
        }
        k.slot("dock/boat/" + std::to_string(b), b % 3 == 2 ? "boat.skiff" : "boat.speedboat", {side * 330.0, z}, kPi, 0.0, Level::Water);
    }
    {
        Part& d = k.part("dock", "dock", rect(-600.0, shore - dock, 600.0, shore - dock * 0.5), Part::Needs::Water);
        d.minDepth = 120.0;
    }
    EnvelopeOut& bh = k.envelope("boathouse", rect(700.0, shore + 700.0, 2100.0, shore + 1900.0), "colonial", "boathouse", "residential", 1, 350.0, "gable");
    bh.front = {0.0, -1.0};
    bh.entrances = {{1400.0, shore + 700.0}};
    k.L.names["island"] = k.spec.name;
}
} // namespace openworld::sites
