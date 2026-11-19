#include "vcs/VcsCommands.h"

#include "ops/Command.h"
#include "core/Workspace.h"
#include "vcs/SourceControl.h"

namespace lila::vcs
{
namespace
{
SourceControl& sc() { return SourceControl::get(); }

fs::path resolve(const std::string& s)
{
    fs::path p(s);
    if (s.size() > 2 && s[0] == '/' && s[1] == '/' && s.find('\\') == std::string::npos)
        return p; // depot syntax
    if (p.is_relative())
        p = (Workspace::get().isOpen() ? Workspace::get().root() : fs::current_path()) / p;
    return p.lexically_normal();
}

/** Positional file arguments. The scene selection is not a fallback here: these are files, not nodes. */
std::vector<fs::path> filesArg(const Args& a, const char* cmd, bool required)
{
    std::vector<fs::path> out;
    for (const json& v : a.positional)
    {
        if (v.is_string())
            out.push_back(resolve(v.get<std::string>()));
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(resolve(x.get<std::string>()));
    }
    if (required && out.empty())
        throw CommandError(std::string(cmd) + ": give one or more files");
    return out;
}

fs::path contextArg(const Args& a)
{
    return a.has("path") ? resolve(a.get<std::string>("path")) : sc().defaultContext();
}

std::shared_ptr<Provider> contextProvider(const Args& a, const std::vector<fs::path>& files)
{
    std::shared_ptr<Provider> p = files.empty() ? sc().providerForContext(contextArg(a)) : sc().providerFor(files.front());
    if (!p)
        throw CommandError("not under source control: " + (files.empty() ? contextArg(a) : files.front()).string());
    return p;
}

/** Turns provider failures into command errors. */
template <class F> json guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const VcsError& e)
    {
        throw CommandError(e.what());
    }
}

FlagDef pathFlag()
{
    return {"path", "", ArgType::String, nullptr, "Directory that picks the provider when no files are given. Default: the workspace root."};
}

FlagDef changelistFlag()
{
    return {"changelist", "", ArgType::String, nullptr, "Perforce changelist number or svn changelist name; default changelist when omitted."};
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "scStatus",
        .doc = "Source control state of files: [{path, state, checkedOutByMe, outOfDate, lockedByOther, otherUsers, ...}]. "
               "state is notTracked | synced | checkedOut | checkedOutOther | outOfDate | locked | added | deleted.",
        .positionalDoc = "files",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scStatus", true);
            return guarded([&] {
                json out = json::array();
                std::vector<FileStatus> st = sc().status(files);
                for (size_t i = 0; i < st.size(); ++i)
                {
                    json j = st[i].toJson();
                    std::shared_ptr<Provider> p = sc().providerFor(files[i]);
                    j["provider"] = p ? p->name() : "none";
                    out.push_back(std::move(j));
                }
                return out;
            });
        },
    });

    ops::registerCommand({
        .name = "scCheckout",
        .doc = "Check files out for edit. Perforce: p4 edit. Git: git lfs lock for LFS files. svn: svn lock for svn:needs-lock files. "
               "Otherwise nothing to do. "
               "Returns {files, warnings}.",
        .positionalDoc = "files",
        .flags = {changelistFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scCheckout", true);
            std::string cl = a.get<std::string>("changelist", "");
            return guarded([&] { return sc().forEach(files, [&](Provider& p, auto& f) { return p.checkout(f, cl); }).toJson(); });
        },
    });

    ops::registerCommand({
        .name = "scAdd",
        .doc = "Mark new files for add. Returns {files, warnings}.",
        .positionalDoc = "files",
        .flags = {changelistFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scAdd", true);
            std::string cl = a.get<std::string>("changelist", "");
            return guarded([&] { return sc().forEach(files, [&](Provider& p, auto& f) { return p.add(f, cl); }).toJson(); });
        },
    });

    ops::registerCommand({
        .name = "scRevert",
        .doc = "Discard local changes and release checkouts. Git: restores from HEAD, unstages new files.",
        .positionalDoc = "files",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scRevert", true);
            return guarded([&] { return sc().forEach(files, [&](Provider& p, auto& f) { return p.revert(f); }).toJson(); });
        },
    });

    ops::registerCommand({
        .name = "scDelete",
        .doc = "Mark files for delete (removes the local copies).",
        .positionalDoc = "files",
        .flags = {changelistFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scDelete", true);
            std::string cl = a.get<std::string>("changelist", "");
            return guarded([&] { return sc().forEach(files, [&](Provider& p, auto& f) { return p.remove(f, cl); }).toJson(); });
        },
    });

    ops::registerCommand({
        .name = "scSubmit",
        .doc = "Submit a changelist (git: commit; never pushes). Files limit it to those files. Returns {change, files, warnings}.",
        .positionalDoc = "files (optional)",
        .flags = {
            {"description", "", T::String, nullptr, "Change description / commit message."},
            changelistFlag(),
            pathFlag(),
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scSubmit", false);
            return guarded([&] {
                return contextProvider(a, files)
                    ->submit(a.get<std::string>("changelist", ""), a.get<std::string>("description", ""), files)
                    .toJson();
            });
        },
    });

    ops::registerCommand({
        .name = "scSync",
        .doc = "Get latest. No files = the whole workspace (git: pull --ff-only).",
        .positionalDoc = "files (optional)",
        .flags = {pathFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scSync", false);
            if (files.empty())
                return guarded([&] { return contextProvider(a, files)->sync({}, contextArg(a)).toJson(); });
            return guarded([&] {
                return sc().forEach(files, [&](Provider& p, auto& f) { return p.sync(f, contextArg(a)); }).toJson();
            });
        },
    });

    ops::registerCommand({
        .name = "scChangelists",
        .doc = "List pending changelists with their files, or create one, move files into one, shelve or unshelve.",
        .positionalDoc = "files (for move or shelve)",
        .flags = {
            {"create", "", T::String, nullptr, "Create a changelist with this description; returns its number (svn: its name)."},
            {"move", "", T::String, nullptr, "Move the given files into this changelist."},
            {"shelve", "", T::String, nullptr, "Shelve this changelist (Perforce; svn: to <shelfRoot>/<user>/<name> in the repository), optionally only the given files."},
            {"unshelve", "", T::String, nullptr, "Unshelve this changelist into itself (Perforce; svn: a shelf name, or user/name for someone else's)."},
            pathFlag(),
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scChangelists", false);
            return guarded([&]() -> json {
                std::shared_ptr<Provider> p = contextProvider(a, files);
                if (a.has("create"))
                    return p->createChangelist(a.get<std::string>("create"), contextArg(a));
                if (a.has("move"))
                {
                    if (files.empty())
                        throw CommandError("scChangelists: move needs files");
                    return p->moveToChangelist(files, a.get<std::string>("move")).toJson();
                }
                if (a.has("shelve"))
                    return p->shelve(a.get<std::string>("shelve"), files).toJson();
                if (a.has("unshelve"))
                    return p->unshelve(a.get<std::string>("unshelve")).toJson();
                json out = json::array();
                for (const Changelist& c : p->changelists(contextArg(a)))
                    out.push_back(c.toJson());
                return out;
            });
        },
    });

    ops::registerCommand({
        .name = "scHistory",
        .doc = "Revisions of a file, newest first: [{revision, change, action, user, time, description}].",
        .positionalDoc = "file",
        .flags = {{"maxCount", "", T::Int, 50, "Most revisions to return.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scHistory", true);
            return guarded([&] {
                json out = json::array();
                for (const Revision& r : contextProvider(a, files)->history(files.front(), a.get<int>("maxCount", 50)))
                    out.push_back(r.toJson());
                return out;
            });
        },
    });

    ops::registerCommand({
        .name = "scLock",
        .doc = "Lock files so nobody else can submit them (git: git lfs lock, svn: svn lock). unlock releases.",
        .positionalDoc = "files",
        .flags = {{"unlock", "", T::Bool, false, "Release the locks instead."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<fs::path> files = filesArg(a, "scLock", true);
            bool un = a.get<bool>("unlock", false);
            return guarded([&] {
                return sc().forEach(files, [&](Provider& p, auto& f) { return un ? p.unlock(f) : p.lock(f); }).toJson();
            });
        },
    });

    ops::registerCommand({
        .name = "scProvider",
        .doc = "Which provider manages a path (default: the workspace root) and its settings: {provider, settings, info}.",
        .positionalDoc = "path (optional)",
        .flags = {{"refresh", "", T::Bool, false, "Forget cached detection first, e.g. after changing P4CLIENT."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("refresh", false))
                sc().refresh();
            std::vector<fs::path> files = filesArg(a, "scProvider", false);
            fs::path where = files.empty() ? sc().defaultContext() : files.front();
            return guarded([&] {
                std::shared_ptr<Provider> p = sc().providerFor(where);
                json out = {{"path", where.string()}, {"provider", p ? p->name() : "none"}, {"settings", sc().settings()}};
                if (p)
                    out["info"] = p->info();
                return out;
            });
        },
    });
}
} // namespace lila::vcs
