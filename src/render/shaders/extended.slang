// ShadingModel::Extended on top of mesh.slang's default lit model: render::ExtendedShading's coat, sheen, subsurface,
// anisotropy, transmission, specular colour, weight and IOR, from the style block render::packExtended writes (constants,
// then the 16-bit table handles of their maps from word 20). Maps sample on the bindless path only.

static const uint kExtended = 11;

#ifdef LILA_BINDLESS
/** A map through its table handle with the pixel's gradients (plain, UDIM or virtual). */
float4 sampleTableHandle(uint h, float2 uv)
{
    uint head = pc.textureTable[h];
    if ((head & kUdimEntry) == 0 && (head & kVtEntry) != 0)
        return vtSample(h, uv);
    if ((head & kUdimEntry) != 0)
    {
        int col = int(floor(uv.x));
        int row = int(floor(1.0 - uv.y));
        uint index = (col < 0 || col >= 10 || row < 0 || row >= int(head & 0xffff)) ? pc.textureTable[h + 1]
                                                                                       : pc.textureTable[h + 2 + uint(row) * 10 + uint(col)];
        return materialTextures[NonUniformResourceIndex(index)].SampleGrad(uv, gUvDx, gUvDy);
    }
    return materialTextures[NonUniformResourceIndex(head)].SampleGrad(uv, gUvDx, gUvDy);
}

/** Extended map k at uv; w < 0 when the material has none. */
float4 extMap(uint k, float2 uv)
{
    uint word = 20 + k / 2;
    uint h = (pc.textureTable[pc.style + word] >> ((k & 1) * 16)) & 0xffff;
    if (h == 0xffff)
        return float4(-1.0);
    return sampleTableHandle(h, uv);
}
#else
float4 extMap(uint k, float2 uv) { return float4(-1.0); }
#endif

/** The pixel's extended inputs into the globals light() and imageBased() read; specular is colour times weight, then ior. */
void extendedInputs(float2 uv, float3 n, float3 tangentU, float3 tangentV, out float4 specular, out float transmission)
{
    float4 p0 = styleVec(0), p1 = styleVec(1), p2 = styleVec(2), p3 = styleVec(3), p4 = styleVec(4);
    gCoat = p0.zw;
    gSheen = p1;
    float3 subsurface = p2.rgb;
    float radius = p2.w;
    float3 specColor = p3.rgb;
    float specWeight = p3.w;
    gAnisotropy = p4.x;
    float rotation = p4.y;
    transmission = p4.z;
    float ior = p4.w;
    float4 m;
    if ((m = extMap(0, uv)).w >= 0.0)
        gSheen.rgb = m.rgb;
    if ((m = extMap(1, uv)).w >= 0.0)
        gSheen.w = m.r;
    if ((m = extMap(2, uv)).w >= 0.0)
        gCoat.x = m.r;
    if ((m = extMap(3, uv)).w >= 0.0)
        gCoat.y = m.r;
    if ((m = extMap(4, uv)).w >= 0.0)
    {
        // Lila's channels are OpenGL normals: green up the texture.
        float3 tn = m.xyz * 2.0 - 1.0;
        gCoatNormal = normalize(tangentU * tn.x + tangentV * tn.y + gCoatNormal * max(tn.z, 1e-3));
    }
    if ((m = extMap(5, uv)).w >= 0.0)
        subsurface = m.rgb;
    if ((m = extMap(6, uv)).w >= 0.0)
        radius = m.r;
    if ((m = extMap(7, uv)).w >= 0.0)
        gAnisotropy = m.r;
    float2 dir = float2(1.0, 0.0);
    if ((m = extMap(8, uv)).w >= 0.0)
        dir = m.rg * 2.0 - 1.0;
    if ((m = extMap(9, uv)).w >= 0.0)
        transmission = m.r;
    if ((m = extMap(10, uv)).w >= 0.0)
        ior = 1.0 + 2.0 * m.r;
    if ((m = extMap(11, uv)).w >= 0.0)
        specWeight = m.r;
    if ((m = extMap(12, uv)).w >= 0.0)
        specColor = m.rgb;
    gCoat = float2(saturate(gCoat.x), clamp(gCoat.y, 0.02, 1.0));
    gSheen.w = clamp(gSheen.w, 0.05, 1.0);
    gAnisotropy = clamp(gAnisotropy, -0.95, 0.95);
    // The radius widens how far light wraps round the form.
    gSubsurface = subsurface * clamp(radius, 0.0, 2.0) * 0.5;
    float c = cos(rotation), s = sin(rotation);
    dir = float2(c * dir.x - s * dir.y, s * dir.x + c * dir.y);
    float3 t = tangentU * dir.x + tangentV * dir.y;
    gAnisoT = normalize(t - n * dot(n, t) + 1e-7);
    specular = float4(specColor * specWeight, max(ior, 1.0));
    transmission = saturate(transmission);
}

/** light()'s spec and diff (cosine included) with the extended lobes: anisotropic highlight, subsurface wrap, sheen. */
void extendedLight(float3 n, float3 v, float3 l, float3 h, float nl, float nv, float nh, float vh, float a, float3 F, Surface s, inout float3 spec,
                   inout float3 diff)
{
    if (abs(gAnisotropy) > 1e-3)
    {
        float3 t = gAnisoT, b = cross(n, t);
        float at = max(a * (1.0 + gAnisotropy), 1e-3), ab = max(a * (1.0 - gAnisotropy), 1e-3);
        float th = dot(t, h), bh = dot(b, h);
        float d2 = th * th / (at * at) + bh * bh / (ab * ab) + nh * nh;
        spec = 1.0 / (PI * at * ab * d2 * d2) * V_SmithGGX(nv, nl, sqrt(at * ab)) * F * nl;
    }
    if (any(gSubsurface > 0.0))
    {
        float3 w = gSubsurface;
        diff = (1.0 - F) * s.diffuse / PI * saturate((dot(n, l) + w) / (1.0 + w));
    }
    if (any(gSheen.rgb > 0.0))
        spec += gSheen.rgb * D_Charlie(nh, max(gSheen.w * gSheen.w, 0.01)) * V_Neubelt(nv, nl) * nl;
}
