// Garment details built from panels, seams and tacks: pockets, flaps, belt loops, collars, drawstrings, lacing and the
// split a zip makes in its seam.

#include "Pattern.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace tailor
{
namespace
{
constexpr float kLift = 0.004f; // how far a piece laid on another starts out from it

float patternDist(const Vec2& a, const Vec2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 unit(const Vec3& a)
{
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 0.f ? Vec3{a[0] / l, a[1] / l, a[2] / l} : a;
}

std::string detailName(const Garment& g, const std::string& base)
{
    std::string name = base;
    for (int k = 2; std::any_of(g.panels.begin(), g.panels.end(), [&](const Panel& p) { return p.name == name; }); ++k)
        name = base + std::to_string(k);
    return name;
}

/** The outline point at arc length s along a run of outline (from..to walking forward). */
Vec2 alongRun(const std::vector<Vec2>& o, int from, int to, float s)
{
    const int n = int(o.size());
    float walked = 0.f;
    for (int v = from % n; v != to % n; v = (v + 1) % n)
    {
        const Vec2 &a = o[size_t(v)], &b = o[size_t((v + 1) % n)];
        const float l = patternDist(a, b);
        if (walked + l >= s && l > 0.f)
        {
            const float t = (s - walked) / l;
            return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
        }
        walked += l;
    }
    return o[size_t(to % n)];
}

float detailRunLength(const std::vector<Vec2>& o, int from, int to)
{
    const int n = int(o.size());
    float l = 0.f;
    for (int v = from % n; v != to % n; v = (v + 1) % n)
        l += patternDist(o[size_t(v)], o[size_t((v + 1) % n)]);
    return l;
}

/** The pattern point at arc length s along a chain of runs, with the panel it lies on. */
std::pair<int, Vec2> alongChain(const Garment& g, const std::vector<SeamSide>& chain, float s)
{
    for (const SeamSide& side : chain)
    {
        const auto& o = g.panels[size_t(side.panel)].outline;
        const float l = detailRunLength(o, side.from, side.to);
        if (s <= l || &side == &chain.back())
            return {side.panel, alongRun(o, side.from, side.to, std::min(s, l))};
        s -= l;
    }
    return {-1, {0.f, 0.f}};
}

/** Tacks a straight row of a piece to a row on its host, a point every `step`: piece points a + t (b - a), host c + t (d - c). */
void tackRow(Garment& g, int piece, Vec2 a, Vec2 b, int host, Vec2 c, Vec2 d, float step)
{
    const int n = std::max(1, int(std::ceil(patternDist(a, b) / std::max(step, 1e-4f))));
    for (int k = 0; k <= n; ++k)
    {
        const float t = float(k) / float(n);
        g.tacks.push_back({piece, {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t}, host, {c[0] + (d[0] - c[0]) * t, c[1] + (d[1] - c[1]) * t}});
    }
}

/** A narrow strip of cloth for a cord, placed flat at a point with its length along `along`. */
int addCord(Garment& g, const std::string& name, float length, float width, const Vec3& at, const Vec3& along, const Vec3& up, int fabric)
{
    Panel p;
    p.name = detailName(g, name);
    p.outline = {{0.f, 0.f}, {length, 0.f}, {length, width}, {0.f, width}};
    p.particleDistance = std::max(0.5f * width, std::min(0.01f, length / 8.f));
    const Vec3 r = unit(along);
    Vec3 u = unit(add(up, scale(r, -(up[0] * r[0] + up[1] * r[1] + up[2] * r[2]))));
    p.origin = add(at, scale(u, -0.5f * width));
    p.right = r;
    p.up = u;
    p.fabric = fabric;
    p.layer = 2;
    g.panels.push_back(std::move(p));
    return int(g.panels.size()) - 1;
}
} // namespace

void placeOnPanel(const Panel& host, const Vec2& at, float lift, Panel& piece)
{
    const Vec3 r = unit(host.right), u = host.up, n = unit(cross(host.right, host.up));
    if (std::abs(host.wrapRadius) < 1e-6f)
    {
        piece.origin = add(add(add(host.origin, scale(r, at[0])), scale(u, at[1])), scale(n, lift));
        piece.right = host.right;
        piece.up = host.up;
        piece.wrapRadius = 0.f;
        return;
    }
    // Round the host's wrap cylinder to the point, then out by lift: same axis, a slightly larger radius.
    const float th = at[0] / host.wrapRadius, c = std::cos(th), s = std::sin(th);
    const Vec3 tangent = add(scale(r, c), scale(n, -s)), outward = add(scale(r, s), scale(n, c));
    piece.origin = add(add(placePatternPoint(host, at), scale(outward, lift)), Vec3{0.f, 0.f, 0.f});
    piece.right = tangent;
    piece.up = host.up;
    piece.wrapRadius = host.wrapRadius + lift;
}

int addPocket(Garment& g, int host, const Vec2& at, float width, float height, bool flap)
{
    if (host < 0 || size_t(host) >= g.panels.size() || width <= 0.f || height <= 0.f)
        return -1;
    const Panel hostPanel = g.panels[size_t(host)];
    Panel p;
    p.name = detailName(g, hostPanel.name + " Pocket");
    // Rounded lower corners, a quarter circle of a fifth of the width each.
    const float rc = 0.2f * std::min(width, height);
    p.outline.push_back({width, height});
    p.outline.push_back({0.f, height});
    for (int k = 4; k >= 0; --k)
    {
        const float t = 1.5707963f * float(k) / 4.f;
        p.outline.push_back({rc - rc * std::sin(t), rc - rc * std::cos(t)});
    }
    for (int k = 0; k <= 4; ++k)
    {
        const float t = 1.5707963f * float(k) / 4.f;
        p.outline.push_back({width - rc + rc * std::sin(t), rc - rc * std::cos(t)});
    }
    p.fabric = hostPanel.fabric;
    p.layer = hostPanel.layer + 1;
    p.particleDistance = std::min(hostPanel.particleDistance, 0.25f * std::min(width, height));
    placeOnPanel(hostPanel, at, kLift, p);
    const int index = int(g.panels.size());
    const std::vector<Vec2> outline = p.outline;
    g.panels.push_back(std::move(p));
    // Every outline point but the open top edge's middle holds to the host point under it.
    const float step = g.panels[size_t(index)].particleDistance;
    const int n = int(outline.size());
    for (int i = 0; i < n; ++i)
    {
        const Vec2 &a = outline[size_t(i)], &b = outline[size_t((i + 1) % n)];
        if (a[1] >= height - 1e-6f && b[1] >= height - 1e-6f)
            continue;
        tackRow(g, index, a, b, host, {at[0] + a[0], at[1] + a[1]}, {at[0] + b[0], at[1] + b[1]}, step);
    }
    if (flap)
    {
        // A flap tacked along its top over the pocket's mouth, hanging down over it: its pattern runs right to left
        // and top to bottom so it still faces out.
        Panel f;
        f.name = detailName(g, hostPanel.name + " Flap");
        const float fw = width + 0.01f, fh = 0.4f * height;
        f.outline = {{0.f, 0.f}, {fw, 0.f}, {fw, fh}, {0.f, fh}};
        f.fabric = hostPanel.fabric;
        f.layer = hostPanel.layer + 2;
        f.particleDistance = std::min(hostPanel.particleDistance, 0.3f * fh);
        placeOnPanel(hostPanel, {at[0] + width + 0.005f, at[1] + height + 0.01f}, 2.f * kLift, f);
        f.right = scale(f.right, -1.f);
        f.up = scale(f.up, -1.f);
        const int fi = int(g.panels.size());
        g.panels.push_back(std::move(f));
        tackRow(g, fi, {0.f, 0.f}, {fw, 0.f}, host, {at[0] + width + 0.005f, at[1] + height + 0.01f}, {at[0] - 0.005f, at[1] + height + 0.01f},
                g.panels[size_t(fi)].particleDistance);
    }
    return index;
}

int addPatch(Garment& g, int host, const Vec2& centre, float width, float height, const std::string& name)
{
    if (host < 0 || size_t(host) >= g.panels.size() || width <= 0.f || height <= 0.f)
        return -1;
    const Panel hostPanel = g.panels[size_t(host)];
    Panel p;
    p.name = detailName(g, hostPanel.name + " " + name);
    const float rc = 0.3f * std::min(width, height), hw = 0.5f * width, hh = 0.5f * height;
    const Vec2 corners[4] = {{hw - rc, -hh + rc}, {hw - rc, hh - rc}, {-hw + rc, hh - rc}, {-hw + rc, -hh + rc}};
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k <= 4; ++k)
        {
            const float t = 1.5707963f * (float(c) + float(k) / 4.f) - 1.5707963f;
            p.outline.push_back({corners[c][0] + rc * std::cos(t), corners[c][1] + rc * std::sin(t)});
        }
    p.fabric = hostPanel.fabric;
    p.layer = hostPanel.layer + 1;
    p.particleDistance = std::min(hostPanel.particleDistance, 0.2f * std::min(width, height));
    placeOnPanel(hostPanel, centre, kLift, p);
    const int index = int(g.panels.size());
    const std::vector<Vec2> outline = p.outline;
    g.panels.push_back(std::move(p));
    const float step = g.panels[size_t(index)].particleDistance;
    const int n = int(outline.size());
    for (int i = 0; i < n; ++i)
    {
        const Vec2 &a = outline[size_t(i)], &b = outline[size_t((i + 1) % n)];
        tackRow(g, index, a, b, host, {centre[0] + a[0], centre[1] + a[1]}, {centre[0] + b[0], centre[1] + b[1]}, step);
    }
    return index;
}

std::vector<int> addBeltLoops(Garment& g, int host, float y, int count, float width, float height)
{
    std::vector<int> out;
    if (host < 0 || size_t(host) >= g.panels.size() || count < 1)
        return out;
    const Panel hostPanel = g.panels[size_t(host)];
    float x0 = 1e30f, x1 = -1e30f;
    for (const Vec2& v : hostPanel.outline)
        if (std::abs(v[1] - y) < 0.05f)
            x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]);
    if (x1 <= x0)
        for (const Vec2& v : hostPanel.outline)
            x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]);
    for (int k = 0; k < count; ++k)
    {
        const float x = x0 + (x1 - x0) * (float(k) + 0.5f) / float(count) - 0.5f * width;
        Panel p;
        p.name = detailName(g, hostPanel.name + " Loop");
        // A little longer than the gap it spans, so it stands off the host for the belt to pass under.
        const float length = height + 0.015f;
        p.outline = {{0.f, 0.f}, {width, 0.f}, {width, length}, {0.f, length}};
        p.fabric = hostPanel.fabric;
        p.layer = hostPanel.layer + 1;
        p.particleDistance = std::min(hostPanel.particleDistance, 0.4f * width);
        placeOnPanel(hostPanel, {x, y}, kLift, p);
        const int index = int(g.panels.size());
        g.panels.push_back(std::move(p));
        tackRow(g, index, {0.f, 0.f}, {width, 0.f}, host, {x, y}, {x + width, y}, 0.5f * width);
        tackRow(g, index, {0.f, length}, {width, length}, host, {x, y + height}, {x + width, y + height}, 0.5f * width);
        out.push_back(index);
    }
    return out;
}

/** The outline point a seam end sews a run's end point to, when one does. */
static std::pair<int, int> sewnTo(const Garment& g, int panel, int vertex)
{
    for (const Seam& s : g.seams)
    {
        if (s.a.size() != 1 || s.b.size() != 1)
            continue;
        const SeamSide &a = s.a.front(), &b = s.b.front();
        const int bFrom = s.reversed ? b.to : b.from, bTo = s.reversed ? b.from : b.to;
        if (a.panel == panel && a.from == vertex)
            return {b.panel, bFrom};
        if (a.panel == panel && a.to == vertex)
            return {b.panel, bTo};
        if (b.panel == panel && bFrom == vertex)
            return {a.panel, a.from};
        if (b.panel == panel && bTo == vertex)
            return {a.panel, a.to};
    }
    return {-1, -1};
}

std::vector<SeamSide> findNeckline(const Garment& g)
{
    std::vector<OpenEdge> up;
    float top = -1e30f;
    for (const OpenEdge& e : openEdges(g))
        if (e.out[1] > 0.3f && g.panels[size_t(e.side.panel)].name.rfind("Sleeve", 0) != 0)
        {
            up.push_back(e);
            top = std::max(top, e.at[2][1]);
        }
    std::vector<SeamSide> neck;
    for (const OpenEdge& e : up)
        if (e.at[2][1] > top - 0.1f)
            neck.push_back(e.side);
    if (neck.empty())
        return {};
    // Chain them through the seams at their ends (the shoulders), starting from one no seam leads into.
    auto next = [&](const SeamSide& s) -> int {
        const auto [panel, vertex] = sewnTo(g, s.panel, s.to);
        for (size_t i = 0; i < neck.size(); ++i)
            if (neck[i].panel == panel && neck[i].from == vertex)
                return int(i);
        return -1;
    };
    size_t start = 0;
    for (size_t i = 0; i < neck.size(); ++i)
    {
        bool led = false;
        for (const SeamSide& o : neck)
            led |= next(o) == int(i);
        if (!led)
        {
            start = i;
            break;
        }
    }
    std::vector<SeamSide> chain{neck[start]};
    std::vector<char> used(neck.size(), 0);
    used[start] = 1;
    for (int k = next(neck[start]); k >= 0 && !used[size_t(k)]; k = next(neck[size_t(k)]))
    {
        used[size_t(k)] = 1;
        chain.push_back(neck[size_t(k)]);
    }
    return chain;
}

int addCollar(Garment& g, const std::vector<SeamSide>& neckline, float height, bool fold)
{
    if (neckline.empty() || height <= 0.f)
        return -1;
    float length = 0.f;
    Vec3 centre{0.f, 0.f, 0.f};
    std::vector<Vec3> ring;
    for (const SeamSide& s : neckline)
    {
        if (s.panel < 0 || size_t(s.panel) >= g.panels.size())
            return -1;
        const Panel& p = g.panels[size_t(s.panel)];
        const float l = detailRunLength(p.outline, s.from, s.to);
        length += l;
        for (int k = 0; k < 4; ++k)
            ring.push_back(placePatternPoint(p, alongRun(p.outline, s.from, s.to, l * float(k) / 4.f)));
    }
    for (const Vec3& q : ring)
        centre = add(centre, scale(q, 1.f / float(ring.size())));
    float radius = 0.f;
    for (const Vec3& q : ring)
        radius += std::hypot(q[0] - centre[0], q[2] - centre[2]) / float(ring.size());
    const Panel& first = g.panels[size_t(neckline.front().panel)];
    const Vec3 start = placePatternPoint(first, first.outline[size_t(neckline.front().from)]);
    const float th = std::atan2(start[0] - centre[0], start[2] - centre[2]);

    Panel c;
    c.name = detailName(g, "Collar");
    c.outline = {{0.f, 0.f}, {length, 0.f}, {length, height}, {0.f, height}};
    c.fabric = first.fabric;
    c.layer = first.layer;
    c.particleDistance = std::min(first.particleDistance, 0.3f * height);
    // Closed, the neckline draws in to a ring as long as itself, so the collar starts as that ring round the neck.
    const auto [lp, lv] = sewnTo(g, neckline.back().panel, neckline.back().to);
    const bool closed = lp == neckline.front().panel && lv == neckline.front().from;
    c.wrapRadius = (closed ? length / 6.2831853f : radius) + 0.005f;
    c.origin = {centre[0] + c.wrapRadius * std::sin(th), centre[1], centre[2] + c.wrapRadius * std::cos(th)};
    c.right = {std::cos(th), 0.f, -std::sin(th)};
    c.up = {0.f, 1.f, 0.f};
    if (fold)
    {
        // A shirt collar: a stand, then the fall folded down over it along this line.
        c.internalLines = {{{0.f, 0.4f * height}, {length, 0.4f * height}}};
        c.lineAngles = {2.8f};
    }
    const int index = int(g.panels.size());
    g.panels.push_back(std::move(c));

    Seam neck;
    neck.a = {{index, 0, 1}};
    neck.b = neckline;
    // The way round the neck the chain runs decides which way the collar's length meets it.
    const Vec3 a1 = placePatternPoint(g.panels[size_t(index)], {0.25f * length, 0.f});
    const auto [p1, q1] = alongChain(g, neckline, 0.25f * length);
    const auto [p3, q3] = alongChain(g, neckline, 0.75f * length);
    const Vec3 b1 = placePatternPoint(g.panels[size_t(p1)], q1), b3 = placePatternPoint(g.panels[size_t(p3)], q3);
    auto dist = [](const Vec3& x, const Vec3& y) { return std::hypot(x[0] - y[0], x[1] - y[1], x[2] - y[2]); };
    neck.reversed = dist(a1, b3) < dist(a1, b1);
    g.seams.push_back(neck);

    if (closed)
    {
        Seam ends;
        ends.a = {{index, 1, 2}};
        ends.b = {{index, 3, 0}};
        ends.reversed = true;
        g.seams.push_back(ends);
    }
    return index;
}

int addDrawstring(Garment& g, const std::vector<SeamSide>& channel, float tail, float gather, float width)
{
    if (channel.empty())
        return -1;
    float length = 0.f;
    for (const SeamSide& s : channel)
    {
        if (s.panel < 0 || size_t(s.panel) >= g.panels.size())
            return -1;
        length += detailRunLength(g.panels[size_t(s.panel)].outline, s.from, s.to);
    }
    gather = std::clamp(gather, 0.2f, 1.f);
    const float inChannel = length * gather;
    const auto [p0, q0] = alongChain(g, channel, 0.f);
    const auto [p1, q1] = alongChain(g, channel, length);
    const Vec3 a = placePatternPoint(g.panels[size_t(p0)], q0), b = placePatternPoint(g.panels[size_t(p1)], q1);
    Vec3 along{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    if (std::hypot(along[0], along[1], along[2]) < 1e-4f)
        along = g.panels[size_t(p0)].right;
    const int fabric = g.panels[size_t(p0)].fabric;
    const int cord = addCord(g, "Drawstring", inChannel + 2.f * tail, width, add(a, scale(unit(along), -tail)), along, {0.f, 1.f, 0.f}, fabric);
    // The cord runs the channel's length shortened by gather, so holding it to the channel draws the channel in.
    const int n = std::max(2, int(std::ceil(length / 0.02f)));
    for (int k = 0; k <= n; ++k)
    {
        const float s = length * float(k) / float(n);
        const auto [panel, at] = alongChain(g, channel, s);
        g.tacks.push_back({cord, {tail + s * gather, 0.5f * width}, panel, at});
    }
    return cord;
}

int addLacing(Garment& g, const std::vector<SeamSide>& sideA, const std::vector<SeamSide>& sideB, int pairs, float inset, float tail,
              float width, std::vector<std::pair<int, Vec2>>* eyelets)
{
    if (sideA.empty() || sideB.empty() || pairs < 1)
        return -1;
    auto chainLen = [&](const std::vector<SeamSide>& c) {
        float l = 0.f;
        for (const SeamSide& s : c)
            l += detailRunLength(g.panels[size_t(s.panel)].outline, s.from, s.to);
        return l;
    };
    // An eyelet a little in from the edge, towards the panel's inside.
    auto eyelet = [&](const std::vector<SeamSide>& c, float s) {
        const auto [panel, at] = alongChain(g, c, s);
        const auto [p2, ahead] = alongChain(g, c, s + 0.005f);
        const float dx = ahead[0] - at[0], dy = ahead[1] - at[1], l = std::hypot(dx, dy);
        const Vec2 in = l > 0.f ? Vec2{-dy / l, dx / l} : Vec2{0.f, 0.f};
        (void)p2;
        return std::pair<int, Vec2>{panel, {at[0] + in[0] * inset, at[1] + in[1] * inset}};
    };
    const float la = chainLen(sideA), lb = chainLen(sideB);
    std::vector<std::pair<int, Vec2>> path;
    for (int k = 0; k < pairs; ++k)
    {
        const float t = (float(k) + 0.5f) / float(pairs);
        // A zigzag from side to side; B runs the other way along its edge, as the two edges of an opening face each other.
        path.push_back(eyelet(sideA, la * t));
        path.push_back(eyelet(sideB, lb * (1.f - t)));
    }
    if (eyelets)
        *eyelets = path;
    std::vector<Vec3> at;
    for (const auto& [panel, q] : path)
        at.push_back(placePatternPoint(g.panels[size_t(panel)], q));
    std::vector<float> s{0.f};
    for (size_t i = 1; i < at.size(); ++i)
        s.push_back(s.back() + std::hypot(at[i][0] - at[i - 1][0], at[i][1] - at[i - 1][1], at[i][2] - at[i - 1][2]));
    const Vec3 along{at.back()[0] - at.front()[0], at.back()[1] - at.front()[1], at.back()[2] - at.front()[2]};
    const int fabric = g.panels[size_t(path.front().first)].fabric;
    const int cord = addCord(g, "Lace", s.back() + 2.f * tail, width, at.front(), along, {0.f, 0.f, 1.f}, fabric);
    for (size_t i = 0; i < path.size(); ++i)
        g.tacks.push_back({cord, {tail + s[i], 0.5f * width}, path[i].first, path[i].second});
    return cord;
}

bool openSeam(Garment& g, int seam, float closed)
{
    if (seam < 0 || size_t(seam) >= g.seams.size())
        return false;
    closed = std::clamp(closed, 0.f, 1.f);
    if (closed >= 1.f)
        return true;
    if (closed <= 0.f)
    {
        g.seams.erase(g.seams.begin() + seam);
        return true;
    }
    Seam& s = g.seams[size_t(seam)];
    if (s.a.size() != 1 || s.b.size() != 1)
        return false;
    // Cut both runs at the same fraction and keep the closed part: from a's start, and from b's start or end as it runs.
    auto cut = [&](SeamSide& side, float f, bool keepStart) {
        Panel& p = g.panels[size_t(side.panel)];
        const float l = detailRunLength(p.outline, side.from, side.to);
        const Vec2 at = alongRun(p.outline, side.from, side.to, f * l);
        // The outline point just before the cut, walking the run.
        const int n = int(p.outline.size());
        int before = side.from;
        float walked = 0.f;
        for (int v = side.from % n; v != side.to % n; v = (v + 1) % n)
        {
            const float step = patternDist(p.outline[size_t(v)], p.outline[size_t((v + 1) % n)]);
            if (walked + step >= f * l)
            {
                before = v;
                break;
            }
            walked += step;
        }
        const int k = before + 1;
        const int panel = side.panel, from = side.from, to = side.to;
        insertOutlinePoint(g, panel, k, at);
        const int nf = from + (from >= k ? 1 : 0), nt = to + (to >= k ? 1 : 0);
        return keepStart ? SeamSide{panel, nf, k} : SeamSide{panel, k, nt};
    };
    const bool rev = s.reversed;
    const SeamSide a = cut(g.seams[size_t(seam)].a.front(), closed, true);
    g.seams[size_t(seam)].a = {a};
    const SeamSide b = cut(g.seams[size_t(seam)].b.front(), rev ? 1.f - closed : closed, !rev);
    g.seams[size_t(seam)].b = {b};
    return true;
}
} // namespace tailor

namespace tailor
{
std::vector<float> bakeWrinkleHeight(const Fabric& fabric, int axis, float compress, int size, float tile, uint32_t seed)
{
    // A square of the fabric, its border pinned flat and drawn in along one axis, buckling into the folds it makes.
    const int n = std::max(9, int(std::lround(tile / 0.005f)) + 1);
    const float h = tile / float(n - 1), c = std::clamp(compress, 0.f, 0.6f);
    ClothMesh m;
    Garment g;
    g.panels.resize(1);
    g.fabrics = {fabric};
    uint32_t state = seed * 2654435761u + 7u;
    auto rnd = [&] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1u << 24) - 0.5f;
    };
    auto squeeze = [&](float u, float v) {
        const float mid = 0.5f * tile;
        return axis == 0 ? Vec2{mid + (u - mid) * (1.f - c), v} : Vec2{u, mid + (v - mid) * (1.f - c)};
    };
    std::vector<Pin> pins;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const float u = h * float(i), v = h * float(j);
            const Vec2 q = squeeze(u, v);
            const bool border = i == 0 || j == 0 || i == n - 1 || j == n - 1;
            m.uv.push_back({u, v});
            m.rest3d.push_back({q[0], q[1], border ? 0.f : 0.0005f * rnd()});
            m.vertPanel.push_back(0);
            if (border)
                pins.push_back({int(m.rest3d.size()) - 1, {q[0], q[1], 0.f}});
        }
    auto id = [&](int i, int j) { return j * n + i; };
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i)
        {
            m.tris.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            m.tris.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
            m.triPanel.push_back(0);
            m.triPanel.push_back(0);
        }
    SolverSettings settings;
    settings.gravity = {0.f, 0.f, 0.f};
    settings.collisionThickness = 0.0005f;
    ClothSolver solver;
    solver.reset(m, g, settings);
    solver.setPins(pins);
    for (int f = 0; f < 150; ++f)
        solver.step();
    // Height over the rest square, by bilinear lookup in its grid: the map tiles because the border stays flat.
    const auto& x = solver.positions();
    std::vector<float> out(size_t(size) * size_t(size), 0.f);
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px)
        {
            const float fu = (float(px) + 0.5f) / float(size) * float(n - 1), fv = (float(py) + 0.5f) / float(size) * float(n - 1);
            const int i = std::min(int(fu), n - 2), j = std::min(int(fv), n - 2);
            const float tu = fu - float(i), tv = fv - float(j);
            const float z = (x[size_t(id(i, j))][2] * (1.f - tu) + x[size_t(id(i + 1, j))][2] * tu) * (1.f - tv) +
                            (x[size_t(id(i, j + 1))][2] * (1.f - tu) + x[size_t(id(i + 1, j + 1))][2] * tu) * tv;
            out[size_t(py) * size_t(size) + size_t(px)] = std::isfinite(z) ? z : 0.f;
        }
    return out;
}

std::vector<float> heightToNormals(const std::vector<float>& height, int size, float tile)
{
    std::vector<float> rgba(size_t(size) * size_t(size) * 4, 1.f);
    const float step = tile / float(size);
    auto at = [&](int px, int py) { return height[size_t((py + size) % size) * size_t(size) + size_t((px + size) % size)]; };
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px)
        {
            // Rows run down the image with V, so green follows the DirectX convention Unreal reads.
            const float du = (at(px + 1, py) - at(px - 1, py)) / (2.f * step), dv = (at(px, py + 1) - at(px, py - 1)) / (2.f * step);
            const float l = std::sqrt(du * du + dv * dv + 1.f);
            float* o = &rgba[(size_t(py) * size_t(size) + size_t(px)) * 4];
            o[0] = 0.5f - 0.5f * du / l;
            o[1] = 0.5f - 0.5f * dv / l;
            o[2] = 0.5f + 0.5f / l;
            o[3] = 1.f;
        }
    return rgba;
}
} // namespace tailor
