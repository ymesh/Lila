#pragma once

#include "core/Export.h"

#include <functional>
#include <thread>

namespace lila::mainq
{
/** Call once from the main thread at startup. */
LILA_FOUNDATION_API void init();
LILA_FOUNDATION_API bool isMainThread();

/** Queue work for the main thread. Safe from any thread. */
LILA_FOUNDATION_API void post(std::function<void()> fn);
/** The same, named in crash reports while it runs. label must outlive the call: a string literal. */
LILA_FOUNDATION_API void post(const char* label, std::function<void()> fn);

/** Run everything queued so far. Main thread only. Does nothing while held. */
LILA_FOUNDATION_API void drain();

/** Holds queued work back (drain does nothing) while long work pumps the event loop; nests. */
class LILA_FOUNDATION_API Hold
{
public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

/** Called (on the posting thread) whenever post() queues work, so the host event loop can schedule a drain. */
LILA_FOUNDATION_API void setWakeup(std::function<void()> wake);
} // namespace lila::mainq
