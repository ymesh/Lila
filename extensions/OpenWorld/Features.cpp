// OSM features through the remix: every node placed like the heights under it, ways cut where regions were cut and
// pasted, buildings kept whole, other areas clipped at region edges, relations following their ways. Works on nodes
// only, so any feature kind comes along.

#include "RemixWork.h"

#include <lila/Profile.h>

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace openworld::detail
{
namespace
{
/** A piece of the output: 0 the (warped) background, r + 1 region r's paste. */
struct Pieces
{
    const Work& w;
    size_t source;
    /** The background's forward map per base point, memoised by node. */
    Vec2 map(int piece, Vec2 b) const
    {
        if (piece == 0)
            return w.warp->forward(b);
        return w.spec.regions[size_t(piece - 1)].rigid().apply(b);
    }
    bool present(int piece, Vec2 b) const
    {
        if (!w.owns(source, b))
            return false;
        if (piece == 0)
            return !w.punched(b) && !w.covered(w.warp->forward(b), -1);
        const size_t r = size_t(piece - 1);
        return inside(w.spec.regions[r].polygon, b) && !w.covered(w.spec.regions[r].rigid().apply(b), int(r));
    }
    /** The edge between a present at a and absent at b, on the base map. */
    Vec2 boundary(int piece, Vec2 a, Vec2 b) const
    {
        double lo = 0.0, hi = 1.0;
        for (int it = 0; it < 28; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (present(piece, a + (b - a) * mid) ? lo : hi) = mid;
        }
        return a + (b - a) * lo;
    }
};

bool isBuilding(const OsmTags& t) { return !tagOf(t, "building").empty() || !tagOf(t, "building:part").empty() || tagOf(t, "type") == "building"; }

std::string flippedRole(const std::string& role) { return role == "outer" ? "inner" : role == "inner" ? "outer" : role; }

Clipper2Lib::PathsD clipPaths(const Polygon& p)
{
    Clipper2Lib::PathD path;
    path.reserve(p.size());
    for (Vec2 v : p)
        path.push_back({v.x, v.y});
    return {path};
}

/** A ring with no edge longer than step, so a warped outline follows the warp between its corners. */
std::vector<Vec2> densified(const Clipper2Lib::PathD& ring, double step)
{
    std::vector<Vec2> out;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const Vec2 a(ring[i].x, ring[i].y), b(ring[(i + 1) % ring.size()].x, ring[(i + 1) % ring.size()].y);
        out.push_back(a);
        const int n = int(glm::length(b - a) / step);
        for (int k = 1; k < n; ++k)
            out.push_back(a + (b - a) * (double(k) / n));
    }
    return out;
}

struct Run
{
    std::vector<int64_t> ids;
    bool startCut = false;
    bool endCut = false;
    double startAt = 0.0;
    double endAt = 0.0;
    int piece = 0;
    size_t way = 0;
    Vec2 baseStart{0.0};
    Vec2 baseEnd{0.0};
};
} // namespace

void moveFeatures(Work& w, Features& f)
{
    LILA_ZONE();

    std::set<int64_t> usedNodes, usedWays, usedRelations;
    std::vector<int> pieces{0};
    for (size_t r = 0; r < w.spec.regions.size(); ++r)
        if (pastes(w.spec.regions[r].op))
            pieces.push_back(int(r) + 1);
    const double probe = std::max(w.spec.seams.width * 0.5, w.out->after.cell);
    // Every region outline on the base map: a segment can only enter or leave a hole or paste where it crosses one.
    std::vector<Polygon> outlines;
    for (const Region& reg : w.spec.regions)
    {
        outlines.push_back(reg.polygon);
        outlines.push_back(reg.target());
    }
    auto crossings = [&](Vec2 a, Vec2 b) {
        std::vector<double> ts;
        const Vec2 d = b - a;
        for (const Polygon& poly : outlines)
            for (size_t e = 0; e < poly.size(); ++e)
            {
                const Vec2 p0 = poly[e], p1 = poly[(e + 1) % poly.size()], q = p1 - p0;
                const double den = d.x * q.y - d.y * q.x;
                if (std::abs(den) < 1e-12)
                    continue;
                const Vec2 ap = p0 - a;
                const double t = (ap.x * q.y - ap.y * q.x) / den, u = (ap.x * d.y - ap.y * d.x) / den;
                if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0)
                    ts.push_back(t);
            }
        return ts;
    };

    for (size_t s = 0; s < w.data.size(); ++s)
    {
        if (!w.data[s] || w.data[s]->osm.empty())
            continue;
        const SourceData& d = *w.data[s];
        const OsmDoc& doc = d.osm;
        const Source& src = w.spec.sources[s];
        const Pieces P{w, s};

        std::unordered_map<int64_t, Vec2> base;
        base.reserve(doc.nodes.size());
        for (const OsmDoc::Node& n : doc.nodes)
            base[n.id] = placeOnBase(src, w.spec.horizontal, d.geo.toLocal(n.at));
        std::map<std::pair<int, int64_t>, int64_t> instances;
        std::map<std::pair<int, int64_t>, bool> presentMemo;
        auto present = [&](int piece, int64_t id) {
            const auto key = std::pair(piece, id);
            if (auto it = presentMemo.find(key); it != presentMemo.end())
                return it->second;
            return presentMemo[key] = P.present(piece, base.at(id));
        };
        auto instance = [&](int piece, int64_t id) {
            const auto key = std::pair(piece, id);
            if (auto it = instances.find(key); it != instances.end())
                return it->second;
            const OsmDoc::Node* n = doc.node(id);
            const Vec2 at = P.map(piece, base.at(id));
            int64_t out;
            if (piece == 0 && usedNodes.insert(id).second)
            {
                out = id;
                f.nodeAt[out] = f.nodes.size();
                f.nodes.push_back({out, at, n ? n->tags : OsmTags{}});
            }
            else
                out = f.addNode(at, n ? n->tags : OsmTags{});
            usedNodes.insert(out);
            return instances[key] = out;
        };
        auto cutNode = [&](int piece, Vec2 b) {
            const int64_t id = f.addNode(P.map(piece, b), {{"remix", "cut"}});
            usedNodes.insert(id);
            return id;
        };
        auto wayId = [&](int64_t orig, bool first) {
            if (first && usedWays.insert(orig).second)
                return orig;
            int64_t id = f.nextId++;
            usedWays.insert(id);
            return id;
        };

        std::map<int64_t, std::vector<int64_t>> derived;
        // Ring ways made by clipping that are holes of the original ring: their relation role flips.
        std::set<int64_t> flipped;
        std::set<int64_t> members;
        // Building relations go whole: every member way follows the piece holding most of the relation.
        std::unordered_map<int64_t, int> wholePiece;
        std::unordered_map<int64_t, const OsmDoc::Way*> wayById;
        for (const OsmDoc::Way& mw : doc.ways)
            wayById[mw.id] = &mw;
        for (const OsmDoc::Relation& rel : doc.relations)
        {
            const bool building = isBuilding(rel.tags);
            std::map<int, size_t> count;
            for (const OsmDoc::Member& m : rel.members)
                if (m.type == 'w')
                {
                    members.insert(m.ref);
                    if (!building)
                        continue;
                    if (const auto it = wayById.find(m.ref); it != wayById.end())
                        for (int64_t id : it->second->nodes)
                            if (base.count(id))
                                for (int p : pieces)
                                    count[p] += present(p, id);
                }
            if (!building)
                continue;
            int best = -1;
            size_t bestCount = 0;
            for (const auto& [p, c] : count)
                if (c > bestCount)
                    best = p, bestCount = c;
            for (const OsmDoc::Member& m : rel.members)
                if (m.type == 'w' && best >= 0)
                    wholePiece[m.ref] = best;
        }
        Polygon footprint = w.footprints[s];
        for (const OsmDoc::Way& way : doc.ways)
        {
            std::vector<int64_t> refs;
            for (int64_t id : way.nodes)
                if (base.count(id))
                    refs.push_back(id);
            if (refs.size() < 2)
                continue;
            const WayKind kind = wayKind(way);
            const bool closed = refs.front() == refs.back();
            const std::string cls(kind == WayKind::Road ? tagOf(way.tags, "highway") : kind == WayKind::Coastline ? "coastline" : "");

            const auto whole = wholePiece.find(way.id);
            if (kind == WayKind::Area || whole != wholePiece.end())
            {
                // Buildings go whole with the piece holding most of them; other areas crossing a region's edge are clipped there.
                const size_t ringNodes = refs.size() - (closed ? 1 : 0);
                int best = -1;
                size_t bestCount = 0;
                for (int p : pieces)
                {
                    size_t c = 0;
                    for (size_t i = 0; i < ringNodes; ++i)
                        c += present(p, refs[i]);
                    if (c > bestCount)
                        best = p, bestCount = c;
                }
                if (whole != wholePiece.end())
                    best = whole->second;
                const bool building = whole != wholePiece.end() || isBuilding(way.tags);
                if (!building && closed && ringNodes >= 3)
                {
                    Polygon ring;
                    for (size_t i = 0; i < ringNodes; ++i)
                        ring.push_back(base.at(refs[i]));
                    bool split = bestCount < ringNodes;
                    for (size_t r = 0; r < w.spec.regions.size() && !split; ++r)
                    {
                        const Region& reg = w.spec.regions[r];
                        if (!punches(reg.op) && !pastes(reg.op))
                            continue;
                        if (!overlaps(ring, reg.polygon))
                            continue;
                        bool allIn = true;
                        for (Vec2 v : ring)
                            allIn = allIn && inside(reg.polygon, v);
                        split = !allIn;
                    }
                    if (split)
                    {
                        const Clipper2Lib::PathsD subject = Clipper2Lib::Intersect(clipPaths(ring), clipPaths(footprint), Clipper2Lib::FillRule::NonZero);
                        Clipper2Lib::PathsD punched;
                        for (const Region& reg : w.spec.regions)
                            if (punches(reg.op))
                                punched = Clipper2Lib::Union(punched, clipPaths(reg.polygon), Clipper2Lib::FillRule::NonZero);
                        const double minArea = 0.25 * w.out->after.cell * w.out->after.cell;
                        bool firstWay = true;
                        for (int p : pieces)
                        {
                            const Clipper2Lib::PathsD part =
                                p == 0 ? Clipper2Lib::Difference(subject, punched, Clipper2Lib::FillRule::NonZero)
                                       : Clipper2Lib::Intersect(subject, clipPaths(w.spec.regions[size_t(p - 1)].polygon), Clipper2Lib::FillRule::NonZero);
                            std::vector<std::pair<int64_t, bool>> rings;
                            for (const Clipper2Lib::PathD& path : part)
                            {
                                if (path.size() < 3 || std::abs(Clipper2Lib::Area(path)) < minArea)
                                    continue;
                                const bool hole = !Clipper2Lib::IsPositive(path);
                                Features::Way out{firstWay && p == 0 ? wayId(way.id, true) : wayId(way.id, false), {}, {}, kind, false};
                                firstWay = false;
                                for (Vec2 b : densified(path, probe))
                                    out.nodes.push_back(cutNode(p, b));
                                for (int64_t id : out.nodes)
                                    f.node(id).tags.clear();
                                out.nodes.push_back(out.nodes.front());
                                rings.push_back({out.id, hole});
                                f.ways.push_back(std::move(out));
                            }
                            if (rings.empty())
                                continue;
                            if (members.count(way.id))
                            {
                                // A ring of a relation: the relation picks the pieces up through derived, holes with the role flipped.
                                for (size_t k = 0; k < rings.size(); ++k)
                                {
                                    f.ways[f.ways.size() - rings.size() + k].tags = way.tags;
                                    derived[way.id].push_back(rings[k].first);
                                    if (rings[k].second)
                                        flipped.insert(rings[k].first);
                                }
                            }
                            else if (rings.size() == 1 && !rings[0].second)
                                f.ways.back().tags = way.tags;
                            else
                            {
                                OsmDoc::Relation mp{f.nextId++, {}, way.tags};
                                mp.tags["type"] = "multipolygon";
                                for (const auto& [id, hole] : rings)
                                    mp.members.push_back({'w', id, hole ? "inner" : "outer"});
                                usedRelations.insert(mp.id);
                                f.relations.push_back(std::move(mp));
                            }
                        }
                        continue;
                    }
                }
                if (best < 0)
                    continue;
                Features::Way out{wayId(way.id, best == 0), {}, way.tags, kind, false};
                for (int64_t id : refs)
                    out.nodes.push_back(instance(best, id));
                derived[way.id].push_back(out.id);
                f.ways.push_back(std::move(out));
                continue;
            }

            std::vector<Run> runs;
            for (int p : pieces)
            {
                Run run;
                run.piece = p;
                auto finish = [&](bool endCut, double at, Vec2 baseEnd) {
                    run.endCut = endCut;
                    run.endAt = at;
                    run.baseEnd = baseEnd;
                    if (run.ids.size() >= 2)
                        runs.push_back(run);
                    run = Run();
                    run.piece = p;
                };
                for (size_t i = 0; i < refs.size(); ++i)
                {
                    const bool here = present(p, refs[i]);
                    const Vec2 b = base.at(refs[i]);
                    if (here)
                    {
                        if (run.ids.empty())
                        {
                            if (i > 0)
                            {
                                const Vec2 c = P.boundary(p, b, base.at(refs[i - 1]));
                                run.ids.push_back(cutNode(p, c));
                                run.startCut = true;
                                run.baseStart = c;
                                run.startAt = double(i) - glm::length(b - c) / std::max(1e-9, glm::length(b - base.at(refs[i - 1])));
                            }
                            else
                                run.startAt = 0.0;
                        }
                        run.ids.push_back(instance(p, refs[i]));
                        // A long segment may cross a small hole or paste with both ends present.
                        if (i + 1 < refs.size() && present(p, refs[i + 1]))
                        {
                            const Vec2 nb = base.at(refs[i + 1]);
                            const double len = glm::length(nb - b);
                            const int samples = int(len / probe);
                            std::vector<double> ts = crossings(b, nb);
                            for (int k = 1; k < samples; ++k)
                                ts.push_back(double(k) / samples);
                            ts.push_back(0.0);
                            ts.push_back(1.0);
                            std::sort(ts.begin(), ts.end());
                            double firstOut = -1.0, lastOut = -1.0;
                            for (size_t k = 0; k + 1 < ts.size(); ++k)
                            {
                                if (ts[k + 1] - ts[k] < 1e-9)
                                    continue;
                                const double t = 0.5 * (ts[k] + ts[k + 1]);
                                if (!P.present(p, b + (nb - b) * t))
                                {
                                    if (firstOut < 0.0)
                                        firstOut = t;
                                    lastOut = t;
                                }
                            }
                            if (firstOut > 0.0)
                            {
                                const Vec2 exit = P.boundary(p, b, b + (nb - b) * firstOut);
                                run.ids.push_back(cutNode(p, exit));
                                finish(true, double(i) + firstOut, exit);
                                const Vec2 entry = P.boundary(p, nb, b + (nb - b) * lastOut);
                                run.ids.push_back(cutNode(p, entry));
                                run.startCut = true;
                                run.baseStart = entry;
                                run.startAt = double(i) + lastOut;
                            }
                        }
                    }
                    else if (!run.ids.empty())
                    {
                        const Vec2 c = P.boundary(p, base.at(refs[i - 1]), b);
                        run.ids.push_back(cutNode(p, c));
                        finish(true, double(i) - 1.0 + glm::length(c - base.at(refs[i - 1])) / std::max(1e-9, glm::length(b - base.at(refs[i - 1]))), c);
                    }
                }
                if (!run.ids.empty())
                    finish(false, double(refs.size() - 1), base.at(refs.back()));
            }
            if (runs.empty())
                continue;
            // A closed line cut somewhere: its last and first runs of a piece meet at the closing node.
            if (closed)
                for (int p : pieces)
                {
                    auto first = std::find_if(runs.begin(), runs.end(), [&](const Run& r) { return r.piece == p && !r.startCut && r.startAt == 0.0; });
                    auto last = std::find_if(runs.begin(), runs.end(), [&](const Run& r) { return r.piece == p && !r.endCut && r.endAt == double(refs.size() - 1); });
                    if (first != runs.end() && last != runs.end() && first != last)
                    {
                        Run merged = *last;
                        merged.ids.insert(merged.ids.end(), first->ids.begin() + 1, first->ids.end());
                        merged.endCut = first->endCut;
                        merged.endAt = first->endAt + double(refs.size() - 1);
                        merged.baseEnd = first->baseEnd;
                        const size_t fi = size_t(first - runs.begin()), li = size_t(last - runs.begin());
                        runs[li] = merged;
                        runs.erase(runs.begin() + std::ptrdiff_t(fi));
                    }
                }
            bool firstOut = true;
            std::vector<std::pair<Run*, size_t>> chain;
            for (Run& r : runs)
            {
                Features::Way out{wayId(way.id, firstOut && r.piece == 0), r.ids, way.tags, kind, false};
                firstOut = false;
                r.way = f.ways.size();
                derived[way.id].push_back(out.id);
                f.ways.push_back(std::move(out));
                if (kind == WayKind::River)
                    f.riverBaseEnd[r.way] = r.baseEnd;
                if (kind != WayKind::Road && kind != WayKind::Coastline)
                    continue;
                size_t startEnd = SIZE_MAX, endEnd = SIZE_MAX;
                // A road running into a sea region ends at the new shore: nothing to join it to.
                const bool road = kind == WayKind::Road;
                if (r.startCut && !(road && w.onSeaEdge(r.baseStart)))
                {
                    startEnd = f.ends.size();
                    f.ends.push_back({r.ids.front(), r.way, true, kind, cls});
                }
                if (r.endCut && !(road && w.onSeaEdge(r.baseEnd)))
                {
                    endEnd = f.ends.size();
                    f.ends.push_back({r.ids.back(), r.way, false, kind, cls});
                }
                const bool duplicate = r.piece > 0 && w.spec.regions[size_t(r.piece - 1)].op == RegionOp::Duplicate;
                if (!duplicate)
                    chain.push_back({&r, (startEnd << 32) ^ endEnd});
            }
            // Partners: consecutive runs along the original way (duplicates excluded, the original stays).
            std::sort(chain.begin(), chain.end(), [](const auto& a, const auto& b) { return a.first->startAt < b.first->startAt; });
            for (size_t k = 0; k + 1 < chain.size(); ++k)
            {
                const Run& a = *chain[k].first;
                const Run& b = *chain[k + 1].first;
                if (!a.endCut || !b.startCut)
                    continue;
                size_t ea = SIZE_MAX, eb = SIZE_MAX;
                for (size_t e = 0; e < f.ends.size(); ++e)
                {
                    if (f.ends[e].way == a.way && !f.ends[e].start)
                        ea = e;
                    if (f.ends[e].way == b.way && f.ends[e].start)
                        eb = e;
                }
                if (ea != SIZE_MAX && eb != SIZE_MAX)
                    f.partners.push_back({ea, eb});
            }
        }

        // Points of interest and other tagged nodes not on a way.
        for (const OsmDoc::Node& n : doc.nodes)
        {
            if (n.tags.empty())
                continue;
            for (int p : pieces)
                if (!instances.count({p, n.id}) && present(p, n.id))
                    instance(p, n.id);
        }

        for (const OsmDoc::Relation& rel : doc.relations)
        {
            OsmDoc::Relation out{0, {}, rel.tags};
            for (const OsmDoc::Member& m : rel.members)
            {
                if (m.type == 'w')
                {
                    if (auto it = derived.find(m.ref); it != derived.end())
                        for (int64_t id : it->second)
                            out.members.push_back({'w', id, flipped.count(id) ? flippedRole(m.role) : m.role});
                }
                else if (m.type == 'n')
                {
                    for (int p : pieces)
                        if (base.count(m.ref) && present(p, m.ref))
                            out.members.push_back({'n', instance(p, m.ref), m.role});
                }
                else
                    out.members.push_back(m);
            }
            if (out.members.empty())
                continue;
            if (usedRelations.insert(rel.id).second)
                out.id = rel.id;
            else
            {
                out.id = f.nextId++;
                usedRelations.insert(out.id);
            }
            f.relations.push_back(std::move(out));
        }
    }
}

} // namespace openworld::detail

namespace openworld
{
size_t remixExtract(const RemixSpec& spec, const RemixResult& r, std::string_view source, terrain::OsmExtract& x)
{
    LILA_ZONE();

    using namespace detail;
    const auto at = std::find_if(spec.sources.begin(), spec.sources.end(), [&](const Source& s) { return s.id == source; });
    if (at == spec.sources.end())
        throw std::runtime_error("remixExtract: no source '" + std::string(source) + "'");
    if (!r.warp)
        throw std::runtime_error("remixExtract: the remix result has no warp");
    Work w(spec);
    for (const Source& s : spec.sources)
    {
        std::shared_ptr<const SourceData> d;
        try
        {
            d = loadSource(s, spec.bathymetry.value("landTolerance", 50.0));
        }
        catch (const std::exception&)
        {
        }
        w.data.push_back(d);
        w.scales.push_back(s.scale > 0.0 ? s.scale : spec.horizontal);
    }
    const size_t si = size_t(at - spec.sources.begin());
    if (!w.data[si])
        throw std::runtime_error("remixExtract: source '" + std::string(source) + "' could not be read");
    w.feather = r.feather > 0.0 ? r.feather : spec.seams.width;
    prepareSources(w, r.lo, r.hi);
    w.warp = r.warp;
    for (const Region& reg : spec.regions)
        w.targets.push_back(reg.target());
    const Pieces P{w, si};
    std::vector<int> pieces{0};
    for (size_t k = 0; k < spec.regions.size(); ++k)
        if (pastes(spec.regions[k].op))
            pieces.push_back(int(k) + 1);
    const SourceData& d = *w.data[si];
    const Source& src = *at;
    auto toBase = [&](LatLon ll) { return placeOnBase(src, spec.horizontal, d.geo.toLocal(ll)); };

    size_t dropped = 0;
    // One element at a time through forEachPoint: its points pick the piece, then all of them move with it.
    auto move = [&](terrain::OsmExtract& one) {
        std::map<int, size_t> count;
        terrain::forEachPoint(one, [&](LatLon& p) {
            const Vec2 b = toBase(p);
            for (int piece : pieces)
                count[piece] += P.present(piece, b);
        });
        int best = -1;
        size_t bestCount = 0;
        for (const auto& [piece, c] : count)
            if (c > bestCount)
                best = piece, bestCount = c;
        if (best < 0)
        {
            ++dropped;
            return false;
        }
        terrain::forEachPoint(one, [&](LatLon& p) { p = r.geo.toGeographic(P.map(best, toBase(p))); });
        return true;
    };
    auto each = [&](auto member) {
        auto& list = x.*member;
        std::decay_t<decltype(list)> kept;
        for (auto& e : list)
        {
            terrain::OsmExtract one;
            (one.*member).push_back(std::move(e));
            if (move(one))
                kept.push_back(std::move((one.*member).front()));
        }
        list = std::move(kept);
    };
    each(&terrain::OsmExtract::features);
    each(&terrain::OsmExtract::areas);
    each(&terrain::OsmExtract::buildings);
    each(&terrain::OsmExtract::places);
    return dropped;
}
} // namespace openworld

namespace openworld::detail
{
void finishFeatures(Work& w, Features& f)
{
    LILA_ZONE();

    RemixResult& out = *w.out;
    OsmDoc& doc = out.osm;
    doc = OsmDoc();
    std::set<int64_t> referenced;
    for (const Features::Way& way : f.ways)
        referenced.insert(way.nodes.begin(), way.nodes.end());
    for (const OsmDoc::Relation& r : f.relations)
        for (const OsmDoc::Member& m : r.members)
            if (m.type == 'n')
                referenced.insert(m.ref);
    for (const Features::Node& n : f.nodes)
        if (referenced.count(n.id) || !n.tags.empty())
        {
            OsmTags tags = n.tags;
            // remix=cut marks ends while repairing; ends that were joined are ordinary nodes in the output.
            if (tagOf(tags, "remix") == "cut")
                tags.erase("remix");
            const LatLon ll = out.geo.toGeographic(n.at);
            doc.nodes.push_back({n.id, ll, std::move(tags)});
        }
    for (const Features::Way& way : f.ways)
    {
        doc.ways.push_back({way.id, way.nodes, way.tags});
        if (way.kind == WayKind::Road || way.kind == WayKind::River || way.kind == WayKind::Coastline)
        {
            RemixResult::Line line;
            line.kind = way.kind;
            line.added = way.added;
            for (int64_t id : way.nodes)
                if (const Features::Node* n = f.find(id))
                    line.points.push_back(n->at);
            out.lines.push_back(std::move(line));
        }
    }
    doc.relations = f.relations;
    doc.index();

    for (const Features::End& e : f.ends)
    {
        const Features::Node* n = f.find(e.node);
        CutEnd c{e.node, n ? n->at : Vec2(0.0), e.cls, e.joined, e.how};
        (e.kind == WayKind::Coastline ? out.coastEnds : out.roadEnds).push_back(c);
    }

    size_t roads = 0, rivers = 0, coast = 0, areas = 0, added = 0;
    for (const Features::Way& way : f.ways)
    {
        roads += way.kind == WayKind::Road;
        rivers += way.kind == WayKind::River;
        coast += way.kind == WayKind::Coastline;
        areas += way.kind == WayKind::Area;
        added += way.added;
    }
    size_t joined = 0, coastJoined = 0;
    for (const CutEnd& e : out.roadEnds)
        joined += e.joined;
    for (const CutEnd& e : out.coastEnds)
        coastJoined += e.joined;
    size_t draining = 0;
    for (const RiverState& r : out.rivers)
        draining += r.drains;
    out.stats["features"] = {{"nodes", doc.nodes.size()}, {"ways", doc.ways.size()}, {"relations", doc.relations.size()}, {"roads", roads},
                             {"rivers", rivers}, {"coastline", coast}, {"areas", areas}, {"added", added}};
    out.stats["roadEnds"] = {{"cut", out.roadEnds.size()}, {"joined", joined}, {"dangling", out.roadEnds.size() - joined}};
    out.stats["coastEnds"] = {{"cut", out.coastEnds.size()}, {"joined", coastJoined}, {"dangling", out.coastEnds.size() - coastJoined}};
    out.stats["rivers"] = {{"checked", out.rivers.size()}, {"draining", draining}};
}
} // namespace openworld::detail
