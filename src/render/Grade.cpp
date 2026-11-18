#include "render/Grade.h"

#include "render/Ocio.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>

namespace lila::render::grade
{
namespace
{
bool hueCurve(Curve c)
{
    return c == kHueVsSat || c == kHueVsHue;
}

glm::vec3 rgbToHsv(glm::vec3 c)
{
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f)
    {
        if (mx == c.r)
            h = std::fmod((c.g - c.b) / d + 6.0f, 6.0f);
        else if (mx == c.g)
            h = (c.b - c.r) / d + 2.0f;
        else
            h = (c.r - c.g) / d + 4.0f;
        h /= 6.0f;
    }
    return {h, mx > 1e-6f ? d / mx : 0.0f, mx};
}

glm::vec3 hsvToRgb(glm::vec3 hsv)
{
    const glm::vec3 k(1.0f, 2.0f / 3.0f, 1.0f / 3.0f);
    glm::vec3 p = glm::abs(glm::fract(glm::vec3(hsv.x) + k) * 6.0f - 3.0f);
    return hsv.z * glm::mix(glm::vec3(1.0f), glm::clamp(p - 1.0f, 0.0f, 1.0f), hsv.y);
}

float sampleRow(const GradeCurves& c, int row, float x, bool wrap)
{
    const int n = GradeCurves::kSize;
    float f = (wrap ? x - std::floor(x) : std::clamp(x, 0.0f, 1.0f)) * float(n - 1);
    const int i = std::min(int(f), n - 2);
    f -= float(i);
    const float* r = c.values.data() + size_t(row) * n;
    return r[i] + (r[i + 1] - r[i]) * f;
}
} // namespace

std::vector<glm::vec2> parseCurve(const std::string& text)
{
    std::vector<glm::vec2> out;
    std::string t = text;
    std::ranges::replace(t, ',', ' ');
    std::istringstream in(t);
    float x = 0.0f, y = 0.0f;
    while (in >> x >> y)
        out.emplace_back(std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f));
    std::ranges::sort(out, [](glm::vec2 a, glm::vec2 b) { return a.x < b.x; });
    return out;
}

std::string formatCurve(const std::vector<glm::vec2>& points)
{
    std::string out;
    char buf[64];
    for (const glm::vec2& p : points)
    {
        std::snprintf(buf, sizeof(buf), "%s%.3f %.3f", out.empty() ? "" : ", ", p.x, p.y);
        out += buf;
    }
    return out;
}

float neutral(Curve c, float x)
{
    return c <= kBlue ? x : 0.5f;
}

float evaluate(Curve c, const std::vector<glm::vec2>& pts, float x)
{
    if (pts.empty())
        return neutral(c, x);
    if (pts.size() == 1)
        return pts[0].y;
    std::vector<glm::vec2> p = pts;
    if (hueCurve(c))
    {
        // Wrap: the last point repeats before 0 and the first after 1, so the curve joins itself at red.
        p.insert(p.begin(), {pts.back().x - 1.0f, pts.back().y});
        p.push_back({pts.front().x + 1.0f, pts.front().y});
        x -= std::floor(x);
    }
    if (x <= p.front().x)
        return p.front().y;
    if (x >= p.back().x)
        return p.back().y;
    // Fritsch-Carlson monotone tangents.
    const size_t n = p.size();
    std::vector<float> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i)
        d[i] = (p[i + 1].y - p[i].y) / std::max(p[i + 1].x - p[i].x, 1e-6f);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = d[i - 1] * d[i] <= 0.0f ? 0.0f : (d[i - 1] + d[i]) * 0.5f;
    for (size_t i = 0; i + 1 < n; ++i)
        if (std::abs(d[i]) < 1e-9f)
            m[i] = m[i + 1] = 0.0f;
        else
        {
            const float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9.0f)
            {
                const float t = 3.0f / std::sqrt(s);
                m[i] = t * a * d[i];
                m[i + 1] = t * b * d[i];
            }
        }
    size_t k = 0;
    while (k + 2 < n && x > p[k + 1].x)
        ++k;
    const float h = std::max(p[k + 1].x - p[k].x, 1e-6f), t = (x - p[k].x) / h;
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p[k].y + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * p[k + 1].y + (t3 - t2) * h * m[k + 1];
}

std::shared_ptr<const GradeCurves> curves(const std::array<std::string, kCurveCount>& texts)
{
    std::string key;
    for (const std::string& t : texts)
        key += t + "|";
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const GradeCurves>> cache;
    std::lock_guard lock(mutex);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    std::shared_ptr<const GradeCurves> out;
    bool any = false;
    std::array<std::vector<glm::vec2>, kCurveCount> pts;
    for (int i = 0; i < kCurveCount; ++i)
    {
        pts[size_t(i)] = parseCurve(texts[size_t(i)]);
        // A curve whose points all sit on the neutral line changes nothing.
        for (const glm::vec2& q : pts[size_t(i)])
            any = any || std::abs(q.y - neutral(Curve(i), q.x)) > 1e-4f;
    }
    if (any)
    {
        auto c = std::make_shared<GradeCurves>();
        c->values.resize(size_t(GradeCurves::kRows) * GradeCurves::kSize);
        for (int r = 0; r < GradeCurves::kRows; ++r)
            for (int i = 0; i < GradeCurves::kSize; ++i)
                c->values[size_t(r) * GradeCurves::kSize + size_t(i)] =
                    evaluate(Curve(r), pts[size_t(r)], float(i) / float(GradeCurves::kSize - 1));
        out = std::move(c);
    }
    if (cache.size() > 64)
        cache.clear();
    cache[key] = out;
    return out;
}

glm::vec3 applyScene(glm::vec3 c, const PostSettings& p)
{
    c *= p.whiteBalance;
    const float grey = 0.18f;
    c = grey * glm::pow(glm::max(c / grey, glm::vec3(0.0f)), glm::vec3(p.contrast));
    const float luma = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return glm::max(glm::vec3(luma) + p.saturation * (c - glm::vec3(luma)), glm::vec3(0.0f));
}

glm::vec3 applyDisplay(glm::vec3 s, const PostSettings& p)
{
    s += p.offset + glm::vec3(p.offsetMaster);
    s = glm::clamp(s * p.gain * p.gainMaster + (p.lift + glm::vec3(p.liftMaster)) * (glm::vec3(1.0f) - s), 0.0f, 1.0f);
    s = glm::pow(s, 1.0f / glm::max(p.gamma * p.gammaMaster, glm::vec3(0.01f)));
    float luma = glm::dot(s, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    const float sr = std::max(p.shadowsRange, 1e-3f), hr = std::min(p.highlightsRange, 1.0f - 1e-3f);
    const float ws = 1.0f - glm::smoothstep(0.0f, sr, luma), wh = glm::smoothstep(hr, 1.0f, luma);
    const float wm = std::clamp(1.0f - ws - wh, 0.0f, 1.0f);
    s = glm::clamp(s * (ws * p.shadows + wm * p.midtones + wh * p.highlights), 0.0f, 1.0f);
    if (p.curves)
    {
        const GradeCurves& c = *p.curves;
        for (int i = 0; i < 3; ++i)
            s[i] = sampleRow(c, kRed + i, sampleRow(c, kMaster, s[i], false), false);
        glm::vec3 hsv = rgbToHsv(s);
        luma = glm::dot(s, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        hsv.y = std::clamp(hsv.y * 2.0f * sampleRow(c, kHueVsSat, hsv.x, true) * 2.0f * sampleRow(c, kLumVsSat, luma, false), 0.0f, 1.0f);
        hsv.x += sampleRow(c, kHueVsHue, hsv.x, true) - 0.5f;
        hsv.x -= std::floor(hsv.x);
        s = hsvToRgb(hsv);
    }
    if (p.lut && p.lutIntensity > 0.0f && p.lut->size >= 2)
    {
        const int n = p.lut->size;
        auto at = [&](int r, int g, int b) {
            const size_t i = (size_t(b) * n * n + size_t(g) * n + size_t(r)) * 3;
            return glm::vec3(p.lut->rgb[i], p.lut->rgb[i + 1], p.lut->rgb[i + 2]);
        };
        glm::vec3 f = glm::clamp(s, 0.0f, 1.0f) * float(n - 1);
        glm::ivec3 i0 = glm::min(glm::ivec3(f), glm::ivec3(n - 2));
        f -= glm::vec3(i0);
        auto lerp3 = [](glm::vec3 a, glm::vec3 b, float t) { return a + (b - a) * t; };
        const glm::vec3 c00 = lerp3(at(i0.x, i0.y, i0.z), at(i0.x + 1, i0.y, i0.z), f.x);
        const glm::vec3 c10 = lerp3(at(i0.x, i0.y + 1, i0.z), at(i0.x + 1, i0.y + 1, i0.z), f.x);
        const glm::vec3 c01 = lerp3(at(i0.x, i0.y, i0.z + 1), at(i0.x + 1, i0.y, i0.z + 1), f.x);
        const glm::vec3 c11 = lerp3(at(i0.x, i0.y + 1, i0.z + 1), at(i0.x + 1, i0.y + 1, i0.z + 1), f.x);
        s = glm::mix(s, lerp3(lerp3(c00, c10, f.y), lerp3(c01, c11, f.y), f.z), std::clamp(p.lutIntensity, 0.0f, 1.0f));
    }
    return s;
}
namespace
{
glm::vec3 neutralTone(glm::vec3 c)
{
    const float start = 0.8f - 0.04f, desaturation = 0.15f;
    const float x = std::min({c.r, c.g, c.b});
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c -= offset;
    const float peak = std::max({c.r, c.g, c.b});
    if (peak < start)
        return c;
    const float d = 1.0f - start;
    const float newPeak = 1.0f - d * d / (peak + d - start);
    c *= newPeak / peak;
    const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return glm::mix(c, glm::vec3(newPeak), g);
}

glm::vec3 agxTone(glm::vec3 c, int look)
{
    // compose.slang builds these row by row and multiplies v * m; glm builds columns, hence the transposes.
    const glm::mat3 inset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f, 0.0784335999999992f, 0.878468636469772f, 0.0784336f,
                          0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    const glm::mat3 outset(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f, -0.0980208811401368f, 1.15190312990417f,
                           -0.0980434501171241f, -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
    const float minEv = -12.47393f, maxEv = 4.026069f;
    glm::vec3 v = glm::max(c, glm::vec3(1e-10f)) * glm::transpose(inset);
    v = glm::clamp((glm::log2(v) - minEv) / (maxEv - minEv), 0.0f, 1.0f);
    const glm::vec3 x2 = v * v, x4 = x2 * x2;
    v = 15.5f * x4 * x2 - 40.14f * x4 * v + 31.96f * x4 - 6.868f * x2 * v + 0.4298f * x2 + 0.1191f * v - 0.00232f;
    if (look != 0)
    {
        glm::vec3 slope(1.0f), power(1.0f);
        float sat = 1.0f;
        if (look == 1)
        {
            power = glm::vec3(1.35f);
            sat = 1.4f;
        }
        else
        {
            slope = {1.0f, 0.9f, 0.5f};
            power = glm::vec3(0.8f);
            sat = 0.8f;
        }
        const float luma = glm::dot(v, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        v = glm::pow(glm::max(v * slope, glm::vec3(0.0f)), power);
        v = glm::vec3(luma) + sat * (v - glm::vec3(luma));
    }
    v = v * glm::transpose(outset);
    return glm::pow(glm::max(v, glm::vec3(0.0f)), glm::vec3(2.2f));
}

glm::vec3 acesTone(glm::vec3 c)
{
    // compose.slang's mul(m, v): rows are the matrix rows.
    const glm::mat3 in = glm::transpose(glm::mat3(0.59719f, 0.35458f, 0.04823f, 0.07600f, 0.90834f, 0.01566f, 0.02840f, 0.13383f, 0.83777f));
    const glm::mat3 out = glm::transpose(glm::mat3(1.60475f, -0.53108f, -0.07367f, -0.10208f, 1.10813f, -0.00605f, -0.00327f, -0.07276f, 1.07602f));
    const glm::vec3 v = in * c;
    const glm::vec3 a = v * (v + 0.0245786f) - 0.000090537f;
    const glm::vec3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return glm::clamp(out * (a / b), 0.0f, 1.0f);
}

float encodeSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
} // namespace

glm::vec3 tonemap(glm::vec3 c, int mode, int agxLook)
{
    switch (mode)
    {
    case PostSettings::AgX: return agxTone(c, agxLook);
    case PostSettings::Aces: return acesTone(c);
    case PostSettings::Linear: return glm::clamp(c, 0.0f, 1.0f);
    default: return neutralTone(c);
    }
}

glm::vec3 applyAll(glm::vec3 c, const PostSettings& p, float exposure, float displayGamma)
{
    c = applyScene(c * std::exp2(exposure), p);
    glm::vec3 s;
    if (p.view && p.view->lut)
    {
        float px[4] = {c.r, c.g, c.b, 1.0f};
        ocio::applyView(*p.view, px);
        s = glm::clamp(glm::vec3(px[0], px[1], px[2]), 0.0f, 1.0f);
    }
    else
    {
        glm::vec3 t = glm::pow(glm::max(tonemap(c, p.tonemapper, p.agxLook), glm::vec3(0.0f)), glm::vec3(1.0f / std::max(displayGamma, 0.01f)));
        s = {encodeSrgb(t.r), encodeSrgb(t.g), encodeSrgb(t.b)};
    }
    return applyDisplay(s, p);
}
} // namespace lila::render::grade
