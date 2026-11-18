#pragma once

#include "core/Export.h"

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila
{
enum class LogLevel : uint8_t
{
    Debug,
    Info,
    Warning,
    Error,
    Output, // Python stdout
    Echo,   // command echo, as Python
};

struct LogEntry
{
    uint64_t seq = 0;
    LogLevel level = LogLevel::Info;
    std::string text;
};

/** Thread-safe. Keeps the last few thousand entries so late listeners (MCP, script editor) can catch up. */
namespace log
{
using Sink = std::function<void(const LogEntry&)>;

LILA_FOUNDATION_API void write(LogLevel level, std::string_view text);

/** Sinks are called on the writing thread. Returns an id for removeSink. */
LILA_FOUNDATION_API int addSink(Sink sink);
LILA_FOUNDATION_API void removeSink(int id);

/** Entries with seq > since, oldest first. */
LILA_FOUNDATION_API std::vector<LogEntry> since(uint64_t since, size_t maxCount = 1000);

LILA_FOUNDATION_API const char* levelName(LogLevel level);

/**
 * The lowest of Debug, Info, Warning and Error printed to the console; sinks and since() still get every entry. Starts
 * from the LILA_LOG_LEVEL environment variable (debug, info, warning or error), Debug when unset.
 */
LILA_FOUNDATION_API LogLevel consoleLevel();
LILA_FOUNDATION_API void setConsoleLevel(LogLevel level);
/** The level a name such as "warning" or "error" stands for. */
LILA_FOUNDATION_API bool levelFromName(std::string_view name, LogLevel& out);

template <class... Args> void debug(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Debug, std::format(f, std::forward<Args>(a)...)); }
template <class... Args> void info(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Info, std::format(f, std::forward<Args>(a)...)); }
template <class... Args> void warn(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Warning, std::format(f, std::forward<Args>(a)...)); }
template <class... Args> void error(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Error, std::format(f, std::forward<Args>(a)...)); }
} // namespace log
} // namespace lila
