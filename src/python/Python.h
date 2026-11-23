#pragma once

#include "core/Export.h"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::python
{
/** Main thread. Leaves the GIL released; use Gil to run anything. */
LILA_SCENE_API bool init(const std::vector<std::string>& argv);
LILA_SCENE_API void shutdown();
LILA_SCENE_API bool initialized();

struct Result
{
    bool ok = false;
    std::string value; // repr of the last expression, if any
};

/** Runs like the script editor: a lone expression is evaluated and its repr returned. Output goes to the log. */
LILA_SCENE_API Result run(std::string_view code, std::string_view filename = "<script>");
LILA_SCENE_API Result runFile(const std::filesystem::path& path);
/** Evaluates one expression in __main__ and returns it as JSON text, or the error, without printing either. */
LILA_SCENE_API Result evalJson(std::string_view expression);

/**
 * Takes the pending Python exception (GIL held): its traceback goes to the log and its message comes back, as
 * str(exception), or the type name when that is empty. Empty when no exception is pending.
 */
LILA_SCENE_API std::string takeError();

/** The UI installs these so lila.ui can reach its action registry; headless they stay empty. */
struct UiHooks
{
    std::function<bool(const std::string&)> runAction;
    std::function<std::vector<std::array<std::string, 3>>()> listActions;
};
LILA_SCENE_API UiHooks& uiHooks();

/** RAII GIL acquire, usable from any thread. */
class LILA_SCENE_API Gil
{
public:
    Gil();
    ~Gil();
    Gil(const Gil&) = delete;
    Gil& operator=(const Gil&) = delete;

private:
    int mState;
};
} // namespace lila::python
