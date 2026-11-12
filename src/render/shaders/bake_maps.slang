// Map kernels over a band's samples and hits, included after bake_trace.slang. Each mirrors its CPU kernel in
// src/bake/Maps*.cpp. On the band path alpha of the output marks the samples that count; resident bakes (kFlagResident)
// keep alpha as data, flag validity apart and skip slots no low triangle claimed.

bool absent(TexelSample s)
{
    return (pc.flags & kFlagResident) != 0 && s.lowTriangle == kNoHit;
}

// World length of one UV unit on a low triangle (worldPerUv in MapsCommon.cpp).
float worldPerUv(LowInfo low, uint t)
{
    float3 p0 = load3(low.positions, t * 3), p1 = load3(low.positions, t * 3 + 1), p2 = load3(low.positions, t * 3 + 2);
    float2 uv0 = low.uvs[t * 3], uv1 = low.uvs[t * 3 + 1], uv2 = low.uvs[t * 3 + 2];
    float area3 = 0.5 * length(cross(p1 - p0, p2 - p0));
    float2 a = uv1 - uv0, b = uv2 - uv0;
    float areaUv = 0.5 * abs(a.x * b.y - a.y * b.x);
    return areaUv > 0.0 ? sqrt(area3 / areaUv) : 0.0;
}

// encodeNormalOut in MapsSurface.cpp: the sample's frame as the normal map stores it, or object space.
float4 encodeNormalOut(TexelSample s, float3 n)
{
    if ((pc.flags & kFlagObjectSpace) != 0)
        return float4(n * 0.5 + 0.5, 1.0);
    float3 e = encodeTangentNormal(n, s.normal, s.tangent, (pc.flags & kFlagGreenUp) != 0, (pc.flags & kFlagFlipX) != 0);
    return float4(e * 0.5 + 0.5, 1.0);
}

float distanceToSegment(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 0.0 ? clamp(dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    return length(p - (a + ab * t));
}

// The maps that read only the sample and its hit.
[shader("compute")]
[numthreads(64, 1, 1)]
void shadeCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    if (absent(s))
        return;
    HitRecord h = pc.hits[i];
    bool hit = (h.flags & kHitValid) != 0;
    float3 lowNormal = unitOr(s.normal, float3(0, 0, 1));
    switch (pc.map)
    {
    case kMapNormal:
    {
        float3 e = encodeTangentNormal(hit ? mappedNormal(h) : lowNormal, s.normal, s.tangent, (pc.flags & kFlagGreenUp) != 0,
                                       (pc.flags & kFlagFlipX) != 0);
        writeOut(i, float4(e * 0.5 + 0.5, 1.0), true);
        break;
    }
    case kMapObjectNormal:
        writeOut(i, float4((hit ? mappedNormal(h) : lowNormal) * 0.5 + 0.5, 1.0), true);
        break;
    case kMapPosition:
    {
        float3 v;
        for (int k = 0; k < 3; ++k)
            v[k] = pc.boundsExt[k] > 0.0 ? (h.position[k] - pc.boundsMin[k]) / pc.boundsExt[k] : 0.5;
        writeOut(i, float4(v, 1.0), hit);
        break;
    }
    case kMapHeight:
    {
        float v = hit ? h.height : 0.0;
        writeOut(i, float4(v, v, v, 1.0), true);
        break;
    }
    case kMapId:
    {
        if (!hit)
        {
            writeOut(i, float4(0.0), false);
            break;
        }
        HighInfo hi = pc.highs[h.high];
        uint slot = min(h.triangle < hi.triangles ? hi.materials[h.triangle] : 0u, hi.materialCount);
        writeOut(i, float4(unpackRgba8(pc.table[hi.materialBase + slot]).xyz, 1.0), true);
        break;
    }
    case kMapAlpha:
        writeOut(i, float4(1.0), true);
        break;
    case kMapIslands:
        writeOut(i, float4(unpackRgba8(pc.table[pc.tableBase[s.low] + s.lowTriangle]).xyz, 1.0), true);
        break;
    case kMapCurvatureMesh:
    {
        if (!hit)
        {
            writeOut(i, float4(0.0), false);
            break;
        }
        CornerFloats c = ((CornerFloats*)pc.extra)[h.high];
        float k = 0.0;
        if (c.present != 0)
        {
            float* v = c.corners + h.triangle * 3;
            k = v[0] * (1.0 - h.bary.x - h.bary.y) + v[1] * h.bary.x + v[2] * h.bary.y;
        }
        writeOut(i, float4(k, k, k, 1.0), true);
        break;
    }
    case kMapSlopes:
    {
        float2 sl = float2(0.0);
        if (hit)
        {
            float3 e = encodeTangentNormal(mappedNormal(h), s.normal, s.tangent, true, false);
            // Past ~84 degrees from the low the slope means nothing (tangentSlopes in MapsCommon.cpp).
            float z = max(e.z, 0.1);
            sl = float2(e.x / z, e.y / z);
        }
        writeOut(i, float4(sl, 0.0, worldPerUv(pc.lows[s.low], s.lowTriangle)), true);
        break;
    }
    case kMapVertexColor:
    {
        CornerColors c = hit ? ((CornerColors*)pc.extra)[h.high] : CornerColors();
        if (!hit || c.present == 0)
        {
            writeOut(i, float4(0.0), false);
            break;
        }
        float4* v = c.corners + h.triangle * 3;
        writeOut(i, v[0] * (1.0 - h.bary.x - h.bary.y) + v[1] * h.bary.x + v[2] * h.bary.y, true);
        break;
    }
    case kMapIdVertexColor:
    {
        if (!hit)
        {
            writeOut(i, float4(0.0), false);
            break;
        }
        CornerColors c = ((CornerColors*)pc.extra)[h.high];
        if (c.present == 0)
        {
            writeOut(i, float4(1.0), true);
            break;
        }
        float w[3] = {1.0 - h.bary.x - h.bary.y, h.bary.x, h.bary.y};
        uint k = 0;
        for (uint j = 1; j < 3; ++j)
            if (w[j] > w[k])
                k = j;
        float4 col = c.corners[h.triangle * 3 + k];
        writeOut(i, float4(quantize8(col.x), quantize8(col.y), quantize8(col.z), 1.0), true);
        break;
    }
    case kMapMaterial:
    {
        if (!hit)
        {
            writeOut(i, float4(0.0), false);
            break;
        }
        HighInfo hi = pc.highs[h.high];
        uint slot = min(h.triangle < hi.triangles ? hi.materials[h.triangle] : 0u, hi.materialCount);
        MaterialSlot m = ((MaterialSlot*)pc.extra)[hi.materialBase + slot];
        bool mapped = m.texture.width != 0;
        float4 t = mapped ? sampleTexture(m.texture, hitUv(h.high, h)) : float4(1.0);
        float4 v = float4(1.0);
        // Colour outputs are written sRGB-encoded and scalars linear, whatever the source's encoding.
        float3 encoded = m.srgb != 0 ? t.xyz : linearToSrgb(t.xyz);
        float3 decoded = m.srgb != 0 ? srgbToLinear(t.xyz) : t.xyz;
        switch (pc.mode)
        {
        case 0: v = float4(mapped ? encoded : linearToSrgb(m.baseColor.xyz), 1.0); break;
        case 1:
        {
            float r = mapped ? decoded[min(m.channel, 2u)] : m.scalars.x;
            v = float4(r, r, r, 1.0);
            break;
        }
        case 2:
        {
            float r = mapped ? decoded[min(m.channel, 2u)] : m.scalars.y;
            v = float4(r, r, r, 1.0);
            break;
        }
        case 3: v = float4(linearToSrgb((mapped ? decoded : m.emissive.xyz) * m.scalars.w), 1.0); break;
        default:
        {
            float r = m.emissive.w == 0.0 ? 1.0 : clamp(m.scalars.z * (mapped ? t.w : 1.0), 0.0, 1.0);
            v = float4(r, r, r, 1.0);
            break;
        }
        }
        writeOut(i, v, true);
        break;
    }
    case kMapEdges:
    {
        LowInfo low = pc.lows[s.low];
        uint bits = (low.edges[s.lowTriangle] >> (pc.mode * 3)) & 7u;
        float2 res = float2(float(max(pc.width, 1u)), float(max(pc.height, 1u)));
        float2 p = s.uv * res;
        float d = 1e30;
        for (uint k = 0; k < 3; ++k)
            if ((bits & (1u << k)) != 0)
                d = min(d, distanceToSegment(p, low.uvs[s.lowTriangle * 3 + k] * res, low.uvs[s.lowTriangle * 3 + (k + 1) % 3] * res));
        writeOut(i, d <= pc.params.x ? float4(1.0) : float4(0.0, 0.0, 0.0, 1.0), true);
        break;
    }
    case kMapGradient:
    {
        float span = pc.params2.y;
        float v = span > 0.0 ? clamp((dot(h.position, pc.params.xyz) - pc.params2.x) / span, 0.0, 1.0) : 0.5;
        writeOut(i, float4(v, v, v, 1.0), hit);
        break;
    }
    default:
        writeOut(i, float4(0.0), false);
        break;
    }
}

float3 aoDirection(uint k, uint rays, float r1, float r2, float3 t, float3 b, float3 n)
{
    float u = fmod((float(k) + 0.5) / float(rays) + r1, 1.0);
    float v = fmod(radicalInverse(k) + r2, 1.0);
    float r = sqrt(u), phi = 6.28318530718 * v;
    return unitOr(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u)), float3(0, 0, 1));
}

// The surface met along n is a back face of a high that also closes behind the point (along -n): an open lid doesn't.
// exit is where the point leaves that high on the nearer side, just outside it.
bool aoBuried(float3 o, float3 n, out float3 exit)
{
    TraceHit up, down;
    up.front = true;
    down.front = true;
    exit = o;
    if (!(traceClosest(o, n, pc.boundsMin.w, false, up) && !up.front && traceClosestHigh(o, -n, pc.boundsMin.w, up.high, down) && !down.front))
        return false;
    exit = up.t <= down.t ? o + n * (up.t + pc.boundsExt.w) : o - n * (down.t + pc.boundsExt.w);
    return true;
}

// Cosine-weighted occlusion from the hit, the same ray sequence as the CPU kernel. rays per sample in pc.rays.
[shader("compute")]
[numthreads(64, 1, 1)]
TRACE_ENTRY
void aoCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    useLinks(s.low, kRayOcclusion);
    if (absent(s))
        return;
    HitRecord h = pc.hits[i];
    if ((h.flags & kHitValid) == 0)
    {
        writeOut(i, float4(0.0), false);
        return;
    }
    float3 n = h.normal;
    float3 t, b;
    basis(n, t, b);
    float3 o = h.position + n * pc.boundsExt.w;
    uint seed = hashBits(s.texel * 64u + s.sub) ^ hashBits(s.tile);
    float r1 = float(seed & 0xffff) / 65536.0, r2 = float(seed >> 16) / 65536.0;
    uint rays = pc.rays;
    bool self = (pc.flags & kFlagScopeSelf) != 0;
    uint blocked = 0, backs = 0;
    TraceHit th;
    for (uint k = 0; k < rays; ++k)
    {
        float3 d = aoDirection(k, rays, r1, r2, t, b, n);
        if (self)
        {
            if (traceAnyHigh(o, d, pc.boundsMin.w, h.high))
                ++blocked;
        }
        else if (traceClosest(o, d, pc.boundsMin.w, false, th))
        {
            ++blocked;
            if (!th.front)
                ++backs;
        }
    }
    // Buried inside another high (where parts meet): seen from where it leaves that high; only front faces occlude.
    float3 exit;
    if (backs * 4 >= rays * 3 && aoBuried(o, n, exit))
    {
        blocked = 0;
        for (uint k = 0; k < rays; ++k)
            if (traceClosest(exit, aoDirection(k, rays, r1, r2, t, b, n), pc.boundsMin.w, true, th))
                ++blocked;
    }
    float ao = 1.0 - float(blocked) / float(rays);
    writeOut(i, float4(ao, ao, ao, 1.0), true);
}

// Mean distance through the high along rays into it (thicknessKernel). params: x distance, y epsilon, z cone half angle.
[shader("compute")]
[numthreads(64, 1, 1)]
TRACE_ENTRY
void thicknessCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    useLinks(s.low, kRayOcclusion);
    if (absent(s))
        return;
    HitRecord h = pc.hits[i];
    if ((h.flags & kHitValid) == 0)
    {
        writeOut(i, float4(0.0), false);
        return;
    }
    float dist = pc.params.x, eps = pc.params.y;
    float3 inward = -h.normal;
    float3 o = h.position + inward * eps;
    uint seed = sampleSeed(s);
    float sum = 0.0;
    for (uint k = 0; k < pc.rays; ++k)
    {
        float3 d = coneDirection(inward, rayPoint(k, pc.rays, seed), pc.params.z);
        TraceHit th;
        th.t = 0.0;
        th.high = 0;
        th.triangle = 0;
        th.bary = float2(0.0);
        th.front = true;
        sum += closestScoped(o, d, dist, h.high, th) ? th.t + eps : dist;
    }
    float t = sum / float(pc.rays);
    writeOut(i, float4(t, t, t, 1.0), true);
}

// The mean unoccluded direction over the cosine hemisphere (bentNormalKernel). params: x distance, y epsilon.
[shader("compute")]
[numthreads(64, 1, 1)]
TRACE_ENTRY
void bentCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    useLinks(s.low, kRayOcclusion);
    if (absent(s))
        return;
    HitRecord h = pc.hits[i];
    bool hit = (h.flags & kHitValid) != 0;
    float3 n = hit ? h.normal : unitOr(s.normal, float3(0, 0, 1));
    if (hit)
    {
        float3 o = h.position + n * pc.params.y;
        uint seed = sampleSeed(s);
        float3 sum = float3(0.0);
        for (uint k = 0; k < pc.rays; ++k)
        {
            float3 d = coneDirection(n, rayPoint(k, pc.rays, seed), 1.57079632679);
            if (!occludedScoped(o, d, pc.params.x, h.high))
                sum += d;
        }
        n = unitOr(sum, n);
    }
    writeOut(i, encodeNormalOut(s, n), true);
}

// A ray-traced bevel: the high's normals averaged over the surface within radius (roundedNormalKernel). params.x radius.
[shader("compute")]
[numthreads(64, 1, 1)]
TRACE_ENTRY
void roundedCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    useLinks(s.low, kRayOcclusion);
    if (absent(s))
        return;
    HitRecord h = pc.hits[i];
    bool hit = (h.flags & kHitValid) != 0;
    float3 n = hit ? h.normal : unitOr(s.normal, float3(0, 0, 1));
    if (hit)
    {
        float radius = pc.params.x;
        float3 p = h.position;
        float3 t, b;
        basis(n, t, b);
        uint seed = sampleSeed(s);
        float3 sum = n;
        uint rays = pc.rays;
        for (uint k = 0; k < rays; ++k)
        {
            // Half the rays run along the normal, a quarter along each tangent, each class with its own sequence.
            uint pick = k % 4;
            float3 axis = pick < 2 ? n : (pick == 2 ? t : b);
            float3 a1, a2;
            basis(axis, a1, a2);
            float2 u = rayPoint(k / 4, (rays + 3) / 4, seed ^ hashBits(pick + 1));
            float r = radius * sqrt(u.x), phi = 6.28318530718 * u.y;
            float3 disc = a1 * (r * cos(phi)) + a2 * (r * sin(phi));
            for (int side = 0; side < 2; ++side)
            {
                float sgn = side == 0 ? 1.0 : -1.0;
                float3 dir = -axis * sgn;
                float3 o = p + disc + axis * (sgn * radius);
                TraceHit th;
                th.t = 0.0;
                th.high = 0;
                th.triangle = 0;
                th.bary = float2(0.0);
                th.front = true;
                if (!closestScoped(o, dir, 2.0 * radius, h.high, th) || !th.front)
                    continue;
                float* nn = pc.highs[th.high].normals + th.triangle * 9;
                float w0 = 1.0 - th.bary.x - th.bary.y;
                float3 raw = float3(nn[0], nn[1], nn[2]) * w0 + float3(nn[3], nn[4], nn[5]) * th.bary.x + float3(nn[6], nn[7], nn[8]) * th.bary.y;
                float rl = length(raw);
                if (rl <= 0.0 || length(o + dir * th.t - p) > radius)
                    continue;
                float3 hn = raw / rl;
                // Weighted by the inverse of how likely the three projections were to land there.
                float pdf = 0.5 * abs(dot(hn, n)) + 0.25 * abs(dot(hn, t)) + 0.25 * abs(dot(hn, b));
                sum += hn / max(pdf, 0.05);
            }
        }
        n = unitOr(sum, n);
    }
    writeOut(i, encodeNormalOut(s, n), true);
}

// Where the projection failed (errorKernel): ok, missed, back face, beyond the cage, degenerate frame. params.x beyond.
[shader("compute")]
[numthreads(64, 1, 1)]
TRACE_ENTRY
void errorCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    useLinks(s.low, kRayProjection);
    if (absent(s))
        return;
    HitRecord h = pc.hits[i];
    float3 n = s.normal, tg = s.tangent.xyz;
    float ln = length(n), lt = length(tg);
    bool degenerate = ln < 1e-6 || lt < 1e-6 || isinf(ln) || isnan(ln) || isinf(lt) || isnan(lt);
    if (!degenerate)
    {
        float w = worldPerUv(pc.lows[s.low], s.lowTriangle);
        degenerate = abs(dot(n / ln, tg / lt)) > 0.99 || !(w > 0.0) || isinf(w);
    }
    float4 v;
    if (degenerate)
        v = float4(0.0, 1.0, 1.0, 1.0);
    else if ((h.flags & kHitBackface) != 0)
        v = float4(1.0, 0.0, 1.0, 1.0);
    else if ((h.flags & kHitValid) != 0)
        v = float4(0.0, 0.0, 0.0, 1.0);
    else
    {
        float beyond = pc.params.x;
        bool far = traceAny(s.origin, s.direction, beyond) || traceAny(s.origin, -s.direction, beyond);
        v = far ? float4(1.0, 1.0, 0.0, 1.0) : float4(1.0, 0.0, 0.0, 1.0);
    }
    writeOut(i, v, true);
}
