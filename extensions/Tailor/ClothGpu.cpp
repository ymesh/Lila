#include "ClothGpu.h"

#include <lila/GpuCompute.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <thread>

namespace tailor
{
namespace
{
namespace gpu = lila::gpu;

using F4 = std::array<float, 4>;

constexpr uint32_t kGroup = 64;
constexpr uint32_t kMaxColliders = 4;
constexpr uint32_t kMaxVt = 16; // must match cloth_vbd.slang
constexpr uint32_t kMaxTv = 32;
constexpr uint32_t kMaxEe = 32;
constexpr uint32_t kMovable = 1;
constexpr uint32_t kSeam = 0;
constexpr uint32_t kStrain = 1;
constexpr uint32_t kMaxIndirectItems = 65535u * kGroup;

struct GpuTri
{
    uint32_t v[4];
    float b[4]; // b00, b01, b10, b11
    float rest[4]; // area, warp, weft, shear
};
struct GpuHinge
{
    uint32_t v[4];
    float p[4];
};
struct GpuLink
{
    uint32_t v[4]; // a, b, kind, closed
    float p[4];    // limit, rest, kStart
};

enum Buf
{
    BX,
    BXStart,
    BXNew,
    BXDetect,
    BY,
    BVel,
    BVInfo,
    BVExtra,
    BPin,
    BPinDual,
    BTriOff,
    BTriAdj,
    BHingeOff,
    BHingeAdj,
    BLinkOff,
    BLinkAdj,
    BColour,
    BWeld,
    BTris,
    BHinges,
    BLinks,
    BLinkDual,
    BColliders,
    BCellHead,
    BCellNext,
    BVtCount,
    BVt,
    BTvCount,
    BTv,
    BDminV,
    BDminT,
    BBound,
    BState,
    BBody,
    BGrid0, // collider distance grids, x fastest
    BGrid1,
    BGrid2,
    BGrid3,
    BFlat,  // per vertex: pattern position and panel, to skip contacts within one stretch of fabric
    BEdges,
    BEdgeHead, // edge hash by midpoint cell
    BEdgeNext,
    BEeCount,
    BEe,     // per vertex: the edge-edge contacts it is in
    BEeSide,
    BSpare, // keeps gravity 16-byte aligned
    BufCount
};

/** Mirrors ClothScene in cloth_vbd.slang. */
struct GpuScene
{
    uint64_t ptr[BufCount];
    uint32_t nVerts, nTris, nHinges, nLinks;
    uint32_t nColliders, hashMask, selfCollision, nMovable;
    float h, lambdaDecay, stiffnessDecay, beta;
    float kMax, thickness, queryRadius, cellSize;
    float boundFraction, frictionSlip, drag, seamKeep;
    float closedGap, floorY, maxHalfEdge;
    uint32_t nEdges;
    float gravity[4];
};
static_assert(BufCount == 46);
static_assert(offsetof(GpuScene, gravity) == 464 && sizeof(GpuScene) == 480);

struct ClothPush
{
    uint64_t scene;
    uint32_t first, count, pass, pad;
};
static_assert(sizeof(ClothPush) == 24);

/** state[]: the clamp flag, then 6 VkDispatchIndirectCommand for re-detection from word 4. */
constexpr uint32_t kStateWords = 4 + 6 * 3;

enum Kernel
{
    KAdvanceSeams,
    KBeginStep,
    KBeginLinks,
    KInitGuess,
    KSolve,
    KCommit,
    KDualLinks,
    KDualPins,
    KEndStep,
    KRedetectArgs,
    KHashClear,
    KHashInsert,
    KHashQuery,
    KContactFinalize,
    KEdgeInsert,
    KEdgeQuery,
    KernelCount
};
constexpr const char* kEntries[KernelCount] = {"advanceSeamsCS", "beginStepCS", "beginLinksCS",   "initGuessCS", "solveCS",
                                               "commitCS",       "dualLinksCS", "dualPinsCS",     "endStepCS",   "redetectArgsCS",
                                               "hashClearCS",    "hashInsertCS", "hashQueryCS",   "contactFinalizeCS",
                                               "edgeInsertCS",   "edgeQueryCS"};

float bitsToFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

using D3 = std::array<double, 3>;
D3 toD(const Vec3& a) { return {a[0], a[1], a[2]}; }
D3 d3Sub(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 d3Cross(const D3& a, const D3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double d3Dot(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double d3Len(const D3& a) { return std::sqrt(d3Dot(a, a)); }

/** ClothSolver's dihedral angle across ab between abc and abd; 0 when degenerate. */
float dihedralAngle(const Vec3& pa, const Vec3& pb, const Vec3& pc, const Vec3& pd)
{
    const D3 a = toD(pa), e = d3Sub(toD(pb), a);
    const double el = d3Len(e);
    const D3 n1 = d3Cross(e, d3Sub(toD(pc), a)), n2 = d3Cross(d3Sub(toD(pd), a), e);
    const double l1 = d3Len(n1), l2 = d3Len(n2);
    if (el < 1e-8 || l1 < 1e-12 || l2 < 1e-12)
        return 0.f;
    const D3 u1{n1[0] / l1, n1[1] / l1, n1[2] / l1}, u2{n2[0] / l2, n2[1] / l2, n2[2] / l2};
    return float(std::atan2(d3Dot(d3Cross(u1, u2), e) / el, d3Dot(u1, u2)));
}

void toCsr(const std::vector<std::vector<uint32_t>>& lists, std::vector<uint32_t>& off, std::vector<uint32_t>& adj)
{
    off.assign(lists.size() + 1, 0);
    for (size_t i = 0; i < lists.size(); ++i)
        off[i + 1] = off[i] + uint32_t(lists[i].size());
    adj.clear();
    adj.reserve(off.back());
    for (const auto& l : lists)
        adj.insert(adj.end(), l.begin(), l.end());
}

int weldRoot(std::vector<int>& parent, int i)
{
    while (parent[size_t(i)] != i)
    {
        parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        i = parent[size_t(i)];
    }
    return i;
}

/** Everything on the GPU that stays fixed between resets, built on the caller's thread exactly as ClothSolver builds it. */
struct Topology
{
    uint32_t n = 0;
    std::vector<F4> x, vinfo, vextra, flat;
    std::vector<float> mass;
    std::vector<uint8_t> movable;
    std::vector<GpuTri> tris;
    std::vector<GpuHinge> hinges;
    std::vector<GpuLink> links;
    std::vector<uint32_t> triOff, triAdj, hingeOff, hingeAdj, linkOff, linkAdj, weld;
    std::vector<uint32_t> colour, colourOff;
    std::vector<std::array<uint32_t, 2>> edges;
    float maxHalfEdge = 0.f;
    float queryRadius = 0.01f;
    float cellSize = 0.02f;
};

Topology buildTopology(const ClothMesh& mesh, const Garment& g, const SolverSettings& s, const GpuTuning& tune, float h)
{
    Topology t;
    const int n = int(mesh.rest3d.size());
    t.n = uint32_t(n);
    static const Fabric kDefaultFabric;
    auto panelOf = [&](int p) -> const Panel* { return p >= 0 && p < int(g.panels.size()) ? &g.panels[size_t(p)] : nullptr; };
    auto vertPanel = [&](int i) { return i < int(mesh.vertPanel.size()) ? mesh.vertPanel[size_t(i)] : 0; };
    auto fabricOf = [&](const Panel* p) -> const Fabric& {
        if (p && p->fabric >= 0 && p->fabric < int(g.fabrics.size()))
            return g.fabrics[size_t(p->fabric)];
        return g.fabrics.empty() ? kDefaultFabric : g.fabrics.front();
    };

    t.mass.assign(size_t(n), 0.f);
    t.movable.assign(size_t(n), 0);
    std::vector<float> thickness(size_t(n), 0.f), friction(size_t(n), 0.f), damping(size_t(n), 0.f);
    for (int i = 0; i < n; ++i)
    {
        const Panel* p = panelOf(vertPanel(i));
        const Fabric& f = fabricOf(p);
        const PanelState state = p ? p->state : PanelState::Active;
        damping[size_t(i)] = f.damping * tune.dampingScale;
        friction[size_t(i)] = f.friction;
        thickness[size_t(i)] = f.thickness;
        t.movable[size_t(i)] = state == PanelState::Active || state == PanelState::Strengthened;
    }

    const bool hasUv = mesh.uv.size() == mesh.rest3d.size();
    std::vector<std::array<int, 3>> tris;
    std::vector<int> triPanel;
    for (size_t f = 0; f < mesh.tris.size(); ++f)
    {
        const auto& tri = mesh.tris[f];
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            ok = ok && tri[size_t(k)] >= 0 && tri[size_t(k)] < n;
        if (!ok || tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
            continue;
        tris.push_back(tri);
        triPanel.push_back(f < mesh.triPanel.size() ? mesh.triPanel[f] : vertPanel(tri[0]));
    }
    const int nt = int(tris.size());

    // Rest corners in the grain frame: warp along x, scaled by the fabric's shrink.
    using V2 = std::array<float, 2>;
    std::vector<std::array<V2, 3>> restCorners(static_cast<size_t>(nt));
    std::vector<std::vector<uint32_t>> vTri(static_cast<size_t>(n)), vHinge(static_cast<size_t>(n)), vLink(static_cast<size_t>(n));
    t.tris.assign(size_t(nt), GpuTri{});
    for (int f = 0; f < nt; ++f)
    {
        const auto& tri = tris[size_t(f)];
        const Panel* p = panelOf(triPanel[size_t(f)]);
        const Fabric& fab = fabricOf(p);
        auto& rcn = restCorners[size_t(f)];
        if (hasUv)
        {
            const float ga = p ? p->grainAngle : 0.f;
            const float wx = std::cos(ga), wy = std::sin(ga);
            for (int k = 0; k < 3; ++k)
            {
                const Vec2& u = mesh.uv[size_t(tri[size_t(k)])];
                rcn[size_t(k)] = {(u[0] * wx + u[1] * wy) * fab.shrinkWarp, (-u[0] * wy + u[1] * wx) * fab.shrinkWeft};
            }
            if (p && !p->shrinkSpots.empty())
            {
                const Vec2 c{(mesh.uv[size_t(tri[0])][0] + mesh.uv[size_t(tri[1])][0] + mesh.uv[size_t(tri[2])][0]) / 3.f,
                             (mesh.uv[size_t(tri[0])][1] + mesh.uv[size_t(tri[1])][1] + mesh.uv[size_t(tri[2])][1]) / 3.f};
                const float sc = shrinkAt(*p, c);
                for (auto& corner : rcn)
                    corner = {corner[0] * sc, corner[1] * sc};
            }
        }
        else
        {
            const D3 r0 = toD(mesh.rest3d[size_t(tri[0])]);
            const D3 e1 = d3Sub(toD(mesh.rest3d[size_t(tri[1])]), r0), e2 = d3Sub(toD(mesh.rest3d[size_t(tri[2])]), r0);
            const double l1 = d3Len(e1);
            const D3 nrm = d3Cross(e1, e2);
            if (l1 > 1e-12 && d3Len(nrm) > 1e-20)
            {
                const D3 ax{e1[0] / l1, e1[1] / l1, e1[2] / l1};
                D3 ay = d3Cross(nrm, ax);
                const double la = d3Len(ay);
                ay = {ay[0] / la, ay[1] / la, ay[2] / la};
                rcn = {V2{0.f, 0.f}, V2{float(l1), 0.f}, V2{float(d3Dot(e2, ax)), float(d3Dot(e2, ay))}};
            }
            else
                rcn = {V2{0.f, 0.f}, V2{0.f, 0.f}, V2{0.f, 0.f}};
        }
        const float d1x = rcn[1][0] - rcn[0][0], d1y = rcn[1][1] - rcn[0][1];
        const float d2x = rcn[2][0] - rcn[0][0], d2y = rcn[2][1] - rcn[0][1];
        const float det = d1x * d2y - d2x * d1y;
        GpuTri& gt = t.tris[size_t(f)];
        for (int k = 0; k < 3; ++k)
            gt.v[k] = uint32_t(tri[size_t(k)]);
        if (std::abs(det) > 1e-14f)
        {
            gt.b[0] = d2y / det;
            gt.b[1] = -d2x / det;
            gt.b[2] = -d1y / det;
            gt.b[3] = d1x / det;
            const float area = 0.5f * std::abs(det);
            const float mult = p && p->state == PanelState::Strengthened ? tune.strengthen : 1.f;
            gt.rest[0] = area;
            gt.rest[1] = fab.stretchWarp * mult;
            gt.rest[2] = fab.stretchWeft * mult;
            gt.rest[3] = fab.shear * mult;
            for (int k = 0; k < 3; ++k)
                t.mass[size_t(tri[size_t(k)])] += fab.density * area / 3.f;
        }
        for (uint32_t k = 0; k < 3; ++k)
            vTri[size_t(tri[k])].push_back(uint32_t(f) << 2 | k);
    }
    for (int i = 0; i < n; ++i)
        t.movable[size_t(i)] = t.movable[size_t(i)] && t.mass[size_t(i)] > 0.f;

    struct EdgeRec
    {
        uint64_t key;
        int tri;
        int local;
    };
    std::vector<EdgeRec> edges;
    edges.reserve(size_t(nt) * 3);
    for (int f = 0; f < nt; ++f)
        for (int k = 0; k < 3; ++k)
        {
            const int a = tris[size_t(f)][size_t(k)], b = tris[size_t(f)][size_t((k + 1) % 3)];
            edges.push_back({(uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b)), f, k});
        }
    std::sort(edges.begin(), edges.end(), [](const EdgeRec& l, const EdgeRec& r) { return l.key != r.key ? l.key < r.key : l.tri < r.tri; });
    std::vector<std::pair<uint64_t, float>> creases;
    for (size_t k = 0; k < mesh.creaseEdges.size(); ++k)
    {
        const auto& ce = mesh.creaseEdges[k];
        creases.emplace_back((uint64_t(std::min(ce[0], ce[1])) << 32) | uint64_t(std::max(ce[0], ce[1])),
                             k < mesh.creaseAngles.size() ? mesh.creaseAngles[k] : std::nanf(""));
    }
    std::sort(creases.begin(), creases.end(), [](const auto& l, const auto& r) { return l.first < r.first; });

    const float h2 = h * h;
    auto addLink = [&](int a, int b, uint32_t kind, float limit, float rest, bool closed) {
        GpuLink l{};
        l.v[0] = uint32_t(a);
        l.v[1] = uint32_t(b);
        l.v[2] = kind;
        l.v[3] = closed ? 1u : 0u;
        l.p[0] = limit;
        l.p[1] = rest;
        l.p[2] = std::max(1.f, 10.f * 0.5f * (t.mass[size_t(a)] + t.mass[size_t(b)]) / h2);
        t.links.push_back(l);
    };

    double edgeSum = 0.0;
    size_t edgeCount = 0;
    for (size_t e = 0; e < edges.size();)
    {
        size_t end = e;
        while (end < edges.size() && edges[end].key == edges[e].key)
            ++end;
        const EdgeRec& r1 = edges[e];
        const auto& t1 = tris[size_t(r1.tri)];
        const int a = t1[size_t(r1.local)], b = t1[size_t((r1.local + 1) % 3)];
        const Panel* p1 = panelOf(triPanel[size_t(r1.tri)]);
        const Fabric& f1 = fabricOf(p1);
        const auto& rc1 = restCorners[size_t(r1.tri)];
        const float rex = rc1[size_t((r1.local + 1) % 3)][0] - rc1[size_t(r1.local)][0];
        const float rey = rc1[size_t((r1.local + 1) % 3)][1] - rc1[size_t(r1.local)][1];
        const float restLen = std::sqrt(rex * rex + rey * rey);
        edgeSum += d3Len(d3Sub(toD(mesh.rest3d[size_t(a)]), toD(mesh.rest3d[size_t(b)])));
        ++edgeCount;
        t.edges.push_back({uint32_t(a), uint32_t(b)});
        t.maxHalfEdge = std::max(t.maxHalfEdge, 0.5f * (1.f + std::max(f1.strainLimit, 0.1f)) * restLen);

        if (end - e == 2 && restLen > 1e-9f)
        {
            const EdgeRec& r2 = edges[e + 1];
            GpuHinge hg{};
            hg.v[0] = uint32_t(a);
            hg.v[1] = uint32_t(b);
            hg.v[2] = uint32_t(t1[size_t((r1.local + 2) % 3)]);
            hg.v[3] = uint32_t(tris[size_t(r2.tri)][size_t((r2.local + 2) % 3)]);
            float kBend = 0.5f * (f1.bendWarp + f1.bendWeft);
            if (hasUv)
            {
                const float c2 = rex * rex / (restLen * restLen), s2 = 1.f - c2;
                kBend = f1.bendWeft * c2 * c2 + f1.bendWarp * s2 * s2 + 2.f * f1.bendBias * c2 * s2;
            }
            if (p1 && p1->state == PanelState::Strengthened)
                kBend *= tune.strengthen;
            const float areas = t.tris[size_t(r1.tri)].rest[0] + t.tris[size_t(r2.tri)].rest[0];
            if (areas > 1e-14f)
            {
                hg.p[1] = 6.f * kBend * restLen * restLen / areas;
                const auto c = std::lower_bound(creases.begin(), creases.end(), r1.key, [](const auto& l, uint64_t k) { return l.first < k; });
                const bool crease = c != creases.end() && c->first == r1.key;
                if (crease && std::isfinite(c->second))
                {
                    hg.p[0] = c->second;
                    hg.p[1] *= tune.foldStiffening;
                }
                else if (crease || !hasUv)
                    hg.p[0] = dihedralAngle(mesh.rest3d[hg.v[0]], mesh.rest3d[hg.v[1]], mesh.rest3d[hg.v[2]], mesh.rest3d[hg.v[3]]);
                const uint32_t hi = uint32_t(t.hinges.size());
                t.hinges.push_back(hg);
                for (uint32_t r = 0; r < 4; ++r)
                    vHinge[hg.v[r]].push_back(hi << 2 | r);
            }
        }
        if (f1.strainLimit > 0.f && restLen > 1e-9f && (t.movable[size_t(a)] || t.movable[size_t(b)]))
            addLink(a, b, kStrain, (1.f + f1.strainLimit) * restLen, 0.f, false);
        e = end;
    }

    // Folded seams, as ClothSolver builds them.
    for (const SeamHinge& sh : seamHinges(mesh, g))
    {
        auto restArea = [&](int f) {
            if (!hasUv)
                return 0.f;
            const auto& tri = mesh.tris[size_t(f)];
            const float ax = mesh.uv[size_t(tri[1])][0] - mesh.uv[size_t(tri[0])][0], ay = mesh.uv[size_t(tri[1])][1] - mesh.uv[size_t(tri[0])][1];
            const float bx = mesh.uv[size_t(tri[2])][0] - mesh.uv[size_t(tri[0])][0], by = mesh.uv[size_t(tri[2])][1] - mesh.uv[size_t(tri[0])][1];
            return 0.5f * std::abs(ax * by - bx * ay);
        };
        const float areas = restArea(sh.tris[0]) + restArea(sh.tris[1]);
        const float restLen = float(d3Len(d3Sub(toD(mesh.rest3d[size_t(sh.v[0])]), toD(mesh.rest3d[size_t(sh.v[1])]))));
        if (areas <= 1e-14f || restLen <= 1e-9f)
            continue;
        GpuHinge hg{};
        for (int k = 0; k < 4; ++k)
            hg.v[k] = uint32_t(sh.v[size_t(k)]);
        hg.p[0] = sh.restAngle;
        hg.p[1] = 6.f * sh.bend * restLen * restLen / areas;
        const uint32_t hi = uint32_t(t.hinges.size());
        t.hinges.push_back(hg);
        for (uint32_t r = 0; r < 4; ++r)
            vHinge[hg.v[r]].push_back(hi << 2 | r);
    }

    for (size_t i = 0; i < mesh.seamPairs.size(); ++i)
    {
        const int a = mesh.seamPairs[i][0], b = mesh.seamPairs[i][1];
        if (a < 0 || b < 0 || a >= n || b >= n || a == b || (!t.movable[size_t(a)] && !t.movable[size_t(b)]))
            continue;
        const float rest = i < mesh.seamRestLength.size() ? std::max(0.f, mesh.seamRestLength[i]) : 0.f;
        float limit = std::max(rest, float(d3Len(d3Sub(toD(mesh.rest3d[size_t(a)]), toD(mesh.rest3d[size_t(b)])))));
        const bool closed = rest <= 1e-6f && limit <= tune.closedGap;
        if (closed)
            limit = 0.f;
        addLink(a, b, kSeam, limit, rest, closed);
    }
    for (const SeamShrink& sk : seamShrinks(mesh, g))
        if (t.movable[size_t(sk.a)] || t.movable[size_t(sk.b)])
            addLink(sk.a, sk.b, kStrain, sk.limit, 0.f, false);
    for (uint32_t k = 0; k < uint32_t(t.links.size()); ++k)
    {
        vLink[t.links[k].v[0]].push_back(k << 1);
        vLink[t.links[k].v[1]].push_back(k << 1 | 1);
    }

    // Seam partners act as one vertex for self collision.
    std::vector<int> weld(static_cast<size_t>(n));
    std::iota(weld.begin(), weld.end(), 0);
    for (const GpuLink& l : t.links)
        if (l.v[2] == kSeam)
        {
            const int ra = weldRoot(weld, int(l.v[0])), rb = weldRoot(weld, int(l.v[1]));
            weld[size_t(std::max(ra, rb))] = std::min(ra, rb);
        }
    t.weld.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        t.weld[size_t(i)] = uint32_t(weldRoot(weld, i));

    // Greedy colouring in vertex order over triangles, hinges and links, as ClothSolver does.
    std::vector<std::vector<int>> adj(static_cast<size_t>(n));
    auto connect = [&](int a, int b) {
        adj[size_t(a)].push_back(b);
        adj[size_t(b)].push_back(a);
    };
    for (const auto& tri : tris)
    {
        connect(tri[0], tri[1]);
        connect(tri[1], tri[2]);
        connect(tri[2], tri[0]);
    }
    for (const GpuHinge& hg : t.hinges)
        connect(int(hg.v[2]), int(hg.v[3]));
    for (const GpuLink& l : t.links)
        connect(int(l.v[0]), int(l.v[1]));
    std::vector<int> col(size_t(n), -1), usedBy;
    std::vector<std::vector<uint32_t>> colours;
    for (int i = 0; i < n; ++i)
    {
        if (!t.movable[size_t(i)])
            continue;
        for (int nb : adj[size_t(i)])
            if (col[size_t(nb)] >= 0)
            {
                if (int(usedBy.size()) <= col[size_t(nb)])
                    usedBy.resize(size_t(col[size_t(nb)]) + 1, -1);
                usedBy[size_t(col[size_t(nb)])] = i;
            }
        int c = 0;
        while (c < int(usedBy.size()) && usedBy[size_t(c)] == i)
            ++c;
        col[size_t(i)] = c;
        if (int(colours.size()) <= c)
            colours.resize(size_t(c) + 1);
        colours[size_t(c)].push_back(uint32_t(i));
    }
    toCsr(colours, t.colourOff, t.colour);

    float maxThickness = 0.f;
    for (float th : thickness)
        maxThickness = std::max(maxThickness, th);
    const float avgEdge = edgeCount ? float(edgeSum / double(edgeCount)) : 0.02f;
    t.queryRadius = std::max(3.f * (s.collisionThickness + maxThickness), 0.5f * avgEdge);
    t.cellSize = std::max(t.queryRadius, avgEdge);

    t.x.resize(size_t(n));
    t.vinfo.resize(size_t(n));
    t.vextra.resize(size_t(n));
    t.flat.resize(size_t(n));
    for (int i = 0; i < n; ++i)
    {
        const size_t u = size_t(i);
        t.x[u] = {mesh.rest3d[u][0], mesh.rest3d[u][1], mesh.rest3d[u][2], 0.f};
        t.vinfo[u] = {t.mass[u], bitsToFloat(t.movable[u] ? kMovable : 0u), friction[u], damping[u]};
        t.vextra[u] = {thickness[u], tune.contactStiffness + 1e3f * t.mass[u] / h2, std::max(1.f, 10.f * t.mass[u] / h2), 0.f};
        // Without a pattern every vertex gets its own panel id, so nothing is skipped.
        const uint32_t panel = hasUv && u < mesh.vertPanel.size() ? uint32_t(mesh.vertPanel[u]) : 0x80000000u | uint32_t(u);
        t.flat[u] = {hasUv ? mesh.uv[u][0] : 0.f, hasUv ? mesh.uv[u][1] : 0.f, bitsToFloat(panel), 0.f};
    }
    toCsr(vTri, t.triOff, t.triAdj);
    toCsr(vHinge, t.hingeOff, t.hingeAdj);
    toCsr(vLink, t.linkOff, t.linkAdj);
    return t;
}

uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v && p < (1u << 30))
        p <<= 1;
    return p;
}

bool validGrid(const Sdf& s) { return s.nx >= 2 && s.ny >= 2 && s.nz >= 2 && s.cell > 0.f && s.values.size() >= size_t(s.nx) * s.ny * s.nz; }

/** Session ids past the topology buffers. */
constexpr int kSceneBuffer = BufCount;
} // namespace

struct ClothGpu::Impl
{
    std::mutex mutex;
    GpuTuning tune;
    SolverSettings settings;
    float h = 1.f / 120.f;
    Topology topo;
    std::vector<Vec3> positions;
    bool ready = false;

    // Host-side changes, uploaded at the next step.
    std::vector<Sdf> colliders;
    std::array<std::array<float, 12>, kMaxColliders> colliderXf{};
    bool collidersDirty = false;
    std::vector<Pin> pins;
    std::vector<int> pinVerts; // the pinned set the duals belong to
    bool pinsDirty = false;
    bool pinDualsReset = false;
    std::vector<Vec3> pendingPositions;

    gpu::Session session;
    uint32_t hashSize = 0;
    uint32_t liveColliders = 0;

    Impl()
    {
        for (auto& xf : colliderXf)
            xf = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    }

    static void upload(gpu::Recorder& r, int id, const void* data, size_t bytes, gpu::BufferKind kind = gpu::BufferKind::Storage)
    {
        r.buffer(id, bytes, kind);
        if (data && bytes)
            r.upload(id, data, bytes);
        else
            r.fill(id, 0);
    }

    template <class T> static void upload(gpu::Recorder& r, int id, const std::vector<T>& v)
    {
        upload(r, id, v.data(), v.size() * sizeof(T));
    }

    /** Pin targets for every vertex; only movable vertices take pins, as in ClothSolver. */
    std::vector<F4> pinTargets() const
    {
        std::vector<F4> p(topo.n, F4{0, 0, 0, 0});
        for (const Pin& pin : pins)
            if (pin.vertex >= 0 && uint32_t(pin.vertex) < topo.n && topo.movable[size_t(pin.vertex)])
                p[size_t(pin.vertex)] = {pin.target[0], pin.target[1], pin.target[2], 1.f};
        return p;
    }

    std::vector<F4> freshPinDuals() const
    {
        std::vector<F4> d(topo.n);
        for (uint32_t i = 0; i < topo.n; ++i)
            d[i] = {0.f, 0.f, 0.f, topo.vextra[i][2]};
        return d;
    }

    void loadKernels(gpu::Recorder& r)
    {
        for (int k = 0; k < KernelCount; ++k)
            r.kernel(k, "cloth_vbd.spv", kEntries[k], sizeof(ClothPush));
    }

    /** Uploads the whole topology and state. */
    void uploadAll(gpu::Recorder& r)
    {
        const uint32_t n = topo.n, nt = uint32_t(topo.tris.size());
        upload(r, BX, topo.x);
        upload(r, BXStart, topo.x);
        upload(r, BXNew, topo.x);
        upload(r, BXDetect, topo.x);
        upload(r, BY, topo.x);
        upload(r, BVel, std::vector<F4>(n, F4{0, 0, 0, 0}));
        upload(r, BVInfo, topo.vinfo);
        upload(r, BVExtra, topo.vextra);
        upload(r, BPin, pinTargets());
        upload(r, BPinDual, freshPinDuals());
        upload(r, BTriOff, topo.triOff);
        upload(r, BTriAdj, topo.triAdj);
        upload(r, BHingeOff, topo.hingeOff);
        upload(r, BHingeAdj, topo.hingeAdj);
        upload(r, BLinkOff, topo.linkOff);
        upload(r, BLinkAdj, topo.linkAdj);
        upload(r, BColour, topo.colour);
        upload(r, BWeld, topo.weld);
        upload(r, BTris, topo.tris);
        upload(r, BHinges, topo.hinges);
        upload(r, BLinks, topo.links);
        std::vector<F4> linkDual(topo.links.size());
        for (size_t k = 0; k < topo.links.size(); ++k)
            linkDual[k] = {0.f, 0.f, 0.f, topo.links[k].p[2]};
        upload(r, BLinkDual, linkDual);
        upload(r, BColliders, nullptr, size_t(kMaxColliders) * 5 * sizeof(F4));
        upload(r, BCellHead, nullptr, size_t(hashSize) * 4);
        upload(r, BCellNext, nullptr, size_t(n) * 4);
        upload(r, BVtCount, nullptr, size_t(n) * 4);
        upload(r, BVt, nullptr, size_t(n) * kMaxVt * 4);
        upload(r, BTvCount, nullptr, size_t(n) * 4);
        upload(r, BTv, nullptr, size_t(n) * kMaxTv * 8);
        upload(r, BDminV, nullptr, size_t(n) * 4);
        upload(r, BDminT, nullptr, size_t(nt) * 4);
        upload(r, BBound, std::vector<float>(n, 1e30f));
        upload(r, BState, nullptr, size_t(kStateWords) * 4, gpu::BufferKind::Indirect);
        std::vector<std::array<float, 2>> body(n);
        for (uint32_t i = 0; i < n; ++i)
            body[i] = {topo.vextra[i][1], 0.f};
        upload(r, BBody, body);
        upload(r, BFlat, topo.flat);
        const size_t ne = std::max<size_t>(topo.edges.size(), 1);
        upload(r, BEdges, topo.edges.empty() ? nullptr : topo.edges.data(), ne * 8);
        upload(r, BEdgeHead, nullptr, size_t(hashSize) * 4);
        upload(r, BEdgeNext, nullptr, ne * 4);
        upload(r, BEeCount, nullptr, size_t(n) * 4);
        upload(r, BEe, nullptr, size_t(n) * kMaxEe * 8);
        upload(r, BEeSide, nullptr, size_t(n) * kMaxEe * 16);
        upload(r, BSpare, nullptr, 16);
        r.buffer(kSceneBuffer, sizeof(GpuScene));
    }

    void uploadColliders(gpu::Recorder& r)
    {
        liveColliders = 0;
        for (uint32_t c = 0; c < kMaxColliders; ++c)
        {
            const int id = BGrid0 + int(c);
            if (c < colliders.size() && validGrid(colliders[c]))
            {
                upload(r, id, colliders[c].values.data(), size_t(colliders[c].nx) * colliders[c].ny * colliders[c].nz * 4);
                liveColliders = c + 1;
            }
            else
                upload(r, id, nullptr, 16);
        }
    }

    GpuScene sceneData(const gpu::Recorder& r) const
    {
        GpuScene g{};
        for (int b = 0; b < BufCount; ++b)
            g.ptr[b] = r.address(b);
        g.nVerts = topo.n;
        g.nTris = uint32_t(topo.tris.size());
        g.nHinges = uint32_t(topo.hinges.size());
        g.nLinks = uint32_t(topo.links.size());
        g.nColliders = liveColliders;
        g.hashMask = hashSize - 1;
        g.selfCollision = settings.selfCollision ? 1u : 0u;
        g.nMovable = uint32_t(topo.colour.size());
        g.h = h;
        g.lambdaDecay = tune.lambdaDecay;
        g.stiffnessDecay = tune.stiffnessDecay;
        g.beta = tune.beta;
        g.kMax = tune.kMax;
        g.thickness = settings.collisionThickness;
        g.queryRadius = topo.queryRadius;
        g.cellSize = topo.cellSize;
        g.boundFraction = tune.boundFraction;
        g.frictionSlip = tune.frictionSlip;
        g.drag = 1.f / (1.f + tune.airDrag * h);
        g.seamKeep = 1.f - std::clamp(settings.seamCloseRate, 0.f, 1.f);
        g.closedGap = tune.closedGap;
        g.maxHalfEdge = topo.maxHalfEdge;
        g.nEdges = uint32_t(topo.edges.size());
        g.floorY = settings.floorY;
        for (int k = 0; k < 3; ++k)
            g.gravity[k] = settings.gravity[size_t(k)];
        return g;
    }

    /** Grid transform rows, origin and cell, dims (as uint bits) per collider; an invalid grid gets zero dims. */
    std::vector<F4> colliderRecords() const
    {
        std::vector<F4> rec(size_t(kMaxColliders) * 5, F4{0, 0, 0, 0});
        for (size_t c = 0; c < colliders.size() && c < kMaxColliders; ++c)
        {
            const Sdf& s = colliders[c];
            const auto& xf = colliderXf[c];
            for (size_t row = 0; row < 3; ++row)
                rec[c * 5 + row] = {xf[row * 4], xf[row * 4 + 1], xf[row * 4 + 2], xf[row * 4 + 3]};
            rec[c * 5 + 3] = {s.origin[0], s.origin[1], s.origin[2], s.cell};
            if (validGrid(s))
                rec[c * 5 + 4] = {bitsToFloat(uint32_t(s.nx)), bitsToFloat(uint32_t(s.ny)), bitsToFloat(uint32_t(s.nz)), 0.f};
        }
        return rec;
    }

    /** Items [first, first + count) of kernel k in chunks under the group limit, then a barrier. */
    void dispatch(gpu::Recorder& r, uint64_t scene, Kernel k, uint32_t first, uint32_t count)
    {
        const uint32_t chunk = gpu::Recorder::maxGroups * kGroup;
        for (uint32_t done = 0; done < count; done += chunk)
        {
            const uint32_t c = std::min(chunk, count - done);
            ClothPush push{scene, first + done, c, 0, 0};
            r.dispatch(k, &push, (c + kGroup - 1) / kGroup);
        }
        if (count)
            r.barrier();
    }

    /** Re-detection sized by redetectArgsCS: zero groups unless a vertex hit its bound in the last iteration. */
    void dispatchIndirect(gpu::Recorder& r, uint64_t scene, Kernel k, uint32_t count, uint32_t slot)
    {
        ClothPush push{scene, 0, count, 0, 0};
        r.dispatchIndirect(k, &push, BState, size_t(4 + slot * 3) * 4);
        r.barrier();
    }

    void detect(gpu::Recorder& r, uint64_t scene)
    {
        dispatch(r, scene, KHashClear, 0, hashSize);
        dispatch(r, scene, KHashInsert, 0, topo.n);
        dispatch(r, scene, KEdgeInsert, 0, uint32_t(topo.edges.size()));
        dispatch(r, scene, KHashQuery, 0, uint32_t(topo.tris.size()));
        dispatch(r, scene, KEdgeQuery, 0, uint32_t(topo.edges.size()));
        dispatch(r, scene, KContactFinalize, 0, topo.n);
    }

    void record(gpu::Recorder& r, uint64_t scene)
    {
        const uint32_t n = topo.n, nl = uint32_t(topo.links.size()), nt = uint32_t(topo.tris.size());
        const bool self = settings.selfCollision;
        const int iterations = std::max(1, settings.iterations);
        // Indirect dispatches take no chunking; past their limit re-detection waits for the next substep.
        const uint32_t ne = uint32_t(topo.edges.size());
        const bool indirect = std::max({hashSize, n, nt, ne}) <= kMaxIndirectItems;

        dispatch(r, scene, KAdvanceSeams, 0, nl);
        for (int sub = 0; sub < std::max(1, settings.substeps); ++sub)
        {
            dispatch(r, scene, KBeginStep, 0, n);
            dispatch(r, scene, KBeginLinks, 0, nl);
            if (self)
                detect(r, scene);
            dispatch(r, scene, KInitGuess, 0, n);
            for (int it = 0; it < iterations; ++it)
            {
                for (size_t c = 0; c + 1 < topo.colourOff.size(); ++c)
                {
                    const uint32_t first = topo.colourOff[c], count = topo.colourOff[c + 1] - first;
                    dispatch(r, scene, KSolve, first, count);
                    dispatch(r, scene, KCommit, first, count);
                }
                dispatch(r, scene, KDualLinks, 0, nl);
                dispatch(r, scene, KDualPins, 0, n);
                if (self && indirect && it % tune.redetectEvery == tune.redetectEvery - 1 && it + 1 < iterations)
                {
                    dispatch(r, scene, KRedetectArgs, 0, 1);
                    dispatchIndirect(r, scene, KHashClear, hashSize, 0);
                    dispatchIndirect(r, scene, KHashInsert, n, 1);
                    dispatchIndirect(r, scene, KEdgeInsert, ne, 4);
                    dispatchIndirect(r, scene, KHashQuery, nt, 2);
                    dispatchIndirect(r, scene, KEdgeQuery, ne, 5);
                    dispatchIndirect(r, scene, KContactFinalize, n, 3);
                }
            }
            dispatch(r, scene, KEndStep, 0, n);
        }
        r.readback(BX, size_t(n) * sizeof(F4));
    }

    /** A reset's upload (uploadTopology) or one frame (simulate). Under mutex. */
    bool run(bool uploadTopology, bool simulate, std::string* err)
    {
        if (!uploadTopology && !session.valid())
        {
            if (err)
                *err = "Tailor: the GPU solver lost its device state; reset it";
            return false;
        }
        const bool ok = session.run(
            [&](gpu::Recorder& r) {
                loadKernels(r);
                if (uploadTopology)
                    uploadAll(r);
                if (collidersDirty || uploadTopology)
                {
                    uploadColliders(r);
                    collidersDirty = false;
                }
                if (pinsDirty && !uploadTopology)
                {
                    upload(r, BPin, pinTargets());
                    if (pinDualsReset)
                        upload(r, BPinDual, freshPinDuals());
                }
                pinsDirty = pinDualsReset = false;
                if (pendingPositions.size() == topo.n)
                {
                    std::vector<F4> x(topo.n);
                    for (uint32_t i = 0; i < topo.n; ++i)
                        x[i] = {pendingPositions[i][0], pendingPositions[i][1], pendingPositions[i][2], 0.f};
                    for (int b : {BX, BXStart, BXNew, BXDetect, BY})
                        upload(r, b, x);
                    upload(r, BVel, std::vector<F4>(topo.n, F4{0, 0, 0, 0}));
                }
                pendingPositions.clear();
                const GpuScene g = sceneData(r);
                r.upload(kSceneBuffer, &g, sizeof g);
                const std::vector<F4> rec = colliderRecords();
                r.upload(BColliders, rec.data(), rec.size() * sizeof(F4));
                r.barrier();
                if (simulate)
                    record(r, r.address(kSceneBuffer));
            },
            err);
        if (!ok || !simulate)
            return ok;
        std::vector<F4> x(topo.n);
        if (!session.read(BX, x.data(), x.size() * sizeof(F4)))
        {
            if (err)
                *err = "Tailor: the GPU solver read back nothing";
            return false;
        }
        positions.resize(topo.n);
        for (uint32_t i = 0; i < topo.n; ++i)
            positions[i] = {x[i][0], x[i][1], x[i][2]};
        return true;
    }
};

ClothGpu::ClothGpu() : impl_(std::make_shared<Impl>()) {}

ClothGpu::~ClothGpu() = default;

bool ClothGpu::available() { return gpu::available(); }

bool ClothGpu::reset(const ClothMesh& mesh, const Garment& g, const SolverSettings& s, std::string* err)
{
    std::scoped_lock lock(impl_->mutex);
    Impl& m = *impl_;
    m.settings = s;
    m.settings.substeps = std::max(1, s.substeps);
    m.settings.iterations = std::max(1, s.iterations);
    m.h = std::max(s.dt, 1e-6f) / float(m.settings.substeps);
    m.topo = buildTopology(mesh, g, m.settings, m.tune, m.h);
    m.positions = mesh.rest3d;
    m.hashSize = nextPow2(std::max<uint32_t>(2 * m.topo.n, 1));
    m.pinVerts.clear();
    for (const Pin& p : m.pins)
        m.pinVerts.push_back(p.vertex);
    m.ready = false;
    if (m.topo.n == 0)
        return true;
    if (!m.run(true, false, err))
        return false;
    m.ready = true;
    return true;
}

void ClothGpu::setColliders(std::vector<const Sdf*> colliders)
{
    std::scoped_lock lock(impl_->mutex);
    impl_->colliders.clear();
    for (const Sdf* s : colliders)
        if (s && impl_->colliders.size() < kMaxColliders)
            impl_->colliders.push_back(*s);
    impl_->collidersDirty = true;
}

void ClothGpu::setColliderTransform(int index, const std::array<float, 12>& worldToGrid)
{
    std::scoped_lock lock(impl_->mutex);
    if (index >= 0 && index < int(kMaxColliders))
        impl_->colliderXf[size_t(index)] = worldToGrid;
}

void ClothGpu::setPins(std::vector<Pin> pins)
{
    std::scoped_lock lock(impl_->mutex);
    Impl& m = *impl_;
    std::vector<int> verts;
    for (const Pin& p : pins)
        verts.push_back(p.vertex);
    // Moving a pin keeps its dual warm; a different pinned set starts over, as ClothSolver::syncPins does.
    if (verts != m.pinVerts)
    {
        m.pinVerts = std::move(verts);
        m.pinDualsReset = true;
    }
    m.pins = std::move(pins);
    m.pinsDirty = true;
}

void ClothGpu::setPositions(std::vector<Vec3> positions)
{
    std::scoped_lock lock(impl_->mutex);
    impl_->positions = positions;
    impl_->pendingPositions = std::move(positions);
}

bool ClothGpu::step(std::string* err)
{
    std::scoped_lock lock(impl_->mutex);
    Impl& m = *impl_;
    if (m.topo.n == 0)
        return true;
    if (!m.ready)
    {
        if (err)
            *err = "Tailor: the GPU solver has no device state; reset it";
        return false;
    }
    if (!m.run(false, true, err))
    {
        m.ready = false;
        return false;
    }
    return true;
}

const std::vector<Vec3>& ClothGpu::positions() const { return impl_->positions; }

std::vector<float> ClothGpu::strainMap() const
{
    std::scoped_lock lock(impl_->mutex);
    const Impl& m = *impl_;
    const auto& x = m.positions;
    std::vector<float> out(x.size(), 1.f), sum(x.size(), 0.f), count(x.size(), 0.f);
    for (const GpuTri& t : m.topo.tris)
    {
        if (t.rest[0] <= 0.f || t.v[0] >= x.size() || t.v[1] >= x.size() || t.v[2] >= x.size())
            continue;
        const D3 x0 = toD(x[t.v[0]]), e1 = d3Sub(toD(x[t.v[1]]), x0), e2 = d3Sub(toD(x[t.v[2]]), x0);
        const D3 f0{e1[0] * t.b[0] + e2[0] * t.b[2], e1[1] * t.b[0] + e2[1] * t.b[2], e1[2] * t.b[0] + e2[2] * t.b[2]};
        const D3 f1{e1[0] * t.b[1] + e2[0] * t.b[3], e1[1] * t.b[1] + e2[1] * t.b[3], e1[2] * t.b[1] + e2[2] * t.b[3]};
        const double a = d3Dot(f0, f0), b = d3Dot(f0, f1), c = d3Dot(f1, f1);
        const double half = 0.5 * (a + c);
        const float stretch = float(std::sqrt(std::max(0.0, half + std::sqrt(std::max(0.0, half * half - (a * c - b * b))))));
        for (int k = 0; k < 3; ++k)
        {
            sum[t.v[k]] += stretch;
            count[t.v[k]] += 1.f;
        }
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (count[i] > 0.f)
            out[i] = sum[i] / count[i];
    return out;
}

GpuTuning& ClothGpu::tuning() { return impl_->tune; }

int ClothGpu::colourCount() const { return impl_->topo.colourOff.empty() ? 0 : int(impl_->topo.colourOff.size()) - 1; }
} // namespace tailor
