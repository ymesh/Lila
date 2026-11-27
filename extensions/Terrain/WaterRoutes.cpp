// Boat and air route graphs: coast lanes along a depth contour, channel strands, navigable river reaches, harbour
// approaches routed over deep enough water, airstrip approach and departure corridors; their checks and JSON.

#include "Coast.h"
#include "Water.h"

#include <lila/Profile.h>

#include <algorithm>
#include <limits>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <numbers>
#include <queue>

namespace terrain
{
using json = nlohmann::json;

namespace routes_detail
{
constexpr double kRouteDeg = 180.0 / std::numbers::pi;

std::vector<glm::dvec2> resampled(const std::vector<glm::dvec2>& line, double step)
{
    if (line.size() < 2 || step <= 0.0)
        return line;
    std::vector<glm::dvec2> out = {line.front()};
    double carry = 0.0;
    for (size_t k = 1; k < line.size(); ++k)
    {
        const glm::dvec2 a = line[k - 1], b = line[k];
        const double len = glm::length(b - a);
        double t = step - carry;
        while (t < len)
        {
            out.push_back(a + (b - a) * (t / len));
            t += step;
        }
        carry = len - (t - step);
    }
    if (glm::length(out.back() - line.back()) > step * 0.25)
        out.push_back(line.back());
    else
        out.back() = line.back();
    return out;
}

std::vector<glm::dvec2> smoothed(std::vector<glm::dvec2> line, int iterations)
{
    for (int it = 0; it < iterations && line.size() >= 3; ++it)
    {
        std::vector<glm::dvec2> next = {line.front()};
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            next.push_back(line[k] * 0.75 + line[k + 1] * 0.25);
            next.push_back(line[k] * 0.25 + line[k + 1] * 0.75);
        }
        next.push_back(line.back());
        line = std::move(next);
    }
    return line;
}

double length2(const std::vector<glm::dvec2>& l)
{
    double s = 0.0;
    for (size_t k = 1; k < l.size(); ++k)
        s += glm::length(l[k] - l[k - 1]);
    return s;
}

double length3(const std::vector<glm::dvec3>& l)
{
    double s = 0.0;
    for (size_t k = 1; k < l.size(); ++k)
        s += glm::length(glm::dvec2(l[k].x, l[k].z) - glm::dvec2(l[k - 1].x, l[k - 1].z));
    return s;
}

glm::dvec2 xz(const glm::dvec3& p) { return {p.x, p.z}; }

/** Nearest distance on the ground from p to any point of lane (its segments). */
double distToLane(glm::dvec2 p, const Lane& l)
{
    double best = 1e300;
    for (size_t k = 0; k + 1 < l.points.size(); ++k)
    {
        const glm::dvec2 a = xz(l.points[k]), b = xz(l.points[k + 1]), ab = b - a;
        const double l2 = glm::dot(ab, ab);
        const double t = l2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(p - (a + ab * t)));
    }
    if (l.points.size() == 1)
        best = glm::length(p - xz(l.points[0]));
    return best;
}

struct UnionFind
{
    std::vector<int> p;
    explicit UnionFind(size_t n) : p(n)
    {
        for (size_t i = 0; i < n; ++i)
            p[i] = int(i);
    }
    int find(int x)
    {
        while (p[size_t(x)] != x)
            x = p[size_t(x)] = p[size_t(p[size_t(x)])];
        return x;
    }
    void join(int a, int b) { p[size_t(find(a))] = find(b); }
};

/** The boat class that fits every point of a run: the one with the deepest draft; the most permissive one when none does. */
std::string fittingCraft(const std::vector<double>& width, const std::vector<double>& depth)
{
    std::string best;
    double draft = -1.0;
    for (const CraftClass& c : craftClasses())
    {
        if (c.kind != "boat")
            continue;
        bool fits = true;
        for (size_t k = 0; k < width.size() && fits; ++k)
            fits = width[k] >= c.minWidth && depth[k] >= c.minDepth;
        if (fits && c.draft > draft)
        {
            draft = c.draft;
            best = c.name;
        }
    }
    return best.empty() ? std::string("airboat") : best;
}

bool anyBoatFits(double width, double depth)
{
    for (const CraftClass& c : craftClasses())
        if (c.kind == "boat" && width >= c.minWidth && depth >= c.minDepth)
            return true;
    return false;
}

/** Water at least depth deep at p and in over a third of the directions round it at radius: a lane end out on open water, not up a dead end. */
bool openWater(const Heightfield& h, glm::dvec2 p, double level, double depth, double radius)
{
    if (level - h.heightAt(p.x, p.y) < depth)
        return false;
    int wet = 0;
    for (int k = 0; k < 16; ++k)
    {
        const double a = k * std::numbers::pi / 8.0;
        const glm::dvec2 q = p + glm::dvec2(std::cos(a), std::sin(a)) * radius;
        // Past the map's edge counts as open water: coast lanes end there.
        wet += !h.contains(q.x, q.y) || level - h.heightAt(q.x, q.y) >= depth;
    }
    return wet >= 6;
}

std::string nodeId(const glm::dvec3& p) { return std::format("n_{}_{}", std::lround(p.x / 100.0), std::lround(p.z / 100.0)); }
} // namespace routes_detail

using namespace routes_detail;

// ---------------------------------------------------------------------------------------------------- routing

std::vector<glm::dvec2> routeOverWater(const Heightfield& h, double waterLevel, double minDepth, double minWidth, glm::dvec2 start,
                                       const std::vector<glm::dvec2>& goals, double maxLength)
{
    LILA_ZONE();

    if (h.empty() || goals.empty())
        return {};
    // A coarser grid over big searches keeps the region near 1200 samples across.
    const int stride = std::max(1, int(std::ceil(maxLength / h.cell / 600.0)));
    const double cell = h.cell * stride;
    const int cx = int(std::lround((start.x - h.origin.x) / h.cell)), cz = int(std::lround((start.y - h.origin.y) / h.cell));
    const int reach = int(std::ceil(maxLength / h.cell));
    const int x0 = std::max(0, cx - reach), x1 = std::min(h.nx - 1, cx + reach), z0 = std::max(0, cz - reach), z1 = std::min(h.nz - 1, cz + reach);
    const int w = (x1 - x0) / stride + 1, d = (z1 - z0) / stride + 1;
    if (w < 2 || d < 2)
        return {};
    auto pos = [&](int gx, int gz) { return h.samplePos(x0 + gx * stride, z0 + gz * stride); };
    const size_t n = size_t(w) * size_t(d);
    std::vector<uint8_t> nav(n), blocked(n);
    for (int gz = 0; gz < d; ++gz)
        for (int gx = 0; gx < w; ++gx)
        {
            const size_t i = size_t(gz) * size_t(w) + size_t(gx);
            nav[i] = waterLevel - h.at(x0 + gx * stride, z0 + gz * stride) >= minDepth;
            blocked[i] = !nav[i];
        }
    const std::vector<float> clear = distanceToSet(blocked, w, d, cell);
    std::vector<uint8_t> allowed(n), goal(n, 0);
    std::vector<int> goalOf(n, -1);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 p = pos(int(i % size_t(w)), int(i / size_t(w)));
        allowed[i] = nav[i] && (clear[i] >= minWidth * 0.5 || glm::length(p - start) <= minWidth * 2.0 + cell);
    }
    for (size_t g = 0; g < goals.size(); ++g)
    {
        const int gx = int(std::lround((goals[g].x - h.origin.x) / h.cell - x0) / stride), gz = int(std::lround((goals[g].y - h.origin.y) / h.cell - z0) / stride);
        if (gx < 0 || gz < 0 || gx >= w || gz >= d)
            continue;
        const size_t i = size_t(gz) * size_t(w) + size_t(gx);
        if (nav[i])
        {
            goal[i] = 1;
            goalOf[i] = int(g);
            allowed[i] = 1;
        }
    }
    // Start at the allowed sample nearest the berth.
    size_t s = n;
    double best = 1e300;
    for (size_t i = 0; i < n; ++i)
        if (allowed[i])
        {
            const double dd = glm::length(pos(int(i % size_t(w)), int(i / size_t(w))) - start);
            if (dd < best && dd <= minWidth * 3.0 + 4.0 * cell)
            {
                best = dd;
                s = i;
            }
        }
    if (s == n)
        return {};
    std::vector<double> cost(n, 1e300);
    std::vector<int64_t> from(n, -1);
    using Q = std::pair<double, size_t>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> open;
    cost[s] = 0.0;
    open.push({0.0, s});
    size_t reached = n;
    while (!open.empty())
    {
        const auto [c, i] = open.top();
        open.pop();
        if (c > cost[i])
            continue;
        if (goal[i])
        {
            reached = i;
            break;
        }
        if (c > maxLength * 1.5)
            break;
        const int gx = int(i % size_t(w)), gz = int(i / size_t(w));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
            {
                if ((!dx && !dz) || gx + dx < 0 || gz + dz < 0 || gx + dx >= w || gz + dz >= d)
                    continue;
                const size_t j = size_t(gz + dz) * size_t(w) + size_t(gx + dx);
                if (!allowed[j])
                    continue;
                // Keep to the middle of the water: a lane hugging the bank costs more.
                const double penalty = 1.0 + 2.0 * std::max(0.0, 1.0 - clear[j] / (2.0 * minWidth));
                const double nc = c + cell * (dx && dz ? std::numbers::sqrt2 : 1.0) * penalty;
                if (nc < cost[j])
                {
                    cost[j] = nc;
                    from[j] = int64_t(i);
                    open.push({nc, j});
                }
            }
    }
    if (reached == n)
        return {};
    std::vector<glm::dvec2> path;
    for (int64_t i = int64_t(reached); i >= 0; i = from[size_t(i)])
        path.push_back(pos(int(size_t(i) % size_t(w)), int(size_t(i) / size_t(w))));
    std::reverse(path.begin(), path.end());
    path.insert(path.begin(), start);
    path.push_back(goals[size_t(goalOf[reached])]);
    return resampled(smoothed(path, 2), std::max(cell * 2.0, 500.0));
}

// ---------------------------------------------------------------------------------------------------- graph

void buildRoutes(const Heightfield& h, const CoastSpec& spec, const WaterResult* water, CoastResult& r)
{
    LILA_ZONE();

    const RouteSettings& rs = spec.routes;
    const double sea = spec.seaLevel;
    auto boatLane = [&](std::string id, std::string kind, std::string craft, const std::vector<glm::dvec2>& pts, std::string source) {
        Lane l;
        l.id = std::move(id);
        l.kind = std::move(kind);
        l.craft = craft;
        l.source = std::move(source);
        const CraftClass* c = craftClass(craft);
        ChannelStrand st;
        st.points = pts;
        measureStrand(h, st, sea, c ? c->minDepth : 0.0, (c ? c->minWidth : 800.0) * 4.0);
        for (size_t k = 0; k < pts.size(); ++k)
            l.points.push_back({pts[k].x, sea, pts[k].y});
        l.width = st.width;
        l.depth = st.depth;
        return l;
    };

    // Coast lanes: the depth contour, longest first.
    if (rs.coastLanes)
    {
        const std::vector<uint8_t> seaSet = seaOf(h, sea);
        std::vector<float> f(h.size());
        for (size_t i = 0; i < h.size(); ++i)
            f[i] = seaSet[i] ? float(sea - h.heights[i]) : -1.0e4f;
        std::vector<std::vector<glm::dvec2>> lines;
        for (const std::vector<glm::dvec2>& g : contourLines(f, h.nx, h.nz, float(rs.coastDepth)))
        {
            std::vector<glm::dvec2> l;
            for (const glm::dvec2& p : g)
                l.push_back(h.origin + p * h.cell);
            if (length2(l) >= rs.minLaneLength)
                lines.push_back(resampled(smoothed(l, 2), rs.spacing));
        }
        std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return length2(a) > length2(b); });
        for (size_t k = 0; k < lines.size(); ++k)
            r.lanes.push_back(boatLane("coast_" + std::to_string(k + 1), "coast", rs.coastCraft, lines[k], "coast"));
    }

    if (rs.channelLanes)
        for (const ChannelResult& c : r.channels)
            for (size_t s = 0; s < c.strands.size(); ++s)
            {
                const ChannelStrand& st = c.strands[s];
                if (st.points.size() < 2)
                    continue;
                Lane l;
                l.id = s == 0 ? "channel_" + c.id : "channel_" + c.id + "_b" + std::to_string(s);
                l.kind = "channel";
                l.craft = c.craft;
                l.source = c.id;
                for (const glm::dvec2& p : st.points)
                    l.points.push_back({p.x, c.waterLevel, p.y});
                l.width = st.width;
                l.depth = st.depth;
                r.lanes.push_back(std::move(l));
            }

    // River reaches some boat can run.
    if (rs.riverLanes && water)
        for (const RiverResult& rr : water->rivers)
        {
            int reachNo = 0;
            size_t k = 0;
            while (k < rr.points.size())
            {
                while (k < rr.points.size() && !anyBoatFits(rr.points[k].width, rr.points[k].depth))
                    ++k;
                const size_t begin = k;
                while (k < rr.points.size() && anyBoatFits(rr.points[k].width, rr.points[k].depth))
                    ++k;
                if (k - begin < 2)
                    continue;
                Lane l;
                l.kind = "river";
                l.source = rr.id;
                for (size_t j = begin; j < k; ++j)
                {
                    const WaterPoint& p = rr.points[j];
                    l.points.push_back({p.p.x, p.level, p.p.y});
                    l.width.push_back(p.width);
                    l.depth.push_back(p.depth);
                }
                if (length3(l.points) < std::max(rs.minLaneLength * 0.25, h.cell * 10.0))
                    continue;
                l.id = "river_" + rr.id + "_" + std::to_string(++reachNo);
                l.craft = fittingCraft(l.width, l.depth);
                r.lanes.push_back(std::move(l));
            }
        }

    // Drawn lanes, then harbour approaches routed out to what is there.
    for (const LaneSpec& s : spec.lanes)
        if (s.path.size() >= 2)
            r.lanes.push_back(boatLane(s.id, s.kind, s.craft, resampled(s.path, rs.spacing), "drawn"));
    for (const LaneSpec& s : spec.lanes)
    {
        if (s.path.size() >= 2 || !s.at)
            continue;
        const CraftClass* c = craftClass(s.craft);
        if (!c || c->kind != "boat")
        {
            r.warnings.push_back("lane " + s.id + ": no boat class " + s.craft + "; routed for a go-fast");
            c = craftClass("goFast");
        }
        std::vector<glm::dvec2> goals;
        for (const Lane& l : r.lanes)
            if (!l.air)
                for (const glm::dvec3& p : l.points)
                    goals.push_back(xz(p));
        const std::vector<glm::dvec2> path = routeOverWater(h, sea, c->minDepth, c->minWidth, *s.at, goals, std::max(rs.linkReach, 10000.0) * 2.0);
        if (path.size() < 2)
        {
            r.unrouted.push_back({s.id, glm::dvec3(s.at->x, sea, s.at->y)});
            r.warnings.push_back(std::format("harbour {}: no water {:.0f} cm deep and {:.0f} cm wide from its berth to a boat lane", s.id, c->minDepth, c->minWidth));
            continue;
        }
        r.lanes.push_back(boatLane(s.id, s.kind.empty() ? "harbour" : s.kind, c->name, path, "harbour"));
    }

    // Boat lane ends in open water with nothing near are linked over water to the nearest other lane.
    {
        const size_t count = r.lanes.size();
        for (size_t i = 0; i < count; ++i)
        {
            const Lane l = r.lanes[i];
            if (l.air || l.points.size() < 2)
                continue;
            const CraftClass* c = craftClass(l.craft);
            for (int end = 0; end < 2; ++end)
            {
                const glm::dvec3 e = end ? l.points.back() : l.points.front();
                bool near = false;
                for (size_t j = 0; j < r.lanes.size() && !near; ++j)
                    near = j != i && !r.lanes[j].air && distToLane(xz(e), r.lanes[j]) <= rs.snap;
                if (near || !c || !openWater(h, xz(e), sea, c->minDepth, c->minWidth * 3.0))
                    continue;
                std::vector<glm::dvec2> goals;
                for (size_t j = 0; j < r.lanes.size(); ++j)
                    if (j != i && !r.lanes[j].air && r.lanes[j].source != l.source)
                        for (const glm::dvec3& p : r.lanes[j].points)
                            goals.push_back(xz(p));
                if (goals.empty())
                    continue;
                const std::vector<glm::dvec2> path = routeOverWater(h, sea, c->minDepth, c->minWidth, xz(e), goals, rs.linkReach);
                if (path.size() >= 2)
                    r.lanes.push_back(boatLane("link_" + l.id + (end ? "_end" : "_start"), "link", l.craft, path, l.id));
            }
        }
        // A closed lane (round an island) has no loose end: link it from its point nearest another lane.
        for (size_t i = 0; i < count; ++i)
        {
            const Lane l = r.lanes[i];
            if (l.air || l.points.size() < 3 || glm::length(xz(l.points.front()) - xz(l.points.back())) > rs.snap)
                continue;
            const CraftClass* c = craftClass(l.craft);
            bool touches = false;
            for (size_t j = 0; j < r.lanes.size() && !touches; ++j)
                if (j != i && !r.lanes[j].air)
                    for (const glm::dvec3& p : l.points)
                        if (distToLane(xz(p), r.lanes[j]) <= rs.snap)
                        {
                            touches = true;
                            break;
                        }
            if (touches || !c)
                continue;
            std::vector<glm::dvec2> goals;
            for (size_t j = 0; j < r.lanes.size(); ++j)
                if (j != i && !r.lanes[j].air)
                    for (const glm::dvec3& p : r.lanes[j].points)
                        goals.push_back(xz(p));
            if (goals.empty())
                continue;
            glm::dvec2 from = xz(l.points.front());
            double best = std::numeric_limits<double>::max();
            for (const glm::dvec3& p : l.points)
                for (const glm::dvec2& g : goals)
                    if (const double d = glm::length(xz(p) - g); d < best)
                        best = d, from = xz(p);
            const std::vector<glm::dvec2> path = routeOverWater(h, sea, c->minDepth, c->minWidth, from, goals, rs.linkReach);
            if (path.size() >= 2)
                r.lanes.push_back(boatLane("link_" + l.id, "link", l.craft, path, l.id));
        }
    }

    // Air lanes: approach and departure corridors off each runway end.
    for (const AirstripSpec& s : spec.airstrips)
    {
        const CraftClass* ac = craftClass(s.aircraft);
        if (!ac || ac->kind != "air")
        {
            r.warnings.push_back("airstrip " + s.id + ": no aircraft class " + s.aircraft + "; corridors for a light aircraft");
            ac = craftClass("light");
        }
        const glm::dvec2 ab = s.b - s.a;
        const double len = glm::length(ab);
        if (len < h.cell)
        {
            r.warnings.push_back("airstrip " + s.id + ": its ends are the same point");
            continue;
        }
        const glm::dvec2 u = ab / len, side(-u.y, u.x);
        const glm::dvec2 mid = (s.a + s.b) * 0.5;
        const double elev = std::isnan(s.elevation) ? h.heightAt(mid.x, mid.y) : s.elevation;
        r.runways.push_back({s.id, ac->name, glm::dvec3(s.a.x, elev, s.a.y), glm::dvec3(s.b.x, elev, s.b.y), len});
        const double runwayHalf = 1500.0;
        auto corridor = [&](const std::string& id, const char* kind, glm::dvec2 end, glm::dvec2 out, double angle, bool towardRunway) {
            Lane l;
            l.id = id;
            l.kind = kind;
            l.craft = ac->name;
            l.air = true;
            l.source = s.id;
            const double step = std::max(rs.spacing * 2.0, h.cell);
            const int steps = std::max(2, int(std::ceil(ac->corridorLength / step)));
            for (int k = 0; k <= steps; ++k)
            {
                const double d = ac->corridorLength * double(k) / steps;
                const glm::dvec2 p = end + out * d;
                const double alt = elev + d * std::tan(angle / kRouteDeg);
                const double half = runwayHalf + (ac->corridorHalfWidth - runwayHalf) * (d / ac->corridorLength);
                double ground = -1e30;
                for (double f : {-1.0, -0.5, 0.0, 0.5, 1.0})
                {
                    const glm::dvec2 q = p + side * (half * f);
                    ground = std::max(ground, h.heightAt(q.x, q.y));
                }
                l.points.push_back({p.x, alt, p.y});
                l.clearance.push_back(alt - ground);
            }
            if (towardRunway)
            {
                std::reverse(l.points.begin(), l.points.end());
                std::reverse(l.clearance.begin(), l.clearance.end());
            }
            r.lanes.push_back(std::move(l));
        };
        corridor("air_" + s.id + "_approach_a", "approach", s.a, -u, ac->approachAngle, true);
        corridor("air_" + s.id + "_departure_b", "departure", s.b, u, ac->climbAngle, false);
        if (s.bothEnds)
        {
            corridor("air_" + s.id + "_approach_b", "approach", s.b, u, ac->approachAngle, true);
            corridor("air_" + s.id + "_departure_a", "departure", s.a, -u, ac->climbAngle, false);
        }
    }

    // Nodes: lane ends within snap of each other or of another lane share one.
    r.nodes.clear();
    auto nodeAt = [&](const glm::dvec3& p) {
        for (size_t k = 0; k < r.nodes.size(); ++k)
            if (glm::length(xz(r.nodes[k].at) - xz(p)) <= rs.snap)
                return int(k);
        r.nodes.push_back({nodeId(p), p, {}});
        return int(r.nodes.size() - 1);
    };
    for (size_t i = 0; i < r.lanes.size(); ++i)
    {
        Lane& l = r.lanes[i];
        if (l.points.empty())
            continue;
        l.from = nodeAt(l.points.front());
        l.to = nodeAt(l.points.back());
        r.nodes[size_t(l.from)].lanes.push_back(int(i));
        if (l.to != l.from)
            r.nodes[size_t(l.to)].lanes.push_back(int(i));
    }
    for (size_t k = 0; k < r.nodes.size(); ++k)
        for (size_t i = 0; i < r.lanes.size(); ++i)
        {
            std::vector<int>& on = r.nodes[k].lanes;
            if (std::find(on.begin(), on.end(), int(i)) == on.end() && r.lanes[i].air == r.lanes[size_t(on.front())].air &&
                distToLane(xz(r.nodes[k].at), r.lanes[i]) <= rs.snap)
                on.push_back(int(i));
        }

    UnionFind uf(r.lanes.size());
    for (const LaneNode& n : r.nodes)
        for (size_t k = 1; k < n.lanes.size(); ++k)
            uf.join(n.lanes[0], n.lanes[k]);
    std::map<int, int> ids;
    std::map<int, double> boatLength;
    for (size_t i = 0; i < r.lanes.size(); ++i)
    {
        const int root = uf.find(int(i));
        auto [it, fresh] = ids.try_emplace(root, int(ids.size()));
        r.lanes[i].component = it->second;
        if (!r.lanes[i].air)
            boatLength[it->second] += length3(r.lanes[i].points) * (r.lanes[i].kind == "coast" ? 4.0 : 1.0);
    }
    double most = -1.0;
    for (const auto& [c, len] : boatLength)
        if (len > most)
        {
            most = len;
            r.mainComponent = c;
        }
}

// ---------------------------------------------------------------------------------------------------- checks

json routeIssues(const Heightfield& h, const CoastResult& r)
{
    json issues = json::array();
    auto issue = [&](const char* type, const std::string& about, const glm::dvec3& at, const std::string& msg) {
        issues.push_back({{"type", type}, {"about", about}, {"position", {at.x, at.y, at.z}}, {"message", msg}});
    };
    for (const ChannelResult& c : r.channels)
    {
        const CraftClass* craft = craftClass(c.craft);
        if (!craft)
            continue;
        for (size_t s = 0; s < c.strands.size(); ++s)
        {
            const ChannelStrand& st = c.strands[s];
            if (st.points.empty())
                continue;
            const std::string about = s == 0 ? c.id : c.id + "_b" + std::to_string(s);
            const size_t kd = size_t(std::min_element(st.depth.begin(), st.depth.end()) - st.depth.begin());
            const size_t kw = size_t(std::min_element(st.width.begin(), st.width.end()) - st.width.begin());
            if (st.minDepth < craft->minDepth)
                issue("channelShallow", about, {st.points[kd].x, c.waterLevel, st.points[kd].y},
                      std::format("{:.0f} cm deep at its shallowest; a {} needs {:.0f}", st.minDepth, craft->label, craft->minDepth));
            if (st.minWidth < craft->minWidth)
                issue("channelNarrow", about, {st.points[kw].x, c.waterLevel, st.points[kw].y},
                      std::format("{:.0f} cm wide at its narrowest; a {} needs {:.0f}", st.minWidth, craft->label, craft->minWidth));
        }
    }
    for (const Lane& l : r.lanes)
    {
        const CraftClass* craft = craftClass(l.craft);
        if (!craft || l.points.empty())
            continue;
        if (l.air)
        {
            size_t worst = 0;
            // The point on the runway itself is at ground level by design.
            const size_t onRunway = l.kind == "approach" ? l.points.size() - 1 : 0;
            // Near the runway the slope is lower than the full clearance: there the ground must keep half the slope's height
            // above the runway clear, and the full clearance applies once the slope is twice that high.
            const double runwayAlt = l.points[onRunway].y;
            double low = 1e300, need = craft->clearance, worstShort = 0.0;
            for (size_t k = 0; k < l.clearance.size(); ++k)
            {
                if (k == onRunway)
                    continue;
                const double required = std::min(craft->clearance, std::max(0.0, l.points[k].y - runwayAlt) * 0.5);
                if (required - l.clearance[k] > worstShort)
                {
                    worstShort = required - l.clearance[k];
                    low = l.clearance[k];
                    need = required;
                    worst = k;
                }
            }
            if (worstShort > 1.0)
                issue("airClearance", l.id, l.points[worst],
                      std::format("clears the ground by {:.0f} cm; a {} needs {:.0f} there", low, craft->label, need));
            continue;
        }
        size_t dry = 0, firstDry = 0;
        for (size_t k = 0; k < l.points.size(); ++k)
            if (h.contains(l.points[k].x, l.points[k].z) && h.heightAt(l.points[k].x, l.points[k].z) >= l.points[k].y - 1.0)
            {
                if (!dry)
                    firstDry = k;
                ++dry;
            }
        if (dry)
            issue("laneOverLand", l.id, l.points[firstDry], std::format("{} of {} points over dry ground", dry, l.points.size()));
        if (l.kind != "channel")
        {
            double minD = 1e300, minW = 1e300;
            size_t kd = 0, kw = 0;
            for (size_t k = 0; k < l.points.size(); ++k)
            {
                const double d = k < l.depth.size() ? l.depth[k] : 0.0, w = k < l.width.size() ? l.width[k] : 0.0;
                if (d <= 0.0)
                    continue;
                if (d < minD)
                {
                    minD = d;
                    kd = k;
                }
                if (w < minW)
                {
                    minW = w;
                    kw = k;
                }
            }
            if (minD < craft->minDepth)
                issue("laneShallow", l.id, l.points[kd], std::format("{:.0f} cm deep at its shallowest; a {} needs {:.0f}", minD, craft->label, craft->minDepth));
            if (minW < craft->minWidth)
                issue("laneNarrow", l.id, l.points[kw], std::format("{:.0f} cm clear at its narrowest; a {} needs {:.0f}", minW, craft->label, craft->minWidth));
        }
        if (r.mainComponent >= 0 && l.component != r.mainComponent)
            issue("laneDisconnected", l.id, l.points.front(), "not joined to the main boat network");
    }
    for (const Runway& w : r.runways)
        if (const CraftClass* craft = craftClass(w.aircraft); craft && w.length < craft->runway)
            issue("shortRunway", w.id, (w.a + w.b) * 0.5, std::format("{:.0f} cm long; a {} needs {:.0f}", w.length, craft->label, craft->runway));
    for (const auto& [id, at] : r.unrouted)
        issue("harbourUnreachable", id, at, "no water route from its berth to a boat lane");
    return issues;
}

// ---------------------------------------------------------------------------------------------------- json

json routesJson(const CoastResult& r, const WorldMap& w)
{
    json crafts = json::array();
    for (const CraftClass& c : craftClasses())
        crafts.push_back(craftJson(c));
    json nodes = json::array();
    for (const LaneNode& n : r.nodes)
    {
        json lanes = json::array();
        for (int i : n.lanes)
            lanes.push_back(r.lanes[size_t(i)].id);
        nodes.push_back({{"id", n.id}, {"position", w.point(n.at.x, n.at.y, n.at.z)}, {"lanes", lanes}});
    }
    json lanes = json::array(), shapes = json::array();
    std::map<std::string, json> profiles;
    for (const Lane& l : r.lanes)
    {
        json pts = json::array(), shapePts = json::array();
        for (size_t k = 0; k < l.points.size(); ++k)
        {
            const glm::dvec3& p = l.points[k];
            json q = {{"position", w.point(p.x, p.y, p.z)}};
            if (k < l.width.size())
                q["width"] = l.width[k] * w.across;
            if (k < l.depth.size())
                q["depth"] = l.depth[k] * w.vertical;
            if (k < l.clearance.size())
                q["clearance"] = l.clearance[k] * w.vertical;
            pts.push_back(q);
            shapePts.push_back(q["position"]);
        }
        json j = {{"id", l.id},         {"kind", l.kind},     {"craft", l.craft}, {"air", l.air},       {"source", l.source},
                  {"length", length3(l.points) * w.across}, {"component", l.component}, {"main", !l.air && l.component == r.mainComponent},
                  {"points", pts}};
        j["from"] = l.from >= 0 ? json(r.nodes[size_t(l.from)].id) : json(nullptr);
        j["to"] = l.to >= 0 ? json(r.nodes[size_t(l.to)].id) : json(nullptr);
        lanes.push_back(j);
        const std::string tag = l.air ? "air" : "boat";
        if (!profiles.count(l.craft))
        {
            const CraftClass* c = craftClass(l.craft);
            const double width = !c ? 800.0 : l.air ? c->corridorHalfWidth * 2.0 : c->minWidth;
            profiles[l.craft] = {{"name", l.craft}, {"lanes", json::array({{{"width", width * w.across}, {"direction", "both"}, {"tags", {tag, l.craft}}}})}};
        }
        json links = json::array();
        for (int nodeIdx : {l.from, l.to})
            if (nodeIdx >= 0)
                for (int other : r.nodes[size_t(nodeIdx)].lanes)
                    if (r.lanes[size_t(other)].id != l.id)
                        links.push_back(r.lanes[size_t(other)].id);
        shapes.push_back({{"id", l.id}, {"profile", l.craft}, {"tags", {tag, l.kind, l.craft}}, {"points", shapePts}, {"links", links}});
    }
    json profileList = json::array();
    for (const auto& [name, p] : profiles)
        profileList.push_back(p);
    json runways = json::array();
    for (const Runway& rw : r.runways)
        runways.push_back({{"id", rw.id},
                           {"aircraft", rw.aircraft},
                           {"a", w.point(rw.a.x, rw.a.y, rw.a.z)},
                           {"b", w.point(rw.b.x, rw.b.y, rw.b.z)},
                           {"length", rw.length * w.across}});
    return {{"format", "lila.waterRoutes"},
            {"version", 1},
            {"units", "cm"},
            {"crafts", crafts},
            {"nodes", nodes},
            {"lanes", lanes},
            {"runways", runways},
            {"zoneGraph", {{"laneProfiles", profileList}, {"shapes", shapes}}}};
}

json coastWaterJson(const CoastResult& r, const std::vector<DamResult>& dams, const WorldMap& w)
{
    json splines = json::array(), locators = json::array();
    for (const ChannelResult& c : r.channels)
        for (size_t s = 0; s < c.strands.size(); ++s)
        {
            const ChannelStrand& st = c.strands[s];
            json pts = json::array();
            double dist = 0.0;
            for (size_t k = 0; k < st.points.size(); ++k)
            {
                if (k)
                    dist += glm::length(st.points[k] - st.points[k - 1]);
                const glm::dvec2 p = st.points[k];
                pts.push_back({{"position", w.point(p.x, c.waterLevel, p.y)},
                               {"width", (k < st.width.size() ? st.width[k] : c.width) * w.across},
                               {"depth", (k < st.depth.size() ? st.depth[k] : c.depth) * w.vertical},
                               {"waterLevel", w.height(p.x, c.waterLevel, p.y)},
                               {"bedHeight", w.height(p.x, c.waterLevel - (k < st.depth.size() ? st.depth[k] : c.depth), p.y)},
                               {"distance", dist * w.across}});
            }
            splines.push_back({{"id", s == 0 ? c.id : c.id + "_b" + std::to_string(s)}, {"kind", "channel"}, {"feature", c.id}, {"craft", c.craft}, {"closed", false}, {"points", pts}});
        }
    for (const StashSpot& s : r.stash)
        locators.push_back({{"id", s.id}, {"kind", "stash"}, {"stashKind", s.kind}, {"channel", s.channel}, {"score", s.score}, {"position", w.point(s.at.x, s.at.y, s.at.z)}});
    for (const DamResult& d : dams)
    {
        locators.push_back({{"id", d.id}, {"kind", "dam"}, {"position", w.point(d.spillway.x, d.crest, d.spillway.y)}, {"crest", w.height(d.spillway.x, d.crest, d.spillway.y)},
                            {"height", d.height * w.vertical}, {"length", d.length * w.across}});
        locators.push_back({{"id", d.id + "_spillway"}, {"kind", "spillway"}, {"dam", d.id}, {"position", w.point(d.spillway.x, d.sill, d.spillway.y)},
                            {"level", w.height(d.spillway.x, d.sill, d.spillway.y)}});
    }
    return {{"format", "lila.terrainWater"}, {"version", 1}, {"units", "cm"}, {"splines", splines}, {"locators", locators}};
}

// ---------------------------------------------------------------------------------------------------- published

namespace
{
template <class T>
struct Board
{
    std::mutex mutex;
    std::map<std::pair<uint64_t, std::string>, T> results;

    void put(uint64_t m, const std::string& n, T r)
    {
        std::lock_guard lock(mutex);
        results[{m, n}] = std::move(r);
    }
    bool get(uint64_t m, const std::string& n, T* out)
    {
        std::lock_guard lock(mutex);
        auto it = results.find({m, n});
        if (it == results.end())
            return false;
        *out = it->second;
        return true;
    }
};

Board<CoastResult>& coastBoard()
{
    static Board<CoastResult> b;
    return b;
}
Board<std::vector<DamResult>>& damBoard()
{
    static Board<std::vector<DamResult>> b;
    return b;
}
Board<BathymetryResult>& bathyBoard()
{
    static Board<BathymetryResult> b;
    return b;
}
} // namespace

void publishCoast(uint64_t modifier, const std::string& node, CoastResult r) { coastBoard().put(modifier, node, std::move(r)); }
bool publishedCoast(uint64_t modifier, const std::string& node, CoastResult* out) { return coastBoard().get(modifier, node, out); }
void publishDams(uint64_t modifier, const std::string& node, std::vector<DamResult> r) { damBoard().put(modifier, node, std::move(r)); }
bool publishedDams(uint64_t modifier, const std::string& node, std::vector<DamResult>* out) { return damBoard().get(modifier, node, out); }
void publishBathymetry(uint64_t modifier, const std::string& node, BathymetryResult r) { bathyBoard().put(modifier, node, std::move(r)); }
bool publishedBathymetry(uint64_t modifier, const std::string& node, BathymetryResult* out) { return bathyBoard().get(modifier, node, out); }
} // namespace terrain
