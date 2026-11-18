#include "core/Log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace lila::log
{
namespace
{
constexpr size_t kMaxEntries = 5000;

struct State
{
    std::mutex mutex;
    std::deque<LogEntry> entries;
    std::vector<std::pair<int, Sink>> sinks;
    uint64_t nextSeq = 1;
    int nextSinkId = 1;
};

State& state()
{
    static State s;
    return s;
}

std::atomic<LogLevel>& consoleLevelRef()
{
    static std::atomic<LogLevel> level = [] {
        LogLevel l = LogLevel::Debug;
        if (const char* env = std::getenv("LILA_LOG_LEVEL"))
            levelFromName(env, l);
        return l;
    }();
    return level;
}
} // namespace

LogLevel consoleLevel()
{
    return consoleLevelRef().load(std::memory_order_relaxed);
}

void setConsoleLevel(LogLevel level)
{
    consoleLevelRef().store(level, std::memory_order_relaxed);
}

bool levelFromName(std::string_view name, LogLevel& out)
{
    for (LogLevel l : {LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error})
        if (name == levelName(l) || (l == LogLevel::Warning && name == "warn"))
        {
            out = l;
            return true;
        }
    return false;
}

void write(LogLevel level, std::string_view text)
{
    State& s = state();
    LogEntry entry;
    std::vector<std::pair<int, Sink>> sinks;
    {
        std::scoped_lock lock(s.mutex);
        entry = {s.nextSeq++, level, std::string(text)};
        s.entries.push_back(entry);
        if (s.entries.size() > kMaxEntries)
            s.entries.pop_front();
        sinks = s.sinks;
    }

    if (level != LogLevel::Output && level != LogLevel::Echo)
    {
        if (level >= consoleLevel())
            std::fprintf(level >= LogLevel::Warning ? stderr : stdout, "[%s] %s\n", levelName(level), entry.text.c_str());
    }
    else
        std::fprintf(stdout, "%s", entry.text.c_str());
    std::fflush(level >= LogLevel::Warning && level <= LogLevel::Error ? stderr : stdout);

    for (auto& [id, sink] : sinks)
        sink(entry);
}

int addSink(Sink sink)
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    s.sinks.emplace_back(s.nextSinkId, std::move(sink));
    return s.nextSinkId++;
}

void removeSink(int id)
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    std::erase_if(s.sinks, [id](auto& p) { return p.first == id; });
}

std::vector<LogEntry> since(uint64_t since, size_t maxCount)
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    std::vector<LogEntry> out;
    for (const LogEntry& e : s.entries)
        if (e.seq > since)
            out.push_back(e);
    if (out.size() > maxCount)
        out.erase(out.begin(), out.end() - maxCount);
    return out;
}

const char* levelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    case LogLevel::Output: return "output";
    case LogLevel::Echo: return "echo";
    }
    return "?";
}
} // namespace lila::log
