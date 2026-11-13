#include "batch/BatchJob.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "io/FileTranslators.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace lila::batch
{
namespace
{
char fold(char c)
{
    if (c == '\\')
        return '/';
    return char(std::tolower(static_cast<unsigned char>(c)));
}

std::string folded(std::string_view s)
{
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(), fold);
    return out;
}

/** [set] at the front of p against c; advances p past it. False with p unchanged when there is no closing bracket. */
bool matchSet(std::string_view& p, char c, bool& matched)
{
    size_t i = 1;
    bool negate = i < p.size() && (p[i] == '!' || p[i] == '^');
    if (negate)
        ++i;
    size_t start = i;
    bool hit = false;
    for (; i < p.size() && (p[i] != ']' || i == start); ++i)
    {
        if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']')
        {
            hit |= c >= p[i] && c <= p[i + 2];
            i += 2;
        }
        else
            hit |= c == p[i];
    }
    if (i >= p.size())
        return false;
    p.remove_prefix(i + 1);
    matched = hit != negate;
    return true;
}

bool matchHere(std::string_view p, std::string_view t)
{
    while (!p.empty())
    {
        if (p.starts_with("**"))
        {
            p.remove_prefix(2);
            // "**/" also matches no folders at all.
            if (p.starts_with('/') && matchHere(p.substr(1), t))
                return true;
            for (size_t i = 0; i <= t.size(); ++i)
                if (matchHere(p, t.substr(i)))
                    return true;
            return false;
        }
        if (p[0] == '*')
        {
            p.remove_prefix(1);
            for (size_t i = 0;; ++i)
            {
                if (matchHere(p, t.substr(i)))
                    return true;
                if (i >= t.size() || t[i] == '/')
                    return false;
            }
        }
        if (t.empty())
            return false;
        if (p[0] == '[')
        {
            bool matched = false;
            if (t[0] != '/' && matchSet(p, t[0], matched))
            {
                if (!matched)
                    return false;
                t.remove_prefix(1);
                continue;
            }
            if (t[0] != '[')
                return false;
        }
        else if (p[0] == '?')
        {
            if (t[0] == '/')
                return false;
        }
        else if (p[0] != t[0])
            return false;
        p.remove_prefix(1);
        t.remove_prefix(1);
    }
    return t.empty();
}

bool hasWildcard(std::string_view s) { return s.find_first_of("*?[") != std::string_view::npos; }

fs::path resolve(const std::string& s, const Spec& spec)
{
    fs::path p = paths::fromUtf8(s);
    if (p.is_relative())
        p = (spec.workspace.empty() ? fs::current_path() : paths::fromUtf8(spec.workspace)) / p;
    return p.lexically_normal();
}

bool isLilaExtension(const std::string& ext) { return ext == ".lila" || ext == ".lilaa"; }

std::string key(const fs::path& p) { return folded(paths::toUtf8(fs::absolute(p).lexically_normal())); }

struct Filter
{
    const Spec& spec;
    std::set<std::string> extensions;

    explicit Filter(const Spec& s) : spec(s)
    {
        for (const std::string& e : s.extensions.empty() ? openableExtensions() : s.extensions)
            extensions.insert(normalizeExtension(e));
    }

    bool extensionOk(const fs::path& p) const { return extensions.contains(normalizeExtension(paths::toUtf8(p.extension()))); }

    bool keep(const fs::path& p, const fs::path& root) const
    {
        const std::string name = paths::toUtf8(p.filename());
        fs::path relPath = paths::relativeTo(p, root);
        const std::string rel = relPath.empty() ? name : paths::toUtf8(relPath);
        auto matches = [&](const std::string& pattern) {
            return globMatch(pattern, pattern.find_first_of("/\\") == std::string::npos ? name : rel);
        };
        if (!spec.include.empty() && std::none_of(spec.include.begin(), spec.include.end(), matches))
            return false;
        if (std::any_of(spec.exclude.begin(), spec.exclude.end(), matches))
            return false;
        const std::string stem = folded(paths::toUtf8(p.stem()));
        if (!spec.excludePrefix.empty() && stem.starts_with(folded(spec.excludePrefix)))
            return false;
        if (!spec.excludeSuffix.empty() && stem.ends_with(folded(spec.excludeSuffix)))
            return false;
        if (!spec.excludeContains.empty() && stem.find(folded(spec.excludeContains)) != std::string::npos)
            return false;
        return true;
    }
};

/** Files below dir up to depth (-1 = all), skipping dot folders such as a workspace's .lila history. Sorted. */
std::vector<fs::path> walk(const fs::path& dir, int depth, std::vector<std::string>* warnings)
{
    std::vector<fs::path> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        if (warnings)
            warnings->push_back("cannot read " + paths::toUtf8(dir) + ": " + ec.message());
        return out;
    }
    for (; it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
            break;
        const fs::directory_entry& e = *it;
        if (e.is_directory(ec))
        {
            if ((depth >= 0 && it.depth() >= depth) || paths::toUtf8(e.path().filename()).starts_with('.'))
                it.disable_recursion_pending();
            continue;
        }
        if (e.is_regular_file(ec))
            out.push_back(e.path().lexically_normal());
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

const char* saveModeName(SaveMode m)
{
    switch (m)
    {
    case SaveMode::Overwrite:
        return "overwrite";
    case SaveMode::SaveAs:
        return "saveAs";
    default:
        return "none";
    }
}

SaveMode saveModeFromName(std::string_view name)
{
    std::string n = folded(name);
    if (n == "none" || n.empty() || n == "false")
        return SaveMode::None;
    if (n == "overwrite" || n == "inplace" || n == "true")
        return SaveMode::Overwrite;
    if (n == "saveas")
        return SaveMode::SaveAs;
    throw std::invalid_argument("save must be none, overwrite or saveAs, not '" + std::string(name) + "'");
}

json toJson(const Spec& s)
{
    return {{"script", s.script},
            {"scriptFile", s.scriptFile},
            {"scriptLabel", s.scriptLabel},
            {"inputs", s.inputs},
            {"depth", s.depth},
            {"extensions", s.extensions},
            {"include", s.include},
            {"exclude", s.exclude},
            {"excludePrefix", s.excludePrefix},
            {"excludeSuffix", s.excludeSuffix},
            {"excludeContains", s.excludeContains},
            {"save", saveModeName(s.save)},
            {"outputDir", s.outputDir},
            {"suffix", s.suffix},
            {"format", s.format},
            {"keepTree", s.keepTree},
            {"checkout", s.checkout},
            {"makeWritable", s.makeWritable},
            {"workers", s.workers},
            {"stopOnError", s.stopOnError},
            {"timeout", s.timeout},
            {"dryRun", s.dryRun},
            {"args", s.args},
            {"csv", s.csv},
            {"workspace", s.workspace}};
}

Spec specFromJson(const json& j)
{
    Spec s;
    if (!j.is_object())
        return s;
    try
    {
        auto str = [&](const char* k, std::string& out) {
            if (j.contains(k) && j[k].is_string())
                out = j[k].get<std::string>();
        };
        auto list = [&](const char* k, std::vector<std::string>& out) {
            if (!j.contains(k))
                return;
            out.clear();
            if (j[k].is_string())
            {
                // "ma, mb" as typed into a field.
                std::string all = j[k].get<std::string>();
                size_t start = 0;
                while (start <= all.size())
                {
                    size_t end = all.find_first_of(",;", start);
                    std::string part = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    part.erase(0, part.find_first_not_of(" \t"));
                    part.erase(part.find_last_not_of(" \t") + 1);
                    if (!part.empty())
                        out.push_back(part);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
            else if (j[k].is_array())
                for (const json& v : j[k])
                    if (v.is_string())
                        out.push_back(v.get<std::string>());
        };
        str("script", s.script);
        str("scriptFile", s.scriptFile);
        str("scriptLabel", s.scriptLabel);
        list("inputs", s.inputs);
        s.depth = j.value("depth", s.depth);
        list("extensions", s.extensions);
        list("include", s.include);
        list("exclude", s.exclude);
        str("excludePrefix", s.excludePrefix);
        str("excludeSuffix", s.excludeSuffix);
        str("excludeContains", s.excludeContains);
        if (j.contains("save"))
            s.save = j["save"].is_boolean() ? (j["save"].get<bool>() ? SaveMode::Overwrite : SaveMode::None)
                                            : saveModeFromName(j["save"].get<std::string>());
        str("outputDir", s.outputDir);
        str("suffix", s.suffix);
        str("format", s.format);
        s.keepTree = j.value("keepTree", s.keepTree);
        str("checkout", s.checkout);
        s.makeWritable = j.value("makeWritable", s.makeWritable);
        s.workers = j.value("workers", s.workers);
        s.stopOnError = j.value("stopOnError", s.stopOnError);
        s.timeout = j.value("timeout", s.timeout);
        s.dryRun = j.value("dryRun", s.dryRun);
        if (j.contains("args") && j["args"].is_object())
            s.args = j["args"];
        str("csv", s.csv);
        str("workspace", s.workspace);
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad batch spec: ") + e.what());
    }
    if (!s.format.empty())
    {
        s.format = folded(s.format);
        if (s.format.starts_with('.'))
            s.format.erase(0, 1);
        if (s.format != "lila" && s.format != "lilaa")
            throw std::invalid_argument("format must be lila or lilaa, not '" + s.format + "'");
    }
    if (s.checkout != "auto" && s.checkout != "never")
        throw std::invalid_argument("checkout must be auto or never, not '" + s.checkout + "'");
    return s;
}

const char* accessName(Access a)
{
    switch (a)
    {
    case Access::New:
        return "new";
    case Access::Replace:
        return "replace";
    case Access::ReadOnly:
        return "read-only";
    case Access::Missing:
        return "missing";
    case Access::Collision:
        return "collision";
    default:
        return "";
    }
}

bool globMatch(std::string_view pattern, std::string_view text) { return matchHere(folded(pattern), folded(text)); }

std::string normalizeExtension(std::string_view ext)
{
    std::string e = folded(ext);
    e.erase(0, e.find_first_not_of(" \t*"));
    e.erase(e.find_last_not_of(" \t") + 1);
    if (!e.empty() && e[0] != '.')
        e.insert(e.begin(), '.');
    return e;
}

std::vector<std::string> openableExtensions()
{
    // Lila's own scenes always open, whatever translators are registered.
    std::vector<std::string> out = {".lila", ".lilaa"};
    for (const io::translators::FileTranslator* t : io::translators::supporting(io::translators::Op::Open))
        for (const std::string& e : t->extensions)
            if (std::find(out.begin(), out.end(), e) == out.end())
                out.push_back(e);
    return out;
}

std::vector<Item> gather(const Spec& s, std::vector<std::string>* warnings)
{
    LILA_ZONE();

    Filter filter(s);
    std::vector<Item> out;
    std::set<std::string> seen;
    auto add = [&](const fs::path& p, const fs::path& root) {
        if (!filter.keep(p, root) || !seen.insert(key(p)).second)
            return;
        out.push_back({.path = p, .root = root});
    };
    for (const std::string& raw : s.inputs)
    {
        std::string in = raw;
        in.erase(0, in.find_first_not_of(" \t\""));
        in.erase(in.find_last_not_of(" \t\"") + 1);
        if (in.empty())
            continue;
        std::error_code ec;
        if (hasWildcard(in))
        {
            std::string generic = in;
            std::replace(generic.begin(), generic.end(), '\\', '/');
            size_t wild = generic.find_first_of("*?[");
            size_t slash = generic.rfind('/', wild);
            fs::path base = resolve(slash == std::string::npos ? std::string(".") : generic.substr(0, slash + 1), s);
            std::string pattern = slash == std::string::npos ? generic : generic.substr(slash + 1);
            if (!base.has_filename())
                base = base.parent_path();
            int depth = pattern.find("**") != std::string::npos ? -1 : int(std::count(pattern.begin(), pattern.end(), '/'));
            if (!fs::is_directory(base, ec))
            {
                if (warnings)
                    warnings->push_back("no folder " + paths::toUtf8(base) + " for " + raw);
                continue;
            }
            size_t before = out.size();
            for (const fs::path& f : walk(base, depth, warnings))
                if (filter.extensionOk(f) && globMatch(pattern, paths::toUtf8(paths::relativeTo(f, base))))
                    add(f, base);
            if (out.size() == before && warnings)
                warnings->push_back("nothing matches " + raw);
            continue;
        }
        fs::path p = resolve(in, s);
        if (fs::is_directory(p, ec))
        {
            for (const fs::path& f : walk(p, s.depth, warnings))
                if (filter.extensionOk(f))
                    add(f, p);
            continue;
        }
        if (!fs::exists(p, ec) && warnings)
            warnings->push_back("not found: " + paths::toUtf8(p));
        add(p, p.parent_path());
    }
    return out;
}

bool isReadOnly(const fs::path& p)
{
    std::error_code ec;
    fs::file_status st = fs::status(p, ec);
    return !ec && (st.permissions() & fs::perms::owner_write) == fs::perms::none;
}

fs::path outputFor(const Spec& s, const Item& item)
{
    if (s.save == SaveMode::None)
        return {};
    const std::string ext = normalizeExtension(paths::toUtf8(item.path.extension()));
    if (s.save == SaveMode::Overwrite)
        return isLilaExtension(ext) ? item.path : fs::path(item.path).replace_extension(".lila");

    fs::path dir = item.path.parent_path();
    if (!s.outputDir.empty())
    {
        dir = resolve(s.outputDir, s);
        if (s.keepTree)
        {
            fs::path below = paths::relativeTo(item.path.parent_path(), item.root);
            if (!below.empty() && below != "." && !paths::toUtf8(below).starts_with(".."))
                dir /= below;
        }
    }
    const std::string newExt = !s.format.empty() ? "." + s.format : isLilaExtension(ext) ? ext : std::string(".lila");
    return (dir / paths::fromUtf8(paths::toUtf8(item.path.stem()) + s.suffix + newExt)).lexically_normal();
}

void plan(const Spec& s, std::vector<Item>& items)
{
    std::map<std::string, size_t> owners;
    for (Item& item : items)
    {
        std::error_code ec;
        item.output = outputFor(s, item);
        if (!fs::exists(item.path, ec))
            item.access = Access::Missing;
        else if (item.output.empty())
            item.access = Access::None;
        else if (!owners.emplace(key(item.output), &item - items.data()).second)
            item.access = Access::Collision;
        else if (!fs::exists(item.output, ec))
            item.access = Access::New;
        else
            item.access = isReadOnly(item.output) ? Access::ReadOnly : Access::Replace;
    }
}

const char* statusName(Status s)
{
    switch (s)
    {
    case Status::Pending:
        return "pending";
    case Status::Running:
        return "running";
    case Status::Done:
        return "done";
    case Status::Failed:
        return "failed";
    case Status::Skipped:
        return "skipped";
    case Status::Cancelled:
        return "cancelled";
    case Status::Planned:
        return "planned";
    }
    return "";
}

namespace
{
Status statusFromName(const std::string& n)
{
    for (Status s : {Status::Pending, Status::Running, Status::Done, Status::Failed, Status::Skipped, Status::Cancelled, Status::Planned})
        if (n == statusName(s))
            return s;
    return Status::Failed;
}

std::string csvField(const std::string& v)
{
    if (v.find_first_of(",\"\r\n") == std::string::npos)
        return v;
    std::string out = "\"";
    for (char c : v)
    {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + "\"";
}

std::string cell(const json& v)
{
    if (v.is_null())
        return {};
    if (v.is_string())
        return v.get<std::string>();
    return v.dump();
}
} // namespace

json toJson(const FileResult& r)
{
    return {{"status", statusName(r.status)}, {"seconds", r.seconds},     {"saved", r.saved},         {"message", r.message},
            {"errorFile", r.errorFile},      {"errorLine", r.errorLine}, {"traceback", r.traceback}, {"results", r.results},
            {"log", r.log}};
}

FileResult resultFromJson(const json& j)
{
    FileResult r;
    if (!j.is_object())
        return r;
    r.status = statusFromName(j.value("status", std::string("failed")));
    r.seconds = j.value("seconds", 0.0);
    r.saved = j.value("saved", false);
    r.message = j.value("message", std::string());
    r.errorFile = j.value("errorFile", std::string());
    r.errorLine = j.value("errorLine", 0);
    r.traceback = j.value("traceback", std::string());
    if (j.contains("results") && j["results"].is_object())
        r.results = j["results"];
    r.log = j.value("log", std::string());
    return r;
}

std::string toCsv(const std::vector<Item>& items, const std::vector<FileResult>& results)
{
    std::vector<std::string> keys;
    for (const FileResult& r : results)
        for (auto it = r.results.begin(); it != r.results.end(); ++it)
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
                keys.push_back(it.key());

    std::string out = "index,file,status,seconds,output,saved,message";
    for (const std::string& k : keys)
        out += "," + csvField(k);
    out += "\n";
    for (size_t i = 0; i < items.size(); ++i)
    {
        const FileResult r = i < results.size() ? results[i] : FileResult{};
        char secs[32];
        std::snprintf(secs, sizeof(secs), "%.2f", r.seconds);
        out += std::to_string(i) + "," + csvField(paths::toUtf8(items[i].path)) + "," + statusName(r.status) + "," + secs + "," +
               csvField(paths::toUtf8(items[i].output)) + "," + (r.saved ? "true" : "false") + "," + csvField(r.message);
        for (const std::string& k : keys)
            out += "," + csvField(r.results.contains(k) ? cell(r.results[k]) : std::string());
        out += "\n";
    }
    return out;
}
} // namespace lila::batch
