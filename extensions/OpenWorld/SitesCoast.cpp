// Site kits on the water: cargo port, cruise terminal, lighthouse. The quay runs along x at z = 0 with the water at -z.

#include "SitesKit.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openworld::sites
{
namespace
{

/** "Naviera La Gaviota" -> "navieraLaGaviota": a type-id safe word from a name. */
std::string slugOf(const std::string& name)
{
    const std::string n = lila::names::normalize(name);
    std::string out;
    bool up = false;
    for (char c : n)
    {
        if (c == ' ')
        {
            up = !out.empty();
            continue;
        }
        if (!std::isalnum(static_cast<unsigned char>(c)))
            continue;
        out += up ? char(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    return out.empty() ? std::string("line") : out;
}

std::string hexColour(double h, double s, double v)
{
    const double c = v * s, x = c * (1.0 - std::abs(std::fmod(h * 6.0, 2.0) - 1.0)), m = v - c;
    double r = 0, g = 0, b = 0;
    switch (int(h * 6.0) % 6)
    {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", int((r + m) * 255), int((g + m) * 255), int((b + m) * 255));
    return buf;
}

json heightFromWater(json flags, double above)
{
    flags["heightFromWater"] = above;
    return flags;
}
} // namespace

/** Yards sit at a fixed height above the water: their skirt widens to stay a slope the town's streets can meet, percent. */
constexpr double kYardSkirtGrade = 8.0;

void buildPort(Kit& k)
{
    const Spec& s = k.spec;
    const double L = s.f("quayLength"), D = s.f("yardDepth"), dredge = s.f("dredgeDepth");
    const double x0 = -L * 0.5, x1 = L * 0.5;
    k.L.boundary = rect(x0, -4500.0, x1, D);

    k.ground("yard", "terrainStampPad",
             heightFromWater({{"footprint", pts(rect(x0, 0.0, x1, D))}, {"mode", "flatten"}, {"falloff", 3000.0}, {"skirtGrade", kYardSkirtGrade},
                              {"masks", {"paved", "port", "cityFootprint"}}},
                             250.0));
    k.ground("berths", "terrainStampRegion",
             heightFromWater({{"footprint", pts(rect(x0, -4500.0, x1, -800.0))}, {"mode", "carve"}, {"falloff", 300.0}, {"masks", {"port"}}}, -dredge));
    // After the berths: their carve takes in whole cells and would reach under the yard's front edge.
    k.ground("quay", "terrainStampQuay",
             heightFromWater({{"path", json::array({pt({x0, 0.0}), pt({x1, 0.0})})}, {"kind", "quay"}, {"side", "left"}, {"depth", dredge}, {"width", 600.0}}, 250.0));
    {
        // Judged a cell or two back from the quay face, which the grid draws as a slope.
        Part& yard = k.part("yard", "yard", rect(x0, 1500.0, x1, D), Part::Needs::Land);
        yard.maxRelief = 300.0;
        yard.groundOp = "yard";
        Part& berth = k.part("berth", "berth", rect(x0, -4000.0, x1, -600.0), Part::Needs::Water);
        berth.minDepth = dredge * 0.9;
        berth.groundOp = "berths";
    }

    // Liveries: fictional shipping lines, one container subtype each.
    const int nLiveries = s.i("liveries");
    std::vector<std::pair<std::string, std::string>> liveries; // (name, slug)
    std::vector<std::string> liveryColours;
    for (int li = 1; li <= nLiveries; ++li)
    {
        const std::string n = k.name("livery:" + std::to_string(li), NK::Business, "shipping");
        std::string slug = slugOf(n);
        for (const auto& [_, other] : liveries)
            if (other == slug)
                slug += std::to_string(li);
        liveries.push_back({n, slug});
        liveryColours.push_back(hexColour(k.rnd(77, li), 0.75, 0.8));
        k.L.liveries.push_back({{"id", li}, {"name", n}, {"slug", slug}, {"colour", liveryColours.back()},
                                {"types", {"container.20ft." + slug, "container.40ft." + slug, "container.reefer." + slug}}});
    }
    auto livery = [&](uint64_t a, uint64_t b, uint64_t c) { return size_t(k.rnd(a, b, c) * double(liveries.size())) % liveries.size(); };

    // Quay edge, fenders, bollards and the crane rails.
    k.along("quay/edge", "quay.edge", {x0, 60.0}, {x1, 60.0}, 500.0);
    for (int i = 0; x0 + 750.0 + i * 1500.0 < x1; ++i)
        k.slot("quay/fender/" + std::to_string(i), "quay.fender", {x0 + 750.0 + i * 1500.0, 15.0}, kPi);
    for (int i = 0; x0 + 1250.0 + i * 2500.0 < x1; ++i)
        k.slot("quay/bollard/" + std::to_string(i), "furniture.bollard", {x0 + 1250.0 + i * 2500.0, 130.0});
    k.along("quay/rail/sea", "rail.track", {x0, 900.0}, {x1, 900.0}, 1000.0);
    k.along("quay/rail/land", "rail.track", {x0, 3500.0}, {x1, 3500.0}, 1000.0);

    // Berths: ship-to-shore cranes and the moored ships.
    const int berths = s.i("berths"), cranes = s.i("stsCranes"), ships = s.i("ships");
    const double berthLen = L / berths;
    for (int b = 0; b < berths; ++b)
    {
        const double cx = x0 + (b + 0.5) * berthLen;
        const std::string bp = "berth" + std::to_string(b + 1);
        for (int j = 0; j < cranes; ++j)
        {
            const double x = cx + (j - (cranes - 1) * 0.5) * std::min(3800.0, berthLen / std::max(1, cranes));
            k.slot(bp + "/sts/" + std::to_string(j), "crane.sts", {x, 2200.0}, kPi).data = {{"berth", b + 1}};
        }
        if (b < ships)
        {
            SlotOut& ship = k.slot(bp + "/ship", "ship.cargo", {cx, -2400.0}, kPi * 0.5, 0.0, Level::Water);
            ship.scale = {1.0, 1.0, std::min(1.0, berthLen * 0.85 / 25000.0)};
            ship.data = {{"berth", b + 1}, {"line", liveries[size_t(b) % liveries.size()].first}};
        }
    }

    // Container yard: blocks of bays (along x) by rows (along z), stacked up to tiers, thinned by fill.
    double back = std::min(10000.0, std::max(4000.0, D * 0.32));
    const double zoneZ0 = 7000.0, zoneZ1 = D - back;
    const int rows = s.i("blockRows"), tiers = s.i("tiers"), rtg = s.i("rtgCranes");
    const double fill = s.f("fill");
    constexpr double bay = 1280.0, rowPitch = 284.0, lane = 450.0, aisleX = 2500.0, aisleZ = 2200.0;
    constexpr int baysPerBlock = 10;
    const double blockLen = baysPerBlock * bay, blockDepth = rows * rowPitch + lane;
    const int nx = std::max(0, int((L - 4000.0 + aisleX) / (blockLen + aisleX)));
    const int nz = std::max(0, int((zoneZ1 - zoneZ0 + aisleZ) / (blockDepth + aisleZ)));
    if (nx == 0 || nz == 0)
        k.L.notes.push_back("the yard has no room for a container block: lengthen the quay or deepen the yard");
    const double spanX = nx * blockLen + (nx - 1) * aisleX;
    int reefers = s.i("reeferRacks"), containers = 0, stacks = 0;
    for (int bj = 0; bj < nz; ++bj)
        for (int bi = 0; bi < nx; ++bi)
        {
            const double bx0 = -spanX * 0.5 + bi * (blockLen + aisleX), bz0 = zoneZ0 + bj * (blockDepth + aisleZ);
            const std::string bp = "yard/b" + std::to_string(bi) + "-" + std::to_string(bj);
            // Reefer blocks sit in the yard's back row, nearest the warehouses' power.
            const bool reefer = bj == nz - 1 && bi < reefers;
            for (int r = 0; r < rows; ++r)
            {
                const double z = bz0 + lane + (r + 0.5) * rowPitch;
                for (int i = 0; i < baysPerBlock; ++i)
                {
                    const double x = bx0 + (i + 0.5) * bay;
                    const double u = k.rnd(uint64_t(bi * 1000 + bj), uint64_t(i), uint64_t(r));
                    const int h = std::clamp(int(u * 2.0 * fill * tiers + 0.5), 0, tiers);
                    stacks += h > 0;
                    const bool twenty = !reefer && k.rnd(5, uint64_t(bi * 1000 + bj), uint64_t(i * 16 + r)) < 0.18;
                    for (int t = 0; t < h; ++t)
                    {
                        const std::string cp = bp + "/i" + std::to_string(i) + "/r" + std::to_string(r) + "/t" + std::to_string(t);
                        auto put = [&](const std::string& path, const std::string& base, double cx, uint64_t salt) {
                            const size_t li = livery(uint64_t(bi * 1000 + bj) * 64 + uint64_t(i), uint64_t(r * 16 + t), salt);
                            SlotOut& c = k.slot(path, base + "." + liveries[li].second, {cx, z}, kPi * 0.5, t * 259.0);
                            c.tags = {"livery"};
                            // "color" is what the viewport and Unreal tint each container with.
                            c.data = {{"livery", liveries[li].first}, {"liveryId", int(li) + 1}, {"color", liveryColours[li]}};
                            ++containers;
                        };
                        if (twenty)
                        {
                            put(cp + "a", "container.20ft", x - 312.0, 1);
                            put(cp + "b", "container.20ft", x + 312.0, 2);
                        }
                        else
                            put(cp, reefer ? "container.reefer" : "container.40ft", x, 0);
                    }
                }
                if (reefer && r + 1 < rows)
                    k.along(bp + "/rack/r" + std::to_string(r), "reefer.rack", {bx0, z + rowPitch * 0.5}, {bx0 + blockLen, z + rowPitch * 0.5}, 1300.0);
            }
            for (int j = 0; j < rtg; ++j)
            {
                SlotOut& g = k.slot(bp + "/rtg/" + std::to_string(j), "crane.rtg", {bx0 + blockLen * (j + 1) / (rtg + 1), bz0 + blockDepth * 0.5}, 0.0);
                g.scale = {1.0, std::max(1.0, (tiers + 1) * 259.0 / 2100.0), blockDepth / 2600.0};
            }
            k.slot("yard/mast/" + std::to_string(bi) + "-" + std::to_string(bj), "light.mast", {bx0 - aisleX * 0.5, bz0 - aisleZ * 0.5});
        }
    k.L.stats["containers"] = containers;
    k.L.stats["stacks"] = stacks;
    k.L.stats["blocks"] = nx * nz;

    // Back of the yard: warehouses, the rail spur behind them, the customs gate on the access road.
    const double gateW = 4 * 450.0 + 3 * 250.0;
    const int warehouses = s.i("warehouses");
    const double wz0 = D - back + 1200.0, wDepth = std::min(4500.0, back * 0.45);
    int placed = 0;
    for (int w = 0; w < warehouses; ++w)
    {
        // Left of the gate first, then right of it.
        const double slotW = 10500.0;
        const int leftSlots = std::max(0, int((-gateW * 0.5 - 4500.0 - (x0 + 2000.0)) / slotW));
        double wx0 = w < leftSlots ? x0 + 2000.0 + w * slotW : gateW * 0.5 + 4500.0 + (w - leftSlots) * slotW;
        if (wx0 + 9000.0 > x1 - 2000.0)
        {
            k.L.notes.push_back("only " + std::to_string(placed) + " of " + std::to_string(warehouses) + " warehouses fit along the back of the yard");
            break;
        }
        const std::string role = "warehouse:" + std::to_string(w + 1);
        EnvelopeOut& e = k.envelope("warehouse/" + std::to_string(w + 1), rect(wx0, wz0, wx0 + 9000.0, wz0 + wDepth), w % 2 ? "warehouse" : "portShed",
                                    "warehouse", "port", 1, 1100.0, "gable");
        e.front = {0.0, -1.0};
        e.entrances = {{wx0 + 2250.0, wz0}, {wx0 + 6750.0, wz0}};
        e.name = k.name(role, NK::Business, "warehouse");
        ++placed;
    }
    const double railZ = wz0 + wDepth + 1300.0;
    const bool rail = s.b("railSpur") && railZ < D - 2200.0;
    if (s.b("railSpur") && !rail)
        k.L.notes.push_back("no room for the rail spur behind the warehouses: deepen the yard");
    const double railEnd = -gateW * 0.5 - 3000.0;
    if (rail)
    {
        k.ground("rail", "terrainAddRoad",
                 {{"kind", "rail"}, {"layer", "Sites"}, {"path", json::array({pt({x0 - 15000.0, railZ}), pt({x0, railZ}), pt({railEnd, railZ})})}, {"smooth", false}});
        k.along("rail/track", "rail.track", {x0, railZ}, {railEnd, railZ}, 1000.0);
        k.slot("rail/bumper", "rail.bumper", {railEnd + 150.0, railZ}, facing({-1.0, 0.0}));
    }
    if (s.b("customsGate"))
    {
        EnvelopeOut& c = k.envelope("customs", rect(gateW * 0.5 + 900.0, D - 3200.0, gateW * 0.5 + 3700.0, D - 600.0), "civic", "customs", "civic", 1, 450.0, "flat");
        c.front = {-1.0, 0.0};
        c.entrances = {{gateW * 0.5 + 900.0, D - 1900.0}};
        c.name = k.name("customs", NK::Business, "customs");
        for (int l = 0; l < 4; ++l)
        {
            const double lx = -gateW * 0.5 + 225.0 + l * 700.0;
            k.slot("gate/boom/" + std::to_string(l), "gate.boom", {lx, D - 1500.0}, 0.0);
            if (l < 3)
            {
                EnvelopeOut& b = k.envelope("gate/booth/" + std::to_string(l + 1), rect(lx + 270.0, D - 1800.0, lx + 430.0, D - 1300.0), "civic", "gateBooth",
                                            "civic", 1, 280.0, "flat");
                b.front = {1.0, 0.0};
            }
        }
    }
    k.slot("gate/main/a", "gate.vehicle", {-gateW * 0.25, D}, 0.0).scale = {gateW * 0.5 / 600.0, 1.0, 1.0};
    k.slot("gate/main/b", "gate.vehicle", {gateW * 0.25, D}, 0.0).scale = {gateW * 0.5 / 600.0, 1.0, 1.0};
    k.ground("access", "terrainAddRoad",
             {{"kind", "road"}, {"profile", "rural"}, {"layer", "Sites"}, {"path", json::array({pt({0.0, D - 400.0}), pt({0.0, D + 20000.0})})}});

    // Fence round the land side, open to the quay, with gaps for the gate and the rail.
    std::vector<std::pair<double, double>> gaps = {{D + L * 0.5 - gateW * 0.5, D + L * 0.5 + gateW * 0.5}};
    if (rail)
        gaps.push_back({railZ - 400.0, railZ + 400.0});
    k.alongRing("fence", "fence.chainlink", {{x0, 0.0}, {x0, D}, {x1, D}, {x1, 0.0}}, false, 300.0, gaps);
}

void buildCruise(Kit& k)
{
    const Spec& s = k.spec;
    const double Lp = s.f("pierLength"), Wp = s.f("pierWidth"), Ls = s.f("shoreLength");
    const double half = std::max(Ls * 0.5, Wp * 0.5 + 6000.0);
    k.L.boundary = rect(-half, -Lp - 1000.0, half, 12000.0);

    k.ground("quay", "terrainStampQuay",
             heightFromWater({{"path", json::array({pt({-Ls * 0.5, 0.0}), pt({Ls * 0.5, 0.0})})}, {"kind", "quay"}, {"side", "left"}, {"depth", 1000.0}, {"width", 500.0}}, 200.0));
    k.ground("plaza", "terrainStampRegion",
             heightFromWater({{"footprint", pts(rect(-Ls * 0.5, 0.0, Ls * 0.5, 11000.0))}, {"mode", "flatten"}, {"falloff", 1500.0}, {"skirtGrade", kYardSkirtGrade},
                              {"masks", {"paved", "cityFootprint"}}},
                             220.0));
    k.ground("berths", "terrainStampRegion",
             heightFromWater({{"footprint", pts(rect(-Wp * 0.5 - 5500.0, -Lp - 500.0, Wp * 0.5 + 5500.0, -1200.0))}, {"mode", "carve"}, {"falloff", 300.0}, {"masks", {"port"}}},
                             -1100.0));
    {
        Part& plaza = k.part("plaza", "plaza", rect(-Ls * 0.5, 1500.0, Ls * 0.5, 11000.0), Part::Needs::Land);
        plaza.maxRelief = 200.0;
        plaza.groundOp = "plaza";
        Part& pier = k.part("pier", "pier", rect(-Wp * 0.5, -Lp, Wp * 0.5, -Lp * 0.5), Part::Needs::Water);
        pier.minDepth = 100.0;
    }

    // The pier: deck pieces on piles, bollards both sides, lamps down the middle.
    const int deck = k.along("pier/deck", "dock.pier", {0.0, -150.0}, {0.0, -Lp}, 500.0, 250.0, Level::Water);
    for (size_t n = k.L.slots.size() - size_t(deck); n < k.L.slots.size(); ++n)
        k.L.slots[n].scale.z = Wp / 300.0;
    for (int i = 0; 1250.0 + i * 2500.0 < Lp; ++i)
        for (int side : {-1, 1})
            k.slot("pier/bollard/" + std::string(side < 0 ? "w" : "e") + std::to_string(i), "furniture.bollard",
                   {side * (Wp * 0.5 - 60.0), -1250.0 - i * 2500.0}, 0.0, 350.0, Level::Water);
    for (int i = 0; 3000.0 + i * 6000.0 < Lp; ++i)
        k.slot("pier/lamp/" + std::to_string(i), "lamp.street", {0.0, -3000.0 - i * 6000.0}, 0.0, 350.0, Level::Water);

    // Ships, one each side of the pier, bows out to sea.
    for (int i = 0; i < s.i("cruiseShips"); ++i)
    {
        const double side = i == 0 ? 1.0 : -1.0;
        const std::string line = k.name("line:" + std::to_string(i + 1), NK::Business, "cruiseLine");
        SlotOut& ship = k.slot("ship/" + std::to_string(i + 1), "ship.cruise", {side * (Wp * 0.5 + 2300.0), -Lp * 0.5 - 600.0}, kPi, 0.0, Level::Water);
        ship.scale = {1.0, 1.0, std::min(1.0, (Lp - 1000.0) / 29000.0)};
        ship.data = {{"line", line}};
        SlotOut& g = k.slot("ship/" + std::to_string(i + 1) + "/gangway", "gangway", {side * (Wp * 0.5 + 250.0), -Lp * 0.4}, facing({side, 0.0}), 350.0, Level::Water);
        g.scale = {1.0, 1.0, 500.0 / 1500.0};
        Part& berth = k.part("berth" + std::to_string(i + 1), "berth", rect(side * (Wp * 0.5 + 500.0), -Lp + 1500.0, side * (Wp * 0.5 + 4000.0), -2500.0),
                             Part::Needs::Water);
        berth.minDepth = 900.0;
        berth.groundOp = "berths";
    }

    // Terminal and its plaza: palms, lamps and benches either side of the pier head.
    EnvelopeOut& t = k.envelope("terminal", rect(-6000.0, 2500.0, 6000.0, 8500.0), "terminal", "cruiseTerminal", "commercial", s.i("terminalFloors"),
                                s.i("terminalFloors") * 450.0, "flat");
    t.front = {0.0, -1.0};
    t.entrances = {{0.0, 2500.0}, {0.0, 8500.0}};
    t.name = s.name;
    for (int i = 0; -Ls * 0.5 + 750.0 + i * 1500.0 < Ls * 0.5; ++i)
    {
        const double x = -Ls * 0.5 + 750.0 + i * 1500.0;
        if (std::abs(x) < Wp * 0.5 + 300.0)
            continue;
        k.slot("plaza/palm/" + std::to_string(i), "tree.palm", {x, 1400.0});
        if (i % 2 == 0)
            k.slot("plaza/lamp/" + std::to_string(i), "lamp.street", {x, 900.0}, kPi);
        else
            k.slot("plaza/bench/" + std::to_string(i), "furniture.bench", {x, 1900.0}, kPi);
    }
    k.ground("access", "terrainAddRoad",
             {{"kind", "road"}, {"profile", "urban"}, {"layer", "Sites"}, {"path", json::array({pt({-half, 10000.0}), pt({half, 10000.0})})}});
}

void buildLighthouse(Kit& k)
{
    const Spec& s = k.spec;
    const double H = s.f("towerHeight");
    k.L.boundary = circle({0.0, 0.0}, 2200.0, 16);
    k.ground("pad", "terrainStampPad", {{"footprint", pts(circle({0.0, 0.0}, 1800.0, 16))}, {"mode", "flatten"}, {"falloff", 800.0}, {"masks", {"cityFootprint"}}});
    {
        Part& pad = k.part("pad", "building", circle({0.0, 0.0}, 1800.0, 16), Part::Needs::Land);
        pad.maxRelief = 250.0;
        pad.groundOp = "pad";
        k.part("sea", "sea", circle({0.0, -9000.0}, 2000.0, 12), Part::Needs::Water);
    }
    EnvelopeOut& tower = k.envelope("tower", circle({0.0, 0.0}, 450.0, 8), "lighthouse", "lighthouse", "civic", std::max(1, int(H / 400.0)), H, "dome");
    tower.roofHeight = 200.0;
    tower.front = {0.0, 1.0};
    tower.entrances = {{0.0, 450.0}};
    tower.name = s.name;
    k.slot("lantern", "lighthouse.lantern", {0.0, 0.0}, 0.0, H).data = {{"range", 2000000}, {"character", "Fl W 10s"}};
    if (s.b("keeperHouse"))
    {
        EnvelopeOut& h = k.envelope("keeper", rect(800.0, 300.0, 1800.0, 1100.0), "colonial", "house", "residential", 1, 320.0, "gable");
        h.front = {-1.0, 0.0};
        h.entrances = {{800.0, 700.0}};
    }
    // Low wall round the pad, open on the landward side.
    const double r = 1700.0, circ = 2.0 * std::numbers::pi * r;
    Ring wall = circle({0.0, 0.0}, r, 16);
    k.alongRing("wall", "wall.low", wall, true, 300.0, {{circ * 0.25 - 200.0, circ * 0.25 + 200.0}});
}
} // namespace openworld::sites
