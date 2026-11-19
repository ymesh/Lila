#pragma once

#include "core/Export.h"

#include "vcs/Process.h"
#include "vcs/Provider.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace lila::vcs
{
struct AsyncResult
{
    bool ok = false;
    json value;
    std::string error;
};

enum class WriteAccess : uint8_t
{
    Writable,
    NeedsPrompt, // read-only, checkout allowed once the user agrees
    Denied,
};

/**
 * Picks the provider for a path and runs work off the main thread.
 *
 * Settings come from the "sourceControl" prefs file, so <workspace>/.lila/sourceControl.json overrides the user's:
 *   {"provider": "auto" | "perforce" | "git" | "svn" | "none",
 *    "perforce": {"port": "", "user": "", "client": "", "charset": "", "exe": ""},
 *    "git": {"exe": ""},
 *    "svn": {"exe": "", "shelfRoot": "^/shelves", "user": ""},
 *    "autoCheckout": "always" | "prompt" | "never",
 *    "autoAdd": true}
 * "auto" uses Perforce when `p4 where` maps the directory, else the nearest git work tree or svn working copy.
 */
class LILA_SCENE_API SourceControl
{
public:
    using Group = std::pair<std::shared_ptr<Provider>, std::vector<fs::path>>;

    static SourceControl& get();

    SourceControl();
    ~SourceControl();

    /** Detected once per directory and cached until refresh(). Null when nothing manages the path. */
    std::shared_ptr<Provider> providerFor(const fs::path& path);
    /** Provider for whole-workspace work (changelists, sync): context, else the workspace root, else the cwd. */
    std::shared_ptr<Provider> providerForContext(const fs::path& context = {});
    fs::path defaultContext() const;

    /** Files grouped by provider; unmanaged files land in a group with a null provider. */
    std::vector<Group> group(const std::vector<fs::path>& files);

    /** Runs op once per provider and merges the results. Throws VcsError if any file is unmanaged. */
    OpResult forEach(const std::vector<fs::path>& files, const std::function<OpResult(Provider&, const std::vector<fs::path>&)>& op);

    /** Unmanaged files come back as notTracked. */
    std::vector<FileStatus> status(const std::vector<fs::path>& files);

    json settings() const;
    void refresh();

    /** Tests swap in a fake; the next detection uses it. */
    void setRunner(std::shared_ptr<ProcessRunner> runner);
    std::shared_ptr<ProcessRunner> runner() const;

    /** Runs work on the source control thread, then done on the main thread via mainq. */
    void async(std::function<json()> work, std::function<void(const AsyncResult&)> done = {});
    void statusAsync(std::vector<fs::path> files, std::function<void(std::vector<FileStatus>, std::string error)> done);

    /** Stops the worker after finishing queued work. */
    void shutdown();

    /**
     * Makes file writable for a save, checking it out per the autoCheckout pref. Missing files are writable.
     * approved = the user said yes to the prompt.
     */
    WriteAccess requestWrite(const fs::path& file, std::string* error, bool approved = false);

private:
    void workerLoop(std::stop_token stop);
    std::shared_ptr<Provider> detect(const fs::path& dir, const json& prefs);

    mutable std::mutex mMutex;
    std::shared_ptr<ProcessRunner> mRunner;
    std::map<std::string, std::shared_ptr<Provider>> mByDir;
    std::map<std::string, std::shared_ptr<Provider>> mGitByRoot;
    std::map<std::string, std::shared_ptr<Provider>> mSvnByRoot;
    std::shared_ptr<Provider> mPerforce;
    bool mPerforceUnreachable = false;
    int mPrefListener = 0;

    std::mutex mQueueMutex;
    std::condition_variable_any mQueueCv;
    std::deque<std::function<void()>> mJobs;
    std::jthread mWorker;
};

/** Save hook: true when file can be written now. With the "prompt" pref this fails with a question in error. */
LILA_SCENE_API bool ensureWritable(const fs::path& file, std::string* error);

/** After saving a file, opens it for add when it is new and under source control (pref autoAdd). Runs async. */
LILA_SCENE_API void markForAddIfNew(const fs::path& file);
} // namespace lila::vcs
