// Particle brushes on the GPU: one thread per particle of a brush dab, the same walk as tex/PaintParticles.cpp (its CPU
// reference) step for step, with the same random numbers, snapping each step to the closest surface point through the
// mesh's BVH. Each thread writes its dabs to its own slots; the caller packs them in order.

static const uint kNone = 0xFFFFFFFF;
static const float kTau = 6.2831853;

struct Tri
{
    float4 p[3];
    float4 n[3];
    float4 t[3];
};

struct BvhNode
{
    float3 lo;
    uint first;
    float3 hi;
    uint count;
};

struct InDab
{
    uint triangle;
    float u;
    float v;
    float radius;
    float opacity;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct OutDab
{
    uint triangle;
    float u;
    float v;
    float radius;
    float opacity;
    uint pad0;
};

struct Spec
{
    float length;
    float size;
    float spread;
    float rate;
    float gravity;
    float friction;
    float wander;
    float pool;
    float fade;
};

struct ParticlePush
{
    float4 down;
    BvhNode* nodes;
    float4* corners;
    Tri* tris;
    uint* paintable;
    InDab* dabs;
    OutDab* out;
    uint* counts;
    uint threads;
    uint perDab;
    uint firstIndex;
    uint seed;
    uint maxOut;
    uint triCount;
    uint groupsX;
    uint kind;    // 0 drips, 1 rain, 2 sparks, 3 flow
    uint emitter; // 0 dab, 1 area, 2 ring
    uint pad0;
    Spec* spec;
};

[[vk::push_constant]] ParticlePush pp;
static Spec gS;

uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/** Uniform in [0, 1), from a seed and up to three counters: tex/PaintParticles.cpp rnd. */
float rnd(uint seed, uint a, uint b, uint c)
{
    uint64_t h = mix64(mix64(mix64((uint64_t(seed) << 32) | uint64_t(a)) ^ uint64_t(b)) ^ (uint64_t(c) << 17));
    return float(uint(h >> 40)) / 16777216.0;
}

float boxDistance2(float3 lo, float3 hi, float3 p)
{
    float3 d = max(max(lo - p, p - hi), float3(0.0));
    return dot(d, d);
}

/** Closest point on triangle abc to p (Ericson), with barycentrics: geo/Bvh.cpp closestOnTri. */
float3 closestOnTri(float3 p, float3 a, float3 b, float3 c, out float3 bary)
{
    float3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        bary = float3(1, 0, 0);
        return a;
    }
    float3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
    {
        bary = float3(0, 1, 0);
        return b;
    }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        float v = d1 / (d1 - d3);
        bary = float3(1 - v, v, 0);
        return a + v * ab;
    }
    float3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
    {
        bary = float3(0, 0, 1);
        return c;
    }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        float w = d2 / (d2 - d6);
        bary = float3(1 - w, 0, w);
        return a + w * ac;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        bary = float3(0, 1 - w, w);
        return b + w * (c - b);
    }
    float denom = 1.0 / (va + vb + vc);
    float v = vb * denom, w = vc * denom;
    bary = float3(1 - v - w, v, w);
    return a + ab * v + ac * w;
}

/** A point on the paintable surface within reach of p: the closest surface point, as MeshBvh::closestPoint finds it. */
bool snap(float3 p, float reach, float radius, float opacity, out OutDab d)
{
    d = (OutDab)0;
    float best2 = max(reach, 1e-6) * max(reach, 1e-6);
    uint bestTri = kNone;
    float3 bestBary = float3(0.0);
    uint stack[96];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0)
    {
        BvhNode nd = pp.nodes[stack[--sp]];
        if (boxDistance2(nd.lo, nd.hi, p) > best2)
            continue;
        if (nd.count != 0)
        {
            for (uint k = nd.first; k < nd.first + nd.count; ++k)
            {
                float4 a = pp.corners[k * 3], b = pp.corners[k * 3 + 1], c = pp.corners[k * 3 + 2];
                float3 bary;
                float3 q = closestOnTri(p, a.xyz, b.xyz, c.xyz, bary);
                float3 e = q - p;
                float d2 = dot(e, e);
                if (d2 < best2)
                {
                    best2 = d2;
                    bestTri = asuint(a.w);
                    bestBary = bary;
                }
            }
            continue;
        }
        BvhNode l = pp.nodes[nd.first];
        BvhNode r = pp.nodes[nd.first + 1];
        float dl = boxDistance2(l.lo, l.hi, p), dr = boxDistance2(r.lo, r.hi, p);
        if (sp + 2 > 96)
            break;
        if (dl < dr)
        {
            stack[sp++] = nd.first + 1;
            stack[sp++] = nd.first;
        }
        else
        {
            stack[sp++] = nd.first;
            stack[sp++] = nd.first + 1;
        }
    }
    if (bestTri == kNone || bestTri >= pp.triCount || pp.paintable[bestTri] == 0)
        return false;
    d.triangle = bestTri;
    d.u = bestBary.y;
    d.v = bestBary.z;
    d.radius = radius;
    d.opacity = saturate(opacity);
    return true;
}

float3 pointOf(uint tri, float u, float v)
{
    Tri t = pp.tris[tri];
    return t.p[0].xyz * (1.0 - u - v) + t.p[1].xyz * u + t.p[2].xyz * v;
}

float3 normalOf(uint tri, float u, float v)
{
    Tri t = pp.tris[tri];
    float3 s = t.n[0].xyz * (1.0 - u - v) + t.n[1].xyz * u + t.n[2].xyz * v;
    return length(s) > 0.0 ? normalize(s) : float3(0, 1, 0);
}

float3 tipFrame(float3 normal)
{
    float3 n = length(normal) > 0.0 ? normalize(normal) : float3(0, 1, 0);
    float3 a = abs(n.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    return normalize(cross(a, n));
}

float3 tangentOf(float3 v, float3 n)
{
    float3 t = v - n * dot(v, n);
    float l = length(t);
    return l > 1e-6 ? t / l : float3(0.0);
}

static uint gBase;
static uint gCount;

void emit(OutDab d)
{
    if (gCount < pp.maxOut)
        pp.out[gBase + gCount] = d;
    ++gCount;
}

/** tex/PaintParticles.cpp simulateFlow. */
void simulateFlow(OutDab start, float R, float r0, float opacity0, uint index, uint pi)
{
    float3 p = pointOf(start.triangle, start.u, start.v), n = normalOf(start.triangle, start.u, start.v);
    float3 t0 = tipFrame(n), b0 = cross(n, t0);
    float heading = rnd(pp.seed, index, pi, 6) * kTau;
    float3 v = gS.gravity > 0.0 ? float3(0.0) : (t0 * cos(heading) + b0 * sin(heading)) * R;
    float travel = gS.length * R * (0.5 + 0.5 * rnd(pp.seed, index, pi, 5));
    float step = max(r0 * 0.5, R * 0.02);
    float walked = 0.0, r = r0;
    int still = 0;
    float3 down = pp.down.xyz;
    for (uint k = 0; walked < travel && k < 1024; ++k)
    {
        float3 g = down * gS.gravity;
        float3 along = g - n * dot(g, n);
        float pull = gS.gravity > 0.0 ? length(along) / gS.gravity : 0.0;
        if (gS.gravity > 0.0 && pull < gS.friction)
        {
            v *= 0.5;
            if (++still > 3)
                break;
        }
        else
            v += along * R * 0.25;
        float3 side = cross(n, length(v) > 1e-9 ? normalize(v) : t0);
        v += side * ((rnd(pp.seed, index, pi, 100 + k) - 0.5) * gS.wander * R);
        v -= n * dot(v, n);
        float speed = length(v);
        if (speed < 1e-6)
            break;
        float3 dir = v / speed;
        p += dir * step;
        walked += step;
        float o = opacity0 * (1.0 - (1.0 - gS.fade) * walked / max(travel, 1e-6));
        OutDab next;
        if (!snap(p, step * 4.0, r, o, next))
            break;
        emit(next);
        p = pointOf(next.triangle, next.u, next.v);
        n = normalOf(next.triangle, next.u, next.v);
        r *= 0.995;
    }
    if (gS.pool > 0.0)
    {
        OutDab pool;
        if (snap(p, step * 4.0, r * gS.pool, opacity0 * gS.fade, pool))
            emit(pool);
    }
}

/** One particle of one brush dab: tex/PaintParticles.cpp emitParticles' loop body. */
[shader("compute")]
[numthreads(64, 1, 1)]
void paintParticlesCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint t = (gid.y * pp.groupsX + gid.x) * 64 + local.x;
    if (t >= pp.threads)
        return;
    gS = pp.spec[0];
    gBase = t * pp.maxOut;
    gCount = 0;
    uint di = t / pp.perDab, pi = t % pp.perDab;
    InDab from = pp.dabs[di];
    uint index = pp.firstIndex + di;
    if (from.triangle >= pp.triCount || rnd(pp.seed, index, 0xFFFF, 0) >= gS.rate)
    {
        pp.counts[t] = 0;
        return;
    }
    float R = from.radius;
    float3 c = pointOf(from.triangle, from.u, from.v), n0 = normalOf(from.triangle, from.u, from.v);
    float3 t0 = tipFrame(n0), b0 = cross(n0, t0);
    float ang = rnd(pp.seed, index, pi, 1) * kTau;
    bool flow = pp.kind == 3;
    float dist = sqrt(rnd(pp.seed, index, pi, 2)) * gS.spread * R * (pp.kind == 1 ? 2.0 : 1.0);
    if (flow && pp.emitter == 0)
        dist = 0.0;
    else if (flow && pp.emitter == 2)
        dist = gS.spread * R;
    float3 start = c + (t0 * cos(ang) + b0 * sin(ang)) * dist;
    float r = R * gS.size * (0.6 + 0.8 * rnd(pp.seed, index, pi, 3));
    OutDab d;
    if (!snap(start, R * (gS.spread + 1.0) * 2.0, r, from.opacity, d))
    {
        pp.counts[t] = 0;
        return;
    }
    emit(d);
    if (flow)
    {
        simulateFlow(d, R, r, from.opacity, index, pi);
        pp.counts[t] = min(gCount, pp.maxOut);
        return;
    }
    float3 p = pointOf(d.triangle, d.u, d.v);
    float3 n = normalOf(d.triangle, d.u, d.v);
    float3 dir;
    float travel = gS.length * R;
    if (pp.kind == 2)
    {
        float a = rnd(pp.seed, index, pi, 4) * kTau;
        dir = tangentOf(t0 * cos(a) + b0 * sin(a), n);
        travel *= 0.5 + rnd(pp.seed, index, pi, 5);
    }
    else
    {
        dir = tangentOf(pp.down.xyz, n);
        if (pp.kind == 1)
            travel *= 0.15 * rnd(pp.seed, index, pi, 5);
        else
            travel *= 0.4 + 0.6 * rnd(pp.seed, index, pi, 5);
    }
    if (length(dir) != 0.0)
    {
        float opacity = from.opacity, walked = 0.0;
        for (uint step = 0; walked < travel && step < 512; ++step)
        {
            float len = max(r * 0.5, R * 0.02);
            float wobble = pp.kind == 0 ? (rnd(pp.seed, index, pi, 100 + step) - 0.5) * 0.6 : 0.0;
            float3 side = cross(n, dir);
            p += (dir + side * wobble) * len;
            walked += len;
            if (pp.kind == 2)
            {
                r *= 0.97;
                opacity = from.opacity * (1.0 - walked / travel);
            }
            else if (pp.kind == 0)
                r *= 0.99;
            OutDab next;
            if (!snap(p, len * 4.0, r, opacity, next))
                break;
            emit(next);
            p = pointOf(next.triangle, next.u, next.v);
            n = normalOf(next.triangle, next.u, next.v);
            float3 along = pp.kind == 2 ? dir : pp.down.xyz;
            dir = tangentOf(along, n);
            if (length(dir) == 0.0)
                break;
        }
    }
    pp.counts[t] = min(gCount, pp.maxOut);
}
