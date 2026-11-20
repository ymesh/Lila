#include "tex/Sequence.h"

#include "core/Paths.h"
#include "scene/Anim.h"
#include "tex/Evaluate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace lila::tex::sequence
{
namespace
{
/** A frame file's name split round its frame number: prefix, digits or #s, suffix. digits is 0 when it has none. */
struct Pattern
{
    std::string prefix;
    std::string suffix;
    size_t digits = 0;
};

Pattern patternOf(const std::string& name)
{
    size_t end = name.find_last_of('#');
    if (end != std::string::npos)
    {
        size_t start = end;
        while (start > 0 && name[start - 1] == '#')
            --start;
        return {name.substr(0, start), name.substr(end + 1), end - start + 1};
    }
    const size_t dot = name.find_last_of('.');
    end = dot == std::string::npos ? name.size() : dot;
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(name[end - 1])))
        --end;
    size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1])))
        --start;
    return {name.substr(0, start), name.substr(end), end - start};
}

struct Listing
{
    fs::file_time_type stamp;
    std::vector<std::string> files;
};

double number(const json& v, double fallback) { return v.is_number() ? v.get<double>() : fallback; }

/** A driven parameter's value at a time: its curve's when keyed, else the attribute's own. */
double driven(scene::Graph& scene, const scene::Node& set, const std::string& attr, double seconds, double fallback)
{
    const int index = set.type().attrIndex(attr);
    if (index < 0)
        return fallback;
    const scene::PlugRef plug{set.id(), index};
    if (const scene::Node* curve = scene.node(scene::anim::curveFor(scene, plug)))
        return scene::anim::curveValueAt(*curve, 0, seconds);
    const json v = scene::toJson(scene.value(plug));
    return v.is_boolean() ? double(v.get<bool>()) : number(v, fallback);
}

/** Sequence frames played between two scene frames at a keyed speed, summed a scene frame at a time. */
double playedBetween(scene::Graph& scene, const scene::Node& set, const std::string& attr, double from, double to)
{
    const double sign = to < from ? -1.0 : 1.0;
    const double lo = std::min(from, to), hi = std::max(from, to);
    auto speedAt = [&](double frame) { return driven(scene, set, attr, scene::anim::secondsOf(frame), 1.0); };
    double sum = 0.0;
    // Capped: further back than this the sum costs more than it shows.
    for (double f = std::max(lo, hi - 100000.0); f < hi; f += 1.0)
    {
        const double step = std::min(1.0, hi - f);
        sum += 0.5 * (speedAt(f) + speedAt(f + step)) * step;
    }
    return sign * sum;
}
} // namespace

std::vector<std::string> files(const std::string& path, const fs::path& root)
{
    if (path.empty() || path.starts_with("external:"))
        return {};
    const fs::path given = paths::fromUtf8(path);
    const Pattern pat = patternOf(paths::toUtf8(given.filename()));
    // A # pattern names no file on disk, so the folder is resolved rather than the file.
    const fs::path dir = given.has_parent_path() ? resolveImagePath(paths::toUtf8(given.parent_path()), root)
                                                 : resolveImagePath(path, root).parent_path();
    if (pat.digits == 0)
        return {paths::toUtf8(dir / given.filename())};

    static std::mutex mutex;
    static std::map<std::string, Listing> cache;
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(dir, ec);
    const bool stamped = !ec;
    const std::string key = paths::toUtf8(dir / given.filename());
    std::scoped_lock lock(mutex);
    if (auto it = cache.find(key); it != cache.end() && stamped && it->second.stamp == stamp)
        return it->second.files;

    std::vector<std::pair<long long, std::string>> found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = paths::toUtf8(it->path().filename());
        if (name.size() <= pat.prefix.size() + pat.suffix.size() || !name.starts_with(pat.prefix) || !name.ends_with(pat.suffix))
            continue;
        const std::string digits = name.substr(pat.prefix.size(), name.size() - pat.prefix.size() - pat.suffix.size());
        if (digits.size() > 18 || !std::ranges::all_of(digits, [](unsigned char c) { return std::isdigit(c) != 0; }))
            continue;
        found.emplace_back(std::stoll(digits), paths::toUtf8(it->path()));
    }
    std::sort(found.begin(), found.end());
    Listing l{stamp, {}};
    for (auto& [n, f] : found)
        l.files.push_back(std::move(f));
    cache[key] = l;
    return l.files;
}

Frames framesAt(double position, int count, std::string_view loop, bool blend)
{
    if (count <= 1)
        return {};
    auto wrap = [&](long long i) -> int {
        if (loop == "hold")
            return int(std::clamp<long long>(i, 0, count - 1));
        if (loop == "pingPong")
        {
            const long long period = 2LL * (count - 1);
            const long long m = ((i % period) + period) % period;
            return int(m < count ? m : period - m);
        }
        return int(((i % count) + count) % count);
    };
    const double base = std::floor(position);
    Frames f;
    f.first = wrap(static_cast<long long>(base));
    f.second = blend ? wrap(static_cast<long long>(base) + 1) : f.first;
    f.mix = f.second != f.first ? position - base : 0.0;
    return f;
}

std::string driver(const GNode& node, std::string_view param)
{
    auto d = node.values.find(kDriversKey);
    if (d == node.values.end() || !d->is_object())
        return {};
    auto it = d->find(std::string(param));
    return it != d->end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string driverName(std::string_view node, std::string_view param)
{
    std::string name = std::string(node) + "_" + std::string(param);
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0])))
        name.insert(0, 1, '_');
    return name;
}

bool animated(const Graph& g)
{
    for (const GNode& n : g.nodes)
    {
        if (n.type == kType)
            return true;
        if (auto d = n.values.find(kDriversKey); d != n.values.end() && d->is_object() && !d->empty())
            return true;
    }
    return false;
}

bool followTime(Graph& g, scene::Graph& scene, const scene::Node& set, double seconds, const fs::path& root)
{
    bool changed = false;
    auto put = [&](GNode& n, const std::string& id, json v) {
        if (!n.values.is_object())
            n.values = json::object();
        if (n.values.contains(id) && n.values[id] == v)
            return;
        n.values[id] = std::move(v);
        changed = true;
    };
    auto valueOf = [](const GNode& n, const char* id) { return n.values.is_object() && n.values.contains(id) ? n.values[id] : json(); };
    for (GNode& n : g.nodes)
    {
        std::map<std::string, std::string> drivers;
        if (const json d = valueOf(n, kDriversKey); d.is_object())
            for (const auto& [param, attr] : d.items())
                if (attr.is_string())
                    drivers[param] = attr.get<std::string>();
        for (const auto& [param, attr] : drivers)
        {
            const json was = valueOf(n, param.c_str());
            const double v = driven(scene, set, attr, seconds, number(was, 0.0));
            put(n, param, was.is_number_integer() ? json(std::llround(v)) : json(v));
        }
        if (n.type != kType)
            continue;

        const json file = valueOf(n, "file");
        const std::vector<std::string> all = files(file.is_string() ? file.get<std::string>() : std::string(), root);
        int count = int(all.size());
        if (const int limit = int(number(valueOf(n, "frames"), 0.0)); limit > 0)
            count = std::min(count, limit);
        if (count == 0)
        {
            put(n, "next", std::string());
            put(n, "mix", 0.0);
            continue;
        }
        const double frame = scene::anim::frameOf(seconds);
        const double start = number(valueOf(n, "start"), 1.0);
        const double played = drivers.contains("speed") ? playedBetween(scene, set, drivers["speed"], start, frame)
                                                        : number(valueOf(n, "speed"), 1.0) * (frame - start);
        const double position = number(valueOf(n, "offset"), 0.0) + played;
        const json loop = valueOf(n, "loop");
        const json interp = valueOf(n, "interpolation");
        // Rounded so a position a hair under a whole frame, from summing speeds, doesn't show the frame before.
        const Frames f = framesAt(std::round(position * 4096.0) / 4096.0, count, loop.is_string() ? loop.get<std::string>() : std::string("loop"),
                                  interp == "blend");
        put(n, "file", all[size_t(f.first)]);
        put(n, "next", f.mix > 0.0 ? all[size_t(f.second)] : std::string());
        put(n, "mix", f.mix);
    }
    return changed;
}
} // namespace lila::tex::sequence
