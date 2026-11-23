#include "render/Atmosphere.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace lila::render
{
namespace
{
constexpr double kPiD = 3.14159265358979323846;
constexpr double kCmPerKm = 1.0e5;
constexpr const char* kKeyPrefix = "atmosphere:";

struct Medium
{
    glm::dvec3 rayleigh{0.0}, mie{0.0}, extinction{0.0};
};

Medium medium(const AtmosphereSettings& a, double h)
{
    h = std::max(h, 0.0);
    const double dr = std::exp(-h / std::max(double(a.rayleighScaleHeight), 1e-9));
    const double dm = std::exp(-h / std::max(double(a.mieScaleHeight), 1e-9));
    const double dOzone = std::max(0.0, double(a.otherTipValue) * (1.0 - std::abs(h - double(a.otherTipAltitude)) / std::max(double(a.otherWidth), 1e-9)));
    Medium m;
    m.rayleigh = glm::dvec3(a.rayleighScattering) * dr;
    m.mie = glm::dvec3(a.mieScattering) * dm;
    m.extinction = m.rayleigh + m.mie + glm::dvec3(a.mieAbsorption) * dm + glm::dvec3(a.otherAbsorption) * dOzone;
    return m;
}

/** Distance along a ray from radius r at cos zenith mu to a sphere of radius R it is inside of. */
double distanceToSphereFromInside(double r, double mu, double R)
{
    const double disc = r * r * (mu * mu - 1.0) + R * R;
    return std::max(0.0, -r * mu + std::sqrt(std::max(disc, 0.0)));
}

bool hitsGround(double r, double mu, double bottom)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + bottom * bottom >= 0.0;
}

/** Nearest hit at or after 0 of a ray with a sphere about the origin; negative when it misses. */
double raySphere(const glm::dvec3& o, const glm::dvec3& d, double R)
{
    const double b = glm::dot(o, d);
    const double c = glm::dot(o, o) - R * R;
    const double disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    const double s = std::sqrt(disc);
    const double t0 = -b - s, t1 = -b + s;
    if (t0 >= 0.0)
        return t0;
    return t1 >= 0.0 ? t1 : -1.0;
}

glm::vec2 transmittanceUv(const AtmosphereSettings& a, double r, double mu)
{
    const double bottom = a.bottomRadius, top = a.topRadius();
    const double H = std::sqrt(std::max(top * top - bottom * bottom, 0.0));
    const double rho = std::sqrt(std::max(r * r - bottom * bottom, 0.0));
    const double d = distanceToSphereFromInside(r, mu, top);
    const double dMin = top - r, dMax = rho + H;
    const double xMu = dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0;
    return {float(std::clamp(xMu, 0.0, 1.0)), float(std::clamp(rho / std::max(H, 1e-9), 0.0, 1.0))};
}

/** r and mu at a transmittance texel's coordinates, the inverse of transmittanceUv. */
void transmittanceRMu(const AtmosphereSettings& a, double xMu, double xR, double& r, double& mu)
{
    const double bottom = a.bottomRadius, top = a.topRadius();
    const double H = std::sqrt(std::max(top * top - bottom * bottom, 0.0));
    const double rho = H * xR;
    r = std::sqrt(rho * rho + bottom * bottom);
    const double dMin = top - r, dMax = rho + H;
    const double d = dMin + xMu * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = std::clamp(mu, -1.0, 1.0);
}

glm::vec3 bilinear(const std::vector<glm::vec3>& t, int w, int h, glm::vec2 uv)
{
    const float x = std::clamp(uv.x * float(w) - 0.5f, 0.0f, float(w - 1));
    const float y = std::clamp(uv.y * float(h) - 0.5f, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const glm::vec3 a = glm::mix(t[size_t(y0) * w + x0], t[size_t(y0) * w + x1], fx);
    const glm::vec3 b = glm::mix(t[size_t(y1) * w + x0], t[size_t(y1) * w + x1], fx);
    return glm::mix(a, b, fy);
}

glm::dvec3 lutTransmittance(const AtmosphereSettings& a, const AtmosphereLuts& l, double r, double mu)
{
    return glm::dvec3(bilinear(l.transmittance, AtmosphereLuts::kTransmittanceW, AtmosphereLuts::kTransmittanceH, transmittanceUv(a, r, mu)));
}

glm::dvec3 lutMultiScatter(const AtmosphereSettings& a, const AtmosphereLuts& l, double r, double muSun)
{
    const double u = std::clamp(muSun * 0.5 + 0.5, 0.0, 1.0);
    const double v = std::clamp((r - a.bottomRadius) / std::max(a.atmosphereHeight, 1e-6), 0.0, 1.0);
    return glm::dvec3(bilinear(l.multiScatter, AtmosphereLuts::kMultiScatter, AtmosphereLuts::kMultiScatter, glm::vec2(float(u), float(v))));
}

double rayleighPhase(double nu)
{
    return 3.0 / (16.0 * kPiD) * (1.0 + nu * nu);
}

double hgPhase(double g, double nu)
{
    const double d = std::max(1.0 + g * g - 2.0 * g * nu, 1e-6);
    return (1.0 - g * g) / (4.0 * kPiD * d * std::sqrt(d));
}

/** The sun's light at p after the atmosphere and the planet's shadow. */
glm::dvec3 sunAt(const AtmosphereSettings& a, const AtmosphereLuts& l, const glm::dvec3& p, const glm::dvec3& sun)
{
    const double r = glm::length(p);
    const double mu = glm::dot(p / r, sun);
    if (hitsGround(r, mu, a.bottomRadius))
        return glm::dvec3(0.0);
    return lutTransmittance(a, l, r, mu);
}

glm::dvec3 upOf(bool zUp)
{
    return zUp ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
}
} // namespace

AtmosphereLuts buildAtmosphereLuts(const AtmosphereSettings& a)
{
    LILA_ZONE();
    AtmosphereLuts l;
    constexpr int tw = AtmosphereLuts::kTransmittanceW, th = AtmosphereLuts::kTransmittanceH;
    l.transmittance.resize(size_t(tw) * th);
    tbb::parallel_for(0, th, [&](int y) {
        for (int x = 0; x < tw; ++x)
        {
            double r, mu;
            transmittanceRMu(a, (double(x) + 0.5) / tw, (double(y) + 0.5) / th, r, mu);
            const double dist = distanceToSphereFromInside(r, mu, a.topRadius());
            constexpr int steps = 40;
            glm::dvec3 depth(0.0);
            for (int i = 0; i < steps; ++i)
            {
                const double t = (double(i) + 0.5) / steps * dist;
                const double ri = std::sqrt(r * r + t * t + 2.0 * r * mu * t);
                depth += medium(a, ri - a.bottomRadius).extinction * (dist / steps);
            }
            l.transmittance[size_t(y) * tw + x] = glm::vec3(glm::exp(-depth));
        }
    });

    // Hillaire's multiple scattering: second order light from every direction, extended by the geometric series 1 / (1 - f).
    constexpr int n = AtmosphereLuts::kMultiScatter;
    l.multiScatter.assign(size_t(n) * n, glm::vec3(0.0f));
    constexpr int dirSide = 8, steps = 20;
    tbb::parallel_for(0, n, [&](int y) {
        for (int x = 0; x < n; ++x)
        {
            const double muSun = (double(x) + 0.5) / n * 2.0 - 1.0;
            const double r = a.bottomRadius + (double(y) + 0.5) / n * a.atmosphereHeight;
            const glm::dvec3 pos(0.0, r, 0.0);
            const glm::dvec3 sun(std::sqrt(std::max(1.0 - muSun * muSun, 0.0)), muSun, 0.0);
            glm::dvec3 lum(0.0), fms(0.0);
            for (int j = 0; j < dirSide; ++j)
                for (int i = 0; i < dirSide; ++i)
                {
                    const double cosTheta = 1.0 - 2.0 * (double(j) + 0.5) / dirSide;
                    const double phi = 2.0 * kPiD * (double(i) + 0.5) / dirSide;
                    const double sinTheta = std::sqrt(std::max(1.0 - cosTheta * cosTheta, 0.0));
                    const glm::dvec3 dir(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                    const double tGround = raySphere(pos, dir, a.bottomRadius);
                    const double tMax = tGround >= 0.0 ? tGround : distanceToSphereFromInside(r, cosTheta, a.topRadius());
                    glm::dvec3 throughput(1.0), L(0.0), f(0.0);
                    const double dt = tMax / steps;
                    for (int s = 0; s < steps; ++s)
                    {
                        const glm::dvec3 p = pos + dir * ((double(s) + 0.5) * dt);
                        const Medium m = medium(a, glm::length(p) - a.bottomRadius);
                        const glm::dvec3 scatter = m.rayleigh + m.mie;
                        const glm::dvec3 segT = glm::exp(-m.extinction * dt);
                        const glm::dvec3 integ = (glm::dvec3(1.0) - segT) / glm::max(m.extinction, glm::dvec3(1e-12));
                        L += throughput * scatter * sunAt(a, l, p, sun) / (4.0 * kPiD) * integ;
                        f += throughput * scatter * integ;
                        throughput *= segT;
                    }
                    if (tGround >= 0.0)
                    {
                        const glm::dvec3 p = pos + dir * tGround;
                        const glm::dvec3 nrm = p / glm::length(p);
                        L += throughput * sunAt(a, l, p, sun) * std::max(glm::dot(nrm, sun), 0.0) * glm::dvec3(a.groundAlbedo) / kPiD;
                    }
                    lum += L;
                    fms += f;
                }
            const double count = double(dirSide * dirSide);
            // Uniform directions over the sphere with an isotropic phase: the 4 pi and 1 / (4 pi) cancel.
            lum /= count;
            fms /= count;
            const glm::dvec3 psi = lum / glm::max(glm::dvec3(1.0) - fms, glm::dvec3(1e-3));
            l.multiScatter[size_t(y) * n + x] = glm::vec3(psi);
        }
    });
    return l;
}

glm::vec3 atmosphereTransmittance(const AtmosphereSettings& a, const AtmosphereLuts& luts, double r, double mu)
{
    return glm::vec3(lutTransmittance(a, luts, r, mu));
}

glm::vec3 skyRadiance(const AtmosphereSettings& a, const AtmosphereLuts& luts, const glm::dvec3& posIn, const glm::dvec3& dir,
                      const glm::dvec3& sunDir, int samples)
{
    const double bottom = a.bottomRadius, top = a.topRadius();
    glm::dvec3 pos = posIn;
    double r = glm::length(pos);
    if (r < bottom + 1e-4)
    {
        pos *= (bottom + 1e-4) / std::max(r, 1e-9);
        r = bottom + 1e-4;
    }
    if (r > top)
    {
        // From space: start where the ray enters the atmosphere.
        const double tTop = raySphere(pos, dir, top);
        if (tTop < 0.0)
            return glm::vec3(0.0f);
        pos += dir * tTop;
        r = top;
    }
    const double tGround = raySphere(pos, dir, bottom);
    // The far root: from a point on the top sphere the near one rounds to either side of 0.
    const double b = glm::dot(pos, dir);
    const double tTop = std::max(-b + std::sqrt(std::max(b * b - glm::dot(pos, pos) + (top + 1e-6) * (top + 1e-6), 0.0)), 0.0);
    const double tMax = tGround >= 0.0 ? tGround : tTop;
    const double nu = glm::dot(dir, sunDir);
    const double phaseR = rayleighPhase(nu), phaseM = hgPhase(a.mieAnisotropy, nu);
    glm::dvec3 L(0.0), throughput(1.0);
    samples = std::max(samples, 4);
    double tPrev = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        // Squared spacing puts more samples near the eye, where the air is densest along most rays.
        const double f1 = double(i + 1) / samples;
        const double t1 = tMax * f1 * f1;
        const double dt = t1 - tPrev;
        const glm::dvec3 p = pos + dir * (tPrev + 0.5 * dt);
        tPrev = t1;
        const double rp = glm::length(p);
        const Medium m = medium(a, rp - bottom);
        const glm::dvec3 sun = sunAt(a, luts, p, sunDir);
        const glm::dvec3 ms = lutMultiScatter(a, luts, rp, glm::dot(p / rp, sunDir)) * double(a.multiScattering);
        const glm::dvec3 S = sun * (m.rayleigh * phaseR + m.mie * phaseM) + (m.rayleigh + m.mie) * ms;
        const glm::dvec3 segT = glm::exp(-m.extinction * dt);
        L += throughput * S * (glm::dvec3(1.0) - segT) / glm::max(m.extinction, glm::dvec3(1e-12));
        throughput *= segT;
    }
    if (tGround >= 0.0)
    {
        const glm::dvec3 p = pos + dir * tGround;
        const glm::dvec3 nrm = p / glm::length(p);
        L += throughput * sunAt(a, luts, p, sunDir) * std::max(glm::dot(nrm, sunDir), 0.0) * glm::dvec3(a.groundAlbedo) / kPiD;
    }
    return glm::vec3(L * glm::dvec3(a.skyLuminanceFactor));
}

glm::vec3 sunTransmittance(const AtmosphereSettings& a, const AtmosphereLuts& luts, const glm::dvec3& pos, const glm::dvec3& sunDir)
{
    const double r = std::max(glm::length(pos), a.bottomRadius + 1e-4 * a.lengthUnit());
    if (r > a.topRadius())
        return glm::vec3(1.0f);
    double mu = glm::dot(pos / std::max(glm::length(pos), 1e-9), sunDir);
    mu = std::max(mu, std::sin(glm::radians(double(a.minLightElevation))));
    if (hitsGround(r, mu, a.bottomRadius))
        return glm::vec3(0.0f);
    return glm::vec3(lutTransmittance(a, luts, r, mu));
}

glm::dvec3 atmosphereCameraKm(const AtmosphereFrame& f, const glm::dvec3& eyeWorld)
{
    glm::dvec3 p = (eyeWorld - f.planetCenter) / kCmPerKm;
    const double r = glm::length(p);
    const double floor = f.settings.bottomRadius + 0.001 * f.settings.lengthUnit();
    if (r < floor)
        p = r > 1e-9 ? p * (floor / r) : upOf(f.zUp) * floor;
    return p;
}

namespace
{
/** The camera as a bake sees it: its direction from the planet's centre rounded, its height in steps that grow with height. */
glm::dvec3 bakedCamera(const AtmosphereFrame& f, const glm::dvec3& eyeWorld)
{
    const glm::dvec3 p = atmosphereCameraKm(f, eyeWorld);
    const double r = glm::length(p);
    glm::dvec3 radial = glm::round(p / r * 100.0) / 100.0;
    radial = glm::length(radial) > 1e-6 ? glm::normalize(radial) : upOf(f.zUp);
    const double h = std::max(r - f.settings.bottomRadius, 0.0);
    const double base = 0.25 * f.settings.lengthUnit();
    const double step = std::round(std::log2(1.0 + h / base) * 4.0) / 4.0;
    const double hq = (std::exp2(step) - 1.0) * base;
    return radial * (f.settings.bottomRadius + hq);
}

void put(std::string& s, double v)
{
    s += std::format("{:.9g},", v);
}

void put3(std::string& s, const glm::dvec3& v)
{
    put(s, v.x);
    put(s, v.y);
    put(s, v.z);
}
} // namespace

std::string atmosphereEnvironmentKey(const AtmosphereFrame& f, const glm::dvec3& eyeWorld)
{
    const AtmosphereSettings& a = f.settings;
    std::string s = kKeyPrefix;
    put(s, a.bottomRadius);
    put(s, a.atmosphereHeight);
    put3(s, a.groundAlbedo);
    put(s, a.multiScattering);
    put3(s, a.rayleighScattering);
    put(s, a.rayleighScaleHeight);
    put3(s, a.mieScattering);
    put3(s, a.mieAbsorption);
    put(s, a.mieAnisotropy);
    put(s, a.mieScaleHeight);
    put3(s, a.otherAbsorption);
    put(s, a.otherTipAltitude);
    put(s, a.otherTipValue);
    put(s, a.otherWidth);
    put3(s, a.skyLuminanceFactor);
    put(s, a.aerialDistanceScale);
    put(s, a.aerialStartDepth);
    put(s, a.minLightElevation);
    put3(s, f.planetCenter);
    put3(s, glm::round(f.sunDir * 2000.0) / 2000.0);
    put3(s, f.sunRadiance);
    put(s, f.sunAngularRadius);
    put(s, f.zUp ? 1.0 : 0.0);
    put3(s, bakedCamera(f, eyeWorld) * kCmPerKm + f.planetCenter);
    return s;
}

bool isAtmosphereKey(const std::string& path)
{
    return path.starts_with(kKeyPrefix);
}

std::optional<AtmosphereFrame> parseAtmosphereKey(const std::string& path, glm::dvec3* eyeWorld)
{
    if (!isAtmosphereKey(path))
        return std::nullopt;
    std::vector<double> v;
    const char* p = path.data() + std::char_traits<char>::length(kKeyPrefix);
    const char* end = path.data() + path.size();
    while (p < end)
    {
        double x = 0.0;
        auto [next, ec] = std::from_chars(p, end, x);
        if (ec != std::errc())
            return std::nullopt;
        v.push_back(x);
        p = next < end && *next == ',' ? next + 1 : next;
    }
    if (v.size() != 44)
        return std::nullopt;
    size_t i = 0;
    auto f1 = [&] { return v[i++]; };
    auto f3 = [&] {
        glm::dvec3 r(v[i], v[i + 1], v[i + 2]);
        i += 3;
        return r;
    };
    AtmosphereFrame f;
    AtmosphereSettings& a = f.settings;
    a.bottomRadius = f1();
    a.atmosphereHeight = f1();
    a.groundAlbedo = glm::vec3(f3());
    a.multiScattering = float(f1());
    a.rayleighScattering = glm::vec3(f3());
    a.rayleighScaleHeight = float(f1());
    a.mieScattering = glm::vec3(f3());
    a.mieAbsorption = glm::vec3(f3());
    a.mieAnisotropy = float(f1());
    a.mieScaleHeight = float(f1());
    a.otherAbsorption = glm::vec3(f3());
    a.otherTipAltitude = float(f1());
    a.otherTipValue = float(f1());
    a.otherWidth = float(f1());
    a.skyLuminanceFactor = glm::vec3(f3());
    a.aerialDistanceScale = float(f1());
    a.aerialStartDepth = float(f1());
    a.minLightElevation = float(f1());
    f.planetCenter = f3();
    f.sunDir = glm::normalize(f3());
    f.sunRadiance = glm::vec3(f3());
    f.sunAngularRadius = float(f1());
    f.zUp = f1() > 0.5;
    const glm::dvec3 eye = f3();
    if (eyeWorld)
        *eyeWorld = eye;
    return f;
}

EnvironmentData buildAtmosphereEnvironment(const AtmosphereFrame& f, const glm::dvec3& eyeWorld)
{
    LILA_ZONE();
    const AtmosphereLuts luts = buildAtmosphereLuts(f.settings);
    const glm::dvec3 cam = atmosphereCameraKm(f, eyeWorld);
    const glm::dvec3 sun = glm::normalize(f.sunDir);
    const glm::vec3 radiance = f.sunRadiance;
    const bool zUp = f.zUp;
    // The environment's axes are Y-up; a Z-up scene's are turned as mesh.slang's toEnv turns them.
    return buildEnvironment(128, [&](const glm::vec3& e) {
        const glm::dvec3 world = zUp ? glm::dvec3(e.x, -e.z, e.y) : glm::dvec3(e);
        return skyRadiance(f.settings, luts, cam, glm::normalize(world), sun, 24) * radiance;
    });
}

std::vector<glm::vec4> packAtmosphere(const AtmosphereFrame& f, const AtmosphereLuts& luts, const glm::dvec3& eyeWorld)
{
    const AtmosphereSettings& a = f.settings;
    GpuAtmosphere g{};
    g.radii = glm::vec4(float(a.bottomRadius), float(a.topRadius()), a.multiScattering, 1.0f);
    g.rayleigh = glm::vec4(a.rayleighScattering, a.rayleighScaleHeight);
    g.mieScatter = glm::vec4(a.mieScattering, a.mieScaleHeight);
    g.mieAbsorb = glm::vec4(a.mieAbsorption, a.mieAnisotropy);
    g.ozone = glm::vec4(a.otherAbsorption, a.otherTipAltitude);
    g.ozoneShape = glm::vec4(a.otherTipValue, a.otherWidth, float(std::sin(glm::radians(double(a.minLightElevation)))), 0.0f);
    g.ground = glm::vec4(a.groundAlbedo, std::cos(f.sunAngularRadius));
    g.skyFactor = glm::vec4(a.skyLuminanceFactor, a.aerialDistanceScale);
    g.camera = glm::vec4(glm::vec3(atmosphereCameraKm(f, eyeWorld)), a.aerialStartDepth);
    g.sunDir = glm::vec4(glm::vec3(glm::normalize(f.sunDir)), f.zUp ? 1.0f : 0.0f);
    g.sunRadiance = glm::vec4(f.sunRadiance, 0.0f);
    std::vector<glm::vec4> out(sizeof(GpuAtmosphere) / sizeof(glm::vec4) + luts.transmittance.size() + luts.multiScatter.size());
    std::memcpy(out.data(), &g, sizeof(g));
    size_t k = sizeof(GpuAtmosphere) / sizeof(glm::vec4);
    for (const glm::vec3& t : luts.transmittance)
        out[k++] = glm::vec4(t, 1.0f);
    for (const glm::vec3& m : luts.multiScatter)
        out[k++] = glm::vec4(m, 1.0f);
    return out;
}
} // namespace lila::render
