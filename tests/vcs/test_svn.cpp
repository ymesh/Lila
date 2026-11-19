#include "vcs/SvnProvider.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>

using namespace lila::vcs;

namespace
{
/** Answers svn calls from a callback and remembers what was asked. */
class SvnFakeRunner final : public ProcessRunner
{
public:
    std::function<ProcessResult(const ProcessSpec&)> answer;
    std::vector<ProcessSpec> calls;

    ProcessResult run(const ProcessSpec& spec) override
    {
        std::scoped_lock lock(mMutex);
        calls.push_back(spec);
        return answer(spec);
    }

    /** argv is {exe, --non-interactive, subcommand, ...}. */
    static std::string command(const ProcessSpec& s) { return s.argv.size() > 2 ? s.argv[2] : std::string(); }
    static bool has(const ProcessSpec& s, const std::string& a) { return std::find(s.argv.begin(), s.argv.end(), a) != s.argv.end(); }

    std::vector<ProcessSpec> of(const std::string& cmd) const
    {
        std::vector<ProcessSpec> out;
        for (const ProcessSpec& s : calls)
            if (command(s) == cmd)
                out.push_back(s);
        return out;
    }

private:
    std::mutex mMutex;
};

ProcessResult svnOk(std::string out, int code = 0)
{
    return {.started = true, .exitCode = code, .out = std::move(out)};
}

/** A folder that looks like a working copy root (it has .svn), removed afterwards. */
struct FakeWc
{
    fs::path root;

    FakeWc()
    {
        std::random_device rd;
        root = fs::temp_directory_path() / ("lila_svn_test_" + std::to_string(rd()));
        fs::create_directories(root / ".svn");
        root = fs::canonical(root);
    }
    ~FakeWc()
    {
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(root, ec))
            fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove_all(root, ec);
    }
    fs::path write(const std::string& name, const std::string& text = "x")
    {
        fs::path p = root / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

// Shapes from svn 1.14 --xml output.
const char* kLocalStatus = R"(<?xml version="1.0" encoding="UTF-8"?>
<status>
<target path="Hero.uasset">
<entry path="Hero.uasset">
<wc-status item="normal" revision="7" props="none">
<commit revision="5"><author>bob</author><date>2026-11-01T10:00:00.000000Z</date></commit>
</wc-status>
</entry>
</target>
<target path="Mine.uasset">
<entry path="Mine.uasset">
<wc-status item="normal" revision="7" props="none">
<lock><token>opaquelocktoken:mine</token><owner>me</owner><created>2026-11-19T00:00:00.000000Z</created></lock>
</wc-status>
</entry>
</target>
<target path="dir with space/b @2.txt">
<entry path="dir with space\b @2.txt">
<wc-status item="modified" revision="7" props="none"></wc-status>
</entry>
</target>
<target path="New.txt">
<entry path="New.txt">
<wc-status item="unversioned" props="none"></wc-status>
</entry>
</target>
<target path="Old.txt">
<entry path="Old.txt">
<wc-status item="normal" revision="7" props="none"></wc-status>
</entry>
</target>
<changelist name="art &amp; fx">
<entry path="Fx.txt">
<wc-status item="added" revision="-1" props="none"></wc-status>
</entry>
</changelist>
</status>
)";

const char* kRemoteStatus = R"(<?xml version="1.0" encoding="UTF-8"?>
<status>
<target path="Hero.uasset">
<entry path="Hero.uasset">
<wc-status item="normal" revision="7" props="none"></wc-status>
<repos-status item="none" props="none">
<lock><token>opaquelocktoken:theirs</token><owner>alice</owner><created>2026-11-19T00:00:00.000000Z</created></lock>
</repos-status>
</entry>
</target>
<target path="Mine.uasset">
<entry path="Mine.uasset">
<wc-status item="normal" revision="7" props="none">
<lock><token>opaquelocktoken:mine</token><owner>me</owner></lock>
</wc-status>
<repos-status item="none" props="none">
<lock><token>opaquelocktoken:mine</token><owner>me</owner></lock>
</repos-status>
</entry>
</target>
<target path="Old.txt">
<entry path="Old.txt">
<wc-status item="normal" revision="7" props="none"></wc-status>
<repos-status item="modified" props="none"></repos-status>
</entry>
<against revision="9"/>
</target>
</status>
)";

const char* kNeedsLock = R"(<?xml version="1.0" encoding="UTF-8"?>
<properties>
<target path="Hero.uasset"><property name="svn:needs-lock">*</property></target>
<target path="Mine.uasset"><property name="svn:needs-lock">*</property></target>
</properties>
)";

std::string TempReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

FileStatus find(const std::vector<FileStatus>& all, const fs::path& p)
{
    for (const FileStatus& s : all)
        if (s.path == p.string())
            return s;
    FAIL("no status for " << p.string());
    return {};
}
} // namespace

TEST_CASE("svn: find the working copy root")
{
    FakeWc wc;
    fs::create_directories(wc.root / "a" / "b");
    auto found = SvnProvider::findRoot(wc.root / "a" / "b" / "not_yet.txt");
    REQUIRE(found);
    CHECK(fs::equivalent(*found, wc.root));
    SvnProvider svn(wc.root, {}, std::make_shared<SvnFakeRunner>());
    CHECK(svn.manages(wc.root / "a"));
    CHECK_FALSE(svn.manages(wc.root.parent_path()));
}

TEST_CASE("svn: status reads locks, needs-lock, out of date and unversioned files")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec& s) {
        std::string cmd = SvnFakeRunner::command(s);
        if (cmd == "status")
            return SvnFakeRunner::has(s, "-u") ? svnOk(kRemoteStatus) : svnOk(kLocalStatus, 1);
        if (cmd == "propget")
            return svnOk(kNeedsLock, 1);
        return svnOk("");
    };
    SvnProvider svn(wc.root, {}, runner);
    fs::path hero = wc.root / "Hero.uasset", mine = wc.root / "Mine.uasset", spaced = wc.root / "dir with space" / "b @2.txt";
    fs::path fresh = wc.root / "New.txt", old = wc.root / "Old.txt";
    std::vector<FileStatus> st = svn.status({hero, mine, spaced, fresh, old});
    REQUIRE(st.size() == 5);

    FileStatus h = find(st, hero);
    CHECK(h.state == FileState::Locked);
    CHECK(h.lockedByOther);
    CHECK(h.exclusive);
    REQUIRE(h.otherUsers.size() == 1);
    CHECK(h.otherUsers[0] == "alice");

    FileStatus m = find(st, mine);
    CHECK(m.state == FileState::CheckedOut);
    CHECK(m.lockedByMe);
    CHECK(m.checkedOutByMe);

    CHECK(find(st, spaced).state == FileState::CheckedOut);
    CHECK(find(st, fresh).state == FileState::NotTracked);
    CHECK(find(st, old).state == FileState::OutOfDate);

    // Targets carry a trailing @ so the @ in "b @2.txt" is not read as a revision.
    auto statusCalls = runner->of("status");
    REQUIRE(statusCalls.size() == 2);
    CHECK(SvnFakeRunner::has(statusCalls[0], "dir with space/b @2.txt@"));
    // Unversioned files never go to the repository.
    CHECK_FALSE(SvnFakeRunner::has(statusCalls[1], "New.txt@"));
}

TEST_CASE("svn: an unreachable repository falls back to local status for a while")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec& s) {
        std::string cmd = SvnFakeRunner::command(s);
        if (cmd == "status" && SvnFakeRunner::has(s, "-u"))
            return ProcessResult{.started = true, .exitCode = 1, .err = "svn: E170013: Unable to connect to a repository"};
        if (cmd == "status")
            return svnOk(kLocalStatus);
        return svnOk("", 1);
    };
    SvnProvider svn(wc.root, {}, runner);
    fs::path old = wc.root / "Old.txt";
    CHECK(svn.status({old}).at(0).state == FileState::Synced);
    CHECK(svn.status({old}).at(0).state == FileState::Synced);
    int remote = 0;
    for (const ProcessSpec& s : runner->of("status"))
        remote += SvnFakeRunner::has(s, "-u");
    CHECK(remote == 1);
}

TEST_CASE("svn: changelists split default from named")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec& s) { return SvnFakeRunner::command(s) == "status" ? svnOk(kLocalStatus) : svnOk(""); };
    SvnProvider svn(wc.root, {}, runner);
    std::vector<Changelist> lists = svn.changelists({});
    REQUIRE(lists.size() == 2);
    CHECK(lists[0].id == "default");
    // The modified file and the file this working copy holds a lock on; nothing unversioned or untouched.
    CHECK(lists[0].files.size() == 2);
    CHECK(lists[1].id == "art & fx");
    REQUIRE(lists[1].files.size() == 1);
    CHECK(fs::path(lists[1].files[0]) == wc.root / "Fx.txt");
}

TEST_CASE("svn: checkout locks needs-lock files and leaves the rest alone")
{
    FakeWc wc;
    fs::path hero = wc.write("Hero.uasset");
    fs::path plain = wc.write("Plain.txt");
    fs::permissions(hero, fs::perms::all ^ fs::perms::owner_read, fs::perm_options::remove);
    fs::permissions(plain, fs::perms::all ^ fs::perms::owner_read, fs::perm_options::remove);
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec& s) {
        std::string cmd = SvnFakeRunner::command(s);
        if (cmd == "propget")
            return svnOk(kNeedsLock, 1);
        if (cmd == "lock")
            return ProcessResult{.started = true, .exitCode = 1, .err = "svn: warning: W160035: Path '/Hero.uasset' is already locked by user 'alice'"};
        return svnOk("");
    };
    SvnProvider svn(wc.root, {}, runner);
    OpResult r = svn.checkout({hero, plain});
    REQUIRE(runner->of("lock").size() == 1);
    CHECK(SvnFakeRunner::has(runner->of("lock")[0], "Hero.uasset@"));
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("alice") != std::string::npos);
    REQUIRE(r.files.size() == 1);
    CHECK(r.files[0] == plain.string());
    CHECK((fs::status(plain).permissions() & fs::perms::owner_write) != fs::perms::none);
}

TEST_CASE("svn: submit reports the revision and keeps named changelists out of default")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    std::string targets;
    runner->answer = [&](const ProcessSpec& s) {
        std::string cmd = SvnFakeRunner::command(s);
        if (cmd == "status")
            return svnOk(kLocalStatus);
        if (cmd == "commit")
        {
            auto it = std::find(s.argv.begin(), s.argv.end(), "--targets");
            REQUIRE(it != s.argv.end());
            std::ifstream f(*(it + 1), std::ios::binary);
            targets.assign(std::istreambuf_iterator<char>(f), {});
            return svnOk("Sending        Mine.uasset\nTransmitting file data .done\nCommitting transaction...\nCommitted revision 10.\n");
        }
        return svnOk("");
    };
    SvnProvider svn(wc.root, {}, runner);
    CHECK_THROWS_AS(svn.submit("default", "  "), VcsError);
    OpResult r = svn.submit("default", "Hero pass");
    CHECK(r.change == "10");
    CHECK(r.files.size() == 2);
    CHECK(targets.find("Mine.uasset@") != std::string::npos);
    CHECK(targets.find("Fx.txt") == std::string::npos);
    CHECK(SvnFakeRunner::has(runner->of("commit")[0], "--depth"));

    svn.submit("art & fx", "Fx");
    ProcessSpec named = runner->of("commit")[1];
    CHECK(SvnFakeRunner::has(named, "--changelist"));
    CHECK(SvnFakeRunner::has(named, "art & fx"));
}

TEST_CASE("svn: sync reports the revision and conflicts")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec&) {
        return svnOk("Updating '.':\nU    Hero.uasset\nC    Level.umap\nUpdated to revision 12.\nSummary of conflicts:\n  Text conflicts: 1\n");
    };
    SvnProvider svn(wc.root, {}, runner);
    OpResult r = svn.sync({}, {});
    CHECK(r.change == "12");
    REQUIRE(r.files.size() == 1);
    CHECK(fs::path(r.files[0]) == wc.root / "Hero.uasset");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("Level.umap") != std::string::npos);
}

TEST_CASE("svn: history parses log entries")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec&) {
        return svnOk(R"(<?xml version="1.0" encoding="UTF-8"?>
<log>
<logentry revision="9"><author>alice</author><date>2026-11-19T01:02:03.123456Z</date><msg>Fix &lt;hero&gt;
second line</msg></logentry>
<logentry revision="5"><author>bob</author><date>1970-01-02T00:00:00.000000Z</date><msg></msg></logentry>
</log>
)");
    };
    SvnProvider svn(wc.root, {}, runner);
    std::vector<Revision> h = svn.history(wc.root / "Hero.uasset");
    REQUIRE(h.size() == 2);
    CHECK(h[0].revision == "9");
    CHECK(h[0].user == "alice");
    CHECK(h[0].time == 1795050123);
    CHECK(h[0].description == "Fix <hero>\nsecond line");
    CHECK(h[1].time == 86400);
}

TEST_CASE("svn: a missing client names the fix")
{
    FakeWc wc;
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [](const ProcessSpec&) { return ProcessResult{}; };
    SvnProvider svn(wc.root, {}, runner);
    try
    {
        svn.history(wc.root / "a.txt");
        FAIL("expected VcsError");
    }
    catch (const VcsError& e)
    {
        CHECK(std::string(e.what()).find("command line client tools") != std::string::npos);
    }
}

TEST_CASE("svn: real client round trip")
{
    if (findExecutable("svn").empty() || findExecutable("svnadmin").empty())
    {
        MESSAGE("svn and svnadmin not on PATH; skipped");
        return;
    }
    std::random_device rd;
    fs::path base = fs::temp_directory_path() / ("lila_svn_real_" + std::to_string(rd()));
    fs::create_directories(base);
    base = fs::canonical(base);
    struct Cleanup
    {
        fs::path p;
        ~Cleanup()
        {
            std::error_code ec;
            for (auto& e : fs::recursive_directory_iterator(p, ec))
                fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
            fs::remove_all(p, ec);
        }
    } cleanup{base};

    std::shared_ptr<ProcessRunner> run = systemRunner();
    REQUIRE(run->run({.argv = {findExecutable("svnadmin").string(), "create", (base / "repo").string()}}).exitCode == 0);
    std::string url = "file:///" + (base / "repo").generic_string();
    REQUIRE(run->run({.argv = {findExecutable("svn").string(), "checkout", "-q", url, (base / "wc").string()}}).exitCode == 0);

    SvnProvider svn(base / "wc", {}, run);
    fs::path a = base / "wc" / "sub" / "a @1.txt";
    fs::create_directories(a.parent_path());
    std::ofstream(a, std::ios::binary) << "one\n";
    CHECK(svn.status({a}).at(0).state == FileState::NotTracked);
    svn.add({a});
    CHECK(svn.status({a}).at(0).state == FileState::Added);
    OpResult c = svn.submit("default", "first", {a});
    CHECK(c.change == "1");
    CHECK(svn.status({a}).at(0).state == FileState::Synced);

    std::ofstream(a, std::ios::binary | std::ios::trunc) << "two\n";
    CHECK(svn.status({a}).at(0).state == FileState::CheckedOut);
    svn.moveToChangelist({a}, "wip");
    CHECK(svn.changelists({}).size() == 2);
    svn.revert({a});
    CHECK(svn.status({a}).at(0).state == FileState::Synced);

    fs::path old = base / "old.txt";
    svn.fetchRevision(a, "1", old);
    std::ifstream f(old, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(f), {}) == "one\n");
    CHECK(svn.history(a).size() == 1);

    if (findExecutable("svnmucc").empty())
    {
        MESSAGE("svnmucc not on PATH; shelving skipped");
        return;
    }
    std::ofstream(a, std::ios::binary | std::ios::trunc) << "shelved\n";
    OpResult shelved = svn.shelve("wip", {a});
    CHECK_FALSE(shelved.change.empty());
    svn.revert({a});
    svn.unshelve("wip");
    std::ifstream back(a, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(back), {}) == "shelved\n");
    CHECK(svn.status({a}).at(0).changelist == "wip");
}

namespace
{
std::string svnStatusXml(const std::vector<std::pair<std::string, std::string>>& items, const std::string& named = {},
                         const std::vector<std::pair<std::string, std::string>>& namedItems = {})
{
    auto entries = [](const std::vector<std::pair<std::string, std::string>>& v) {
        std::string out;
        for (const auto& [path, item] : v)
        {
            std::string props = item == "props" ? "modified" : "none";
            out += "<entry path=\"" + path + "\"><wc-status item=\"" + (item == "props" ? "normal" : item) +
                   "\" revision=\"7\" props=\"" + props + "\"></wc-status></entry>\n";
        }
        return out;
    };
    std::string xml = "<?xml version=\"1.0\"?>\n<status>\n<target path=\".\">\n" + entries(items) + "</target>\n";
    if (!named.empty())
        xml += "<changelist name=\"" + named + "\">\n" + entries(namedItems) + "</changelist>\n";
    return xml + "</status>\n";
}

/** Runs a default submit against the given status and returns the commit's argv and --targets lines. */
std::pair<std::vector<std::string>, std::string> svnDefaultSubmit(FakeWc& wc, const std::string& status)
{
    auto runner = std::make_shared<SvnFakeRunner>();
    std::vector<std::string> argv;
    std::string targets;
    runner->answer = [&](const ProcessSpec& s) {
        if (SvnFakeRunner::command(s) == "commit")
        {
            argv = s.argv;
            auto it = std::find(s.argv.begin(), s.argv.end(), "--targets");
            std::ifstream f(*(it + 1), std::ios::binary);
            targets.assign(std::istreambuf_iterator<char>(f), {});
            return svnOk("Committed revision 11.\n");
        }
        return svnOk(status);
    };
    SvnProvider svn(wc.root, {}, runner);
    svn.submit("default", "d");
    return {argv, targets};
}
} // namespace

TEST_CASE("svn: default submit commits folder property edits beside named changelists")
{
    FakeWc wc;
    fs::create_directories(wc.root / "Art");
    auto [argv, targets] = svnDefaultSubmit(wc, svnStatusXml({{"Art", "props"}, {"Mine.uasset", "modified"}}, "wip", {{"Art/Fx.txt", "modified"}}));
    CHECK(std::find(argv.begin(), argv.end(), "--depth") != argv.end());
    CHECK(targets.find("Art@") != std::string::npos);
    CHECK(targets.find("Fx.txt") == std::string::npos);
}

TEST_CASE("svn: default submit commits a deleted folder recursively")
{
    FakeWc wc;
    auto [argv, targets] = svnDefaultSubmit(
        wc, svnStatusXml({{"Gone", "deleted"}, {"Gone/a.txt", "deleted"}, {"Mine.uasset", "modified"}}, "wip", {{"Fx.txt", "modified"}}));
    CHECK(std::find(argv.begin(), argv.end(), "--depth") == argv.end());
    CHECK(targets.find("Gone@") != std::string::npos);
    CHECK(targets.find("a.txt") == std::string::npos);
    CHECK(targets.find("Mine.uasset@") != std::string::npos);
    CHECK(targets.find("Fx.txt") == std::string::npos);
}

TEST_CASE("svn: default submit refuses a commit that would sweep in a named changelist")
{
    FakeWc wc;
    fs::create_directories(wc.root / "Art");
    CHECK_THROWS_AS(svnDefaultSubmit(wc, svnStatusXml({{"Gone", "deleted"}, {"Gone/a.txt", "deleted"}, {"Art", "props"}}, "wip",
                                                      {{"Art/Fx.txt", "modified"}})),
                    VcsError);
}

namespace
{
const char* kInfoRoot = R"(<?xml version="1.0" encoding="UTF-8"?>
<info><entry kind="dir" path="." revision="7"><url>https://svn.example.invalid/game/trunk</url>
<repository><root>https://svn.example.invalid/game</root><uuid>u</uuid></repository></entry></info>
)";

bool isMucc(const ProcessSpec& s)
{
    return s.argv.front().find("svnmucc") != std::string::npos;
}

std::vector<std::string> muccArgs(const ProcessSpec& s)
{
    auto it = std::find(s.argv.begin(), s.argv.end(), "-X");
    std::ifstream f(*(it + 1), std::ios::binary);
    std::vector<std::string> out;
    for (std::string l; std::getline(f, l);)
        out.push_back(l);
    return out;
}
} // namespace

TEST_CASE("svn: shelve writes the changed files and their actions in one commit")
{
    FakeWc wc;
    wc.write("Art/Hero @1.uasset", "hero v2");
    wc.write("New.txt", "new");
    auto runner = std::make_shared<SvnFakeRunner>();
    std::vector<std::string> mucc;
    runner->answer = [&](const ProcessSpec& s) {
        if (isMucc(s))
        {
            mucc = muccArgs(s);
            return svnOk("r12 committed by me at 2026-11-19T00:00:00.000000Z\n");
        }
        std::string cmd = SvnFakeRunner::command(s);
        if (cmd == "info" && SvnFakeRunner::has(s, "."))
            return svnOk(kInfoRoot);
        if (cmd == "info") // the shelves folder exists, this user's does not
            return svnOk("", SvnFakeRunner::has(s, "https://svn.example.invalid/game/shelves@") ? 0 : 1);
        if (cmd == "status")
            return svnOk(svnStatusXml({{"Art/Hero @1.uasset", "modified"}, {"New.txt", "added"}, {"Gone.txt", "deleted"}}));
        return svnOk("");
    };
    SvnSettings settings;
    settings.user = "me";
    SvnProvider svn(wc.root, settings, runner);
    OpResult r = svn.shelve("default", {wc.root / "Art" / "Hero @1.uasset", wc.root / "New.txt", wc.root / "Gone.txt"});
    CHECK(r.change == "12");
    CHECK(r.files.size() == 3);

    const std::string shelf = "https://svn.example.invalid/game/shelves/me/default";
    auto at = [&](const std::string& a) { return std::find(mucc.begin(), mucc.end(), a); };
    CHECK(at("https://svn.example.invalid/game/shelves") == mucc.end());
    CHECK(at("https://svn.example.invalid/game/shelves/me") != mucc.end());
    CHECK(at(shelf + "/Art") != mucc.end());
    CHECK(at(shelf + "/Art/Hero%20%401.uasset") != mucc.end());
    CHECK(at(shelf + "/New.txt") != mucc.end());
    CHECK(at(shelf + "/Gone.txt") == mucc.end());
    auto prop = at("lila:shelf");
    REQUIRE(prop != mucc.end());
    json meta = json::parse(*(prop + 1));
    REQUIRE(meta["files"].size() == 3);
    CHECK(meta["files"][0]["path"] == "Art/Hero @1.uasset");
    CHECK(meta["files"][0]["action"] == "edit");
    CHECK(meta["files"][2]["action"] == "delete");
}

TEST_CASE("svn: unshelve writes files, adds, deletes and skips local changes")
{
    FakeWc wc;
    wc.write("Hero.uasset", "hero v1");
    wc.write("Busy.txt", "mine");
    wc.write("Gone.txt", "old");
    json meta = {{"name", "wip"},
                 {"files", json::array({{{"path", "Hero.uasset"}, {"action", "edit"}, {"rev", "5"}},
                                        {{"path", "Busy.txt"}, {"action", "edit"}, {"rev", "7"}},
                                        {{"path", "sub/New.txt"}, {"action", "add"}, {"rev", ""}},
                                        {{"path", "Gone.txt"}, {"action", "delete"}, {"rev", "7"}}})}};
    auto runner = std::make_shared<SvnFakeRunner>();
    runner->answer = [&](const ProcessSpec& s) {
        std::string cmd = SvnFakeRunner::command(s);
        if (cmd == "info")
            return svnOk(kInfoRoot);
        if (cmd == "propget" && SvnFakeRunner::has(s, "svn:needs-lock"))
            return svnOk("", 1);
        if (cmd == "propget")
            return svnOk(meta.dump() + "\n");
        if (cmd == "status")
            return svnOk(svnStatusXml({{"Hero.uasset", "normal"}, {"Busy.txt", "modified"}, {"Gone.txt", "normal"}}));
        if (cmd == "cat")
            return svnOk(s.argv.back().find("Hero") != std::string::npos ? "hero v2" : "new file");
        return svnOk("");
    };
    SvnSettings settings;
    settings.user = "me";
    SvnProvider svn(wc.root, settings, runner);
    OpResult r = svn.unshelve("alice/wip");
    CHECK(r.change == "wip");
    CHECK(TempReadAll(wc.root / "Hero.uasset") == "hero v2");
    CHECK(TempReadAll(wc.root / "sub" / "New.txt") == "new file");
    CHECK(TempReadAll(wc.root / "Busy.txt") == "mine");
    REQUIRE(r.warnings.size() == 2);
    CHECK(r.warnings[0].find("r5") != std::string::npos);
    CHECK(r.warnings[1].find("local changes") != std::string::npos);

    REQUIRE(runner->of("propget").size() >= 1);
    CHECK(SvnFakeRunner::has(runner->of("propget")[0], "https://svn.example.invalid/game/shelves/alice/wip@"));
    REQUIRE(runner->of("add").size() == 1);
    CHECK(SvnFakeRunner::has(runner->of("add")[0], "sub/New.txt@"));
    REQUIRE(runner->of("delete").size() == 1);
    CHECK(SvnFakeRunner::has(runner->of("delete")[0], "Gone.txt@"));
    REQUIRE(runner->of("changelist").size() == 1);
    CHECK(SvnFakeRunner::has(runner->of("changelist")[0], "wip"));
}
