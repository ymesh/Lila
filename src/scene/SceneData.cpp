#include "scene/SceneData.h"

#include "core/Registry.h"
#include "ops/Command.h"
#include "ops/Undo.h"

namespace lila::scene::data
{
namespace
{
json& store()
{
    static json s = json::object();
    return s;
}

/** Swaps one key's value, so undo and redo are the same operation. */
struct SwapAction final : UndoAction
{
    std::string key;
    json other;
    SwapAction(std::string k, json before) : key(std::move(k)), other(std::move(before)) {}
    void undo() override
    {
        json current = get(key);
        if (other.is_null())
            store().erase(key);
        else
            store()[key] = other;
        other = std::move(current);
    }
    void redo() override { undo(); }
    size_t bytes() const override { return sizeof(*this) + key.capacity() + other.dump().size(); }
};

struct LoadedCallback
{
    std::string owner;
    std::function<void()> fn;
};

std::vector<LoadedCallback>& loadedCallbacks()
{
    // Never destroyed: extensions register these, and their code is unloaded before statics are torn down at exit.
    static auto& c = *new std::vector<LoadedCallback>();
    // An unloading extension's callbacks go with it: its code is about to leave the process.
    static const uint64_t sweep = owners::track([](std::string_view owner) {
        return owner.empty() ? size_t(0) : std::erase_if(c, [&](const LoadedCallback& x) { return x.owner == owner; });
    });
    (void)sweep;
    return c;
}
} // namespace

const json& all()
{
    return store();
}

json get(const std::string& key, const json& fallback)
{
    auto it = store().find(key);
    return it == store().end() ? fallback : *it;
}

void set(const std::string& key, const json& value)
{
    json before = get(key);
    if (before == value)
        return;
    UndoStack::get().record(std::make_unique<SwapAction>(key, std::move(before)));
    if (value.is_null())
        store().erase(key);
    else
        store()[key] = value;
}

void clear()
{
    store() = json::object();
}

json toJson()
{
    return store();
}

void fromJson(const json& j)
{
    store() = j.is_object() ? j : json::object();
    for (const LoadedCallback& c : loadedCallbacks())
        c.fn();
}

void onLoaded(std::function<void()> fn)
{
    const std::string* owner = owners::attributedOwner();
    loadedCallbacks().push_back({owner ? *owner : std::string(), std::move(fn)});
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "sceneData",
        .doc = "Store, query or remove a JSON value saved in the scene file, for tools and scripts that keep data with a "
               "scene. With no key, returns every key and value. Prefix keys with the tool's name to avoid clashes. "
               "Workspace and user-wide values go in prefs instead (pref / set_pref in Python).",
        .positionalDoc = "key",
        .flags = {
            {"value", "v", T::Any, nullptr, "The value to store: any JSON (number, string, list, object)."},
            {"remove", "rm", T::Bool, false, "Delete the key."},
        },
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                return all();
            std::string key = a.positional[0].get<std::string>();
            if (key.empty())
                throw CommandError("sceneData: the key is empty");
            if (a.get<bool>("remove"))
                set(key, nullptr);
            else if (a.has("value"))
                set(key, a.get<json>("value"));
            return get(key);
        },
    });
}
} // namespace lila::scene::data
