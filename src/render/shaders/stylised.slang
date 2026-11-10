// Painterly, Toon and Foliage on top of mesh.slang's default lit model; included by it after the lighting functions.
// The style block (render::packStyle) holds eight float4s in the frame's table at pc.style.

static const uint kNoStyle = 0xffffffff;
static const uint kNoStroke = 0xffffffff;
static const uint kDefaultLit = 0;
static const uint kPainterly = 1;
static const uint kToon = 2;
static const uint kFoliage = 3;
static const uint kUnlit = 4;
static const uint kSkin = 5;
static const uint kCloth = 6;
static const uint kClearcoat = 7;
static const uint kHair = 8;
static const uint kEye = 9;
static const uint kMatcap = 10;

static const uint kMapObject = 0;
static const uint kMapWorld = 1;
static const uint kMapScreen = 2;
static const uint kMapUv = 3;

uint styleModel()
{
    return pc.style == kNoStyle ? kDefaultLit : pc.textureTable[pc.style];
}

float4 styleVec(uint i)
{
    uint b = pc.style + i * 4;
    return asfloat(uint4(pc.textureTable[b], pc.textureTable[b + 1], pc.textureTable[b + 2], pc.textureTable[b + 3]));
}

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/** Environment direction back to world space: toEnv's inverse. */
float3 fromEnv(float3 d)
{
    float c = pc.env.x, s = pc.env.y;
    d = float3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
    if (pc.params.y > 0.5)
        d = float3(d.x, -d.z, d.y);
    return d;
}

/**
 * The one light the stylised models step and tint against: the studio rig's key, or the environment's dominant
 * direction (its first-order spherical harmonics) once one is loaded.
 */
float3 keyLight(float3 v)
{
    if (environmentLighting())
    {
        float3 d = float3(luminance(envU.sh[3].rgb), luminance(envU.sh[1].rgb), luminance(envU.sh[2].rgb));
        if (dot(d, d) > 1e-8)
            return normalize(fromEnv(d));
    }
    float3 up = pc.params.y > 0.5 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 fwd = -v;
    float3 right0 = normalize(cross(fwd, up) + 1e-5);
    float3 up0 = cross(right0, fwd);
    float ca = cos(pc.extra.w), sa = sin(pc.extra.w);
    return normalize(-fwd * 0.6 + (right0 * sa + up0 * ca) * 0.7 - (right0 * ca - up0 * sa) * 0.5);
}

/** Light arriving along the key direction, matching the studio rig's key or the environment's brightness that way. */
float3 keyRadiance(float3 l)
{
    if (environmentLighting())
        return irradiance(toEnv(l)) * pc.env.z * PI;
    return float3(2.6);
}

// ---------------------------------------------------------------- painterly

float4 sampleStroke(float2 uv)
{
    uint h = asuint(styleVec(0).y);
    if (h == kNoStroke)
        return float4(0.5, 0.5, 0.5, 0.5);
#ifdef LILA_BINDLESS
    return materialTextures[NonUniformResourceIndex(pc.textureTable[h])].Sample(uv);
#else
    return strokeTex.Sample(uv);
#endif
}

struct Stroke
{
    float3 n;    // tangent space, z up
    float shade; // 0..1, 0.5 neutral
    float rnd;   // per stroke
};

float3 rnmBlend(float3 n1, float3 n2)
{
    n1 += float3(0, 0, 1);
    n2 *= float3(-1, -1, 1);
    return n1 * dot(n1, n2) / n1.z - n2;
}

/** Both stroke layers at one projection uv: the coarse sheet, then the finer one RNM-blended over it. */
Stroke strokeAt(float2 uv, float tilt)
{
    float4 p = styleVec(1);
    float2 boil = styleVec(5).xy;
    float4 a = sampleStroke(uv + boil);
    float4 b = sampleStroke(uv * p.y + float2(0.37, 0.71) + boil * 1.7);
    float2 ta = (a.xy * 2.0 - 1.0) * tilt, tb = (b.xy * 2.0 - 1.0) * tilt * 0.6;
    float3 na = float3(ta, sqrt(saturate(1.0 - dot(ta, ta))));
    float3 nb = float3(tb, sqrt(saturate(1.0 - dot(tb, tb))));
    Stroke s;
    s.n = normalize(rnmBlend(na, nb));
    s.shade = lerp(a.z, b.z, 0.35);
    s.rnd = a.w;
    return s;
}

/**
 * Triplanar strokes around unit normal nb at position p (both in one space), Golus-style: each axis's tangent normal is
 * RNM-blended onto the swizzled surface normal, so a flat sheet gives back nb exactly.
 */
Stroke triplanarStroke(float3 p, float3 nb, float scale, float tilt)
{
    float3 w = pow(abs(nb), 4.0);
    w /= max(w.x + w.y + w.z, 1e-6);
    Stroke x = strokeAt(p.zy * scale, tilt), y = strokeAt(p.xz * scale, tilt), z = strokeAt(p.xy * scale, tilt);
    float3 an = abs(nb);
    float3 tx = rnmBlend(float3(nb.zy, an.x), x.n);
    float3 ty = rnmBlend(float3(nb.xz, an.y), y.n);
    float3 tz = rnmBlend(float3(nb.xy, an.z), z.n);
    float3 sgn = select(nb < 0.0, -1.0, 1.0);
    tx.z *= sgn.x;
    ty.z *= sgn.y;
    tz.z *= sgn.z;
    Stroke s;
    s.n = normalize(tx.zyx * w.x + ty.xzy * w.y + tz.xyz * w.z);
    s.shade = x.shade * w.x + y.shade * w.y + z.shade * w.z;
    // The dominant axis keeps stroke identity crisp where the projections overlap.
    s.rnd = w.x > w.y && w.x > w.z ? x.rnd : w.y > w.z ? y.rnd : z.rnd;
    return s;
}

float3x3 normalMatrix(uint instance)
{
    return cofactor((float3x3)instanceWorld(instance));
}

/** The painterly stroke at a fragment: s.n is the stroke-bent world normal for geometric normal ng (front side). */
Stroke painterlyStroke(VSOut i, float3 ng, bool front)
{
    float4 p0 = styleVec(0), p1 = styleVec(1);
    float tilt = saturate(p0.z * p0.w);
    uint mapping = uint(p1.z + 0.5);
    float scale = p1.x;
    Stroke s;
    if (mapping == kMapUv)
    {
        float2 uv = i.uv * scale * 100.0;
        s = strokeAt(uv, tilt);
        // Cotangent frame from derivatives: works without stored tangents.
        float3 dp1 = ddx(i.worldPos), dp2 = ddy(i.worldPos);
        float2 du1 = ddx(uv), du2 = ddy(uv);
        float3 t = dp1 * du2.y - dp2 * du1.y;
        float3 b = dp2 * du1.x - dp1 * du2.x;
        float inv = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
        s.n = normalize(t * inv * s.n.x - b * inv * s.n.y + ng * s.n.z);
        return s;
    }
    if (mapping == kMapObject)
    {
        float3 on = normalize(objectNormal(i)) * (front ? 1.0 : -1.0);
        s = triplanarStroke(objectPos(i), on, scale, tilt);
        s.n = normalize(mul(normalMatrix(i.instance), s.n));
        return s;
    }
    if (mapping == kMapWorld)
        return triplanarStroke(i.worldPos, ng, scale, tilt);
    // Screen: the tile size follows camera distance in octaves, so strokes keep their size on screen without swimming.
    float lod = log2(max(distance(pc.eye.xyz, i.worldPos), 1e-3) / 300.0);
    float octave = floor(lod);
    float k = scale * exp2(-octave);
    Stroke a = triplanarStroke(i.worldPos, ng, k, tilt), b = triplanarStroke(i.worldPos, ng, k * 0.5, tilt);
    float f = lod - octave;
    s.n = normalize(lerp(a.n, b.n, f));
    s.shade = lerp(a.shade, b.shade, f);
    s.rnd = f < 0.5 ? a.rnd : b.rnd;
    return s;
}

/** The mapped normal n bent by how far the stroke turned the geometric normal ng. */
float3 bendNormal(float3 n, float3 ng, Stroke s)
{
    return normalize(n + (s.n - ng));
}

/** Base colour pushed through the palette by the light term and the strokes, and the specular tinted toward the light colour. */
void painterlySurface(Stroke st, float3 n, float3 v, float3 baseColor, float metallic, inout Surface s)
{
    float4 p0 = styleVec(0), p1 = styleVec(1), shadow = styleVec(2), lightC = styleVec(3), accent = styleVec(4);
    float strength = saturate(p0.z);
    if (strength <= 0.0)
        return;
    float t = dot(n, keyLight(v)) * 0.5 + 0.5;
    t *= lerp(1.0, s.ssao * s.ao, 0.5);
    t += (st.shade - 0.5) * 2.0 * accent.w;
    float band = lerp(0.5, 0.02, saturate(p1.w));
    t = smoothstep(0.5 - band, 0.5 + band, t);
    uint count = uint(lightC.w + 0.5);
    float3 palette = t < 0.5 ? lerp(shadow.rgb, baseColor, t * 2.0) : (count >= 3 ? lerp(baseColor, lightC.rgb, (t - 0.5) * 1.2) : baseColor);
    if (count >= 4)
        palette = lerp(palette, accent.rgb, smoothstep(0.82, 0.9, st.rnd) * 0.7);
    float3 albedo = lerp(baseColor, palette, strength * 0.75);
    s.diffuse = albedo * (1.0 - metallic);
    float3 f0 = lerp(float3(0.08 * pc.surface.y), albedo, metallic);
    float3 tint = lightC.rgb / max(luminance(lightC.rgb), 1e-3);
    s.f0 = lerp(f0, f0 * tint, saturate(shadow.w) * strength);
}

// ---------------------------------------------------------------- toon

float3 toonShade(float3 n, float3 v, float3 baseColor, Surface s)
{
    float4 p0 = styleVec(0), p1 = styleVec(1);
    float3 shadowC = styleVec(2).rgb, lightTint = styleVec(3).rgb, outerC = styleVec(4).rgb, rimC = styleVec(5).rgb;
    float3 l = keyLight(v);
    float nl = dot(n, l);
    float threshold = p0.z, soft = max(p0.w, 1e-3);
    float lit = smoothstep(threshold - soft, threshold + soft, nl);
    float3 shade = shadowC;
    if (p1.x > 0.0)
    {
        float edge = threshold - p1.x;
        shade = lerp(outerC, shadowC, smoothstep(edge - soft * 0.25, edge + soft * 0.25, nl));
    }
    float3 c = baseColor * lerp(shade, lightTint, lit);
    c *= lerp(1.0, s.ssao * s.ao, 0.6);
    // A hard highlight whose size follows roughness; specular scales it.
    float3 h = normalize(v + l);
    float gloss = 1.0 - s.rough;
    float hl = smoothstep(0.5, 0.55, pow(saturate(dot(n, h)), lerp(8.0, 256.0, gloss * gloss)));
    c += hl * lit * gloss * pc.surface.y * 0.6 * lightTint;
    float rimLine = 1.0 - saturate(p1.y);
    float rim = smoothstep(rimLine, rimLine + 0.06, 1.0 - saturate(dot(n, v)));
    c += rimC * rim * p1.z * (0.3 + 0.7 * lit);
    if (environmentLighting() || flatLighting())
        c *= pc.env.z;
    return c;
}

// ---------------------------------------------------------------- foliage

/** The base colour graded bottom to top over the mesh bounds, darkened facing down and lifted facing up. */
void foliageSurface(VSOut i, float3 n, float3 baseColor, float metallic, inout Surface s)
{
    float4 p0 = styleVec(0), p1 = styleVec(1), lo = styleVec(6), hi = styleVec(7);
    uint axis = uint(lo.w + 0.5);
    float h0 = axis == 2 ? lo.z : lo.y, h1 = axis == 2 ? hi.z : hi.y;
    float3 op = objectPos(i);
    float h = saturate(((axis == 2 ? op.z : op.y) - h0) / max(h1 - h0, 1e-4));
    float3 albedo = lerp(baseColor, lerp(styleVec(2).rgb, styleVec(3).rgb, h), saturate(p0.z));
    float3 up = pc.params.y > 0.5 ? float3(0, 0, 1) : float3(0, 1, 0);
    float f = dot(n, up);
    albedo *= 1.0 - saturate(p1.x) * saturate(-f);
    albedo *= 1.0 + p1.y * saturate(f);
    s.diffuse = albedo * (1.0 - metallic);
    s.f0 = lerp(float3(0.08 * pc.surface.y), albedo, metallic);
}

/** Wrapped key light (soft clumps) and light through the leaves toward the camera, added to the default lit result. */
float3 foliageLight(float3 c, float3 n, float3 v, Surface s)
{
    float4 p0 = styleVec(0), p1 = styleVec(1);
    float3 l = keyLight(v);
    float3 radiance = keyRadiance(l);
    float wrap = saturate(p0.w);
    float nl = dot(n, l);
    float wrapped = saturate((nl + wrap) / (1.0 + wrap));
    c += s.diffuse / PI * radiance * max(wrapped - saturate(nl), 0.0) * s.ao;
    float through = pow(saturate(dot(v, -l)), 4.0) * saturate(1.0 - nl) * 0.5;
    c += s.diffuse * radiance * through * p1.z * 0.25;
    return c;
}
