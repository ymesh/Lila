#pragma once

/**
 * Lila C++ extension SDK: the entry points.
 *
 * An extension is a package folder <Id>.lilaext holding extension.json (id, version, sdk, entry library, ...) and the
 * library it names, which exports:
 *
 *     LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx);    // 0 = loaded
 *     LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context& ctx); // optional
 *
 * Register everything through ctx (or a registration function that takes it) so it is tagged with the extension's id
 * and removed when it unloads. Link Lila's import libraries and include only <lila/...> headers. Build with the same
 * compiler and runtime as Lila, since C++ types cross the boundary.
 *
 * Including this header also exports lilaExtensionSdkVersion(), the SDK version the extension was built against, which
 * Lila checks before calling init.
 */

#include "core/ExtensionContext.h"

#include <cstdint>

#ifdef _WIN32
#define LILA_EXTENSION_EXPORT extern "C" __declspec(dllexport)
#else
// used: GCC emits an inline function only where it is called, and the loader looks these up by name.
#define LILA_EXTENSION_EXPORT extern "C" __attribute__((visibility("default"), used))
#endif

using LilaExtensionInitFn = int (*)(lila::ext::Context&);
using LilaExtensionUninitFn = void (*)(lila::ext::Context&);
using LilaExtensionSdkVersionFn = uint32_t (*)();

#if !defined(LILA_FOUNDATION_EXPORTS) && !defined(LILA_SCENE_EXPORTS) && !defined(LILA_UI_EXPORTS)
LILA_EXTENSION_EXPORT inline uint32_t lilaExtensionSdkVersion()
{
    return lila::ext::SdkVersion::pack(LILA_SDK_VERSION_MAJOR, LILA_SDK_VERSION_MINOR);
}
#endif
