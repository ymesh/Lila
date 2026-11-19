#pragma once

#include "core/Export.h"

#include "vcs/Process.h"
#include "vcs/Provider.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace lila::vcs
{
struct LILA_SCENE_API SvnSettings
{
    std::string exe; // empty = svn on PATH, then the TortoiseSVN and SlikSVN install locations
    /** Repository folder holding everyone's shelves, one subfolder per user. ^/ is the repository root. */
    std::string shelfRoot = "^/shelves";
    std::string user; // shelf folder name; empty = the OS user name

    /** The "svn" object of sourceControl.json. */
    static SvnSettings fromJson(const json& j);
    json toJson() const;
    bool operator==(const SvnSettings&) const = default;
};

/**
 * One Subversion working copy. Checkout takes an `svn lock` on svn:needs-lock files and is a no-op otherwise.
 * Changelists are svn's own named changelists; "default" is every change outside one. Status asks the repository
 * for out-of-date files and others' locks, and goes local-only for a minute when it can't be reached.
 *
 * A shelf is one svnmucc commit to <shelfRoot>/<user>/<changelist>: the changed files, plus a lila:shelf property
 * listing each file's action and base revision. Shelving keeps the local changes; unshelve writes the shelf into the
 * working copy and its changelist. Anyone can unshelve "user/name".
 */
class LILA_SCENE_API SvnProvider final : public Provider
{
public:
    SvnProvider(fs::path root, SvnSettings settings, std::shared_ptr<ProcessRunner> runner);

    /** Top of the working copy containing dir (the folder holding .svn), if any. Spawns nothing. */
    static std::optional<fs::path> findRoot(const fs::path& dir);

    std::string name() const override { return "svn"; }
    json info() override;
    bool manages(const fs::path& dir) override;

    std::vector<FileStatus> status(const std::vector<fs::path>& files) override;
    OpResult checkout(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult add(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult revert(const std::vector<fs::path>& files) override;
    OpResult remove(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult move(const fs::path& from, const fs::path& to, const std::string& changelist = {}) override;
    OpResult submit(const std::string& changelist, const std::string& description,
                    const std::vector<fs::path>& files = {}) override;
    OpResult sync(const std::vector<fs::path>& files, const fs::path& context) override;
    std::vector<Changelist> changelists(const fs::path& context) override;
    std::vector<Revision> history(const fs::path& file, int maxCount = 50) override;
    void fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest) override;
    OpResult lock(const std::vector<fs::path>& files) override;
    OpResult unlock(const std::vector<fs::path>& files) override;

    /** svn changelists exist only while they hold files, so this returns the name for a later move. */
    std::string createChangelist(const std::string& description, const fs::path& context) override;
    OpResult moveToChangelist(const std::vector<fs::path>& files, const std::string& changelist) override;
    /** changelist "default" or empty shelves the default changelist; files narrows it. Replaces a shelf of that name. */
    OpResult shelve(const std::string& changelist, const std::vector<fs::path>& files) override;
    /** "name" is one of mine, "user/name" anyone's. Files with local changes are skipped with a warning. */
    OpResult unshelve(const std::string& changelist) override;
    bool supportsChangelists() const override { return true; }
    bool supportsShelving() const override { return true; }

    const fs::path& root() const { return mRoot; }
    const SvnSettings& settings() const { return mSettings; }

    /** Runs svn --non-interactive in the root. Throws VcsError on a non-zero exit unless exitCode is given. */
    std::string run(const std::vector<std::string>& args, int* exitCode = nullptr, int timeoutMs = 0);
    /** Runs svnmucc with args read from a file, so long shelves fit on the command line. */
    std::string runMucc(const std::string& message, const std::vector<std::string>& args);

    /** URL of shelf "name" or "user/name". */
    std::string shelfUrl(const std::string& shelf);
    std::string shelfUser() const;

private:
    struct Entry
    {
        std::string path;  // as svn printed it
        std::string item;  // wc-status item: normal, modified, added, unversioned...
        std::string props; // wc-status props
        std::string revision;
        std::string changelist;
        std::string lockToken; // set when this working copy holds the lock
        std::string reposItem; // repos-status item, with remote status only
        std::string reposProps;
        std::string reposLockOwner;
        std::string reposLockToken;
        bool remote = false;
    };

    std::string rel(const fs::path& file) const;
    fs::path abs(const std::string& rel) const;
    std::vector<std::string> targets(const std::vector<fs::path>& files) const;
    /** Entries keyed by key(rel path). remote adds -u when the repository answered recently. */
    std::map<std::string, Entry> statusEntries(const std::vector<std::string>& rels, bool verbose, bool remote);
    /** Commit targets for the default changelist while named ones exist; adds --depth empty to args when it can. */
    std::vector<std::string> defaultCommitTargets(const std::vector<std::string>& files, const std::vector<std::string>& namedKeys,
                                                  std::vector<std::string>& args);
    std::map<std::string, bool> needsLock(const std::vector<std::string>& rels);
    OpResult each(const char* verb, const std::vector<fs::path>& files);

    fs::path mRoot;
    std::string mRootKey;
    SvnSettings mSettings;
    std::shared_ptr<ProcessRunner> mRunner;

    /** The repository root URL, read once. */
    std::string repositoryRoot();
    std::vector<Changelist> shelves();
    /** Changelists in this working copy only, without shelves or any repository call. */
    std::vector<Changelist> localChangelists();

    std::mutex mMutex;
    std::string mRepositoryRoot;
    std::chrono::steady_clock::time_point mOfflineUntil{};
};
} // namespace lila::vcs
