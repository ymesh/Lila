// Tailor cloth on the GPU: Vertex Block Descent (Chen, Macklin, Yuksel 2024) with Augmented VBD hard constraints
// (Giles et al. 2025). Each vertex takes a 3x3 Newton step on its local energy; vertices of one graph colour share no
// element and solve in parallel. ClothSolver.cpp is the reference: every term here mirrors its solveVertex, detect,
// updateDuals and substep, and ClothGpu.cpp drives these kernels.

static const uint kMovable = 1u;
static const uint kNone = 0xffffffffu;
static const uint kSideBit = 0x80000000u; // set on a contact's triangle index when the vertex was behind the triangle
static const uint kMaxVt = 16u;           // triangles near a vertex
static const uint kMaxTv = 32u;           // contacts a vertex takes part in as a triangle corner
static const uint kMaxEe = 32u;           // edge-edge contacts a vertex takes part in
static const uint kSeam = 0u;
static const uint kMaxColliders = 4u;

struct Tri
{
    uint4 v;      // a, b, c, unused
    float4 dmInv; // b00, b01, b10, b11: inverse rest shape in the grain frame
    float4 rest;  // area (0 = no membrane), warp, weft, shear stiffness
};

struct Hinge
{
    uint4 v;  // edge v0-v1, opposite corners v2 (first triangle) and v3
    float4 p; // rest angle, stiffness (energy 0.5 k (angle - rest)^2), unused, unused
};

struct Link
{
    uint4 v;  // a, b, kind (0 seam, 1 strain), closed
    float4 p; // limit, rest, kStart, unused
};

struct ClothScene
{
    float4* x;       // current iterate
    float4* xStart;  // start of the substep
    float4* xNew;    // a colour's results before commit
    float4* xDetect; // positions self contact was detected at
    float4* y;       // inertial target
    float4* vel;
    float4* vinfo;   // mass, flags (bits), friction, damping
    float4* vextra;  // fabric thickness, contact stiffness, pin kStart, unused
    float4* pin;     // target xyz, w > 0 when pinned
    float4* pinDual; // lambda xyz, stiffness

    uint* triOff;
    uint* triAdj;    // tri << 2 | corner
    uint* hingeOff;
    uint* hingeAdj;  // hinge << 2 | role
    uint* linkOff;
    uint* linkAdj;   // link << 1 | side
    uint* colour;    // movable vertices grouped by colour
    uint* weld;      // seam partners share a root and never collide with each other

    Tri* tris;
    Hinge* hinges;
    Link* links;
    float4* linkDual;  // lambda (xyz closed, x open), stiffness
    float4* colliders; // 5 per collider: world to grid rows (3), origin + cell, dims (uint bits)

    uint* cellHead;
    uint* cellNext;
    uint* vtCount;
    uint* vt;  // triangle | kSideBit
    uint* tvCount;
    uint2* tv; // vertex, triangle | kSideBit
    uint* dminV;  // float bits
    uint* dminT;  // float bits
    float* bound;
    uint* state;  // [0] a vertex hit its bound this iteration; [4..] indirect dispatch args for re-detection
    float2* body;   // collider contact as an AVBD constraint per vertex: stiffness, normal force
    float* grid0;   // collider distance grids, x fastest
    float* grid1;
    float* grid2;
    float* grid3;
    float4* flat;   // pattern position (xy) and panel (z, bits) per vertex
    uint2* edges;
    uint* edgeHead; // edge hash by midpoint cell
    uint* edgeNext;
    uint* eeCount;
    uint2* ee;      // per vertex: edge-edge contacts (e1, e2) it is in
    float4* eeSide; // the way e1 lay from e2 when found
    uint* spare;

    uint nVerts, nTris, nHinges, nLinks;
    uint nColliders, hashMask, selfCollision, nMovable;
    float h, lambdaDecay, stiffnessDecay, beta;
    float kMax, thickness, queryRadius, cellSize;
    float boundFraction, frictionSlip, drag, seamKeep;
    float closedGap, floorY, maxHalfEdge;
    uint nEdges;
    float4 gravity;
};

struct ClothPush
{
    ClothScene* scene;
    uint first;
    uint count;
    uint pass;
    uint pad;
};

[[vk::push_constant]] ClothPush pc;

// ------------------------------------------------------------------------------------------------ math

static const float3x3 kI = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
static const float kPi = 3.14159265358979;

/** a b^T */
float3x3 outer(float3 a, float3 b) { return float3x3(b * a.x, b * a.y, b * a.z); }

/** Solves A x = b by the adjugate; false when A is singular or the result is not finite. */
bool solve3(float3x3 A, float3 b, out float3 x)
{
    float3 c0 = cross(A[1], A[2]), c1 = cross(A[2], A[0]), c2 = cross(A[0], A[1]);
    float det = dot(A[0], c0);
    x = float3(0.0);
    if (!(abs(det) > 1e-30) || isinf(det))
        return false;
    x = (c0 * b.x + c1 * b.y + c2 * b.z) / det;
    return all(isfinite(x));
}

float wrapAngle(float a)
{
    // atan2 differences stay within (-2 pi, 2 pi), so one fold each way is enough.
    if (a > kPi)
        a -= 2.0 * kPi;
    if (a < -kPi)
        a += 2.0 * kPi;
    return a;
}

/** Barycentric weights of the point on triangle abc closest to p (Ericson), as ClothSolver's closestOnTriangle. */
float3 closestBary(float3 p, float3 a, float3 b, float3 c)
{
    float3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return float3(1, 0, 0);
    float3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return float3(0, 1, 0);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        float t = d1 / (d1 - d3);
        return float3(1.0 - t, t, 0.0);
    }
    float3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return float3(0, 0, 1);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        float t = d2 / (d2 - d6);
        return float3(1.0 - t, 0.0, t);
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0)
    {
        float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return float3(0.0, 1.0 - t, t);
    }
    float sum = va + vb + vc;
    if (abs(sum) < 1e-30)
        return float3(1, 0, 0);
    float v = vb / sum, w = vc / sum;
    return float3(1.0 - v - w, v, w);
}

float3 P(ClothScene* s, uint i) { return s->x[i].xyz; }
bool movable(ClothScene* s, uint i) { return (asuint(s->vinfo[i].y) & kMovable) != 0; }

float3 clampToBound(ClothScene* s, uint i, float3 p, out bool clamped)
{
    float3 d = p - s->xDetect[i].xyz;
    float l2 = dot(d, d);
    float b = s->bound[i];
    clamped = l2 > b * b;
    return clamped ? s->xDetect[i].xyz + d * (b / sqrt(l2)) : p;
}

// ------------------------------------------------------------------------------------------------ SDF colliders

float texel(ClothScene* s, uint c, int3 p, int3 dims)
{
    uint i = uint((p.z * dims.y + p.y) * dims.x + p.x);
    switch (c)
    {
    case 0:
        return s->grid0[i];
    case 1:
        return s->grid1[i];
    case 2:
        return s->grid2[i];
    default:
        return s->grid3[i];
    }
}

/** Sdf::sample: trilinear over grid nodes at origin + i * cell, clamped to the grid plus the distance outside it. */
float sdfSample(ClothScene* s, uint c, float3 p, out float3 grad)
{
    float4 r0 = s->colliders[c * 5], r1 = s->colliders[c * 5 + 1], r2 = s->colliders[c * 5 + 2];
    float4 oc = s->colliders[c * 5 + 3];
    int3 dims = int3(asuint(s->colliders[c * 5 + 4].xyz));
    grad = float3(0, 1, 0);
    if (any(dims < 2) || oc.w <= 0.0)
        return 1e30;
    float3 local = float3(dot(r0, float4(p, 1.0)), dot(r1, float4(p, 1.0)), dot(r2, float4(p, 1.0)));
    float3 u = (local - oc.xyz) / oc.w;
    float3 uc = clamp(u, float3(0.0), float3(dims - 1));
    float3 out3 = (u - uc) * oc.w;
    int3 i0 = min(int3(uc), dims - 2);
    float3 fr = uc - float3(i0);
    float c000 = texel(s, c, i0, dims), c100 = texel(s, c, i0 + int3(1, 0, 0), dims);
    float c010 = texel(s, c, i0 + int3(0, 1, 0), dims), c110 = texel(s, c, i0 + int3(1, 1, 0), dims);
    float c001 = texel(s, c, i0 + int3(0, 0, 1), dims), c101 = texel(s, c, i0 + int3(1, 0, 1), dims);
    float c011 = texel(s, c, i0 + int3(0, 1, 1), dims), c111 = texel(s, c, i0 + int3(1, 1, 1), dims);
    float fx = fr.x, fy = fr.y, fz = fr.z;
    float c00 = c000 + (c100 - c000) * fx, c10 = c010 + (c110 - c010) * fx;
    float c01 = c001 + (c101 - c001) * fx, c11 = c011 + (c111 - c011) * fx;
    float cc0 = c00 + (c10 - c00) * fy, cc1 = c01 + (c11 - c01) * fy;
    float value = cc0 + (cc1 - cc0) * fz;
    float gx = (c100 - c000) * (1 - fy) * (1 - fz) + (c110 - c010) * fy * (1 - fz) + (c101 - c001) * (1 - fy) * fz + (c111 - c011) * fy * fz;
    float gy = (c10 - c00) * (1 - fz) + (c11 - c01) * fz;
    float gz = cc1 - cc0;
    float3 g = float3(gx, gy, gz) / oc.w;
    // The grid transform is rigid, so its transpose takes the gradient back to world space.
    grad = r0.xyz * g.x + r1.xyz * g.y + r2.xyz * g.z;
    return value + length(out3);
}

/** ClothSolver::bodyContact: the deepest penetration into any collider and its push-out normal; false when clear. */
bool bodyContact(ClothScene* s, uint i, float3 p, out float pen, out float3 n)
{
    float r = s->thickness + 0.5 * s->vextra[i].x;
    pen = 0.0;
    n = float3(0, 1, 0);
    for (uint c = 0; c < s->nColliders; ++c)
    {
        float3 g;
        float phi = sdfSample(s, c, p, g);
        if (phi >= r || r - phi <= pen)
            continue;
        float gl = length(g);
        if (gl < 1e-8)
            continue;
        pen = r - phi;
        n = g / gl;
    }
    float floorPen = r - (p.y - s->floorY);
    if (floorPen > pen)
    {
        pen = floorPen;
        n = float3(0, 1, 0);
    }
    return pen > 0.0;
}

void addSdfContacts(ClothScene* s, uint i, float3 xi, float3 xs, inout float3 f, inout float3x3 H)
{
    float pen;
    float3 n;
    if (!bodyContact(s, i, xi, pen, n))
        return;
    float2 dual = s->body[i];
    float kc = dual.x;
    float fn = max(0.0, kc * pen + dual.y);
    f += fn * n;
    float3x3 nn = outer(n, n);
    H += kc * nn;
    // Lagged friction against the tangential slip this substep.
    float3 slip = xi - xs;
    float3 u = slip - n * dot(n, slip);
    float scale = s->vinfo[i].z * fn / max(length(u), s->frictionSlip);
    f -= scale * u;
    H += scale * (kI - nn);
}

// ------------------------------------------------------------------------------------------------ elastic terms

void addMembrane(ClothScene* s, uint i, inout float3 fe, inout float3x3 He, inout float3x3 Hg)
{
    for (uint k = s->triOff[i]; k < s->triOff[i + 1]; ++k)
    {
        uint e = s->triAdj[k];
        Tri t = s->tris[e >> 2];
        if (t.rest.x <= 0.0)
            continue;
        uint role = e & 3u;
        float b00 = t.dmInv.x, b01 = t.dmInv.y, b10 = t.dmInv.z, b11 = t.dmInv.w;
        float3 x0 = P(s, t.v.x), e1 = P(s, t.v.y) - x0, e2 = P(s, t.v.z) - x0;
        float3 f0 = e1 * b00 + e2 * b10, f1 = e1 * b01 + e2 * b11;
        float E00 = 0.5 * (dot(f0, f0) - 1.0), E11 = 0.5 * (dot(f1, f1) - 1.0), E01 = 0.5 * dot(f0, f1);
        float kWarp = t.rest.y, kWeft = t.rest.z, kShear = t.rest.w;
        float S00 = kWarp * E00, S11 = kWeft * E11, S01 = 2.0 * kShear * E01;
        float c0, c1;
        if (role == 1)
        {
            c0 = b00;
            c1 = b01;
        }
        else if (role == 2)
        {
            c0 = b10;
            c1 = b11;
        }
        else
        {
            c0 = -(b00 + b10);
            c1 = -(b01 + b11);
        }
        fe -= t.rest.x * (f0 * (S00 * c0 + S01 * c1) + f1 * (S01 * c0 + S11 * c1));
        float3 w = c1 * f0 + c0 * f1;
        // The geometric term goes indefinite under compression; its negative part is dropped to keep the block SPD.
        float geometric = max(0.0, S00 * c0 * c0 + 2.0 * S01 * c0 * c1 + S11 * c1 * c1);
        He += t.rest.x * (kWarp * c0 * c0 * outer(f0, f0) + kWeft * c1 * c1 * outer(f1, f1) + kShear * outer(w, w));
        Hg += (t.rest.x * geometric) * kI;
    }
}

/** ClothSolver's dihedral: the angle across edge ab between abc and abd, and the gradient for vertex `role`. */
bool dihedral(float3 a, float3 b, float3 c, float3 d, uint role, out float theta, out float3 grad)
{
    theta = 0.0;
    grad = float3(0.0);
    float3 e = b - a;
    float el2 = dot(e, e);
    if (el2 < 1e-16)
        return false;
    float el = sqrt(el2);
    float3 n1 = cross(e, c - a), n2 = cross(d - a, e);
    float l1 = length(n1), l2 = length(n2);
    if (l1 < 1e-12 || l2 < 1e-12)
        return false;
    float3 u1 = n1 / l1, u2 = n2 / l2;
    theta = atan2(dot(cross(u1, u2), e) / el, dot(u1, u2));
    float3 gc = -u1 * (el / l1), gd = -u2 * (el / l2);
    float tc = dot(c - a, e) / el2, td = dot(d - a, e) / el2;
    if (role == 0)
        grad = -(1.0 - tc) * gc - (1.0 - td) * gd;
    else if (role == 1)
        grad = -tc * gc - td * gd;
    else if (role == 2)
        grad = gc;
    else
        grad = gd;
    return true;
}

void addBending(ClothScene* s, uint i, inout float3 fe, inout float3x3 He)
{
    for (uint k = s->hingeOff[i]; k < s->hingeOff[i + 1]; ++k)
    {
        uint e = s->hingeAdj[k];
        Hinge hg = s->hinges[e >> 2];
        float theta;
        float3 g;
        if (!dihedral(P(s, hg.v.x), P(s, hg.v.y), P(s, hg.v.z), P(s, hg.v.w), e & 3u, theta, g))
            continue;
        fe -= hg.p.y * wrapAngle(theta - hg.p.x) * g;
        He += hg.p.y * outer(g, g);
    }
}

// ------------------------------------------------------------------------------------------------ hard constraints

void addLinks(ClothScene* s, uint i, inout float3 f, inout float3x3 H)
{
    for (uint k = s->linkOff[i]; k < s->linkOff[i + 1]; ++k)
    {
        uint e = s->linkAdj[k];
        Link l = s->links[e >> 1];
        float4 dual = s->linkDual[e >> 1];
        float sgn = (e & 1u) == 0 ? 1.0 : -1.0;
        float3 d = P(s, l.v.x) - P(s, l.v.y);
        if (l.v.w != 0)
        {
            f -= sgn * (dual.w * d + dual.xyz);
            H += dual.w * kI;
            continue;
        }
        float len = length(d);
        if (len < 1e-9)
            continue;
        float F = dual.w * (len - l.p.x) + dual.x;
        if (F <= 0.0)
            continue;
        float3 nd = d / len;
        float3x3 nn = outer(nd, nd);
        f -= sgn * F * nd;
        H += dual.w * nn + (F / len) * (kI - nn);
    }

    float4 pin = s->pin[i];
    if (pin.w > 0.0)
    {
        float4 pd = s->pinDual[i];
        f -= pd.w * (P(s, i) - pin.xyz) + pd.xyz;
        H += pd.w * kI;
    }
}

// ------------------------------------------------------------------------------------------------ self contact

/**
 * ClothSolver::contactGap: penetration of vertex v into triangle t (with the side it was found on), the push normal
 * and the barycentric weights of the closest point.
 */
bool contactGap(ClothScene* s, uint v, uint tPacked, out float pen, out float3 n, out float3 bary)
{
    pen = 0.0;
    n = float3(0.0);
    bary = float3(0.0);
    uint3 tv = s->tris[tPacked & ~kSideBit].v.xyz;
    float3 a = P(s, tv.x), b = P(s, tv.y), c = P(s, tv.z), p = P(s, v);
    float3 nt = cross(b - a, c - a);
    float l = length(nt);
    if (l < 1e-12)
        return false;
    nt *= ((tPacked & kSideBit) != 0 ? -1.0 : 1.0) / l;
    bary = closestBary(p, a, b, c);
    float3 d = p - (a * bary.x + b * bary.y + c * bary.z);
    float dist = length(d);
    bool interior = bary.x > 1e-4 && bary.y > 1e-4 && bary.z > 1e-4;
    if (!interior && dist > 1e-7)
        n = d / dist;
    else
    {
        n = nt;
        dist = dot(d, nt);
    }
    pen = s->thickness + s->vextra[v].x - dist;
    return pen > 0.0;
}

/** Closest points of segments p1-q1 and p2-q2 (ClothSolver's closestOnSegments): parameters along each. */
float2 closestSegments(float3 p1, float3 q1, float3 p2, float3 q2)
{
    float3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    if (a <= 1e-20 && e <= 1e-20)
        return float2(0.0);
    if (a <= 1e-20)
        return float2(0.0, clamp(f / e, 0.0, 1.0));
    float c = dot(d1, r);
    if (e <= 1e-20)
        return float2(clamp(-c / a, 0.0, 1.0), 0.0);
    float b = dot(d1, d2), denom = a * e - b * b;
    float s = denom > 1e-20 ? clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
    float t = (b * s + f) / e;
    if (t < 0.0)
        return float2(clamp(-c / a, 0.0, 1.0), 0.0);
    if (t > 1.0)
        return float2(clamp((b - c) / a, 0.0, 1.0), 1.0);
    return float2(s, t);
}

/** ClothSolver::edgeGap: penetration between two edges, kept apart the way they lay when found. */
bool edgeGap(ClothScene* s, uint2 pair, float3 side, out float pen, out float3 n, out float2 st)
{
    uint2 e1 = s->edges[pair.x], e2 = s->edges[pair.y];
    float3 a0 = P(s, e1.x), a1 = P(s, e1.y), b0 = P(s, e2.x), b1 = P(s, e2.y);
    st = closestSegments(a0, a1, b0, b1);
    float3 d = lerp(a0, a1, st.x) - lerp(b0, b1, st.y);
    float len = length(d), along = dot(d, side);
    float dist;
    if (along > 0.0 && len > 1e-7)
    {
        n = d / len;
        dist = len;
    }
    else
    {
        n = side;
        dist = along;
    }
    pen = s->thickness + s->vextra[e1.x].x - dist;
    return pen > 0.0;
}

void addSelfContacts(ClothScene* s, uint i, inout float3 f, inout float3x3 H)
{
    uint nee = s->eeCount[i];
    for (uint k = 0; k < nee; ++k)
    {
        uint2 pr = s->ee[i * kMaxEe + k];
        float pen;
        float3 n;
        float2 st;
        if (!edgeGap(s, pr, s->eeSide[i * kMaxEe + k].xyz, pen, n, st))
            continue;
        uint2 e1 = s->edges[pr.x], e2 = s->edges[pr.y];
        float w = e1.x == i ? 1.0 - st.x : e1.y == i ? st.x : e2.x == i ? st.y - 1.0 : -st.y;
        float kc = s->vextra[e1.x].y;
        f += kc * pen * w * n;
        H += kc * w * w * outer(n, n);
    }
    uint nvt = s->vtCount[i];
    for (uint k = 0; k < nvt; ++k)
    {
        float pen;
        float3 n, bary;
        if (!contactGap(s, i, s->vt[i * kMaxVt + k], pen, n, bary))
            continue;
        float kc = s->vextra[i].y;
        f += kc * pen * n;
        H += kc * outer(n, n);
    }
    uint ntv = s->tvCount[i];
    for (uint k = 0; k < ntv; ++k)
    {
        uint2 pr = s->tv[i * kMaxTv + k];
        float pen;
        float3 n, bary;
        if (!contactGap(s, pr.x, pr.y, pen, n, bary))
            continue;
        uint3 tv = s->tris[pr.y & ~kSideBit].v.xyz;
        float w = tv.x == i ? bary.x : tv.y == i ? bary.y : bary.z;
        float kc = s->vextra[pr.x].y;
        f -= kc * pen * w * n;
        H += kc * w * w * outer(n, n);
    }
}

// ------------------------------------------------------------------------------------------------ frame and substep kernels

/** Once per frame, before the substeps: open seams close by seamKeep, and close for good once the gap is tiny. */
[shader("compute")]
[numthreads(64, 1, 1)]
void advanceSeamsCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint k = pc.first + tid.x;
    Link l = s->links[k];
    if (l.v.z != kSeam || l.v.w != 0)
        return;
    l.p.x = l.p.y + (l.p.x - l.p.y) * s->seamKeep;
    if (l.p.y <= 1e-6 && l.p.x <= s->closedGap)
    {
        float4 dual = s->linkDual[k];
        float3 d = P(s, l.v.x) - P(s, l.v.y);
        float len = length(d);
        dual.xyz = len > 1e-9 ? d * (dual.x / len) : float3(0.0);
        s->linkDual[k] = dual;
        l.p.x = 0.0;
        l.v.w = 1;
    }
    s->links[k] = l;
}

/** Start of a substep: inertial target and warm-started pin duals. x is left as it was for detection. */
[shader("compute")]
[numthreads(64, 1, 1)]
void beginStepCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint i = pc.first + tid.x;
    if (i == 0)
        s->state[0] = 0;
    float3 x = P(s, i);
    s->xStart[i] = float4(x, 0.0);
    s->y[i] = float4(movable(s, i) ? x + s->h * s->vel[i].xyz + s->h * s->h * s->gravity.xyz : x, 0.0);
    if (s->pin[i].w > 0.0)
    {
        float4 pd = s->pinDual[i];
        pd.xyz *= s->lambdaDecay;
        pd.w = max(s->vextra[i].z, pd.w * s->stiffnessDecay);
        s->pinDual[i] = pd;
    }
    float2 b = s->body[i];
    s->body[i] = float2(max(s->vextra[i].y, b.x * s->stiffnessDecay), b.y * s->lambdaDecay);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void beginLinksCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint k = pc.first + tid.x;
    float4 dual = s->linkDual[k];
    dual.xyz *= s->lambdaDecay;
    dual.w = max(s->links[k].p.z, dual.w * s->stiffnessDecay);
    s->linkDual[k] = dual;
}

/** The first guess: the inertial target, held inside the self-contact bound. */
[shader("compute")]
[numthreads(64, 1, 1)]
void initGuessCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint i = pc.first + tid.x;
    if (!movable(s, i))
        return;
    bool clamped;
    s->x[i] = float4(clampToBound(s, i, s->y[i].xyz, clamped), 0.0);
}

/** One VBD block solve for every vertex of a colour (pc.first indexes the colour list). Writes xNew. */
[shader("compute")]
[numthreads(64, 1, 1)]
void solveCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint i = s->colour[pc.first + tid.x];
    float3 xi = P(s, i);
    float4 info = s->vinfo[i];
    float3 xs = s->xStart[i].xyz;
    float inertia = info.x / (s->h * s->h);
    float3 f = -inertia * (xi - s->y[i].xyz);
    float3x3 H = inertia * kI;

    float3 fe = float3(0.0);
    float3x3 He = float3x3(0, 0, 0, 0, 0, 0, 0, 0, 0);
    float3x3 Hg = float3x3(0, 0, 0, 0, 0, 0, 0, 0, 0);
    addMembrane(s, i, fe, He, Hg);
    addBending(s, i, fe, He);
    // Rayleigh damping on the elastic terms, against the motion since the substep started.
    float kd = info.w / s->h;
    fe -= kd * mul(He, xi - xs);
    He *= 1.0 + kd;
    f += fe;
    H += He + Hg;

    addLinks(s, i, f, H);
    addSdfContacts(s, i, xi, xs, f, H);
    if (s->selfCollision != 0)
        addSelfContacts(s, i, f, H);

    float3 dx;
    float3 xn = solve3(H, f, dx) ? xi + dx : xi;
    bool clamped;
    xn = clampToBound(s, i, xn, clamped);
    if (clamped)
        s->state[0] = 1;
    s->xNew[i] = float4(xn, 0.0);
}

/** Publishes a colour's results so the next colour sees them; same-colour vertices only meet through contacts. */
[shader("compute")]
[numthreads(64, 1, 1)]
void commitCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint i = s->colour[pc.first + tid.x];
    s->x[i] = s->xNew[i];
}

/** AVBD dual and stiffness updates for seams and strain limits, after each iteration. */
[shader("compute")]
[numthreads(64, 1, 1)]
void dualLinksCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint k = pc.first + tid.x;
    Link l = s->links[k];
    float4 dual = s->linkDual[k];
    float3 d = P(s, l.v.x) - P(s, l.v.y);
    if (l.v.w != 0)
    {
        dual.xyz += dual.w * d;
        dual.w = min(dual.w + s->beta * length(d), s->kMax);
    }
    else
    {
        float C = length(d) - l.p.x;
        float F = dual.w * C + dual.x;
        if (F > 0.0)
        {
            dual.x = F;
            dual.w = min(dual.w + s->beta * abs(C), s->kMax);
        }
        else
            dual.x = 0.0;
    }
    s->linkDual[k] = dual;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void dualPinsCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint i = pc.first + tid.x;
    if (s->nColliders > 0 || s->floorY > -1e29)
    {
        float2 b = s->body[i];
        float pen;
        float3 n;
        if (movable(s, i) && bodyContact(s, i, P(s, i), pen, n))
            s->body[i] = float2(min(b.x + s->beta * pen, s->kMax), max(0.0, b.x * pen + b.y));
        else
            s->body[i] = float2(b.x, 0.0);
    }
    float4 pin = s->pin[i];
    if (pin.w <= 0.0)
        return;
    float4 pd = s->pinDual[i];
    float3 C = P(s, i) - pin.xyz;
    pd.xyz += pd.w * C;
    pd.w = min(pd.w + s->beta * length(C), s->kMax);
    s->pinDual[i] = pd;
}

/** End of a substep: velocity from the displacement, with air drag. */
[shader("compute")]
[numthreads(64, 1, 1)]
void endStepCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    ClothScene* s = pc.scene;
    uint i = pc.first + tid.x;
    float3 v = movable(s, i) ? (P(s, i) - s->xStart[i].xyz) * (s->drag / s->h) : float3(0.0);
    s->vel[i] = float4(v, 0.0);
}

// ------------------------------------------------------------------------------------------------ self-contact detection

int3 cellOf(ClothScene* s, float3 p) { return int3(floor(p / s->cellSize)); }

uint hashCell(ClothScene* s, int3 c) { return ((uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u)) & s->hashMask; }

/**
 * After an iteration: indirect dispatch sizes for the re-detection kernels, all zero unless some vertex hit its bound
 * (ClothSolver re-detects only then). Resets the flag for the next iteration.
 */
[shader("compute")]
[numthreads(1, 1, 1)]
void redetectArgsCS(uint3 tid: SV_DispatchThreadID)
{
    ClothScene* s = pc.scene;
    bool again = s->state[0] != 0;
    s->state[0] = 0;
    uint counts[6] = { s->hashMask + 1, s->nVerts, s->nTris, s->nVerts, s->nEdges, s->nEdges };
    for (uint k = 0; k < 6; ++k)
    {
        s->state[4 + k * 3] = again ? (counts[k] + 63) / 64 : 0;
        s->state[5 + k * 3] = 1;
        s->state[6 + k * 3] = 1;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void hashClearCS(uint3 tid: SV_DispatchThreadID)
{
    uint i = pc.first + tid.x;
    if (tid.x >= pc.count || i > pc.scene->hashMask)
        return;
    pc.scene->cellHead[i] = kNone;
    pc.scene->edgeHead[i] = kNone;
}

/** Links every vertex into its cell's list and resets its contact lists at the current positions. */
[shader("compute")]
[numthreads(64, 1, 1)]
void hashInsertCS(uint3 tid: SV_DispatchThreadID)
{
    ClothScene* s = pc.scene;
    uint i = pc.first + tid.x;
    if (tid.x >= pc.count || i >= s->nVerts)
        return;
    float3 p = P(s, i);
    s->xDetect[i] = float4(p, 0.0);
    s->vtCount[i] = 0;
    s->tvCount[i] = 0;
    s->eeCount[i] = 0;
    s->dminV[i] = asuint(s->queryRadius);
    uint prev;
    InterlockedExchange(s->cellHead[hashCell(s, cellOf(s, p))], i, prev);
    s->cellNext[i] = prev;
}

/** Per triangle: vertices within the query radius become contacts; both sides record their nearest distance. */
[shader("compute")]
[numthreads(64, 1, 1)]
void hashQueryCS(uint3 tid: SV_DispatchThreadID)
{
    ClothScene* s = pc.scene;
    uint t = pc.first + tid.x;
    if (tid.x >= pc.count || t >= s->nTris)
        return;
    float R = s->queryRadius;
    uint3 tv = s->tris[t].v.xyz;
    float3 a = P(s, tv.x), b = P(s, tv.y), c = P(s, tv.z);
    float3 nrm = cross(b - a, c - a);
    float nl = length(nrm);
    float dT = R;
    if (nl >= 1e-12)
    {
        nrm /= nl;
        bool triMoves = movable(s, tv.x) || movable(s, tv.y) || movable(s, tv.z);
        uint3 w0 = uint3(s->weld[tv.x], s->weld[tv.y], s->weld[tv.z]);
        int3 lo = cellOf(s, min(a, min(b, c)) - R);
        int3 hi = min(cellOf(s, max(a, max(b, c)) + R), lo + 3);
        for (int cz = lo.z; cz <= hi.z; ++cz)
            for (int cy = lo.y; cy <= hi.y; ++cy)
                for (int cx = lo.x; cx <= hi.x; ++cx)
                {
                    int3 cell = int3(cx, cy, cz);
                    uint vi = s->cellHead[hashCell(s, cell)];
                    for (uint guard = 0; vi != kNone && guard < 4096; ++guard, vi = s->cellNext[vi])
                    {
                        float3 p = P(s, vi);
                        // Another cell hashed to this bucket: that cell's visit sees vi.
                        if (any(cellOf(s, p) != cell))
                            continue;
                        uint wv = s->weld[vi];
                        if (wv == w0.x || wv == w0.y || wv == w0.z || (!movable(s, vi) && !triMoves))
                            continue;
                        // Closer in the flat pattern than twice the query radius: the same stretch of fabric, not a contact.
                        float4 fv = s->flat[vi];
                        float4 fa = s->flat[tv.x], fb = s->flat[tv.y], fc = s->flat[tv.z];
                        if (asuint(fv.z) == asuint(fa.z) && asuint(fv.z) == asuint(fb.z) && asuint(fv.z) == asuint(fc.z))
                        {
                            float3 rw = closestBary(float3(fv.xy, 0.0), float3(fa.xy, 0.0), float3(fb.xy, 0.0), float3(fc.xy, 0.0));
                            float2 rq = fa.xy * rw.x + fb.xy * rw.y + fc.xy * rw.z;
                            if (length(fv.xy - rq) < 2.0 * R)
                                continue;
                        }
                        float3 bary = closestBary(p, a, b, c);
                        float3 q = a * bary.x + b * bary.y + c * bary.z;
                        float dist = length(p - q);
                        if (dist >= R)
                            continue;
                        dT = min(dT, dist);
                        InterlockedMin(s->dminV[vi], asuint(dist));
                        uint packed = t | (dot(p - q, nrm) >= 0.0 ? 0u : kSideBit);
                        uint slot;
                        InterlockedAdd(s->vtCount[vi], 1u, slot);
                        if (slot < kMaxVt)
                            s->vt[vi * kMaxVt + slot] = packed;
                        for (uint k = 0; k < 3; ++k)
                        {
                            uint corner = tv[k];
                            InterlockedAdd(s->tvCount[corner], 1u, slot);
                            if (slot < kMaxTv)
                                s->tv[corner * kMaxTv + slot] = uint2(vi, packed);
                        }
                    }
                }
    }
    s->dminT[t] = asuint(dT);
}

/** Links every edge into the cell of its middle. */
[shader("compute")]
[numthreads(64, 1, 1)]
void edgeInsertCS(uint3 tid: SV_DispatchThreadID)
{
    ClothScene* s = pc.scene;
    uint e = pc.first + tid.x;
    if (tid.x >= pc.count || e >= s->nEdges)
        return;
    uint2 ed = s->edges[e];
    uint prev;
    InterlockedExchange(s->edgeHead[hashCell(s, cellOf(s, 0.5 * (P(s, ed.x) + P(s, ed.y))))], e, prev);
    s->edgeNext[e] = prev;
}

/** Per edge: later edges crossing within the query radius, away from both their ends, become contacts of all four vertices. */
[shader("compute")]
[numthreads(64, 1, 1)]
void edgeQueryCS(uint3 tid: SV_DispatchThreadID)
{
    ClothScene* s = pc.scene;
    uint e1 = pc.first + tid.x;
    if (tid.x >= pc.count || e1 >= s->nEdges)
        return;
    float R = s->queryRadius;
    // Another edge within R has its middle within R plus half its length.
    float reach = R + s->maxHalfEdge;
    uint2 ea = s->edges[e1];
    float3 a0 = P(s, ea.x), a1 = P(s, ea.y);
    uint wa = s->weld[ea.x], wb = s->weld[ea.y];
    bool aMoves = movable(s, ea.x) || movable(s, ea.y);
    float4 fa0 = s->flat[ea.x], fa1 = s->flat[ea.y];
    int3 lo = cellOf(s, min(a0, a1) - reach);
    int3 hi = min(cellOf(s, max(a0, a1) + reach), lo + 4);
    for (int cz = lo.z; cz <= hi.z; ++cz)
        for (int cy = lo.y; cy <= hi.y; ++cy)
            for (int cx = lo.x; cx <= hi.x; ++cx)
            {
                int3 cell = int3(cx, cy, cz);
                uint e2 = s->edgeHead[hashCell(s, cell)];
                for (uint guard = 0; e2 != kNone && guard < 4096; ++guard, e2 = s->edgeNext[e2])
                {
                    if (e2 <= e1)
                        continue;
                    uint2 eb = s->edges[e2];
                    float3 b0 = P(s, eb.x), b1 = P(s, eb.y);
                    if (any(cellOf(s, 0.5 * (b0 + b1)) != cell))
                        continue;
                    uint wc = s->weld[eb.x], wd = s->weld[eb.y];
                    if (wa == wc || wa == wd || wb == wc || wb == wd || (!aMoves && !movable(s, eb.x) && !movable(s, eb.y)))
                        continue;
                    float4 fb0 = s->flat[eb.x], fb1 = s->flat[eb.y];
                    uint panel = asuint(fa0.z);
                    if (asuint(fa1.z) == panel && asuint(fb0.z) == panel && asuint(fb1.z) == panel)
                    {
                        float2 fst = closestSegments(float3(fa0.xy, 0.0), float3(fa1.xy, 0.0), float3(fb0.xy, 0.0), float3(fb1.xy, 0.0));
                        if (length(lerp(fa0.xy, fa1.xy, fst.x) - lerp(fb0.xy, fb1.xy, fst.y)) < 2.0 * R)
                            continue;
                    }
                    float2 st = closestSegments(a0, a1, b0, b1);
                    // Near an end, the end's vertex-triangle contacts already hold it.
                    if (st.x < 0.02 || st.x > 0.98 || st.y < 0.02 || st.y > 0.98)
                        continue;
                    float3 d = lerp(a0, a1, st.x) - lerp(b0, b1, st.y);
                    float dist = length(d);
                    if (dist >= R || dist < 1e-9)
                        continue;
                    uint4 corners = uint4(ea.x, ea.y, eb.x, eb.y);
                    for (uint k = 0; k < 4; ++k)
                    {
                        uint v = corners[k];
                        InterlockedMin(s->dminV[v], asuint(dist));
                        uint slot;
                        InterlockedAdd(s->eeCount[v], 1u, slot);
                        if (slot < kMaxEe)
                        {
                            s->ee[v * kMaxEe + slot] = uint2(e1, e2);
                            s->eeSide[v * kMaxEe + slot] = float4(d / dist, 0.0);
                        }
                    }
                }
            }
}

bool pairLess(uint2 a, uint2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); }

/**
 * Per vertex: sorts its contact lists (atomics fill them in any order; sorting keeps the sums deterministic) and sets
 * its displacement bound from the nearest geometry to it and to its triangles.
 */
[shader("compute")]
[numthreads(64, 1, 1)]
void contactFinalizeCS(uint3 tid: SV_DispatchThreadID)
{
    ClothScene* s = pc.scene;
    uint i = pc.first + tid.x;
    if (tid.x >= pc.count || i >= s->nVerts)
        return;
    uint nvt = min(s->vtCount[i], kMaxVt);
    s->vtCount[i] = nvt;
    for (uint a = 1; a < nvt; ++a)
    {
        uint key = s->vt[i * kMaxVt + a];
        uint b = a;
        for (; b > 0 && s->vt[i * kMaxVt + b - 1] > key; --b)
            s->vt[i * kMaxVt + b] = s->vt[i * kMaxVt + b - 1];
        s->vt[i * kMaxVt + b] = key;
    }
    uint nee = min(s->eeCount[i], kMaxEe);
    s->eeCount[i] = nee;
    for (uint a = 1; a < nee; ++a)
    {
        uint2 key = s->ee[i * kMaxEe + a];
        float4 side = s->eeSide[i * kMaxEe + a];
        uint b = a;
        for (; b > 0 && pairLess(key, s->ee[i * kMaxEe + b - 1]); --b)
        {
            s->ee[i * kMaxEe + b] = s->ee[i * kMaxEe + b - 1];
            s->eeSide[i * kMaxEe + b] = s->eeSide[i * kMaxEe + b - 1];
        }
        s->ee[i * kMaxEe + b] = key;
        s->eeSide[i * kMaxEe + b] = side;
    }
    uint ntv = min(s->tvCount[i], kMaxTv);
    s->tvCount[i] = ntv;
    for (uint a = 1; a < ntv; ++a)
    {
        uint2 key = s->tv[i * kMaxTv + a];
        uint b = a;
        for (; b > 0 && pairLess(key, s->tv[i * kMaxTv + b - 1]); --b)
            s->tv[i * kMaxTv + b] = s->tv[i * kMaxTv + b - 1];
        s->tv[i * kMaxTv + b] = key;
    }

    float d = asfloat(s->dminV[i]);
    for (uint k = s->triOff[i]; k < s->triOff[i + 1]; ++k)
        d = min(d, asfloat(s->dminT[s->triAdj[k] >> 2]));
    // A small floor keeps touching vertices from locking in place.
    s->bound[i] = max(s->boundFraction * d, 0.02 * s->queryRadius);
}
