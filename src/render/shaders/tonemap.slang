// The display tonemappers the compose pass and the path tracer share: 0 neutral, 1 AgX, 2 ACES, 3 linear (clipped).
// Include after scene_color.slang.

// AgX (Sobotka), the analytic fit by Benjamin Wrensch: Rec.709 in, display-linear Rec.709 out.
float3 agxContrast(float3 x)
{
    float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx(float3 c, uint look)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772,
                                    0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417,
                                     -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    float3 v = mul(max(c, 1e-10), inset);
    v = saturate((log2(v) - minEv) / (maxEv - minEv));
    v = agxContrast(v);
    if (look != 0)
    {
        float3 slope = 1.0, power = 1.0;
        float sat = 1.0;
        if (look == 1)
        {
            power = 1.35;
            sat = 1.4;
        }
        else
        {
            slope = float3(1.0, 0.9, 0.5);
            power = 0.8;
            sat = 0.8;
        }
        float luma = dot(v, float3(0.2126, 0.7152, 0.0722));
        v = pow(max(v * slope, 0.0), power);
        v = luma + sat * (v - luma);
    }
    v = mul(v, outset);
    return pow(max(v, 0.0), 2.2);
}

// ACES filmic fit (Stephen Hill): sRGB to ACES AP1 through the RRT and ODT fit, back to display-linear sRGB.
float3 aces(float3 c)
{
    const float3x3 inputM = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 outputM = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    float3 v = mul(inputM, c);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return saturate(mul(outputM, a / b));
}

float3 tonemap(float3 c, uint mode, uint look)
{
    if (mode == 1)
        return agx(c, look);
    if (mode == 2)
        return aces(c);
    if (mode == 3)
        return saturate(c);
    return neutralTonemap(c);
}
