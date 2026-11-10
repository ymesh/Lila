#include "groom/Style.h"

#include "core/Profile.h"
#include "groom/Attrs.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <random>

namespace lila::groom
{
namespace
{
constexpr std::array<const char*, size_t(Style::Count)> kStyleNames = {"split", "clump", "curl", "wave", "frizz", "noise", "length", "cut", "flyaways"};

/** Fraction of the length at each point. */
std::vector<double> alongOf(const Spline& s)
{
    std::vector<double> t(s.size(), 0.0);
    for (size_t i = 1; i < s.size(); ++i)
        t[i] = t[i - 1] + glm::distance(s.points[i - 1], s.points[i]);
    const double len = t.empty() ? 0.0 : t.back();
    if (len > 1e-12)
        for (double& v : t)
            v /= len;
    return t;
}

/** The curve as a poly with points at most step apart (and at least its own count), every attribute carried. */
Spline dense(const Spline& s, double step)
{
    const double len = attr::length(s);
    const int n = std::clamp(int(std::ceil(len / std::max(1e-3, step))) + 1, int(std::max<size_t>(2, s.size())), 1024);
    return attr::resampled(s, n);
}

double defaultRamp(Style k, double t)
{
    switch (k)
    {
    // Nothing at the root, so the lock leaves the scalp where it was planted.
    case Style::Curl:
    case Style::Wave: return glm::smoothstep(0.0, 0.2, t);
    case Style::Frizz:
    case Style::Noise: return glm::smoothstep(0.0, 0.6, t);
    case Style::Clump: return glm::smoothstep(0.0, 0.8, t);
    case Style::Split: return 0.15 + 0.85 * glm::smoothstep(0.0, 1.0, t);
    default: return 1.0;
    }
}

double rampAt(const StyleOptions& o, double t) { return o.ramp ? o.ramp(t) : defaultRamp(o.kind, t); }

/** The part of the curve up to fraction f of its length, as many points as it had. */
Spline trimmed(const Spline& s, double f)
{
    if (f >= 1.0 - 1e-9 || s.size() < 2)
        return s;
    Spline d = dense(s, attr::length(s) / 256.0);
    const std::vector<double> t = alongOf(d);
    size_t k = 1;
    while (k + 1 < d.size() && t[k] < f)
        ++k;
    Spline head = d;
    const double span = t[k] - t[k - 1];
    const double w = span > 1e-12 ? std::clamp((f - t[k - 1]) / span, 0.0, 1.0) : 0.0;
    head.points.resize(k + 1);
    head.points[k] = glm::mix(d.points[k - 1], d.points[k], w);
    auto cut = [&](std::vector<double>& v) {
        if (v.size() != d.size())
            return;
        v.resize(k + 1);
        v[k] = glm::mix(v[k - 1], v[k], w);
    };
    cut(head.radius);
    cut(head.tilt);
    for (auto& [name, v] : head.pointAttrs)
        cut(v);
    return attr::resampled(head, int(std::max<size_t>(2, s.size())));
}

/** Smooth random values along a curve: knots every 1/frequency of its length, cubic between them. */
struct ValueNoise
{
    std::vector<glm::dvec2> knots;
    double frequency = 1.0;

    ValueNoise(uint32_t seed, double length, double freq) : frequency(std::max(1e-3, freq))
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        const int n = int(std::ceil(length * frequency)) + 4;
        for (int i = 0; i < n; ++i)
            knots.push_back({u(rng), u(rng)});
    }

    glm::dvec2 at(double distance) const
    {
        const double x = distance * frequency;
        const int i = std::clamp(int(std::floor(x)), 0, int(knots.size()) - 2);
        const double f = x - i;
        const double s = f * f * (3.0 - 2.0 * f);
        return glm::mix(knots[size_t(i)], knots[size_t(i) + 1], s);
    }
};

void curl(Splines& all, const StyleOptions& o)
{
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        Spline s = dense(all[ci], 1.0 / std::max(1e-3, o.frequency * 12.0));
        if (s.size() < 3)
            continue;
        std::mt19937 rng(attr::seed(s, ci, o.seed));
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const double phase = u(rng) * glm::two_pi<double>() * o.randomness;
        const double size = 1.0 + (u(rng) - 0.5) * o.randomness;
        const double freq = o.frequency * (1.0 + (u(rng) - 0.5) * 0.4 * o.randomness);
        const Frames f = lockFrames(s, nullptr, 0.0);
        const std::vector<double> t = alongOf(s);
        const double len = attr::length(s);
        const std::vector<D3> base = s.points;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double amount = (o.amount + attr::point(s, attr::kCurl, i, 0.0)) * rampAt(o, t[i]) * size;
            if (amount == 0.0)
                continue;
            const double a = phase + glm::two_pi<double>() * freq * t[i] * len;
            // Measured from the axis the curl winds around, so the root stays put when the ramp starts at zero.
            const D3 off = (f.side[i] * std::cos(a) + f.thick[i] * std::sin(a) - f.side[i] * std::cos(phase) - f.thick[i] * std::sin(phase));
            s.points[i] = base[i] + off * (o.radius * amount);
        }
        all[ci] = std::move(s);
    }
}

void wave(Splines& all, const StyleOptions& o, const Surface* scalp)
{
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        Spline s = dense(all[ci], 1.0 / std::max(1e-3, o.frequency * 10.0));
        if (s.size() < 3)
            continue;
        std::mt19937 rng(attr::seed(s, ci, o.seed));
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const double phase = u(rng) * glm::two_pi<double>() * o.randomness;
        const double size = 1.0 + (u(rng) - 0.5) * o.randomness;
        const Frames f = lockFrames(s, scalp, 1.0);
        const std::vector<double> t = alongOf(s);
        const double len = attr::length(s);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double amp = (o.amount + attr::point(s, attr::kWave, i, 0.0)) * rampAt(o, t[i]) * size;
            s.points[i] += f.side[i] * (amp * (std::sin(phase + glm::two_pi<double>() * o.frequency * t[i] * len) - std::sin(phase)));
        }
        all[ci] = std::move(s);
    }
}

void frizz(Splines& all, const StyleOptions& o)
{
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        Spline s = dense(all[ci], 1.0 / std::max(1e-3, o.frequency * 4.0));
        if (s.size() < 3)
            continue;
        const double len = attr::length(s);
        const ValueNoise noise(attr::seed(s, ci, o.seed), len, o.frequency);
        std::mt19937 rng(attr::seed(s, ci, o.seed ^ 0x5bd1e995u));
        const double size = 1.0 + (std::uniform_real_distribution<double>(0.0, 1.0)(rng) - 0.5) * o.randomness;
        const Frames f = lockFrames(s, nullptr, 0.0);
        const std::vector<double> t = alongOf(s);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double amp = (o.amount + attr::point(s, attr::kFrizz, i, 0.0)) * rampAt(o, t[i]) * size;
            const glm::dvec2 n = noise.at(t[i] * len) - noise.at(0.0);
            s.points[i] += (f.side[i] * n.x + f.thick[i] * n.y) * amp;
        }
        all[ci] = std::move(s);
    }
}

void noise(Splines& all, const StyleOptions& o)
{
    const D3 offset = D3(17.3, -4.1, 9.7) * double(o.seed % 997);
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        Spline s = dense(all[ci], 1.0 / std::max(1e-3, o.frequency * 6.0));
        const std::vector<double> t = alongOf(s);
        const std::vector<D3> base = s.points;
        const D3 root = base.empty() ? D3(0.0) : base.front();
        auto field = [&](const D3& p) {
            const glm::dvec3 q = (p + offset) * o.frequency;
            return D3(glm::simplex(q), glm::simplex(q + D3(31.4, 0.0, 0.0)), glm::simplex(q + D3(0.0, 47.2, 0.0)));
        };
        const D3 atRoot = field(root);
        for (size_t i = 0; i < s.size(); ++i)
            s.points[i] = base[i] + (field(base[i]) - atRoot) * (o.amount * rampAt(o, t[i]));
        all[ci] = std::move(s);
    }
}

/** Clump of each curve: from the clump attr, else roots gathered into count groups by farthest point seeding and k-means. */
std::vector<int> clumpsOf(Splines& all, const StyleOptions& o)
{
    std::vector<int> group(all.size(), 0);
    if (all.empty())
        return group;
    if (o.count <= 0)
    {
        std::map<int, int> ids;
        for (size_t i = 0; i < all.size(); ++i)
            group[i] = ids.try_emplace(int(attr::get(all[i], attr::kClump, 0.0)), int(ids.size())).first->second;
        return group;
    }
    const int k = std::min<int>(o.count, int(all.size()));
    std::vector<D3> roots;
    for (const Spline& s : all)
        roots.push_back(s.points.empty() ? D3(0.0) : s.points.front());
    std::vector<D3> centres;
    std::mt19937 rng(o.seed);
    centres.push_back(roots[rng() % roots.size()]);
    std::vector<double> nearest(roots.size(), 1e300);
    while (int(centres.size()) < k)
    {
        size_t far = 0;
        for (size_t i = 0; i < roots.size(); ++i)
        {
            nearest[i] = std::min(nearest[i], glm::distance(roots[i], centres.back()));
            if (nearest[i] > nearest[far])
                far = i;
        }
        centres.push_back(roots[far]);
    }
    for (int it = 0; it < 12; ++it)
    {
        for (size_t i = 0; i < roots.size(); ++i)
        {
            double best = 1e300;
            for (int c = 0; c < k; ++c)
                if (double d = glm::distance(roots[i], centres[size_t(c)]); d < best)
                {
                    best = d;
                    group[i] = c;
                }
        }
        std::vector<D3> sum(size_t(k), D3(0.0));
        std::vector<int> count(size_t(k), 0);
        for (size_t i = 0; i < roots.size(); ++i)
        {
            sum[size_t(group[i])] += roots[i];
            ++count[size_t(group[i])];
        }
        for (int c = 0; c < k; ++c)
            if (count[size_t(c)])
                centres[size_t(c)] = sum[size_t(c)] / double(count[size_t(c)]);
    }
    for (size_t i = 0; i < all.size(); ++i)
        attr::set(all[i], attr::kClump, double(group[i]));
    return group;
}

void clump(Splines& all, const StyleOptions& o)
{
    const std::vector<int> group = clumpsOf(all, o);
    const int samples = 64;
    std::map<int, std::vector<D3>> middle;
    std::map<int, int> members;
    for (size_t i = 0; i < all.size(); ++i)
    {
        const Spline r = attr::resampled(all[i], samples);
        std::vector<D3>& m = middle[group[i]];
        m.resize(samples, D3(0.0));
        for (int k = 0; k < samples; ++k)
            m[size_t(k)] += r.points[size_t(k)];
        ++members[group[i]];
    }
    for (auto& [g, m] : middle)
        for (D3& p : m)
            p /= double(members[g]);
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        if (members[group[ci]] < 2)
            continue;
        Spline& s = all[ci];
        const std::vector<double> t = alongOf(s);
        const std::vector<D3>& m = middle[group[ci]];
        for (size_t i = 1; i < s.size(); ++i)
        {
            const double x = t[i] * (samples - 1);
            const int k = std::clamp(int(x), 0, samples - 2);
            const D3 c = glm::mix(m[size_t(k)], m[size_t(k) + 1], x - k);
            const double w = std::clamp((o.amount + attr::point(s, attr::kClumpWeight, i, 0.0)) * rampAt(o, t[i]), 0.0, 1.0);
            s.points[i] += (c - s.points[i]) * w;
        }
    }
}

void split(Splines& all, const StyleOptions& o, const Surface* scalp)
{
    const int count = std::clamp(o.count, 1, 64);
    if (count == 1)
        return;
    Splines out;
    out.reserve(all.size() * size_t(count));
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        const Spline& parent = all[ci];
        if (parent.size() < 2)
            continue;
        const Spline s = dense(parent, std::max(0.1, attr::length(parent) / 24.0));
        const Frames f = lockFrames(s, scalp, 1.0);
        const std::vector<double> t = alongOf(s);
        std::mt19937 rng(attr::seed(s, ci, o.seed));
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const double width = attr::get(parent, attr::kWidth, 1.0);
        const int64_t id = int64_t(attr::get(parent, attr::kId, double(ci + 1)));
        for (int k = 0; k < count; ++k)
        {
            Spline sub = s;
            // Spread across the lock's width, most of it sideways so the sub-locks stay a flat fan.
            const double across = count > 1 ? -1.0 + 2.0 * (k + 0.5) / count : 0.0;
            const double jitter = (u(rng) - 0.5) * o.randomness;
            const double depth = (u(rng) - 0.5) * 0.5 * o.randomness;
            const double reach = 1.0 + (u(rng) - 0.5) * o.randomness;
            for (size_t i = 0; i < sub.size(); ++i)
            {
                const double r = rampAt(o, t[i]) * o.spread * reach;
                sub.points[i] += (f.side[i] * (across + jitter / count) + f.thick[i] * depth) * r;
            }
            attr::set(sub, attr::kWidth, width * o.widthScale * (1.0 + (u(rng) - 0.5) * 0.3 * o.randomness));
            attr::set(sub, attr::kId, double(id * 64 + k + 1));
            attr::set(sub, attr::kGuide, 0.0);
            attr::set(sub, attr::kClump, double(id));
            out.push_back(std::move(sub));
        }
    }
    all = std::move(out);
}

void lengthVariation(Splines& all, const StyleOptions& o)
{
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        std::mt19937 rng(attr::seed(all[ci], ci, o.seed));
        const double f = 1.0 - std::clamp(o.amount, 0.0, 0.95) * std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        all[ci] = trimmed(all[ci], f);
    }
}

void cut(Splines& all, const StyleOptions& o)
{
    for (Spline& s : all)
    {
        const double len = attr::length(s);
        if (len > o.length && o.length > 0.0)
            s = trimmed(s, o.length / len);
    }
}

void flyaways(Splines& all, const StyleOptions& o)
{
    Splines strays;
    for (size_t ci = 0; ci < all.size(); ++ci)
    {
        const Spline& lock = all[ci];
        if (lock.size() < 2 || attr::get(lock, attr::kStray, 0.0) > 0.5)
            continue;
        std::mt19937 rng(attr::seed(lock, ci, o.seed));
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const int n = int(std::floor(std::max(0.0, o.amount) + u(rng)));
        if (n == 0)
            continue;
        const Spline s = dense(lock, std::max(0.1, attr::length(lock) / 32.0));
        const Frames f = lockFrames(s, nullptr, 0.0);
        const std::vector<double> t = alongOf(s);
        const int64_t id = int64_t(attr::get(lock, attr::kId, double(ci + 1)));
        const double len = attr::length(s);
        for (int k = 0; k < n; ++k)
        {
            Spline st = s;
            const double a = u(rng) * glm::two_pi<double>();
            const double start = 0.15 + 0.5 * u(rng);
            const ValueNoise kink(uint32_t(rng()), len, 0.6);
            for (size_t i = 0; i < st.size(); ++i)
            {
                const double out = std::pow(std::max(0.0, t[i] - start) / (1.0 - start), 1.5) * o.spread;
                const glm::dvec2 q = kink.at(t[i] * len) * 0.15 * o.spread * glm::smoothstep(start, 1.0, t[i]);
                st.points[i] += f.side[i] * (std::cos(a) * out + q.x) + f.thick[i] * (std::sin(a) * out + q.y);
            }
            st = trimmed(st, std::clamp(o.length * (0.6 + 0.4 * u(rng)), 0.05, 1.0));
            st.pointAttrs.clear();
            st.radius.clear();
            attr::set(st, attr::kStray, 1.0);
            attr::set(st, attr::kGuide, 0.0);
            attr::set(st, attr::kWidth, 1.0);
            attr::set(st, attr::kProfile, 0.0);
            attr::set(st, attr::kId, double(id * 64 + 63 - k));
            strays.push_back(std::move(st));
        }
    }
    for (Spline& s : strays)
        all.push_back(std::move(s));
}
} // namespace

const char* styleName(Style s) { return size_t(s) < kStyleNames.size() ? kStyleNames[size_t(s)] : "?"; }

bool parseStyle(std::string_view name, Style* out)
{
    for (size_t k = 0; k < kStyleNames.size(); ++k)
        if (name == kStyleNames[k])
        {
            *out = Style(k);
            return true;
        }
    return false;
}

void applyStyle(Splines& s, const StyleOptions& o, const Surface* scalp)
{
    LILA_ZONE();

    switch (o.kind)
    {
    case Style::Split: split(s, o, scalp); break;
    case Style::Clump: clump(s, o); break;
    case Style::Curl: curl(s, o); break;
    case Style::Wave: wave(s, o, scalp); break;
    case Style::Frizz: frizz(s, o); break;
    case Style::Noise: noise(s, o); break;
    case Style::Length: lengthVariation(s, o); break;
    case Style::Cut: cut(s, o); break;
    case Style::Flyaways: flyaways(s, o); break;
    default: break;
    }
}
} // namespace lila::groom
