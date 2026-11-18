// Shared by the reflection passes (reflect.slang, rtreflect.slang): the prepass decoding, the reflected ray of a pixel
// sampled from its roughness lobe, and the push constants every pass reads.

/** The frame's reflection settings, in a buffer the push constant points at; matches render::ReflectParams. */
struct ReflectParams
{
    float4x4 invViewProj;  // as the prepass was drawn (jittered)
    float4x4 prevViewProj; // last frame's, moved into this frame's camera-relative space
    float4x4 viewProj;     // this frame's, jittered
    float4 size;           // half-resolution extent, then the full extent
    float4 params;         // x frame seed, y roughness cutoff, z temporal weight (0 resets), w 1 to clip the history
    float4 env;            // xy cos/sin of the environment's rotation, z intensity, w 1 if Z-up
    float4 extra;          // x light saturation, y exposure that last frame's colour carries (inverse), z max distance (cm), w hi-Z levels
    uint64_t instances;    // TraceInstance per traced placement
    uint64_t lights;       // the frame's GpuFrame, then its lights
    uint lightCount;
    uint traceShadows; // RT: 1 to shadow hit lighting
    uint pad[2];
};

struct ReflectPush
{
    ReflectParams* params;
    uint level; // hiZ: the level being built
    uint pad;
};

[[vk::push_constant]] ReflectPush rpc;
#define rp (*rpc.params)

static const float PI = 3.14159265;

/** The prepass packs roughness into the normal's length: 2 for mirror smooth, 1 for fully rough (and unwritten). */
float decodeRoughness(float3 packedNormal)
{
    return saturate(2.0 - length(packedNormal));
}

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float rand(inout uint state)
{
    state = hash(state);
    return float(state & 0xffffff) / 16777216.0;
}

void basis(float3 n, out float3 t, out float3 b)
{
    t = abs(n.z) < 0.999 ? normalize(cross(float3(0, 0, 1), n)) : normalize(cross(float3(1, 0, 0), n));
    b = cross(n, t);
}

/** A GGX microfacet normal seen from v (Heitz 2018, visible normals), in world space around n. */
float3 sampleGgxVndf(float3 n, float3 v, float alpha, float2 u)
{
    float3 t, b;
    basis(n, t, b);
    float3 ve = float3(dot(v, t), dot(v, b), dot(v, n));
    float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1, 0, 0);
    float3 t2 = cross(vh, t1);
    float r = sqrt(u.x), phi = 2.0 * PI * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    float3 ne = normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 1e-6)));
    return normalize(t * ne.x + b * ne.y + n * ne.z);
}

/** The reflected direction for this pixel and frame: off a microfacet of the roughness lobe, kept above the surface. */
float3 reflectedRay(float3 n, float3 v, float rough, inout uint state)
{
    float alpha = max(rough * rough, 1e-4);
    float3 r = reflect(-v, n);
    if (rough > 0.02)
        for (int tries = 0; tries < 4; ++tries)
        {
            float3 m = sampleGgxVndf(n, v, alpha, float2(rand(state), rand(state)));
            r = reflect(-v, m);
            if (dot(r, n) > 0.0)
                break;
        }
    return dot(r, n) > 0.0 ? r : reflect(-v, n);
}

float3 worldAt(float2 uv, float depth)
{
    float4 h = mul(rp.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return h.xyz / h.w;
}

/** The camera position, from the inverse view-projection. */
float3 eyePosition()
{
    float4 a = mul(rp.invViewProj, float4(0, 0, 1, 1));
    float4 b = mul(rp.invViewProj, float4(0, 0, 0.5, 1));
    float3 pa = a.xyz / a.w, pb = b.xyz / b.w;
    // Orthographic views put the eye at infinity: the near point stands in, the view direction is the same everywhere.
    return abs(a.w - b.w) < 1e-6 ? pa + normalize(pa - pb) * 1e6 : pa;
}

/** World direction to environment space: the environment's own up is +Y, rotated around it. Matches mesh.slang's toEnv. */
float3 toEnv(float3 d)
{
    if (rp.env.w > 0.5)
        d = float3(d.x, d.z, -d.y);
    float c = rp.env.x, s = rp.env.y;
    return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

float3 envSaturate(float3 c)
{
    return lerp(dot(c, float3(0.2126, 0.7152, 0.0722)).xxx, c, rp.extra.x);
}
