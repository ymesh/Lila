// The path tracer's extended shading channels (render::ExtendedShading): their maps applied to a hit, and the lobes they
// add to the BSDF. Coat and sheen layer over the base, anisotropy stretches its highlight along the direction map, specular
// colour, weight and IOR set its reflectance; transmission (thin) and subsurface (diffuse transmission) are branches of
// the path in traceCS.

static const uint kExtSheenColor = 0;
static const uint kExtSheenRoughness = 1;
static const uint kExtCoat = 2;
static const uint kExtCoatRoughness = 3;
static const uint kExtCoatNormal = 4;
static const uint kExtSubsurfaceColor = 5;
static const uint kExtSubsurfaceRadius = 6;
static const uint kExtAnisotropy = 7;
static const uint kExtAnisotropyDirection = 8;
static const uint kExtTransmission = 9;
static const uint kExtIor = 10;
static const uint kExtSpecularWeight = 11;
static const uint kExtSpecularColor = 12;

/** Map k of a section's extended maps at uv; w < 0 when it has none. */
float4 extendedMap(PtSection s, uint k, float2 uv)
{
    uint word = (k / 2) < 4 ? s.extMaps0[k / 2] : s.extMaps1[k / 2 - 4];
    uint h = (word >> ((k & 1) * 16)) & 0xffff;
    if (h == kNoHandle)
        return float4(-1.0);
    return sampleHandle(h, 0, uv);
}

void applyExtendedMaps(PtSection s, inout Hit h)
{
    h.coatN = h.ng;
    h.anisoT = h.t;
    if (uint(s.emissive.w + 0.5) != 11)
        return;
    float4 m;
    if ((m = extendedMap(s, kExtSheenColor, h.uv)).w >= 0.0)
        h.sheen = m.rgb;
    if ((m = extendedMap(s, kExtSheenRoughness, h.uv)).w >= 0.0)
        h.sheenRough = clamp(m.r, 0.05, 1.0);
    if ((m = extendedMap(s, kExtCoat, h.uv)).w >= 0.0)
        h.coat = saturate(m.r);
    if ((m = extendedMap(s, kExtCoatRoughness, h.uv)).w >= 0.0)
        h.coatRough = clamp(m.r, 0.02, 1.0);
    if ((m = extendedMap(s, kExtCoatNormal, h.uv)).w >= 0.0)
    {
        // Lila's channels are OpenGL normals.
        float3 tn = m.xyz * 2.0 - 1.0;
        float3 n = normalize(h.t * tn.x + h.b * tn.y + h.ng * max(tn.z, 1e-3));
        if (dot(n, h.ng) > 0.02)
            h.coatN = n;
    }
    if ((m = extendedMap(s, kExtSubsurfaceColor, h.uv)).w >= 0.0)
        h.subsurface = m.rgb;
    if ((m = extendedMap(s, kExtAnisotropy, h.uv)).w >= 0.0)
        h.aniso = clamp(m.r, -0.95, 0.95);
    float2 dir = float2(1.0, 0.0);
    if ((m = extendedMap(s, kExtAnisotropyDirection, h.uv)).w >= 0.0)
        dir = m.rg * 2.0 - 1.0;
    float c = cos(h.anisoRot), sn = sin(h.anisoRot);
    dir = float2(c * dir.x - sn * dir.y, sn * dir.x + c * dir.y);
    float3 t = h.t * dir.x + h.b * dir.y;
    h.anisoT = normalize(t - h.n * dot(h.n, t) + 1e-7);
    if ((m = extendedMap(s, kExtTransmission, h.uv)).w >= 0.0)
        h.transmission = saturate(m.r);
    if ((m = extendedMap(s, kExtIor, h.uv)).w >= 0.0)
        h.ior = max(1.0 + 2.0 * m.r, 1.0);
    float weight = 1.0;
    if ((m = extendedMap(s, kExtSpecularWeight, h.uv)).w >= 0.0)
        weight = m.r;
    if ((m = extendedMap(s, kExtSpecularColor, h.uv)).w >= 0.0)
        h.specColor = m.rgb * weight;
    else
        h.specColor *= weight;
}

/** The share of the diffuse lobe that passes through the surface as subsurface light. */
float subsurfaceShare(Hit h)
{
    return 0.5 * max(h.subsurface.r, max(h.subsurface.g, h.subsurface.b)) * (1.0 - h.transmission);
}

float D_Charlie(float nh, float a)
{
    float inv = 1.0 / a;
    float sin2 = max(1.0 - nh * nh, 0.0078125);
    return (2.0 + inv) * pow(sin2, inv * 0.5) / (2.0 * PI);
}

float V_Neubelt(float nv, float nl) { return saturate(1.0 / (4.0 * (nl + nv - nl * nv))); }

/** The BSDF (without the cosine) with the extended lobes over the base's spec and diff. */
float3 extendedCombine(Hit h, Lobes lb, float3 n, float3 v, float3 l, float3 hv, float nl, float nv, float nh, float vh, float3 spec, float3 diff)
{
    if (abs(h.aniso) > 1e-3)
    {
        // Anisotropic GGX (Burley), the highlight stretched along the direction.
        float3 t = h.anisoT, b = cross(n, t);
        float at = max(lb.alpha * (1.0 + h.aniso), 1e-3), ab = max(lb.alpha * (1.0 - h.aniso), 1e-3);
        float th = dot(t, hv), bh = dot(b, hv);
        float d2 = th * th / (at * at) + bh * bh / (ab * ab) + nh * nh;
        float D = 1.0 / (PI * at * ab * d2 * d2);
        float3 F = fresnel(lb.f0, vh);
        spec = D * V_SmithGGX(nv, nl, sqrt(at * ab)) * F;
    }
    float3 f = spec + diff;
    if (any(h.sheen > 0.0))
        f += h.sheen * D_Charlie(nh, max(h.sheenRough * h.sheenRough, 0.01)) * V_Neubelt(nv, nl);
    if (h.coat > 0.0)
    {
        float cnl = saturate(dot(h.coatN, l)), cnv = max(dot(h.coatN, v), 1e-4), cnh = saturate(dot(h.coatN, hv));
        float ac = max(h.coatRough * h.coatRough, 0.002);
        float Fc = fresnel(float3(0.04), vh).x * h.coat;
        float coatSpec = D_GGX(cnh, ac) * V_SmithGGX(cnv, cnl, ac) * Fc;
        // evalBsdf multiplies by the base normal's cosine; the coat's own is the coat normal's.
        f = f * (1.0 - Fc) + coatSpec * cnl / max(nl, 1e-4);
    }
    return f;
}
