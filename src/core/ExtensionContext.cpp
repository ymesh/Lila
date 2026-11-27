#include "core/ExtensionContext.h"

#include "ops/CommandOptions.h"

#include <charconv>
#include <map>

namespace lila::ext
{
std::optional<SdkVersion> SdkVersion::parse(std::string_view text)
{
    auto number = [](std::string_view s, int& out) {
        if (s.empty())
            return false;
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc() && end == s.data() + s.size() && out >= 0;
    };
    SdkVersion v;
    size_t dot = text.find('.');
    if (!number(text.substr(0, dot), v.majorVersion))
        return std::nullopt;
    if (dot != std::string_view::npos && !number(text.substr(dot + 1), v.minorVersion))
        return std::nullopt;
    return v;
}

std::string SdkVersion::str() const
{
    return std::to_string(majorVersion) + "." + std::to_string(minorVersion);
}

SdkVersion hostSdkVersion()
{
    return kSdkVersion;
}

std::string sdkMismatch(SdkVersion extension, SdkVersion host)
{
    if (extension.majorVersion != host.majorVersion)
        return "built for SDK " + extension.str() + ", but this Lila provides SDK " + host.str() + " (the major version must match)";
    if (extension.minorVersion > host.minorVersion)
        return "built for SDK " + extension.str() + ", newer than this Lila's SDK " + host.str();
    return {};
}

struct Context::Impl
{
    struct Entry
    {
        std::string what;
        std::function<void()> remove;
    };

    std::string owner;
    std::filesystem::path root;
    std::map<Handle, Entry> entries; // handle order is registration order
    Handle next = 1;
};

Context::Context(std::string owner, std::filesystem::path root) : mImpl(std::make_unique<Impl>())
{
    mImpl->owner = std::move(owner);
    mImpl->root = std::move(root);
}

Context::~Context() = default;

const std::string& Context::owner() const
{
    return mImpl->owner;
}

const std::filesystem::path& Context::root() const
{
    return mImpl->root;
}

Context::Handle Context::track(std::string what, std::function<void()> remove)
{
    Handle h = mImpl->next++;
    mImpl->entries.emplace(h, Impl::Entry{std::move(what), std::move(remove)});
    return h;
}

bool Context::remove(Handle handle)
{
    auto it = mImpl->entries.find(handle);
    if (it == mImpl->entries.end())
        return false;
    std::function<void()> fn = std::move(it->second.remove);
    mImpl->entries.erase(it);
    if (fn)
        fn();
    return true;
}

void Context::unloadAll()
{
    while (!mImpl->entries.empty())
    {
        auto last = std::prev(mImpl->entries.end());
        std::string what = last->second.what;
        std::function<void()> fn = std::move(last->second.remove);
        mImpl->entries.erase(last);
        try
        {
            if (fn)
                fn();
        }
        catch (const std::exception& e)
        {
            log::error("extension {}: removing {} failed: {}", mImpl->owner, what, e.what());
        }
    }
}

std::vector<std::string> Context::registrations() const
{
    std::vector<std::string> out;
    for (auto& [h, e] : mImpl->entries)
        out.push_back(e.what);
    return out;
}

size_t Context::registrationCount() const
{
    return mImpl->entries.size();
}

Context::Handle Context::registerCommand(CommandDef def)
{
    if (const CommandDef* existing = ops::find(def.name); existing && existing->owner != owner())
    {
        log::error("extension {}: command '{}' already exists{}", owner(), def.name,
                   existing->owner.empty() ? std::string() : " (from extension " + existing->owner + ")");
        return 0;
    }
    def.owner = owner();
    std::string name = def.name;
    ops::registerCommand(std::move(def));
    std::string own = owner();
    return track("command:" + name, [name, own] {
        if (const CommandDef* d = ops::find(name); d && d->owner == own)
        {
            ops::unregisterCommand(name);
            // Its option-box override can hold the extension's own code (fills), which is about to go.
            ops::options::clearOverride(name);
        }
    });
}

Context::Handle Context::subscribe(events::Event event, events::Handler handler)
{
    events::SubscriptionId id = events::subscribe(event, std::move(handler), owner());
    if (!id)
        return 0;
    return track(std::string("event:") + events::name(event), [id] { events::unsubscribe(id); });
}

void Context::log(LogLevel level, std::string_view text) const
{
    log::write(level, "[" + owner() + "] " + std::string(text));
}
} // namespace lila::ext
