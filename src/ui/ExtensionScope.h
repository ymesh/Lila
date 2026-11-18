#pragma once

#include "core/Export.h"

#include <string_view>

class QObject;

namespace lila::ext
{
class Context;
}

namespace lila::ui
{
/**
 * A QObject that lives while ctx's extension is loaded and is deleted as it unloads. Give it as the context of
 * QTimer::singleShot, QObject::connect and QMetaObject::invokeMethod from extension code: whatever is still queued
 * when the extension unloads is dropped instead of calling into its unloaded DLL. The same object on every call.
 */
LILA_UI_API QObject* scope(ext::Context& ctx);
/** The scope of the loaded extension owner made by scope(ctx); nullptr, logged, when there is none. */
LILA_UI_API QObject* scope(std::string_view owner);
} // namespace lila::ui
