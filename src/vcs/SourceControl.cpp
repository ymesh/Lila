#include "vcs/SourceControl.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "vcs/GitProvider.h"
#include "vcs/PerforceProvider.h"
#include "vcs/SvnProvider.h"

#include <algorithm>
#include <cctype>

namespace lila::vcs
{
namespace
{
std::string dirKey(const fs::path& dir)
{
    std::string s = dir.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}


fs::path dirOf(const fs::path& path)
{
    std::error_code ec;
    fs::path p = fs::absolute(path, ec).lexically_normal();
    if (fs::is_directory(p, ec))
        return p;
    return p.parent_path();
}

bool isReadOnly(const fs::path& p)
{
    std::error_code ec;
    fs::file_status st = fs::status(p, ec);
    return !ec && fs::exists(st) && (st.permissions() & fs::perms::owner_write) == fs::perms::none;
}

std::string stringOr(const json& j, const char* key, const std::string& fallback)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}
} // namespace

SourceControl& SourceControl::get()
{
    static SourceControl sc;
    return sc;
}

SourceControl::SourceControl() : mRunner(systemRunner())
{
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "sourceControl")
            refresh();
    });
}

SourceControl::~SourceControl()
{
    shutdown();
    PrefStore::get().removeOnChanged(mPrefListener);
}

json SourceControl::settings() const
{
    json j = PrefStore::get().merged("sourceControl");
    return j.is_object() ? j : json::object();
}

void SourceControl::refresh()
{
    std::scoped_lock lock(mMutex);
    mByDir.clear();
    mGitByRoot.clear();
    mSvnByRoot.clear();
    mPerforce.reset();
    mPerforceUnreachable = false;
}

void SourceControl::setRunner(std::shared_ptr<ProcessRunner> runner)
{
    {
        std::scoped_lock lock(mMutex);
        mRunner = runner ? std::move(runner) : systemRunner();
    }
    refresh();
}

std::shared_ptr<ProcessRunner> SourceControl::runner() const
{
    std::scoped_lock lock(mMutex);
    return mRunner;
}

fs::path SourceControl::defaultContext() const
{
    if (Workspace::get().isOpen())
        return Workspace::get().root();
    std::error_code ec;
    return fs::current_path(ec);
}

std::shared_ptr<Provider> SourceControl::detect(const fs::path& dir, const json& prefs)
{
    LILA_ZONE();

    std::string mode = stringOr(prefs, "provider", "auto");
    if (mode == "none")
        return nullptr;
    std::shared_ptr<ProcessRunner> run = runner();

    if (mode == "auto" || mode == "perforce")
    {
        PerforceSettings ps = PerforceSettings::fromJson(prefs.value("perforce", json::object()));
        std::shared_ptr<PerforceProvider> p4;
        bool unreachable = false;
        {
            std::scoped_lock lock(mMutex);
            auto cur = std::dynamic_pointer_cast<PerforceProvider>(mPerforce);
            if (!cur || !(cur->settings() == ps))
            {
                cur = std::make_shared<PerforceProvider>(ps, run);
                mPerforce = cur;
                mPerforceUnreachable = false;
            }
            p4 = cur;
            unreachable = mPerforceUnreachable;
        }
        if (mode == "perforce")
            return p4;
        if (!unreachable)
        {
            if (p4->manages(dir))
                return p4;
            // Without a server every directory would pay the connect timeout again.
            std::string err = p4->lastError();
            if (err.find("can't reach") != std::string::npos || err.find("not installed") != std::string::npos)
            {
                std::scoped_lock lock(mMutex);
                mPerforceUnreachable = true;
            }
        }
    }

    std::optional<fs::path> gitRoot, svnRoot;
    GitSettings gs = GitSettings::fromJson(prefs.value("git", json::object()));
    SvnSettings ss = SvnSettings::fromJson(prefs.value("svn", json::object()));
    if (mode == "auto" || mode == "git")
        gitRoot = GitProvider::findRoot(dir, gs, *run);
    if (mode == "auto" || mode == "svn")
        svnRoot = SvnProvider::findRoot(dir);
    // One can sit inside the other; the nearer root owns the directory.
    if (gitRoot && svnRoot)
        (dirKey(*svnRoot).size() > dirKey(*gitRoot).size() ? gitRoot : svnRoot).reset();

    if (gitRoot)
    {
        auto git = std::make_shared<GitProvider>(*gitRoot, gs, run);
        std::scoped_lock lock(mMutex);
        auto& slot = mGitByRoot[dirKey(git->root())];
        if (!slot)
            slot = git;
        return slot;
    }
    if (svnRoot)
    {
        auto svn = std::make_shared<SvnProvider>(*svnRoot, ss, run);
        std::scoped_lock lock(mMutex);
        auto& slot = mSvnByRoot[dirKey(svn->root())];
        if (!slot)
            slot = svn;
        return slot;
    }
    return nullptr;
}

std::shared_ptr<Provider> SourceControl::providerFor(const fs::path& path)
{
    LILA_ZONE();

    if (isDepotSyntax(path.string()))
    {
        std::shared_ptr<Provider> p = providerForContext();
        return p && p->name() == "perforce" ? p : nullptr;
    }
    fs::path dir = dirOf(path);
    std::string key = dirKey(dir);
    {
        std::scoped_lock lock(mMutex);
        if (auto it = mByDir.find(key); it != mByDir.end())
            return it->second;
    }
    std::shared_ptr<Provider> p = detect(dir, settings());
    std::scoped_lock lock(mMutex);
    mByDir[key] = p;
    return p;
}

std::shared_ptr<Provider> SourceControl::providerForContext(const fs::path& context)
{
    fs::path dir = context.empty() ? defaultContext() : context;
    return dir.empty() ? nullptr : providerFor(dir);
}

std::vector<SourceControl::Group> SourceControl::group(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    std::vector<Group> groups;
    for (const fs::path& f : files)
    {
        std::shared_ptr<Provider> p = providerFor(f);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.first == p; });
        if (it == groups.end())
            it = groups.insert(groups.end(), Group{p, {}});
        it->second.push_back(f);
    }
    return groups;
}

OpResult SourceControl::forEach(const std::vector<fs::path>& files,
                                const std::function<OpResult(Provider&, const std::vector<fs::path>&)>& op)
{
    LILA_ZONE();

    std::vector<Group> groups = group(files);
    for (const Group& g : groups)
        if (!g.first)
            throw VcsError("not under source control: " + g.second.front().string());
    OpResult all;
    for (const Group& g : groups)
    {
        OpResult r = op(*g.first, g.second);
        all.files.insert(all.files.end(), r.files.begin(), r.files.end());
        all.warnings.insert(all.warnings.end(), r.warnings.begin(), r.warnings.end());
        if (!r.change.empty())
            all.change = r.change;
    }
    return all;
}

std::vector<FileStatus> SourceControl::status(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    std::vector<FileStatus> out(files.size());
    std::vector<std::shared_ptr<Provider>> owners;
    for (const fs::path& f : files)
        owners.push_back(providerFor(f));

    std::vector<bool> done(files.size(), false);
    for (size_t first = 0; first < files.size(); ++first)
    {
        if (done[first])
            continue;
        std::vector<size_t> idx;
        std::vector<fs::path> batch;
        for (size_t i = first; i < files.size(); ++i)
            if (!done[i] && owners[i] == owners[first])
            {
                idx.push_back(i);
                batch.push_back(files[i]);
                done[i] = true;
            }
        std::vector<FileStatus> part;
        if (owners[first])
            part = owners[first]->status(batch);
        for (size_t k = 0; k < idx.size(); ++k)
            out[idx[k]] = k < part.size() ? std::move(part[k]) : FileStatus{.path = batch[k].string(), .state = FileState::NotTracked};
    }
    return out;
}

void SourceControl::workerLoop(std::stop_token stop)
{
    LILA_THREAD_NAME("SourceControl");
    while (true)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(mQueueMutex);
            mQueueCv.wait(lock, stop, [&] { return !mJobs.empty(); });
            if (mJobs.empty())
                return;
            job = std::move(mJobs.front());
            mJobs.pop_front();
        }
        job();
    }
}

void SourceControl::async(std::function<json()> work, std::function<void(const AsyncResult&)> done)
{
    std::scoped_lock lock(mQueueMutex);
    if (!mWorker.joinable())
        mWorker = std::jthread([this](std::stop_token st) { workerLoop(st); });
    mJobs.push_back([work = std::move(work), done = std::move(done)] {
        LILA_ZONE_N("SourceControl::job");
        AsyncResult r;
        try
        {
            r.value = work();
            r.ok = true;
        }
        catch (const std::exception& e)
        {
            r.error = e.what();
        }
        if (done)
            mainq::post([done, r = std::move(r)] { done(r); });
        else if (!r.ok)
            log::warn("{}", r.error);
    });
    mQueueCv.notify_one();
}

void SourceControl::statusAsync(std::vector<fs::path> files, std::function<void(std::vector<FileStatus>, std::string)> done)
{
    auto result = std::make_shared<std::vector<FileStatus>>();
    async(
        [this, files = std::move(files), result] {
            *result = status(files);
            return json();
        },
        [result, done = std::move(done)](const AsyncResult& r) { done(r.ok ? std::move(*result) : std::vector<FileStatus>{}, r.error); });
}

void SourceControl::shutdown()
{
    std::jthread worker;
    {
        std::scoped_lock lock(mQueueMutex);
        worker = std::move(mWorker);
    }
    if (!worker.joinable())
        return;
    // The worker finishes what is queued before it sees the stop.
    worker.request_stop();
    worker.join();
}

WriteAccess SourceControl::requestWrite(const fs::path& file, std::string* error, bool approved)
{
    LILA_ZONE();

    auto fail = [&](WriteAccess a, std::string msg) {
        if (error)
            *error = std::move(msg);
        return a;
    };
    std::error_code ec;
    if (!fs::exists(file, ec) || !isReadOnly(file))
        return WriteAccess::Writable;

    std::string mode = stringOr(settings(), "autoCheckout", "prompt");
    std::shared_ptr<Provider> p = providerFor(file);
    if (!p)
        return fail(WriteAccess::Denied, file.string() + " is read-only and not under source control");
    if (mode == "never")
        return fail(WriteAccess::Denied, file.string() + " is read-only; check it out first");

    try
    {
        std::vector<FileStatus> st = p->status({file});
        if (!st.empty())
        {
            const FileStatus& s = st.front();
            if (s.state == FileState::NotTracked)
                return fail(WriteAccess::Denied, file.string() + " is read-only and not tracked by " + p->name());
            if (s.state == FileState::Locked || s.lockedByOther)
            {
                std::string who = s.otherUsers.empty() ? std::string("someone else") : s.otherUsers.front();
                return fail(WriteAccess::Denied, file.string() + " is locked by " + who);
            }
            if (mode == "prompt" && !approved)
            {
                std::string msg = file.string() + " is read-only under " + p->name() + ". Check it out?";
                if (!s.otherUsers.empty())
                    msg += " (also checked out by " + s.otherUsers.front() + ")";
                if (s.outOfDate)
                    msg += " (not the latest revision)";
                return fail(WriteAccess::NeedsPrompt, msg);
            }
        }

        OpResult r = p->checkout({file});
        if (isReadOnly(file) && p->name() == "git")
            fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
        if (isReadOnly(file))
        {
            std::string why = r.warnings.empty() ? std::string("checkout did not make it writable") : r.warnings.front();
            return fail(WriteAccess::Denied, file.string() + ": " + why);
        }
        log::info("checked out {}", file.string());
        return WriteAccess::Writable;
    }
    catch (const VcsError& e)
    {
        return fail(WriteAccess::Denied, e.what());
    }
}

bool ensureWritable(const fs::path& file, std::string* error)
{
    return SourceControl::get().requestWrite(file, error) == WriteAccess::Writable;
}

void markForAddIfNew(const fs::path& file)
{
    SourceControl& sc = SourceControl::get();
    json prefs = sc.settings();
    if (prefs.contains("autoAdd") && prefs["autoAdd"].is_boolean() && !prefs["autoAdd"].get<bool>())
        return;
    sc.async(
        [file] {
            std::shared_ptr<Provider> p = SourceControl::get().providerFor(file);
            if (!p)
                return json();
            std::vector<FileStatus> st = p->status({file});
            if (st.empty() || st.front().state != FileState::NotTracked)
                return json();
            OpResult r = p->add({file});
            return r.toJson();
        },
        [file](const AsyncResult& r) {
            if (!r.ok)
                log::debug("auto add {}: {}", file.string(), r.error);
            else if (r.value.is_object() && !r.value["files"].empty())
                log::info("marked {} for add", file.string());
        });
}
} // namespace lila::vcs
