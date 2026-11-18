#include "ui/ExtensionScope.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"

#include <QObject>

#include <map>
#include <string>

namespace lila::ui
{
namespace
{
std::map<std::string, QObject*, std::less<>>& scopes()
{
    static std::map<std::string, QObject*, std::less<>> s;
    return s;
}
} // namespace

QObject* scope(ext::Context& ctx)
{
    if (auto it = scopes().find(ctx.owner()); it != scopes().end())
        return it->second;
    auto* o = new QObject;
    o->setObjectName(QString::fromStdString("scope:" + ctx.owner()));
    scopes()[ctx.owner()] = o;
    ctx.track("scope:" + ctx.owner(), [owner = ctx.owner()] {
        if (auto it = scopes().find(owner); it != scopes().end())
        {
            delete it->second;
            scopes().erase(it);
        }
    });
    return o;
}

QObject* scope(std::string_view owner)
{
    if (auto it = scopes().find(owner); it != scopes().end())
        return it->second;
    log::warn("ui::scope: {} has no scope; call ui::scope(ctx) while it loads", owner);
    return nullptr;
}
} // namespace lila::ui
