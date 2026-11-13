#pragma once

// Pure key-timing maths behind the Stacker, Cycle Maker and Anim Bank: no graph, so the tests drive it directly.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace puppeteer::anim
{
/** Seconds within which two key times are the same key. */
inline constexpr double kTimeEps = 1e-6;

inline bool sameTime(double a, double b) { return std::abs(a - b) <= kTimeEps; }

/**
 * A monotone, piecewise-linear remapping of key times. Between knots it interpolates; before the first knot and after
 * the last it shifts by that knot's offset. No knots is the identity.
 */
struct TimeWarp
{
    std::vector<std::pair<double, double>> knots; // (from, to), increasing in both

    double map(double t) const
    {
        if (knots.empty())
            return t;
        if (t <= knots.front().first)
            return t + (knots.front().second - knots.front().first);
        if (t >= knots.back().first)
            return t + (knots.back().second - knots.back().first);
        for (size_t i = 1; i < knots.size(); ++i)
            if (t <= knots[i].first)
            {
                const auto [a0, b0] = knots[i - 1];
                const auto [a1, b1] = knots[i];
                const double span = a1 - a0;
                return span <= 0.0 ? b1 : b0 + (t - a0) * (b1 - b0) / span;
            }
        return t;
    }

    std::vector<double> apply(const std::vector<double>& times) const
    {
        std::vector<double> out;
        out.reserve(times.size());
        for (double t : times)
            out.push_back(map(t));
        return out;
    }
};

/** Seconds from key i to the next one; 0 for the last key. */
inline double gapAfter(const std::vector<double>& keys, size_t i) { return i + 1 < keys.size() ? keys[i + 1] - keys[i] : 0.0; }

/**
 * Sets the gap after key i to gap: that key's span stretches to fit and every later key moves by the difference.
 * Throws std::invalid_argument for the last key or a gap that isn't positive.
 */
inline TimeWarp retimeGap(const std::vector<double>& keys, size_t i, double gap)
{
    if (i + 1 >= keys.size())
        throw std::invalid_argument("the last key has no gap after it");
    if (!(gap > kTimeEps))
        throw std::invalid_argument("a gap must be longer than zero");
    return {{{keys[i], keys[i]}, {keys[i + 1], keys[i] + gap}}};
}

/**
 * Deleting key i with its timing: everything after it moves earlier by its gap, so the next key lands where it was.
 * Deleting the last key moves nothing.
 */
inline TimeWarp deleteWithTiming(const std::vector<double>& keys, size_t i)
{
    if (i >= keys.size())
        throw std::invalid_argument("no such key");
    if (i + 1 >= keys.size())
        return {};
    return {{{keys[i], keys[i]}, {keys[i + 1], keys[i]}}};
}

/** Opening room for span seconds after key i: every later key moves later by span. */
inline TimeWarp openAfter(const std::vector<double>& keys, size_t i, double span)
{
    if (i >= keys.size())
        throw std::invalid_argument("no such key");
    if (i + 1 >= keys.size())
        return {};
    return {{{keys[i], keys[i]}, {keys[i + 1], keys[i + 1] + span}}};
}

/** The gap a key made after key i takes: the gap before i, else the one after it, else fallback. */
inline double lastGap(const std::vector<double>& keys, size_t i, double fallback)
{
    if (i > 0 && i < keys.size())
        return keys[i] - keys[i - 1];
    if (keys.size() > 1)
        return keys[1] - keys[0];
    return fallback;
}

enum class QuickSelect
{
    All,
    FirstHalf,
    SecondHalf,
};

/**
 * Key indices for a quick select. The loop key (the last of count when looping) mirrors the first and is never
 * picked. An odd count gives the extra key to the second half.
 */
inline std::vector<size_t> quickSelect(size_t count, bool loop, QuickSelect which)
{
    const size_t n = loop && count > 0 ? count - 1 : count;
    const size_t half = n / 2;
    size_t from = 0, to = n;
    if (which == QuickSelect::FirstHalf)
        to = half;
    else if (which == QuickSelect::SecondHalf)
        from = half;
    std::vector<size_t> out;
    for (size_t i = from; i < to; ++i)
        out.push_back(i);
    return out;
}

/** One key copied by Cycle Step: the pose at from, mirrored, keyed at to. */
struct StepCopy
{
    double from = 0.0;
    double to = 0.0;
};

struct StepPlan
{
    std::vector<StepCopy> copies;
    /** Keys in the second half that the generated step replaces. */
    std::vector<double> replaced;
};

/**
 * Cycle Step: the keys in the first half of the cycle [start, end) are mirrored into the second half, half a cycle
 * later; keys already in the second half are replaced. end is the loop key, which is left alone.
 */
inline StepPlan cycleStepPlan(const std::vector<double>& keys, double start, double end)
{
    if (!(end - start > kTimeEps))
        throw std::invalid_argument("the cycle has no length");
    const double half = (end - start) * 0.5;
    StepPlan p;
    for (double t : keys)
    {
        if (t < start - kTimeEps || t > end - kTimeEps)
            continue;
        if (t < start + half - kTimeEps)
            p.copies.push_back({t, t + half});
        else
            p.replaced.push_back(t);
    }
    return p;
}

/**
 * Cycle Side: key k of the n keys before the loop key takes the opposite side's pose from key (k + n/2) mod n, half a
 * cycle away. Returns the source index for every destination index. Throws on an odd count, which has no half.
 */
inline std::vector<size_t> cycleSideMap(size_t n)
{
    if (n == 0)
        throw std::invalid_argument("no keys to mirror");
    if (n % 2 != 0)
        throw std::invalid_argument("Cycle Side needs an even number of keys (not counting the loop key); this cycle has " +
                                    std::to_string(n));
    std::vector<size_t> src(n);
    for (size_t k = 0; k < n; ++k)
        src[k] = (k + n / 2) % n;
    return src;
}

/** "Walk" -> "Walk_01", "Walk_01" -> "Walk_02": the first free two-digit suffix after the base name. */
inline std::string numberedName(const std::string& name, const std::vector<std::string>& taken)
{
    std::string base = name;
    if (const size_t u = name.rfind('_'); u != std::string::npos && u + 1 < name.size() &&
                                          std::all_of(name.begin() + long(u) + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
        base = name.substr(0, u);
    for (int i = 1; i < 10000; ++i)
    {
        std::string n = base + "_" + (i < 10 ? "0" : "") + std::to_string(i);
        if (std::find(taken.begin(), taken.end(), n) == taken.end())
            return n;
    }
    return base + "_copy";
}

/** A folder path with empty parts, stray slashes and surrounding spaces dropped: " a//b/ " -> "a/b". */
inline std::string normalFolder(const std::string& path)
{
    std::string out, part;
    auto flush = [&] {
        const size_t b = part.find_first_not_of(" \t");
        const size_t e = part.find_last_not_of(" \t");
        if (b != std::string::npos)
        {
            if (!out.empty())
                out += '/';
            out += part.substr(b, e - b + 1);
        }
        part.clear();
    };
    for (char c : path)
    {
        if (c == '/' || c == '\\')
            flush();
        else
            part += c;
    }
    flush();
    return out;
}

/** The folder and every folder above it: "a/b/c" -> "a", "a/b", "a/b/c". */
inline std::vector<std::string> folderChain(const std::string& path)
{
    std::vector<std::string> out;
    const std::string p = normalFolder(path);
    for (size_t at = 0; at <= p.size() && !p.empty();)
    {
        const size_t s = p.find('/', at);
        out.push_back(p.substr(0, s));
        if (s == std::string::npos)
            break;
        at = s + 1;
    }
    return out;
}

/** Key marks, for the Stacker's breakdown and extreme flags. */
enum class Mark
{
    None,
    Breakdown,
    Extreme,
    /** Keyed by generated motion between the animator's keys; refilling replaces it. */
    Generated,
};

inline const char* markName(Mark m)
{
    return m == Mark::Breakdown ? "breakdown" : m == Mark::Extreme ? "extreme" : m == Mark::Generated ? "generated" : "none";
}

inline Mark markFromName(const std::string& s)
{
    if (s == "breakdown")
        return Mark::Breakdown;
    if (s == "extreme")
        return Mark::Extreme;
    if (s == "generated")
        return Mark::Generated;
    if (s == "none" || s.empty())
        return Mark::None;
    throw std::invalid_argument("mark is breakdown, extreme, generated or none");
}

/** Marks by key time: retimed with the keys, dropped with them. */
struct Marks
{
    std::vector<std::pair<double, Mark>> at; // sorted by time

    Mark get(double t) const
    {
        for (const auto& [time, m] : at)
            if (sameTime(time, t))
                return m;
        return Mark::None;
    }

    void set(double t, Mark m)
    {
        std::erase_if(at, [&](const auto& e) { return sameTime(e.first, t); });
        if (m != Mark::None)
            at.push_back({t, m});
        std::sort(at.begin(), at.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    void warp(const TimeWarp& w)
    {
        for (auto& e : at)
            e.first = w.map(e.first);
    }

    void erase(double t)
    {
        std::erase_if(at, [&](const auto& e) { return sameTime(e.first, t); });
    }
};

/** One Stacker row. */
struct Row
{
    size_t index = 0;
    double time = 0.0;  // seconds
    double gap = 0.0;   // seconds to the next key; 0 on the last
    bool loop = false;  // the loop key, mirroring the first
    Mark mark = Mark::None;
};

/** The Stacker's rows: keys in order, the last flagged as the loop key when it is one. */
inline std::vector<Row> rows(const std::vector<double>& keys, double loopTime, const Marks& marks)
{
    std::vector<Row> out;
    for (size_t i = 0; i < keys.size(); ++i)
        out.push_back({i, keys[i], gapAfter(keys, i), i + 1 == keys.size() && !std::isnan(loopTime) && sameTime(keys[i], loopTime),
                       marks.get(keys[i])});
    return out;
}

/** Seconds from the first key to the last. */
inline double totalTime(const std::vector<double>& keys) { return keys.size() > 1 ? keys.back() - keys.front() : 0.0; }
} // namespace puppeteer::anim
