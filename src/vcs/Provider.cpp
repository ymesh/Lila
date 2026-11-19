#include "vcs/Provider.h"

namespace lila::vcs
{
const char* stateName(FileState s)
{
    switch (s)
    {
    case FileState::Unknown: return "unknown";
    case FileState::NotTracked: return "notTracked";
    case FileState::Synced: return "synced";
    case FileState::CheckedOut: return "checkedOut";
    case FileState::CheckedOutOther: return "checkedOutOther";
    case FileState::OutOfDate: return "outOfDate";
    case FileState::Locked: return "locked";
    case FileState::Added: return "added";
    case FileState::Deleted: return "deleted";
    }
    return "unknown";
}

json FileStatus::toJson() const
{
    json j = {
        {"path", path},
        {"state", stateName(state)},
        {"checkedOutByMe", checkedOutByMe},
        {"outOfDate", outOfDate},
        {"lockedByMe", lockedByMe},
        {"lockedByOther", lockedByOther},
        {"exclusive", exclusive},
        {"otherUsers", otherUsers},
    };
    auto opt = [&](const char* key, const std::string& v) {
        if (!v.empty())
            j[key] = v;
    };
    opt("depotPath", depotPath);
    opt("action", action);
    opt("changelist", changelist);
    opt("haveRev", haveRev);
    opt("headRev", headRev);
    opt("fileType", fileType);
    return j;
}

json OpResult::toJson() const
{
    json j = {{"files", files}, {"warnings", warnings}};
    if (!change.empty())
        j["change"] = change;
    return j;
}

json Changelist::toJson() const
{
    return {{"id", id}, {"description", description}, {"user", user}, {"shelved", shelved}, {"files", files}};
}

json Revision::toJson() const
{
    return {{"revision", revision}, {"change", change}, {"action", action},
            {"user", user},         {"time", time},     {"description", description}};
}

void Provider::fetchRevision(const fs::path&, const std::string&, const fs::path&)
{
    throw VcsError(name() + ": fetching old revisions is not supported");
}

std::string Provider::createChangelist(const std::string&, const fs::path&)
{
    throw VcsError(name() + " has no changelists");
}

OpResult Provider::moveToChangelist(const std::vector<fs::path>&, const std::string&)
{
    throw VcsError(name() + " has no changelists");
}

OpResult Provider::shelve(const std::string&, const std::vector<fs::path>&)
{
    throw VcsError(name() + " has no shelving");
}

OpResult Provider::unshelve(const std::string&)
{
    throw VcsError(name() + " has no shelving");
}
} // namespace lila::vcs
