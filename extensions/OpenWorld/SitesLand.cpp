// Site kits inland: mausoleum cemetery, hidden airstrip, industrial farm, mountain village.

#include "SitesKit.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace openworld::sites
{
namespace
{

std::vector<std::string> splitList(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ','))
    {
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        if (!part.empty())
            out.push_back(part);
    }
    return out;
}
} // namespace

void buildCemetery(Kit& k)
{
    const Spec& s = k.spec;
    const double W = s.f("cemeteryWidth"), D = s.f("cemeteryDepth"), pw = s.f("plotWidth"), share = s.f("mausoleums");
    const double x0 = -W * 0.5, x1 = W * 0.5;
    const double pd = std::max(280.0, pw * 1.4);
    k.L.boundary = rect(x0, 0.0, x1, D);
    k.ground("grounds", "terrainStampRegion",
             {{"footprint", pts(rect(x0, 0.0, x1, D))}, {"mode", "blend"}, {"strength", 0.7}, {"falloff", 1000.0}, {"masks", {"cemetery"}}});
    {
        Part& g = k.part("grounds", "plot", rect(x0, 0.0, x1, D), Part::Needs::Land);
        g.maxSlope = 14.0;
    }

    // Walls with the gate in the middle of the front, paths, the chapel at the end of the main path.
    const double wallH = 250.0;
    k.alongRing("wall", "wall.perimeter", rect(x0, 0.0, x1, D), true, 300.0, {{W * 0.5 - 300.0, W * 0.5 + 300.0}}, wallH / 300.0);
    SlotOut& gate = k.slot("gate", "gate.main", {0.0, 0.0}, 0.0);
    gate.scale = {600.0 / 500.0, wallH / 350.0 + 0.2, 1.0};
    const bool chapel = s.b("chapel") && D > 6000.0;
    const double mainEnd = chapel ? D - 2400.0 : D - 300.0, zc = D * 0.5;
    const int mainPieces = k.along("paths/main", "path.cemetery", {0.0, 0.0}, {0.0, mainEnd}, 300.0);
    for (size_t n = k.L.slots.size() - size_t(mainPieces); n < k.L.slots.size(); ++n)
        k.L.slots[n].scale.z = 400.0 / 250.0;
    const int crossPieces = k.along("paths/cross", "path.cemetery", {x0 + 300.0, zc}, {x1 - 300.0, zc}, 300.0);
    for (size_t n = k.L.slots.size() - size_t(crossPieces); n < k.L.slots.size(); ++n)
        k.L.slots[n].scale.z = 300.0 / 250.0;
    for (int i = 0; 800.0 + i * 1500.0 < mainEnd - 300.0; ++i)
        for (int side : {-1, 1})
            k.slot("trees/" + std::string(side < 0 ? "w" : "e") + std::to_string(i), "tree.conifer", {side * 450.0, 800.0 + i * 1500.0});
    if (chapel)
    {
        EnvelopeOut& c = k.envelope("chapel", rect(-1000.0, D - 2300.0, 1000.0, D - 300.0), "church", "church", "religious", 1, 700.0, "gable");
        c.front = {0.0, -1.0};
        c.entrances = {{0.0, D - 2300.0}};
        c.name = k.name("chapel", NK::Business, "church");
    }

    // Quadrants between the main and cross paths: rows of plots in pairs, back to back, each pair on its own path.
    struct Quad
    {
        double xa, xb, za, zb;
    };
    const std::vector<Quad> quads = {{x0 + 300.0, -500.0, 300.0, zc - 350.0},
                                     {500.0, x1 - 300.0, 300.0, zc - 350.0},
                                     {x0 + 300.0, -500.0, zc + 350.0, D - 300.0},
                                     {500.0, x1 - 300.0, zc + 350.0, D - 300.0}};
    int graves = 0, mausoleums = 0, cooled = 0;
    for (size_t q = 0; q < quads.size(); ++q)
    {
        const Quad& Q = quads[q];
        if (Q.xb - Q.xa < pw || Q.zb - Q.za < pd)
            continue;
        double z = Q.za;
        for (int pair = 0;; ++pair)
        {
            // A path, then a row facing it and a row backing onto that one, facing the next path.
            const double pathZ = z + 125.0;
            if (pathZ + 125.0 + pd > Q.zb)
                break;
            const int pieces = k.along("paths/q" + std::to_string(q) + "/" + std::to_string(pair), "path.cemetery", {Q.xa, pathZ}, {Q.xb, pathZ}, 300.0);
            (void)pieces;
            for (int row = 0; row < 2; ++row)
            {
                const double rz0 = z + 250.0 + row * pd;
                if (rz0 + pd > Q.zb)
                    break;
                const P2 front = row == 0 ? P2(0.0, -1.0) : P2(0.0, 1.0);
                const int n = int((Q.xb - Q.xa) / (pw + 60.0));
                for (int i = 0; i < n; ++i)
                {
                    const std::string id = "plot/q" + std::to_string(q) + "/r" + std::to_string(pair * 2 + row) + "/p" + std::to_string(i);
                    const double cx = Q.xa + (i + 0.5) * (pw + 60.0), cz = rz0 + pd * 0.5;
                    // The chapel's ground and its forecourt stay clear.
                    if (chapel && std::abs(cx) < 1300.0 + pw && rz0 + pd > D - 2700.0)
                        continue;
                    const uint64_t key = lila::names::textKey(id);
                    if (k.rnd(key) < share)
                    {
                        // A family takes two plots now and then.
                        const bool wide = i + 1 < n && k.rnd(key, 2) < 0.3;
                        const double w = wide ? 2.0 * pw : pw - 20.0;
                        const double mx = wide ? cx + (pw + 60.0) * 0.5 : cx;
                        const double r = k.rnd(key, 3);
                        const std::string roof = r < 0.3 ? "dome" : r < 0.7 ? "flat" : "gable";
                        const int floors = k.rnd(key, 4) < 0.25 ? 2 : 1;
                        EnvelopeOut& e = k.envelope(id, rect(mx - w * 0.5, cz - pd * 0.5 + 20.0, mx + w * 0.5, cz + pd * 0.5 - 20.0), "mausoleum", "mausoleum",
                                                    "cemetery", floors, floors == 2 ? 520.0 : 320.0 + k.rnd(key, 5) * 120.0, roof);
                        e.front = front;
                        e.entrances = {{mx, cz + front.y * (pd * 0.5 - 20.0)}};
                        const bool ac = k.rnd(key, 6) < 0.35;
                        e.name = k.name("family:" + id, NK::Business, "mausoleum");
                        e.extra = {{"family", e.name}, {"domes", roof == "dome"}, {"columns", k.rnd(key, 7) < 0.5}, {"glassFront", k.rnd(key, 8) < 0.4},
                                   {"airConditioned", ac}};
                        if (ac)
                        {
                            k.slot(id + "/ac", "ac.unit", {mx, cz - front.y * (pd * 0.5 - 20.0)}, facing(-front), 120.0);
                            ++cooled;
                        }
                        ++mausoleums;
                        if (wide)
                            ++i;
                        continue;
                    }
                    const double g = k.rnd(key, 9);
                    k.slot(id, g < 0.3 ? "grave.plain" : g < 0.75 ? "grave.headstone" : "grave.cross", {cx, cz}, facing(-front));
                    ++graves;
                }
            }
            z += 250.0 + 2.0 * pd + 60.0;
        }
    }
    k.L.stats["graves"] = graves;
    k.L.stats["mausoleums"] = mausoleums;
    k.L.stats["airConditioned"] = cooled;
    k.ground("access", "terrainAddRoad",
             {{"kind", "road"}, {"profile", "lane"}, {"layer", "Sites"}, {"path", json::array({pt({x0 - 4000.0, -1200.0}), pt({x1 + 4000.0, -1200.0})})}});
}

void buildAirstrip(Kit& k)
{
    const Spec& s = k.spec;
    const double Lr = s.f("runwayLength"), Wr = s.f("runwayWidth");
    const double ex = Lr * 0.5;
    k.L.boundary = rect(-ex - 1000.0, -Wr * 0.5 - 3000.0, ex + 1000.0, Wr * 0.5 + 9000.0);

    k.ground("strip", "terrainAddAirstrip", {{"a", pt({-ex, 0.0})}, {"b", pt({ex, 0.0})}, {"aircraft", s.s("aircraft")}, {"width", Wr + 1200.0}, {"pad", true}});
    k.ground("runway", "terrainAddRoad",
             {{"kind", "runway"},
              {"profile", "runway"},
              {"layer", "Sites"},
              {"path", json::array({pt({-ex, 0.0}), pt({ex, 0.0})})},
              {"smooth", false},
              {"lanes", 1},
              {"laneWidth", Wr},
              {"shoulder", 0.0},
              {"verge", 200.0},
              {"kerbHeight", 0.0},
              {"surface", "dirt"}});
    const Ring apron = rect(ex - 9500.0, Wr * 0.5 + 1500.0, ex - 1000.0, Wr * 0.5 + 7500.0);
    k.ground("apron", "terrainStampPad", {{"footprint", pts(apron)}, {"mode", "flatten"}, {"falloff", 1200.0}, {"masks", {"cityFootprint"}}});
    {
        Part& r = k.part("runway", "runway", rect(-ex, -Wr * 0.5, ex, Wr * 0.5), Part::Needs::Land);
        r.maxRelief = 400.0;
        r.groundOp = "strip";
        Part& a = k.part("apron", "yard", apron, Part::Needs::Land);
        a.maxRelief = 250.0;
        a.groundOp = "apron";
    }

    // Markings: threshold bars across both ends, edge markers down both sides.
    for (int end : {-1, 1})
    {
        SlotOut& t = k.slot(std::string("marking/threshold/") + (end < 0 ? "w" : "e"), "runway.threshold", {end * (ex - 400.0), 0.0}, alongDir({0.0, 1.0}));
        t.scale = {Wr * 0.8 / 1200.0, 1.0, 1.0};
    }
    for (int i = 0; -ex + i * 6000.0 <= ex + 1.0; ++i)
        for (int side : {-1, 1})
            k.slot("marking/edge/" + std::string(side < 0 ? "s" : "n") + std::to_string(i), "runway.edge", {-ex + i * 6000.0, side * (Wr * 0.5 + 100.0)});
    if (s.b("windsock"))
        k.slot("windsock", "windsock", {0.0, Wr * 0.5 + 1500.0});

    // Apron: hangars facing the runway, fuel drums beside the first, a shack by the track.
    const int hangars = s.i("hangars");
    int placed = 0;
    for (int h = 0; h < hangars; ++h)
    {
        const double hx0 = ex - 9000.0 + h * 2700.0;
        if (hx0 + 2200.0 > ex - 1200.0)
        {
            k.L.notes.push_back("only " + std::to_string(placed) + " of " + std::to_string(hangars) + " hangars fit on the apron");
            break;
        }
        const double hz0 = Wr * 0.5 + 4300.0;
        EnvelopeOut& e = k.envelope("hangar/" + std::to_string(h + 1), rect(hx0, hz0, hx0 + 2200.0, hz0 + 2000.0), "hangar", "hangar", "industrial", 1, 650.0, "gable");
        e.front = {0.0, -1.0};
        e.entrances = {{hx0 + 1100.0, hz0}};
        k.slot("hangar/" + std::to_string(h + 1) + "/door", "hangar.door", {hx0 + 1100.0, hz0}, facing({0.0, -1.0}));
        ++placed;
    }
    const int drums = s.i("fuelDrums");
    for (int d = 0; d < drums; ++d)
        k.slot("fuel/" + std::to_string(d), "drum.fuel", {ex - 6200.0 + (d % 4) * 80.0, Wr * 0.5 + 3300.0 + (d / 4) * 80.0});
    if (s.b("accessTrack"))
    {
        k.ground("track", "terrainAddRoad",
                 {{"kind", "track"}, {"layer", "Sites"}, {"path", json::array({pt({ex - 5000.0, Wr * 0.5 + 7400.0}), pt({ex - 5000.0, Wr * 0.5 + 30000.0})})}});
        EnvelopeOut& sh = k.envelope("shack", rect(ex - 3800.0, Wr * 0.5 + 6600.0, ex - 3200.0, Wr * 0.5 + 7100.0), "ruralShack", "shed", "industrial", 1, 260.0, "flat");
        sh.front = {-1.0, 0.0};
        sh.entrances = {{ex - 3800.0, Wr * 0.5 + 6850.0}};
    }
}

void buildFarm(Kit& k)
{
    const Spec& s = k.spec;
    const double W = s.f("farmWidth"), D = s.f("farmDepth"), fw = s.f("fieldWidth"), rs = s.f("rowSpacing");
    const double x0 = -W * 0.5, x1 = W * 0.5, hd = D * 0.5;
    k.L.boundary = rect(x0, -hd, x1, hd);
    const std::vector<std::string> crops = splitList(s.s("crops"));
    if (crops.empty())
        throw std::runtime_error("crops lists no crop");

    k.ground("road", "terrainAddRoad", {{"kind", "track"}, {"layer", "Sites"}, {"path", json::array({pt({x0 - 10000.0, 0.0}), pt({x1, 0.0})})}});

    // Farmstead at the road's near end: packing plant and cold storage on one side, trucking yard on the other.
    const double fsW = std::min(16000.0, W * 0.3);
    const double houseDepth = std::min(8000.0, hd - 800.0);
    k.ground("farmstead", "terrainStampPad",
             {{"footprint", pts(rect(x0, 800.0, x0 + fsW, 800.0 + houseDepth))}, {"mode", "flatten"}, {"falloff", 1500.0}, {"masks", {"cityFootprint"}}});
    {
        Part& f = k.part("farmstead", "yard", rect(x0, 800.0, x0 + fsW, 800.0 + houseDepth), Part::Needs::Land);
        f.maxRelief = 250.0;
        f.groundOp = "farmstead";
    }
    if (s.b("packingPlant"))
    {
        const double w = std::min(6500.0, fsW * 0.5), d = std::min(4000.0, houseDepth - 1500.0);
        EnvelopeOut& e = k.envelope("packingPlant", rect(x0 + 1000.0, 1500.0, x0 + 1000.0 + w, 1500.0 + d), "packingPlant", "packingPlant", "industrial", 1, 900.0, "gable");
        e.front = {0.0, -1.0};
        e.entrances = {{x0 + 1000.0 + w * 0.3, 1500.0}, {x0 + 1000.0 + w * 0.7, 1500.0}};
        e.name = k.name("packingPlant", NK::Business, "packingPlant");
    }
    if (s.b("coldStorage"))
    {
        const double cx0 = x0 + 1000.0 + std::min(6500.0, fsW * 0.5) + 800.0;
        const double w = std::min(3500.0, x0 + fsW - 800.0 - cx0), d = std::min(2800.0, houseDepth - 1500.0);
        if (w > 1000.0)
        {
            EnvelopeOut& e = k.envelope("coldStorage", rect(cx0, 1500.0, cx0 + w, 1500.0 + d), "warehouse", "coldStorage", "industrial", 1, 800.0, "flat");
            e.front = {0.0, -1.0};
            e.entrances = {{cx0 + w * 0.5, 1500.0}};
            e.name = k.name("coldStorage", NK::Business, "coldStorage");
        }
        else
            k.L.notes.push_back("no room for the cold storage beside the packing plant: widen the farm");
    }
    if (s.b("truckYard"))
    {
        const Ring yard = rect(x0 + 1000.0, -800.0 - std::min(7000.0, hd - 1500.0), x0 + fsW - 1000.0, -800.0);
        k.ground("truckYard", "terrainStampPad", {{"footprint", pts(yard)}, {"mode", "flatten"}, {"falloff", 1000.0}, {"masks", {"paved", "cityFootprint"}}});
        Part& p = k.part("truckYard", "yard", yard, Part::Needs::Land);
        p.maxRelief = 200.0;
        p.groundOp = "truckYard";
        k.name("truckYard", NK::Business, "truckYard");
        const int trailers = s.i("trailers");
        const double zMid = (yard[0].y + yard[2].y) * 0.5;
        int put = 0;
        for (int t = 0; t < trailers; ++t)
        {
            const double x = x0 + 1500.0 + t * 400.0;
            if (x > x0 + fsW - 1500.0)
            {
                k.L.notes.push_back("only " + std::to_string(put) + " of " + std::to_string(trailers) + " trailers fit in the trucking yard");
                break;
            }
            k.slot("truckYard/trailer/" + std::to_string(t), "truck.trailer", {x, zMid}, t % 2 ? 0.0 : kPi);
            if (t % 4 == 0)
                k.slot("truckYard/tractor/" + std::to_string(t), "truck.tractor", {x, zMid - (t % 2 ? -1.0 : 1.0) * 1050.0}, t % 2 ? 0.0 : kPi);
            ++put;
        }
    }

    // Field strips both sides of the road, an irrigation canal along the back of each side.
    const double xf0 = x0 + fsW + 1000.0, back = hd - 1200.0;
    const int strips = std::max(0, int((x1 - xf0) / fw));
    if (strips == 0)
        k.L.notes.push_back("no room for fields past the farmstead: widen the farm");
    const double gh = s.f("greenhouses"), sh = s.f("shadeHouses");
    const bool canal = s.b("canal");
    int fields = 0, rowsMade = 0, houses = 0;
    for (int side = 0; side < 2; ++side)
    {
        const double sd = side == 0 ? 1.0 : -1.0;
        const std::string sn = side == 0 ? "n" : "s";
        if (canal && strips > 0)
        {
            k.ground("canal/" + sn, "terrainAddFeature",
                     {{"kind", "canal"}, {"layer", "Sites"}, {"path", json::array({pt({xf0 - 600.0, sd * (hd - 600.0)}), pt({x1, sd * (hd - 600.0)})})}, {"width", 300.0},
                      {"depth", 150.0}});
            k.slot("canal/" + sn + "/pump", "irrigation.pump", {xf0 - 900.0, sd * (hd - 600.0)}, facing({1.0, 0.0}));
        }
        for (int i = 0; i < strips; ++i)
        {
            const double fx0 = xf0 + i * fw + 100.0, fx1 = xf0 + (i + 1) * fw - 100.0;
            const double za = sd * 800.0, zb = sd * back;
            const Ring ring = rect(fx0, std::min(za, zb), fx1, std::max(za, zb));
            const std::string id = "field/" + sn + std::to_string(i);
            const double u = k.rnd(lila::names::textKey(id));
            const char* what = u < gh ? "greenhouse" : u < gh + sh ? "shadehouse" : "open";
            ++fields;
            if (canal)
                k.slot(id + "/gate", "irrigation.gate", {(fx0 + fx1) * 0.5, sd * (hd - 1000.0)}, facing({0.0, -sd}));
            if (std::string_view(what) == "open")
            {
                const std::string crop = crops[size_t(i * 2 + side) % crops.size()];
                Part& p = k.part(id, "field", ring, Part::Needs::Land);
                p.maxSlope = 8.0;
                const int nRows = std::max(1, int((fx1 - fx0) / rs));
                const size_t first = k.L.slots.size();
                for (int r = 0; r < nRows; ++r)
                {
                    const double x = fx0 + (r + 0.5) * (fx1 - fx0) / nRows;
                    rowsMade += k.along(id + "/r" + std::to_string(r), "crop.row", {x, za}, {x, zb}, 2500.0);
                }
                for (size_t n = first; n < k.L.slots.size(); ++n)
                {
                    k.L.slots[n].tags = {"crop:" + crop};
                    k.L.slots[n].data = {{"crop", crop}};
                }
                continue;
            }
            // Greenhouses and shade houses stand on a graded pad, spans along the strip.
            k.ground(id, "terrainStampPad", {{"footprint", pts(ring)}, {"mode", "flatten"}, {"falloff", 800.0}, {"masks", {"greenhouse"}}});
            Part& p = k.part(id, "field", ring, Part::Needs::Land);
            p.maxRelief = 150.0;
            p.groundOp = id;
            const std::string type = std::string(what) + ".frame";
            const int nx = std::max(1, int((fx1 - fx0) / 960.0)), nz = std::max(1, int(std::abs(zb - za) / 2500.0));
            const double sx = (fx1 - fx0) / nx, sz = std::abs(zb - za) / nz;
            for (int a = 0; a < nx; ++a)
                for (int b = 0; b < nz; ++b)
                {
                    SlotOut& f = k.slot(id + "/s" + std::to_string(a) + "/b" + std::to_string(b), type,
                                        {fx0 + (a + 0.5) * sx, std::min(za, zb) + (b + 0.5) * sz}, 0.0);
                    f.scale = {sx / 960.0, 1.0, sz / 2500.0};
                }
            ++houses;
        }
    }
    k.L.stats["fields"] = fields;
    k.L.stats["cropRows"] = rowsMade;
    k.L.stats["coveredFields"] = houses;
}

void buildVillage(Kit& k)
{
    const Spec& s = k.spec;
    const double R = s.f("villageRadius");
    const int nS = s.i("streets"), nH = s.i("houses");
    k.L.boundary = circle({0.0, 0.0}, R + 2000.0, 24);
    const bool plaza = s.b("plaza"), church = s.b("church");

    if (plaza)
    {
        const Ring sq = rect(-1200.0, -1200.0, 1200.0, 1200.0);
        k.ground("plaza", "terrainStampRegion", {{"footprint", pts(sq)}, {"mode", "flatten"}, {"falloff", 800.0}, {"masks", {"plaza", "cityFootprint"}}});
        Part& p = k.part("plaza", "plaza", sq, Part::Needs::Land);
        p.maxRelief = 150.0;
        p.groundOp = "plaza";
        k.slot("plaza/well", "furniture.well", {0.0, 0.0});
        for (int c = 0; c < 4; ++c)
        {
            const P2 corner(c % 2 ? 900.0 : -900.0, c / 2 ? 900.0 : -900.0);
            k.slot("plaza/tree/" + std::to_string(c), "tree.broadleaf", corner);
            k.slot("plaza/bench/" + std::to_string(c), "furniture.bench", corner * 0.6, facing(-corner));
            k.slot("plaza/lamp/" + std::to_string(c), "lamp.street", P2(corner.x, -corner.y) * 1.2, facing(P2(-corner.x, corner.y)));
        }
    }
    std::vector<Ring> taken;
    if (church)
    {
        const Ring c = rect(-700.0, 1350.0, 700.0, 3950.0);
        k.ground("church", "terrainStampPad", {{"footprint", pts(c)}, {"mode", "flatten"}, {"falloff", 600.0}, {"masks", {"cityFootprint"}}});
        EnvelopeOut& e = k.envelope("church", c, "church", "church", "religious", 1, 900.0, "gable");
        e.front = {0.0, -1.0};
        e.entrances = {{0.0, 1350.0}};
        e.extra = {{"tower", true}, {"towerHeight", 1800.0}};
        e.name = k.name("church", NK::Business, "church");
        taken.push_back(rect(-900.0, 1150.0, 900.0, 4150.0));
    }

    // Dirt streets out from the plaza with a bend each, away from the church front.
    struct Street
    {
        P2 a, m, b;
    };
    std::vector<Street> streets;
    for (int t = 0; t < nS; ++t)
    {
        double ang = -kPi * 0.5 + 2.0 * kPi * (t + 0.5) / nS + (k.rnd(11, t) - 0.5) * 0.4;
        if (church && std::abs(std::remainder(ang - kPi * 0.5, 2.0 * kPi)) < 0.45)
            ang += 0.6;
        const P2 dir(std::cos(ang), std::sin(ang)), side(-dir.y, dir.x);
        const P2 a = dir * 1300.0, b = dir * R, m = (a + b) * 0.5 + side * (k.rnd(12, t) - 0.5) * 0.3 * R;
        streets.push_back({a, m, b});
        k.ground("street/" + std::to_string(t), "terrainAddRoad",
                 {{"kind", "track"}, {"profile", "track"}, {"layer", "Sites"}, {"surface", "dirt"}, {"path", json::array({pt(a), pt(m), pt(b)})}});
        k.street("street:" + std::to_string(t + 1), 2);
    }

    // Houses along the streets on both sides, fronts to the street, each on its own pad, clear of every street's track
    // (a house set off one street's straight can still reach across its bend or a neighbouring street near the plaza).
    constexpr double kTrackClear = 450.0;
    // The bent polyline, a smooth curve through it and the straight chord: Terrain's road design may lay any of them
    // (a track's curve radius can pull the bend out entirely).
    auto trackSegments = [](P2 a, P2 m, P2 b) {
        std::vector<std::pair<P2, P2>> out = {{a, m}, {m, b}, {a, b}};
        const P2 ctrl[5] = {a, a, m, b, b};
        P2 last = a;
        for (int seg = 0; seg < 2; ++seg)
            for (int i = 1; i <= 8; ++i)
            {
                const double t = i / 8.0, t2 = t * t, t3 = t2 * t;
                const P2 &p0 = ctrl[seg], &p1 = ctrl[seg + 1], &p2 = ctrl[seg + 2], &p3 = ctrl[seg + 3];
                const P2 q = 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
                out.push_back({last, q});
                last = q;
            }
        return out;
    };
    auto onStreet = [&](const Ring& pad) {
        auto dist = [](P2 p, P2 a, P2 b) {
            const P2 ab = b - a;
            const double t = std::clamp(glm::dot(p - a, ab) / std::max(1e-9, glm::dot(ab, ab)), 0.0, 1.0);
            return glm::length(p - (a + ab * t));
        };
        for (const Street& st : streets)
            for (const auto& [a, b] : trackSegments(st.a, st.m, st.b))
            {
                for (size_t i = 0; i < pad.size(); ++i)
                    if (dist(pad[i], a, b) < kTrackClear || dist(a, pad[i], pad[(i + 1) % pad.size()]) < kTrackClear ||
                        dist(b, pad[i], pad[(i + 1) % pad.size()]) < kTrackClear)
                        return true;
                for (int k2 = 1; k2 < 8; ++k2)
                    if (insideRing(pad, a + (b - a) * (k2 / 8.0)))
                        return true;
            }
        return false;
    };
    int placed = 0, skipped = 0;
    for (int h = 0; h < nH; ++h)
    {
        const Street& st = streets[size_t(h) % streets.size()];
        const int along = h / int(streets.size());
        const int perStreet = (nH + int(streets.size()) - 1) / int(streets.size());
        const double side = (along % 2) ? -1.0 : 1.0;
        const std::string id = "house/" + std::to_string(h);
        const uint64_t key = lila::names::textKey(id);
        const double w = 700.0 + k.rnd(key, 1) * 300.0, d = 600.0 + k.rnd(key, 2) * 300.0;
        const double setback = 450.0 + d * 0.5 + k.rnd(key, 3) * 200.0;
        bool ok = false;
        // A spot beside street st at t along it, on a side, set back further by extra; placed when it is free.
        auto tryAt = [&](const Street& on, double t, double sd, double extra) {
            // On the bent street: first half a to m, second half m to b.
            const P2 p = t < 0.5 ? on.a + (on.m - on.a) * (t * 2.0) : on.m + (on.b - on.m) * ((t - 0.5) * 2.0);
            const P2 dir = glm::normalize(t < 0.5 ? on.m - on.a : on.b - on.m), n(-dir.y, dir.x);
            const P2 c = p + n * sd * (setback + extra);
            const double heading = facing(-n * sd);
            const Ring fp = orientedRect(c, w * 0.5, d * 0.5, heading);
            const Ring pad = orientedRect(c, w * 0.5 + 150.0, d * 0.5 + 150.0, heading);
            if (glm::length(c) < 1700.0 || glm::length(c) > R + 1500.0)
                return false;
            bool clash = onStreet(fp);
            for (const Ring& o : taken)
                clash = clash || ringsOverlap(o, pad);
            if (clash)
                return false;
            taken.push_back(pad);
            const bool shack = k.rnd(key, 4) < 0.15;
            EnvelopeOut& e = k.envelope(id, fp, shack ? "ruralShack" : "adobe", "house", "residential", k.rnd(key, 5) < 0.1 ? 2 : 1,
                                        280.0 + k.rnd(key, 6) * 40.0, k.rnd(key, 7) < 0.5 ? "flat" : "gable");
            e.front = -n * sd;
            e.entrances = {c - n * sd * (d * 0.5)};
            if (e.floors == 2)
                e.height = 560.0;
            k.ground(id, "terrainStampPad", {{"footprint", pts(pad)}, {"mode", "flatten"}, {"falloff", 300.0}, {"masks", {"cityFootprint"}}});
            Part& pt2 = k.part(id, "building", pad, Part::Needs::Land);
            pt2.maxRelief = 120.0;
            pt2.groundOp = id;
            return true;
        };
        for (int attempt = 0; attempt < 4 && !ok; ++attempt)
            ok = tryAt(st, std::fmod(0.12 + 0.85 * (along / 2 + 0.5 + attempt * 0.23) / std::max(1, (perStreet + 1) / 2), 0.97), side, 0.0);
        // Its own street's spots taken or on a track: along every street, both sides, set further back, nearest the plaza first.
        for (size_t o = 0; o < streets.size() && !ok; ++o)
            for (double extra : {0.0, 400.0, 900.0, 1500.0})
                for (int step = 0; step <= 24 && !ok; ++step)
                    for (double sd : {side, -side})
                        if (!ok)
                            ok = tryAt(streets[(size_t(h) + o) % streets.size()], 0.1 + 0.86 * step / 24.0, sd, extra);
        if (ok)
            ++placed;
        skipped += !ok;
    }
    if (skipped > 0)
        k.L.notes.push_back(std::to_string(skipped) + " of " + std::to_string(nH) + " houses found no free spot along the streets: add streets or widen the radius");
    k.L.stats["houses"] = placed;
    {
        Part& g = k.part("village", "plot", circle({0.0, 0.0}, R, 24), Part::Needs::Land);
        g.maxSlope = 35.0;
    }
}
} // namespace openworld::sites
