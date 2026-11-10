#include "layout/Edit.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace lila::layout
{
namespace
{
using Points = std::vector<glm::dvec3>;

double spacingOf(const PanelLayout& l, const EditContext& c) { return c.spacing > 0.0 ? c.spacing : defaultSpacing(l, c.surface); }

Path pathOf(const PanelLayout& l, const EditContext& c, const Run& r) { return runPath(l, r, c.surface, spacingOf(l, c)); }

void reverseRun(Run& r)
{
    std::swap(r.a, r.b);
    std::reverse(r.controls.begin(), r.controls.end());
}

bool sameLook(const Run& a, const Run& b) { return a.set == b.set && a.width == b.width && a.depth == b.depth && a.profile == b.profile; }

uint32_t addJunction(PanelLayout& l, const EditContext& c, const glm::dvec3& p, bool corner = false)
{
    Junction j;
    j.id = l.newId();
    j.at = c.surface.anchorAt(p);
    j.corner = corner;
    l.junctions.push_back(j);
    return j.id;
}

/** Points move onto the mirror plane when they come within tolerance of it. */
glm::dvec3 onPlane(const PanelLayout& l, const EditContext& c, glm::dvec3 p)
{
    if (l.mirrorAxis >= 0 && std::abs(p[l.mirrorAxis]) < c.tolerance)
        p[l.mirrorAxis] = 0.0;
    return p;
}

void replaceJunction(PanelLayout& l, uint32_t from, uint32_t to)
{
    if (from == to)
        return;
    for (Run& r : l.runs)
    {
        if (r.a == from)
            r.a = to;
        if (r.b == from)
            r.b = to;
    }
    std::erase_if(l.junctions, [from](const Junction& j) { return j.id == from; });
}

/** Closest points between segments p0p1 and q0q1: parameters on each and the distance. */
struct SegClosest
{
    double s = 0.0, t = 0.0, distance = 0.0;
};

SegClosest segmentClosest(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& q0, const glm::dvec3& q1)
{
    const glm::dvec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    double s = 0.0, t = 0.0;
    if (a <= 1e-30 && e <= 1e-30)
        return {0.0, 0.0, glm::length(r)};
    if (a <= 1e-30)
        t = std::clamp(f / e, 0.0, 1.0);
    else
    {
        const double cc = glm::dot(d1, r);
        if (e <= 1e-30)
            s = std::clamp(-cc / a, 0.0, 1.0);
        else
        {
            const double b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom > 1e-30 ? std::clamp((b * f - cc * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0)
            {
                t = 0.0;
                s = std::clamp(-cc / a, 0.0, 1.0);
            }
            else if (t > 1.0)
            {
                t = 1.0;
                s = std::clamp((b - cc) / a, 0.0, 1.0);
            }
        }
    }
    return {s, t, glm::length((p0 + d1 * s) - (q0 + d2 * t))};
}

double side(const glm::dvec3& n, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& p) { return glm::dot(n, glm::cross(b - a, p - a)); }

struct Crossing
{
    double sA = 0.0, sB = 0.0;
    glm::dvec3 point{0.0};
};

/** Where two paths cross on the surface: close in space and on opposite sides of each other in the tangent plane. */
std::vector<Crossing> crossings(const Path& A, const Path& B, double reach)
{
    std::vector<Crossing> out;
    if (A.empty() || B.empty())
        return out;
    for (size_t i = 0; i + 1 < A.size(); ++i)
    {
        const glm::dvec3 a0 = A.points[i], a1 = A.points[i + 1];
        const glm::dvec3 lo = glm::min(a0, a1) - reach, hi = glm::max(a0, a1) + reach;
        for (size_t j = 0; j + 1 < B.size(); ++j)
        {
            const glm::dvec3 b0 = B.points[j], b1 = B.points[j + 1];
            if (glm::any(glm::lessThan(glm::max(b0, b1), lo)) || glm::any(glm::greaterThan(glm::min(b0, b1), hi)))
                continue;
            const SegClosest c = segmentClosest(a0, a1, b0, b1);
            if (c.distance > reach)
                continue;
            const glm::dvec3 n = A.normals[i] + A.normals[i + 1] + B.normals[j] + B.normals[j + 1];
            const double s0 = side(n, a0, a1, b0), s1 = side(n, a0, a1, b1);
            const double t0 = side(n, b0, b1, a0), t1 = side(n, b0, b1, a1);
            if (s0 * s1 > 0.0 || t0 * t1 > 0.0)
                continue;
            Crossing x;
            x.sA = A.lengths[i] + (A.lengths[i + 1] - A.lengths[i]) * c.s;
            x.sB = B.lengths[j] + (B.lengths[j + 1] - B.lengths[j]) * c.t;
            x.point = (glm::mix(a0, a1, c.s) + glm::mix(b0, b1, c.t)) * 0.5;
            if (out.empty() || std::abs(out.back().sA - x.sA) > reach * 2.0)
                out.push_back(x);
        }
    }
    return out;
}

/** Distance along the path to each anchor, by closest point. */
std::vector<double> controlParams(const Path& path, const Surface& s, const std::vector<Anchor>& controls)
{
    std::vector<double> out;
    out.reserve(controls.size());
    for (const Anchor& a : controls)
        out.push_back(nearestOnPath(path, s.resolve(a)).s);
    return out;
}

struct NearRun
{
    uint32_t run = 0;
    PathNearest at;
    Path path;
};

std::optional<NearRun> nearestRun(const PanelLayout& l, const EditContext& c, const glm::dvec3& p, double within, uint32_t exclude = 0)
{
    std::optional<NearRun> best;
    for (const Run& r : l.runs)
    {
        if (r.id == exclude)
            continue;
        Path path = pathOf(l, c, r);
        const PathNearest n = nearestOnPath(path, p);
        if (n.distance <= within && (!best || n.distance < best->at.distance))
            best = NearRun{r.id, n, std::move(path)};
    }
    return best;
}

std::optional<uint32_t> nearestJunction(const PanelLayout& l, const EditContext& c, const glm::dvec3& p, double within)
{
    std::optional<uint32_t> best;
    double bestD = within;
    for (const Junction& j : l.junctions)
    {
        const double d = glm::length(c.surface.resolve(j.at) - p);
        if (d <= bestD)
        {
            bestD = d;
            best = j.id;
        }
    }
    return best;
}

/** Splits run at the path distance s; near an end it gives that end (taking over `existing` when given). */
uint32_t splitAt(PanelLayout& l, const EditContext& c, uint32_t runId, const Path& path, double s, uint32_t existing)
{
    Run* r = l.run(runId);
    if (!r || path.empty())
        return 0;
    const double tol = c.tolerance, len = path.length();
    if (s <= tol || s >= len - tol)
    {
        const uint32_t end = (s <= tol) ? r->a : r->b;
        if (existing && existing != end)
        {
            replaceJunction(l, end, existing);
            return existing;
        }
        return end;
    }
    const glm::dvec3 at = path.pointAt(s);
    const uint32_t j = existing ? existing : addJunction(l, c, at);
    r = l.run(runId);
    const std::vector<double> params = controlParams(path, c.surface, r->controls);
    Run second = *r;
    second.id = l.newId();
    second.a = j;
    second.controls.clear();
    std::vector<Anchor> first;
    for (size_t k = 0; k < r->controls.size(); ++k)
    {
        if (params[k] < s - tol * 0.5)
            first.push_back(r->controls[k]);
        else if (params[k] > s + tol * 0.5)
            second.controls.push_back(r->controls[k]);
    }
    r->controls = std::move(first);
    r->b = j;
    l.runs.push_back(std::move(second));
    return j;
}

/** The junction a stroke end at p joins: one within tolerance, or a new T on a line within tolerance. 0 for none. */
uint32_t snapEnd(PanelLayout& l, const EditContext& c, const glm::dvec3& p, uint32_t excludeRun = 0)
{
    if (auto j = nearestJunction(l, c, p, c.tolerance))
        return *j;
    if (auto n = nearestRun(l, c, p, c.tolerance, excludeRun))
        return splitAt(l, c, n->run, n->path, n->at.s, 0);
    return 0;
}

/** Direction the stroke heads in over its first stretch. */
glm::dvec3 heading(const Points& pts, double reach)
{
    double walked = 0.0;
    for (size_t k = 1; k < pts.size(); ++k)
    {
        walked += glm::length(pts[k] - pts[k - 1]);
        if (walked >= reach || k + 1 == pts.size())
        {
            const glm::dvec3 d = pts[k] - pts[0];
            const double len = glm::length(d);
            return len > 1e-15 ? d / len : glm::dvec3(0.0);
        }
    }
    return glm::dvec3(0.0);
}

/** Splits every crossing between the given runs and all others; the given runs grow as they split. */
void splitCrossings(PanelLayout& l, const EditContext& c, std::vector<uint32_t>& fresh)
{
    const double reach = std::max(c.tolerance * 0.3, spacingOf(l, c) * 0.75);
    for (int guard = 0; guard < 2000; ++guard)
    {
        bool split = false;
        for (size_t i = 0; i < fresh.size() && !split; ++i)
        {
            const Run* n = l.run(fresh[i]);
            if (!n)
                continue;
            const Path pn = pathOf(l, c, *n);
            const double ln = pn.length();
            for (const Run& o : l.runs)
            {
                if (o.id == n->id)
                    continue;
                const Path po = pathOf(l, c, o);
                const double lo = po.length();
                for (const Crossing& x : crossings(pn, po, reach))
                {
                    // Lines meeting at a shared end are joined already.
                    if (x.sA <= c.tolerance || x.sA >= ln - c.tolerance)
                        continue;
                    if (x.sB <= c.tolerance || x.sB >= lo - c.tolerance)
                    {
                        // Crossing at the other line's end: that end becomes the junction here.
                        const uint32_t end = x.sB <= c.tolerance ? o.a : o.b;
                        const size_t before = l.runs.size();
                        splitAt(l, c, n->id, pn, x.sA, end);
                        if (l.runs.size() > before)
                            fresh.push_back(l.runs.back().id);
                        split = true;
                        break;
                    }
                    const uint32_t oid = o.id;
                    const uint32_t j = splitAt(l, c, oid, po, x.sB, 0);
                    const size_t before = l.runs.size();
                    splitAt(l, c, fresh[i], pn, x.sA, j);
                    if (l.runs.size() > before)
                        fresh.push_back(l.runs.back().id);
                    split = true;
                    break;
                }
                if (split)
                    break;
            }
        }
        if (!split)
            return;
    }
}

/** The overdraw a stroke asks for, if it starts on a line heading along it. */
struct Overdraw
{
    uint32_t run = 0;
    double s = 0.0;
    Path path;
};

std::optional<Overdraw> overdrawAt(PanelLayout& l, const EditContext& c, const Points& pts)
{
    const double len = polylineLength(pts);
    if (len < 3.0 * c.tolerance)
        return std::nullopt;
    auto near = nearestRun(l, c, pts.front(), c.tolerance);
    if (!near)
        return std::nullopt;
    const Run* r = l.run(near->run);
    const double runLen = near->path.length();
    const bool atA = near->at.s <= c.tolerance, atB = near->at.s >= runLen - c.tolerance;
    // An end other lines share stays where it is: a stroke from there is a new line.
    if ((atA && l.degree(r->a) > 1 && r->a != r->b) || (atB && l.degree(r->b) > 1 && r->a != r->b))
        return std::nullopt;
    const glm::dvec3 h = heading(pts, std::min(3.0 * c.tolerance, len / 3.0));
    const glm::dvec3 t = near->path.tangentAt(near->at.s);
    if (std::abs(glm::dot(h, t)) < std::cos(glm::radians(30.0)))
        return std::nullopt;
    Overdraw o;
    o.run = near->run;
    o.s = near->at.s;
    o.path = std::move(near->path);
    // Always redraw towards b: a stroke heading towards a redraws the reversed run.
    if (glm::dot(h, t) < 0.0)
    {
        reverseRun(*l.run(o.run));
        std::reverse(o.path.points.begin(), o.path.points.end());
        std::reverse(o.path.normals.begin(), o.path.normals.end());
        o.path.measure();
        o.s = o.path.length() - o.s;
    }
    return o;
}

EditResult applyOverdraw(PanelLayout& l, const EditContext& c, Points pts, const Overdraw& o, const StrokeOptions& opts)
{
    EditResult res;
    Run* r = l.run(o.run);
    const std::vector<double> params = controlParams(o.path, c.surface, r->controls);
    const double tol = c.tolerance;
    const Points fitted = fitPoints(pts, tol, opts.cleanup);

    // The stroke ends back on the same line: only the stretch between is replaced.
    const PathNearest endOn = nearestOnPath(o.path, pts.back());
    std::vector<Anchor> controls;
    for (size_t k = 0; k < r->controls.size(); ++k)
        if (params[k] < o.s - tol)
            controls.push_back(r->controls[k]);
    if (endOn.distance <= tol && endOn.s > o.s + tol)
    {
        for (const glm::dvec3& p : fitted)
            controls.push_back(c.surface.anchorAt(p));
        for (size_t k = 0; k < r->controls.size(); ++k)
            if (params[k] > endOn.s + tol)
                controls.push_back(r->controls[k]);
        r->controls = std::move(controls);
        r->loose = false;
        res.changed = true;
        res.runs = {r->id};
        return res;
    }

    // Otherwise the rest of the line from the start is replaced by the stroke, which brings its own end.
    for (size_t k = 0; k + 1 < fitted.size(); ++k)
        controls.push_back(c.surface.anchorAt(fitted[k]));
    const uint32_t runId = r->id, oldEnd = r->b;
    r->controls = std::move(controls);
    r->loose = false;
    const glm::dvec3 endPos = onPlane(l, c, fitted.back());
    uint32_t end = snapEnd(l, c, endPos, runId);
    r = l.run(runId);
    if (!end)
    {
        if (l.degree(oldEnd) == 1 && r->a != oldEnd)
        {
            l.junction(oldEnd)->at = c.surface.anchorAt(endPos);
            end = oldEnd;
        }
        else
            end = addJunction(l, c, endPos);
    }
    r->b = end;
    res.changed = true;
    res.runs = {runId};
    return res;
}

/** Trims the start of a piece back to the first line it crosses within `reach` of the start; the crossing's junction or 0. */
uint32_t trimOvershoot(PanelLayout& l, const EditContext& c, Points& piece, double reach)
{
    const Path p = surfaceCurve(c.surface, piece, spacingOf(l, c));
    if (p.empty())
        return 0;
    const double crossReach = std::max(c.tolerance * 0.3, spacingOf(l, c) * 0.75);
    std::optional<std::pair<double, std::pair<uint32_t, Crossing>>> first;
    for (const Run& o : l.runs)
    {
        const Path po = pathOf(l, c, o);
        for (const Crossing& x : crossings(p, po, crossReach))
            if (x.sA > 0.0 && x.sA <= reach && x.sA < p.length() - c.tolerance && (!first || x.sA < first->first))
                first = {x.sA, {o.id, x}};
    }
    if (!first)
        return 0;
    const auto& [s, hit] = *first;
    const Run* o = l.run(hit.first);
    const Path po = pathOf(l, c, *o);
    const uint32_t j = splitAt(l, c, hit.first, po, hit.second.sB, 0);
    Points trimmed = {hit.second.point};
    for (const glm::dvec3& q : piece)
        if (nearestOnPath(p, q).s > s + c.tolerance * 0.5)
            trimmed.push_back(q);
    if (trimmed.size() < 2)
        trimmed.push_back(piece.back());
    piece = std::move(trimmed);
    return j;
}

void reflect(Points& pts, int axis)
{
    for (glm::dvec3& p : pts)
        p[axis] = -p[axis];
}
} // namespace

void tidy(PanelLayout& l, const EditContext& c)
{
    // Runs that close on one junction with too little to be a loop, or that join two coincident junctions directly.
    std::erase_if(l.runs, [&](const Run& r) {
        if (!l.junction(r.a) || !l.junction(r.b))
            return true;
        if (r.a == r.b)
            return r.controls.size() < 2;
        return r.controls.empty() && glm::length(c.surface.resolve(l.junction(r.a)->at) - c.surface.resolve(l.junction(r.b)->at)) < 1e-9;
    });
    for (bool merged = true; merged;)
    {
        merged = false;
        for (const Junction& j : l.junctions)
        {
            if (j.corner || l.degree(j.id) != 2)
                continue;
            const std::vector<uint32_t> at = l.runsAt(j.id);
            if (at.size() != 2)
                continue;
            Run* r1 = l.run(at[0]);
            Run* r2 = l.run(at[1]);
            if (!sameLook(*r1, *r2))
                continue;
            if (r1->b != j.id)
                reverseRun(*r1);
            if (r2->a != j.id)
                reverseRun(*r2);
            r1->controls.push_back(j.at);
            r1->controls.insert(r1->controls.end(), r2->controls.begin(), r2->controls.end());
            r1->b = r2->b;
            r1->loose = r1->loose || r2->loose;
            const uint32_t gone = r2->id, jid = j.id;
            std::erase_if(l.runs, [gone](const Run& r) { return r.id == gone; });
            std::erase_if(l.junctions, [jid](const Junction& x) { return x.id == jid; });
            merged = true;
            break;
        }
    }
    l.removeUnusedJunctions();
}

EditResult addStroke(PanelLayout& l, const EditContext& c, std::span<const glm::dvec3> stroke, const StrokeOptions& opts)
{
    LILA_ZONE();
    EditResult res;
    Points pts(stroke.begin(), stroke.end());
    if (pts.size() < 2 || polylineLength(pts) < c.tolerance)
    {
        res.note = "stroke too short";
        return res;
    }
    if (l.sets.empty())
    {
        const PanelLayout d = PanelLayout::withDefaultSets();
        l.sets = d.sets;
        l.activeSet = d.activeSet;
        l.nextId = std::max(l.nextId, d.nextId);
    }
    // Strokes on the generated side of a mirror are drawn on the authored side.
    if (l.mirrorAxis >= 0)
    {
        double sum = 0.0;
        for (const glm::dvec3& p : pts)
            sum += p[l.mirrorAxis];
        if (sum * l.mirrorSide < 0.0)
            reflect(pts, l.mirrorAxis);
    }

    if (auto o = overdrawAt(l, c, pts))
        res = applyOverdraw(l, c, pts, *o, opts);
    else
    {
        Points rev(pts.rbegin(), pts.rend());
        if (auto oe = overdrawAt(l, c, rev))
            res = applyOverdraw(l, c, rev, *oe, opts);
    }
    if (res.changed)
    {
        tidy(l, c);
        std::erase_if(res.runs, [&](uint32_t id) { return !l.run(id); });
        return res;
    }

    FitOptions fo;
    fo.tolerance = c.tolerance;
    fo.cleanup = opts.cleanup;
    fo.cornerAngle = opts.cornerAngle;
    fo.straight = opts.straight;
    fo.straightenLast = opts.straightenLast;
    FitResult fit = fitStroke(pts, fo);
    if (fit.pieces.empty())
    {
        res.note = "stroke too short";
        return res;
    }
    const uint32_t set = opts.set ? opts.set : l.activeSet;
    const double overshoot = opts.overshoot > 0.0 ? opts.overshoot : 3.0 * c.tolerance;

    std::vector<Points> pieces;
    for (FitPiece& p : fit.pieces)
        pieces.push_back(std::move(p.points));

    uint32_t start = snapEnd(l, c, pieces.front().front());
    if (!start)
        start = trimOvershoot(l, c, pieces.front(), overshoot);
    uint32_t end = 0;
    if (!fit.closed)
    {
        end = snapEnd(l, c, pieces.back().back());
        if (!end)
        {
            std::reverse(pieces.back().begin(), pieces.back().end());
            end = trimOvershoot(l, c, pieces.back(), overshoot);
            std::reverse(pieces.back().begin(), pieces.back().end());
        }
    }
    if (!start)
        start = addJunction(l, c, onPlane(l, c, pieces.front().front()), fit.closed && fit.closedCorner);
    if (fit.closed)
        end = start;
    if (!end)
        end = addJunction(l, c, onPlane(l, c, pieces.back().back()));

    std::vector<uint32_t> fresh;
    uint32_t from = start;
    for (size_t k = 0; k < pieces.size(); ++k)
    {
        const uint32_t to = k + 1 == pieces.size() ? end : addJunction(l, c, pieces[k].back(), true);
        Run r;
        r.id = l.newId();
        r.a = from;
        r.b = to;
        r.set = set;
        for (size_t i = 1; i + 1 < pieces[k].size(); ++i)
            r.controls.push_back(c.surface.anchorAt(pieces[k][i]));
        l.runs.push_back(std::move(r));
        fresh.push_back(l.runs.back().id);
        from = to;
    }
    splitCrossings(l, c, fresh);
    tidy(l, c);
    res.changed = true;
    for (uint32_t id : fresh)
        if (l.run(id))
            res.runs.push_back(id);
    return res;
}

EditResult eraseStroke(PanelLayout& l, const EditContext& c, std::span<const glm::dvec3> stroke)
{
    EditResult res;
    if (stroke.empty())
        return res;
    Points pts(stroke.begin(), stroke.end());
    if (pts.size() == 1)
        pts.push_back(pts[0]);
    std::vector<uint32_t> gone;
    for (const Run& r : l.runs)
    {
        const Path p = pathOf(l, c, r);
        bool hit = false;
        for (size_t i = 0; i + 1 < p.size() && !hit; ++i)
            for (size_t k = 0; k + 1 < pts.size() && !hit; ++k)
                hit = segmentClosest(p.points[i], p.points[i + 1], pts[k], pts[k + 1]).distance <= c.tolerance;
        if (hit)
            gone.push_back(r.id);
    }
    if (gone.empty())
        return res;
    return deleteRuns(l, c, gone);
}

EditResult smoothStroke(PanelLayout& l, const EditContext& c, std::span<const glm::dvec3> stroke, double radius, double strength)
{
    EditResult res;
    if (stroke.empty() || radius <= 0.0)
        return res;
    Points pts(stroke.begin(), stroke.end());
    std::vector<double> lens(pts.size(), 0.0);
    for (size_t k = 1; k < pts.size(); ++k)
        lens[k] = lens[k - 1] + glm::length(pts[k] - pts[k - 1]);
    strength = std::clamp(strength, 0.0, 1.0);
    for (Run& r : l.runs)
    {
        if (r.controls.empty())
            continue;
        const Points through = runThrough(l, r, c.surface);
        bool moved = false;
        for (size_t k = 1; k + 1 < through.size(); ++k)
        {
            const double d = nearestOnPath(pts, lens, through[k]).distance;
            if (d >= radius)
                continue;
            const double f = 1.0 - d / radius, w = strength * f * f;
            const glm::dvec3 target = (through[k - 1] + through[k + 1]) * 0.5;
            r.controls[k - 1] = c.surface.anchorAt(glm::mix(through[k], target, w));
            moved = true;
        }
        if (moved)
            res.runs.push_back(r.id);
    }
    res.changed = !res.runs.empty();
    return res;
}

EditResult moveJunction(PanelLayout& l, const EditContext& c, uint32_t id, const glm::dvec3& to)
{
    EditResult res;
    Junction* j = l.junction(id);
    if (!j)
        return res;
    j->at = c.surface.anchorAt(onPlane(l, c, to));
    res.changed = true;
    res.junctions = {id};
    res.runs = l.runsAt(id);
    return res;
}

EditResult bendRun(PanelLayout& l, const EditContext& c, uint32_t id, const glm::dvec3& grab, const glm::dvec3& to)
{
    EditResult res;
    Run* r = l.run(id);
    if (!r)
        return res;
    const Path path = pathOf(l, c, *r);
    if (path.empty())
        return res;
    const double sGrab = nearestOnPath(path, grab).s;
    const std::vector<double> params = controlParams(path, c.surface, r->controls);
    const Anchor moved = c.surface.anchorAt(to);
    size_t nearest = params.size();
    double best = path.length() * 0.15;
    for (size_t k = 0; k < params.size(); ++k)
        if (std::abs(params[k] - sGrab) < best)
        {
            best = std::abs(params[k] - sGrab);
            nearest = k;
        }
    if (nearest < params.size())
        r->controls[nearest] = moved;
    else
    {
        const size_t at = size_t(std::upper_bound(params.begin(), params.end(), sGrab) - params.begin());
        r->controls.insert(r->controls.begin() + ptrdiff_t(at), moved);
    }
    res.changed = true;
    res.runs = {id};
    return res;
}

EditResult splitRun(PanelLayout& l, const EditContext& c, uint32_t id, const glm::dvec3& at, uint32_t junction)
{
    EditResult res;
    const Run* r = l.run(id);
    if (!r)
        return res;
    const Path path = pathOf(l, c, *r);
    const uint32_t j = splitAt(l, c, id, path, nearestOnPath(path, at).s, junction);
    if (!j)
        return res;
    res.changed = true;
    res.junctions = {j};
    res.runs = l.runsAt(j);
    return res;
}

EditResult toggleCorner(PanelLayout& l, uint32_t id)
{
    EditResult res;
    if (Junction* j = l.junction(id))
    {
        j->corner = !j->corner;
        res.changed = true;
        res.junctions = {id};
    }
    return res;
}

EditResult deleteRuns(PanelLayout& l, const EditContext& c, std::span<const uint32_t> ids)
{
    EditResult res;
    const size_t before = l.runs.size();
    std::erase_if(l.runs, [&](const Run& r) { return std::find(ids.begin(), ids.end(), r.id) != ids.end(); });
    res.changed = l.runs.size() != before;
    if (res.changed)
        tidy(l, c);
    return res;
}

EditResult deleteJunctions(PanelLayout& l, const EditContext& c, std::span<const uint32_t> ids)
{
    EditResult res;
    for (uint32_t id : ids)
    {
        Junction* j = l.junction(id);
        if (!j)
            continue;
        const std::vector<uint32_t> at = l.runsAt(id);
        if (at.size() == 2 && l.degree(id) == 2)
        {
            // Two runs meeting: the junction becomes a control of the joined run, whatever their sets.
            j->corner = false;
            Run* r2 = l.run(at[1]);
            Run* r1 = l.run(at[0]);
            r2->set = r1->set;
            r2->width = r1->width;
            r2->depth = r1->depth;
            r2->profile = r1->profile;
        }
        else
            std::erase_if(l.runs, [id](const Run& r) { return r.a == id || r.b == id; });
        res.changed = true;
    }
    if (res.changed)
        tidy(l, c);
    return res;
}

EditResult offsetRuns(PanelLayout& l, const EditContext& c, std::span<const uint32_t> ids, double distance)
{
    EditResult res;
    if (distance == 0.0)
        return res;
    std::vector<Run> made;
    for (uint32_t id : ids)
    {
        const Run* r = l.run(id);
        if (!r)
            continue;
        const Path p = pathOf(l, c, *r);
        if (p.empty())
            continue;
        Points shifted;
        for (size_t k = 0; k < p.size(); ++k)
        {
            const glm::dvec3 t = p.tangentAt(p.lengths[k]);
            glm::dvec3 sideDir = glm::cross(p.normals[k], t);
            const double len = glm::length(sideDir);
            if (len < 1e-12)
                continue;
            sideDir /= len;
            shifted.push_back(c.surface.project(p.points[k] + sideDir * distance).point);
        }
        if (shifted.size() < 2)
            continue;
        const Points fitted = fitPoints(shifted, c.tolerance, 0.5);
        Run copy = *r;
        copy.id = l.newId();
        copy.loose = false;
        copy.controls.clear();
        const bool loop = r->a == r->b;
        copy.a = addJunction(l, c, fitted.front());
        copy.b = loop ? copy.a : addJunction(l, c, fitted.back());
        for (size_t k = 1; k + 1 < fitted.size(); ++k)
            copy.controls.push_back(c.surface.anchorAt(fitted[k]));
        made.push_back(std::move(copy));
    }
    for (Run& r : made)
    {
        res.runs.push_back(r.id);
        l.runs.push_back(std::move(r));
    }
    res.changed = !res.runs.empty();
    if (res.changed)
    {
        std::vector<uint32_t> fresh = res.runs;
        splitCrossings(l, c, fresh);
        tidy(l, c);
        res.runs.clear();
        for (uint32_t id : fresh)
            if (l.run(id))
                res.runs.push_back(id);
    }
    return res;
}

EditResult loosenRuns(PanelLayout& l, const EditContext& c, std::span<const uint32_t> ids)
{
    EditResult res;
    const double step = std::max(c.tolerance, spacingOf(l, c) * 2.0);
    for (uint32_t id : ids)
    {
        Run* r = l.run(id);
        if (!r)
            continue;
        const Path p = pathOf(l, c, *r);
        if (p.empty())
            continue;
        const Points dense = resample(p.points, step);
        r->controls.clear();
        for (size_t k = 1; k + 1 < dense.size(); ++k)
            r->controls.push_back(c.surface.anchorAt(dense[k]));
        r->loose = true;
        res.runs.push_back(id);
    }
    res.changed = !res.runs.empty();
    return res;
}

EditResult refitRuns(PanelLayout& l, const EditContext& c, double cleanup, std::span<const uint32_t> ids)
{
    EditResult res;
    for (Run& r : l.runs)
    {
        const bool named = std::find(ids.begin(), ids.end(), r.id) != ids.end();
        if (!(ids.empty() ? r.loose : named))
            continue;
        const Path p = pathOf(l, c, r);
        if (p.empty())
            continue;
        const Points fitted = (chordDeviation(p.points) < c.tolerance * (0.4 + 1.6 * cleanup) && r.a != r.b)
                                  ? Points{p.points.front(), p.points.back()}
                                  : fitPoints(p.points, c.tolerance, cleanup);
        r.controls.clear();
        for (size_t k = 1; k + 1 < fitted.size(); ++k)
            r.controls.push_back(c.surface.anchorAt(fitted[k]));
        r.loose = false;
        res.runs.push_back(r.id);
    }
    res.changed = !res.runs.empty();
    return res;
}

std::vector<uint32_t> chainOf(const PanelLayout& l, uint32_t id)
{
    std::vector<uint32_t> out;
    const Run* start = l.run(id);
    if (!start)
        return out;
    out.push_back(id);
    // Walk from each end through junctions where exactly two runs meet.
    for (int dir = 0; dir < 2; ++dir)
    {
        uint32_t at = dir == 0 ? start->b : start->a, prev = id;
        for (int guard = 0; guard < 100000; ++guard)
        {
            if (l.degree(at) != 2)
                break;
            const std::vector<uint32_t> runs = l.runsAt(at);
            if (runs.size() != 2)
                break;
            const uint32_t next = runs[0] == prev ? runs[1] : runs[0];
            if (std::find(out.begin(), out.end(), next) != out.end())
                break;
            if (dir == 0)
                out.push_back(next);
            else
                out.insert(out.begin(), next);
            const Run* n = l.run(next);
            at = n->a == at ? n->b : n->a;
            prev = next;
        }
    }
    return out;
}
} // namespace lila::layout
