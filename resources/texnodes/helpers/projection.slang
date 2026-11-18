//! Layer projections: where a layer's content lands on the mesh, from a box placed in the mesh's object space.

float3x3 projRotation(float3 deg)
{
    float3 r = radians(deg);
    float cx = cos(r.x), sx = sin(r.x), cy = cos(r.y), sy = sin(r.y), cz = cos(r.z), sz = sin(r.z);
    float3x3 rx = float3x3(1.0, 0.0, 0.0, 0.0, cx, -sx, 0.0, sx, cx);
    float3x3 ry = float3x3(cy, 0.0, sy, 0.0, 1.0, 0.0, -sy, 0.0, cy);
    float3x3 rz = float3x3(cz, -sz, 0.0, sz, cz, 0.0, 0.0, 0.0, 1.0);
    return mul(rz, mul(ry, rx));
}

/** A bake-space point in the mesh's object space; x, y, z, w are the columns of the bake's world-to-object matrix. */
float3 projToObject(float3 pos, float3 x, float3 y, float3 z, float3 w)
{
    return x * pos.x + y * pos.y + z * pos.z + w;
}

/** A bake-space normal in the mesh's object space: through that matrix's inverse transpose (its cofactors over the determinant), unit length. */
float3 projNormalToObject(float3 n, float3 x, float3 y, float3 z)
{
    float3 o = cross(y, z) * n.x + cross(z, x) * n.y + cross(x, y) * n.z;
    float l = length(o);
    return l > 1e-12 ? o / l * sign(dot(x, cross(y, z))) : n;
}

/** An object-space point in the projector's frame: centred and turned, still in cm. */
float3 projFrame(float3 pos, float3 centre, float3 deg)
{
    return mul(transpose(projRotation(deg)), pos - centre);
}

/**
 * The coordinate a projection reads its input at. mode: 0 uv, 1 planar, 2 triplanar (its dominant axis), 3 spherical,
 * 4 cylindrical, 5 warp (planar with its corners pinned). f is the point in the projector's frame, fn its normal there,
 * size the projector's box. follow carries a planar coordinate over edges: where the surface turns away from the
 * projector, the depth below the projector plane continues the coordinate.
 */
float2 projCoord(int mode, float2 uv, float3 f, float3 fn, float3 size, float follow, float4 w0, float4 w1)
{
    if (mode == 0)
        return uv;
    float3 l = f / max(size, float3(1e-6, 1e-6, 1e-6));
    if (mode == 3)
    {
        float r = length(l);
        return float2(atan2(l.y, l.x) / 6.2831853 + 0.5, r > 1e-9 ? acos(clamp(l.z / r, -1.0, 1.0)) / 3.14159265 : 0.0);
    }
    if (mode == 4)
        return float2(atan2(l.y, l.x) / 6.2831853 + 0.5, l.z + 0.5);
    if (mode == 2)
    {
        float3 a = abs(fn);
        if (a.x >= a.y && a.x >= a.z)
            return l.yz + 0.5;
        if (a.y >= a.z)
            return l.xz + 0.5;
        return l.xy + 0.5;
    }
    float2 c = l.xy + 0.5;
    float turn = 1.0 - abs(fn.z);
    float2 side = fn.xy / max(abs(fn.x) + abs(fn.y), 1e-6);
    c += side * max(-f.z, 0.0) * turn * follow / max(size.xy, float2(1e-6, 1e-6));
    if (mode == 5)
        c += lerp(lerp(w0.xy, w0.zw, c.x), lerp(w1.xy, w1.zw, c.x), c.y);
    return c;
}
