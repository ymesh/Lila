#pragma once

#include "core/Export.h"
#include "core/UninitVector.h"

#include <filesystem>

namespace lila
{
/** The whole file, read in slices on several threads at once. Throws std::runtime_error when it can't be read. */
LILA_FOUNDATION_API UninitVector<char> readFileBytes(const std::filesystem::path& path);

/** Writes bytes as the whole file, in slices on several threads at once. Throws std::runtime_error when it can't. */
LILA_FOUNDATION_API void writeFileBytes(const std::filesystem::path& path, std::string_view bytes);

/**
 * std::filesystem::rename that rides out a file or folder briefly held open by another process. On Windows a virus
 * scanner or the search indexer opens freshly written files, and renaming them (or a folder holding them) fails with
 * access denied until it lets go, so this retries for up to a couple of seconds. Returns the last error, empty on success.
 */
LILA_FOUNDATION_API std::error_code renameRetrying(const std::filesystem::path& from, const std::filesystem::path& to);

/** Frees a big buffer on the releaser thread (core/Release.h): gigabytes take long enough to free to be noticed. */
template <class T> void freeInBackground(T&& big);
} // namespace lila

#include "core/Release.h"

template <class T> void lila::freeInBackground(T&& big)
{
    release::later(std::forward<T>(big));
}
