#include "uv/SymmetryUv.h"

#include "core/Profile.h"
#include "geo/Symmetry.h"
#include "uv/Layout.h"
#include "uv/Metrics.h"
#include "uv/Pack.h"
#include "uv/Seams.h"
#include "uv/UvCommon.h"

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::uv
{
namespace
{
using detail::usable;

/** Seams of the faces copied across the plane, so the islands can pair up. Returns how many were added. */
Index mirrorSeams(geo::Mesh& mesh, const geo::SymmetryMap& map, std::span<const Index> faces)
{
    std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::vector<Index> edges;
    for (Index f : faces)
        mesh.forEachFaceCorner(f, [&](Index c) {
            if (seam[mesh.cornerEdge(c)])
                edges.push_back(mesh.cornerEdge(c));
        });
    Index added = 0;
    for (Index e : map.mirrorEdges(mesh, edges))
        if (e != kInvalid && !seam[e])
        {
            seam[e] = true;
            ++added;
        }
    return added;
}

/** Mesh corner -> (chart, vertex). */
struct CornerIndex
{
    std::vector<Index> chart, vertex;

    CornerIndex(const geo::Mesh& mesh, std::span<const Chart> charts)
        : chart(mesh.cornerCapacity(), kInvalid), vertex(mesh.cornerCapacity(), kInvalid)
    {
        for (size_t c = 0; c < charts.size(); ++c)
            for (size_t k = 0; k < charts[c].corners.size(); ++k)
            {
                chart[charts[c].corners[k]] = Index(c);
                vertex[charts[c].corners[k]] = charts[c].cornerVertex[k];
            }
    }
};

/** The corner mirroring c: in the mirrored face, at the mirrored point. */
Index mirrorCorner(const geo::Mesh& mesh, const geo::SymmetryMap& map, Index c)
{
    Index f = map.mirrorFace(mesh, mesh.cornerFace(c));
    Index p = map.mirrorPoint(mesh.cornerPoint(c));
    if (f == kInvalid || p == kInvalid)
        return kInvalid;
    return mesh.findCorner(f, p);
}
} // namespace

namespace
{
/** The wedge a face's centre lies in, so a face across no boundary has exactly one. */
int faceWedge(const geo::Mesh& mesh, const geo::SymmetryMap& map, Index f)
{
    glm::dvec3 centre(0.0);
    Index n = 0;
    mesh.forEachFaceCorner(f, [&](Index c) {
        centre += mesh.position(mesh.cornerPoint(c));
        ++n;
    });
    return map.radial().wedgeOf(centre / double(std::max<Index>(n, 1)));
}
} // namespace

int radialFaceWedge(const geo::Mesh& mesh, const geo::SymmetryMap& map, std::span<const Index> faces)
{
    std::vector<Index> count(size_t(map.order()), 0);
    for (Index f : faces)
        if (mesh.faceValid(f))
            ++count[size_t(faceWedge(mesh, map, f))];
    return int(std::max_element(count.begin(), count.end()) - count.begin());
}

size_t stackRadialUvs(geo::Mesh& mesh, const geo::SymmetryMap& map, std::string_view uvSet, int drive, std::span<const Index> faces)
{
    LILA_ZONE();

    std::span<glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty() || map.empty() || !map.isRadial())
        return 0;
    std::vector<Index> all;
    if (faces.empty())
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
            all.push_back(f);
    else
        all.assign(faces.begin(), faces.end());
    std::vector<uint8_t> took(mesh.faceCapacity(), 0);
    size_t count = 0;
    for (Index d : all)
    {
        if (!mesh.faceValid(d) || took[d] || faceWedge(mesh, map, d) != drive)
            continue;
        for (int g = 1; g < map.order(); ++g)
        {
            const Index f = map.imageFace(mesh, g, d);
            if (f == kInvalid || f == d || took[f])
                continue;
            mesh.forEachFaceCorner(d, [&](Index c) {
                if (const Index ic = mesh.findCorner(f, map.image(g, mesh.cornerPoint(c))); ic != kInvalid)
                    uv[ic] = uv[c];
            });
            took[f] = 1;
            ++count;
        }
    }
    return count;
}

Index symmetricSeams(geo::Mesh& mesh, const geo::SymmetryMap& map, std::span<const Index> faceIds, bool open)
{
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    Index added = 0;
    // Opening closed parts can cut asymmetrically; mirror and re-open until it settles.
    mirrorSeams(mesh, map, faces);
    for (int round = 0; open && round < 3; ++round)
    {
        Index cuts = cutToDisks(mesh, faces);
        added += cuts;
        if (cuts == 0 || mirrorSeams(mesh, map, faces) == 0)
            break;
    }
    return added;
}

double symmetrizeChart(Chart& ch, std::span<const Index> partner)
{
    const size_t V = ch.uv.size();
    if (V == 0 || partner.size() != V)
        return 0.0;
    glm::dvec2 mu(0.0);
    for (const glm::dvec2& p : ch.uv)
        mu += p;
    mu /= double(V);
    // Best mirror line through the centroid: maximise sum(y_w . Refl(theta) y_v).
    double A = 0.0, B = 0.0;
    for (size_t v = 0; v < V; ++v)
    {
        glm::dvec2 a = ch.uv[v] - mu, b = ch.uv[partner[v]] - mu;
        A += b.x * a.x - b.y * a.y;
        B += b.x * a.y + b.y * a.x;
    }
    const double theta = 0.5 * std::atan2(B, A);
    const double c2 = std::cos(2.0 * theta), s2 = std::sin(2.0 * theta);
    auto reflect = [&](const glm::dvec2& p) {
        glm::dvec2 y = p - mu;
        return mu + glm::dvec2(c2 * y.x + s2 * y.y, s2 * y.x - c2 * y.y);
    };
    std::vector<glm::dvec2> out(V);
    for (size_t v = 0; v < V; ++v)
        out[v] = 0.5 * (ch.uv[v] + reflect(ch.uv[partner[v]]));
    // Turn the mirror line upright, then make the pairs exact.
    const double turn = glm::half_pi<double>() - theta;
    const double ct = std::cos(turn), st = std::sin(turn);
    for (glm::dvec2& p : out)
    {
        glm::dvec2 y = p - mu;
        p = mu + glm::dvec2(ct * y.x - st * y.y, st * y.x + ct * y.y);
    }
    for (size_t v = 0; v < V; ++v)
    {
        Index w = partner[v];
        if (w == v)
            out[v].x = mu.x;
        else if (v < w)
            out[w] = glm::dvec2(2.0 * mu.x - out[v].x, out[v].y);
    }
    // Correction measured in the chart's own frame: compare against the original turned upright.
    double worst = 0.0;
    for (size_t v = 0; v < V; ++v)
    {
        glm::dvec2 y = ch.uv[v] - mu;
        glm::dvec2 turned = mu + glm::dvec2(ct * y.x - st * y.y, st * y.x + ct * y.y);
        worst = std::max(worst, glm::length(turned - out[v]));
    }
    ch.uv = std::move(out);
    return worst;
}

SymmetryUvResult unwrapSymmetric(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const SymmetryUvOptions& opts)
{
    LILA_ZONE();

    SymmetryUvResult res;
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return res;
    const int axis = std::clamp(opts.axis, 0, 2);
    const double tol = std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, axis, tol, nullptr, opts.offset);

    if (opts.mirrorSeams)
        res.seamsAdded = symmetricSeams(mesh, map, faces);
    else
        res.seamsAdded = cutToDisks(mesh, faces);

    ChartOptions co;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    res.islands = charts.size();
    CornerIndex index(mesh, charts);

    // Partner chart per chart (itself when it straddles the plane), and vertex partners.
    std::vector<Index> partnerChart(charts.size(), kInvalid);
    std::vector<std::vector<Index>> partnerVertex(charts.size());
    for (size_t c = 0; c < charts.size(); ++c)
    {
        const Chart& ch = charts[c];
        Index other = kInvalid;
        std::vector<Index> pv(ch.pos.size(), kInvalid);
        bool ok = true;
        for (size_t k = 0; k < ch.corners.size() && ok; ++k)
        {
            Index mc = mirrorCorner(mesh, map, ch.corners[k]);
            if (mc == kInvalid || index.chart[mc] == kInvalid)
            {
                ok = false;
                break;
            }
            if (other == kInvalid)
                other = index.chart[mc];
            Index& slot = pv[ch.cornerVertex[k]];
            ok = index.chart[mc] == other && (slot == kInvalid || slot == index.vertex[mc]);
            slot = index.vertex[mc];
        }
        if (ok && other != kInvalid && charts[other].pos.size() == ch.pos.size())
        {
            partnerChart[c] = other;
            partnerVertex[c] = std::move(pv);
        }
    }

    // Drivers: one of each pair (the positive side), straddling islands and loners.
    enum class Role : uint8_t
    {
        Alone,
        Self,
        Driver,
        Follower,
    };
    std::vector<Role> role(charts.size(), Role::Alone);
    for (size_t c = 0; c < charts.size(); ++c)
    {
        Index o = partnerChart[c];
        if (o == kInvalid || partnerChart[o] != Index(c))
            continue;
        if (o == Index(c))
        {
            role[c] = Role::Self;
            continue;
        }
        int side = map.majoritySide(charts[c].vertexPoint), otherSide = map.majoritySide(charts[o].vertexPoint);
        bool drive = side != otherSide ? side > 0 : c < size_t(o);
        role[c] = drive ? Role::Driver : Role::Follower;
    }

    std::vector<uint8_t> hadUvs(charts.size(), 0);
    std::vector<glm::dvec2> oldCentroid(charts.size());
    std::vector<double> oldArea(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t c) {
        Chart& ch = charts[c];
        hadUvs[c] = usable(ch) ? 1 : 0;
        oldCentroid[c] = ch.uvCentroid();
        oldArea[c] = std::abs(ch.areaUv());
        if (role[c] == Role::Follower)
            return;
        UnwrapOptions uo = opts.unwrap;
        uo.usePins = false;
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        unwrapChart(ch, uo);
        if (role[c] == Role::Self)
            symmetrizeChart(ch, partnerVertex[c]);
    });

    for (size_t c = 0; c < charts.size(); ++c)
    {
        res.pairs += role[c] == Role::Driver;
        res.selfSymmetric += role[c] == Role::Self;
        res.asymmetric += role[c] == Role::Alone;
        if (role[c] == Role::Follower || opts.pack || !hadUvs[c])
            continue;
        // A driver keeps the pair's old spot: its own centroid, or between the two when flipped.
        glm::dvec2 at = oldCentroid[c];
        double area = oldArea[c];
        if (role[c] == Role::Driver && hadUvs[partnerChart[c]])
            area = 0.5 * (oldArea[c] + oldArea[partnerChart[c]]);
        fitChart(charts[c], at, area);
    }

    auto mirrorFollowers = [&] {
        for (size_t c = 0; c < charts.size(); ++c)
        {
            if (role[c] != Role::Driver)
                continue;
            const Chart& d = charts[c];
            Chart& f = charts[partnerChart[c]];
            const std::vector<Index>& pv = partnerVertex[c];
            const double gap = 0.02 * std::max(d.uvMax().x - d.uvMin().x, d.uvMax().y - d.uvMin().y);
            const double axisU = d.uvMax().x + 0.5 * gap;
            for (size_t v = 0; v < d.uv.size(); ++v)
            {
                glm::dvec2 p = d.uv[v];
                f.uv[pv[v]] = opts.mode == MirrorUvMode::Stack ? p : glm::dvec2(2.0 * axisU - p.x, p.y);
            }
        }
    };
    mirrorFollowers();

    // Flipped partners pack as islands of their own; stacked ones ride on their driver's spot. Without pack,
    // only islands that had no uvs are packed. Packing never turns them, so mirror lines stay vertical.
    std::vector<Chart*> toPack;
    for (size_t c = 0; c < charts.size(); ++c)
    {
        const bool follower = role[c] == Role::Follower;
        if (follower && opts.mode == MirrorUvMode::Stack)
            continue;
        if (opts.pack || !hadUvs[follower ? partnerChart[c] : Index(c)])
            toPack.push_back(&charts[c]);
    }
    if (!toPack.empty())
    {
        std::vector<Chart> packed;
        for (Chart* p : toPack)
            packed.push_back(std::move(*p));
        PackOptions po;
        po.rotateStep = 0;
        po.preOrient = false;
        packCharts(packed, po);
        for (size_t i = 0; i < toPack.size(); ++i)
            *toPack[i] = std::move(packed[i]);
        if (opts.mode == MirrorUvMode::Stack)
            mirrorFollowers();
    }
    writeCharts(mesh, charts, uvSet);
    return res;
}

namespace
{
const glm::dmat2 kFlipU(-1.0, 0.0, 0.0, 1.0);

struct MirrorFit
{
    glm::dmat2 linear = kFlipU;
    glm::dvec2 offset{0.0};
    bool ok = false;
};

/** Best similarity taking from[i] to to[i] (a reflection either way round, or with reflectionOnly a unit reflection). */
MirrorFit fitMirror(std::span<const glm::dvec2> from, std::span<const glm::dvec2> to, bool reflectionOnly)
{
    MirrorFit fit;
    const size_t n = from.size();
    if (n == 0)
        return fit;
    glm::dvec2 mf(0.0), mt(0.0);
    for (size_t i = 0; i < n; ++i)
    {
        mf += from[i];
        mt += to[i];
    }
    mf /= double(n);
    mt /= double(n);
    double sxx = 0.0, c = 0.0, d = 0.0, a = 0.0, b = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec2 x = from[i] - mf, y = to[i] - mt;
        sxx += glm::dot(x, x);
        c += glm::dot(x, y);
        d += x.x * y.y - x.y * y.x;
        a += x.x * y.x - x.y * y.y;
        b += x.y * y.x + x.x * y.y;
    }
    if (sxx > 1e-16)
    {
        const double rot = std::hypot(c, d), ref = std::hypot(a, b);
        if (!reflectionOnly && rot > ref)
        {
            const double th = std::atan2(d, c), s = rot / sxx;
            fit.linear = s * glm::dmat2(std::cos(th), std::sin(th), -std::sin(th), std::cos(th));
        }
        else
        {
            const double phi2 = std::atan2(b, a), s = reflectionOnly ? 1.0 : ref / sxx;
            fit.linear = s * glm::dmat2(std::cos(phi2), std::sin(phi2), std::sin(phi2), -std::cos(phi2));
        }
        fit.ok = true;
    }
    fit.offset = mt - fit.linear * mf;
    return fit;
}

/** Per island, the reflection pairing its corners across its own centre line, fitted to uv. */
MirrorFit fitSpanning(const UvMirror& m, int island, std::span<const glm::vec2> uv)
{
    std::vector<glm::dvec2> from, to;
    for (size_t c = 0; c < m.partner.size(); ++c)
        if (m.island[c] == island && m.partner[c] != kInvalid && m.island[m.partner[c]] == island)
        {
            from.emplace_back(uv[c]);
            to.emplace_back(uv[m.partner[c]]);
        }
    return fitMirror(from, to, true);
}
} // namespace

UvMirror buildUvMirror(const geo::Mesh& mesh, const geo::SymmetryMap& map, std::string_view uvSet)
{
    LILA_ZONE();

    UvMirror out;
    const Index nc = mesh.cornerCapacity();
    out.partner.assign(nc, kInvalid);
    out.side.assign(nc, 0);
    out.island.assign(nc, -1);
    if (map.empty())
        return out;
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    ChartOptions co;
    co.source = uv.empty() ? IslandSource::Seams : IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<std::vector<Index>> islands = islandFaces(mesh, {}, co);
    out.islandSize.assign(islands.size(), 0);
    for (size_t i = 0; i < islands.size(); ++i)
        for (Index f : islands[i])
            mesh.forEachFaceCorner(f, [&](Index c) {
                out.island[c] = int(i);
                ++out.islandSize[i];
            });

    // A corner at an on-plane point takes its face's side; a face mirroring onto itself has none.
    auto faceSide = [&](Index f) {
        const Index mf = map.mirrorFace(mesh, f);
        if (mf == kInvalid || mf == f)
            return 0;
        int sum = 0;
        mesh.forEachFaceCorner(f, [&](Index c) { sum += map.side(mesh.cornerPoint(c)); });
        return sum > 0 ? 1 : sum < 0 ? -1 : f < mf ? 1 : -1;
    };
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const int fs = faceSide(f);
        mesh.forEachFaceCorner(f, [&](Index c) {
            const Index pc = mirrorCorner(mesh, map, c);
            if (pc == kInvalid)
                return;
            out.partner[c] = pc;
            const int ps = map.side(mesh.cornerPoint(c));
            // On-plane corners inside one island hold it together across the plane.
            const bool centre = pc == c || (ps == 0 && out.island[c] >= 0 && out.island[c] == out.island[pc]);
            out.side[c] = int8_t(centre ? 0 : ps != 0 ? ps : fs);
        });
    }

    const size_t ni = islands.size();
    out.islandPartner.assign(ni, -1);
    std::vector<std::map<int, size_t>> votes(ni);
    for (Index c = 0; c < nc; ++c)
        if (out.island[c] >= 0 && out.partner[c] != kInvalid && out.island[out.partner[c]] >= 0)
            ++votes[size_t(out.island[c])][out.island[out.partner[c]]];
    for (size_t i = 0; i < ni; ++i)
    {
        size_t best = 0;
        for (auto [j, count] : votes[i])
            if (count > best)
            {
                best = count;
                out.islandPartner[i] = j;
            }
    }
    fitUvMirror(out, uv);
    return out;
}

void fitUvMirror(UvMirror& m, std::span<const glm::vec2> uv)
{
    const size_t nc = m.partner.size(), ni = m.islandPartner.size();
    m.linear.assign(ni, kFlipU);
    m.offset.assign(ni, glm::dvec2(0.0));
    m.fitted.assign(ni, 0);
    std::vector<std::vector<glm::dvec2>> from(ni), to(ni);
    for (size_t c = 0; c < nc; ++c)
    {
        const int i = m.island[c];
        if (i < 0 || m.partner[c] == kInvalid || m.island[m.partner[c]] != m.islandPartner[size_t(i)])
            continue;
        from[size_t(i)].emplace_back(c < uv.size() ? glm::dvec2(uv[c]) : glm::dvec2(0.0));
        to[size_t(i)].emplace_back(m.partner[c] < uv.size() ? glm::dvec2(uv[m.partner[c]]) : glm::dvec2(0.0));
    }
    for (size_t i = 0; i < ni; ++i)
    {
        if (m.islandPartner[i] < 0)
            continue;
        MirrorFit fit = fitMirror(from[i], to[i], m.islandPartner[i] == int(i));
        m.linear[i] = fit.linear;
        m.offset[i] = fit.offset;
        m.fitted[i] = fit.ok ? 1 : 0;
    }
}

int uvMirrorDrive(const UvMirror& mirror, std::span<const Index> corners)
{
    size_t pos = 0, neg = 0;
    for (Index c : corners)
        if (c < mirror.side.size())
        {
            pos += mirror.side[c] > 0;
            neg += mirror.side[c] < 0;
        }
    return neg > pos ? -1 : 1;
}

std::vector<UvFollow> uvFollowRules(const UvMirror& m, std::span<const Index> moved, int drive)
{
    std::vector<UvFollow> rules;
    const size_t n = m.partner.size();
    if (m.empty())
        return rules;
    std::vector<uint8_t> isMoved(n, 0);
    std::vector<size_t> movedCount(m.islandPartner.size(), 0);
    for (Index c : moved)
        if (c < n && !isMoved[c])
        {
            isMoved[c] = 1;
            if (m.island[c] >= 0)
                ++movedCount[size_t(m.island[c])];
        }
    auto spanningWhole = [&](int i) { return i >= 0 && m.islandPartner[size_t(i)] == i && movedCount[size_t(i)] == m.islandSize[size_t(i)]; };
    // Centre-line corners keep the part of the move along the line; partners on the line move as one.
    for (Index c = 0; c < n; ++c)
    {
        const Index pc = m.partner[c];
        const int i = m.island[c];
        if (m.side[c] != 0 || pc == kInvalid || i < 0 || m.islandPartner[size_t(i)] != i || m.island[pc] != i || spanningWhole(i) ||
            (!isMoved[c] && !isMoved[pc]))
            continue;
        rules.push_back({c, isMoved[c] ? c : pc, 0.5 * (glm::dmat2(1.0) + m.linear[size_t(i)])});
    }
    for (Index c = 0; c < n; ++c)
    {
        const Index pc = m.partner[c];
        if (pc == kInvalid || pc == c || m.side[c] != -drive || m.side[pc] != drive || (!isMoved[c] && !isMoved[pc]))
            continue;
        const int i = m.island[pc], j = m.island[c];
        if (i == j && spanningWhole(i))
            continue;
        const bool paired = i >= 0 && m.islandPartner[size_t(i)] == j;
        rules.push_back({c, pc, paired ? m.linear[size_t(i)] : kFlipU});
    }
    return rules;
}

size_t applyUvFollow(std::span<const UvFollow> rules, std::span<const glm::vec2> before, std::span<glm::vec2> uv)
{
    size_t changed = 0;
    for (const UvFollow& r : rules)
    {
        if (r.corner >= uv.size() || r.source >= uv.size() || r.corner >= before.size() || r.source >= before.size())
            continue;
        glm::dvec2 delta = glm::dvec2(uv[r.source]) - glm::dvec2(before[r.source]);
        const glm::vec2 next = glm::vec2(glm::dvec2(before[r.corner]) + r.linear * delta);
        if (next != uv[r.corner])
        {
            uv[r.corner] = next;
            ++changed;
        }
    }
    return changed;
}

size_t symmetrizeUvIslands(const UvMirror& m, std::span<const Index> changed, int drive, std::span<glm::vec2> uv)
{
    LILA_ZONE();

    const size_t n = std::min(m.partner.size(), uv.size()), ni = m.islandPartner.size();
    if (m.empty() || n == 0)
        return 0;
    std::vector<uint8_t> touched(ni, 0);
    for (Index c : changed)
        if (c < n && m.island[c] >= 0)
        {
            touched[size_t(m.island[c])] = 1;
            if (int j = m.islandPartner[size_t(m.island[c])]; j >= 0)
                touched[size_t(j)] = 1;
        }
    std::vector<glm::dvec2> centroid(ni, glm::dvec2(0.0));
    for (size_t c = 0; c < n; ++c)
        if (m.island[c] >= 0)
            centroid[size_t(m.island[c])] += glm::dvec2(uv[c]);
    for (size_t i = 0; i < ni; ++i)
        if (m.islandSize[i])
            centroid[i] /= double(m.islandSize[i]);
    std::vector<MirrorFit> spanning(ni);
    for (size_t i = 0; i < ni; ++i)
        if (touched[i] && m.islandPartner[i] == int(i))
            spanning[i] = fitSpanning(m, int(i), uv);

    size_t count = 0;
    auto set = [&](size_t c, glm::dvec2 p) {
        glm::vec2 next(p);
        if (next != uv[c])
        {
            uv[c] = next;
            ++count;
        }
    };
    for (size_t c = 0; c < n; ++c)
    {
        const int j = m.island[c];
        const Index pc = m.partner[c];
        if (j < 0 || !touched[size_t(j)] || pc == kInvalid)
            continue;
        const int i = m.island[pc];
        if (i < 0)
            continue;
        if (i == j && m.islandPartner[size_t(i)] == i)
        {
            const MirrorFit& f = spanning[size_t(i)];
            if (m.side[c] == 0)
            {
                // Each pair once, from both its uvs, onto the line.
                if (pc < c)
                    continue;
                const glm::dvec2 mid = 0.5 * (glm::dvec2(uv[c]) + glm::dvec2(uv[pc]));
                const glm::dvec2 onLine = 0.5 * (mid + f.linear * mid + f.offset);
                set(c, onLine);
                if (pc != c)
                    set(pc, onLine);
            }
            else if (m.side[c] == -drive)
                set(c, f.linear * glm::dvec2(uv[pc]) + f.offset);
            continue;
        }
        if (m.side[c] != -drive || m.side[pc] != drive || m.islandPartner[size_t(i)] != j)
            continue;
        if (m.fitted[size_t(i)])
            set(c, m.linear[size_t(i)] * glm::dvec2(uv[pc]) + m.offset[size_t(i)]);
        else
            set(c, centroid[size_t(j)] + kFlipU * (glm::dvec2(uv[pc]) - centroid[size_t(i)]));
    }
    return count;
}
} // namespace lila::uv
