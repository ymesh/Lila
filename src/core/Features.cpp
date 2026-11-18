#include "core/Features.h"

#include "core/Paths.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace lila::features
{
using json = nlohmann::json;

namespace
{
std::mutex& storeMutex()
{
    static std::mutex m;
    return m;
}

/** Replaced whole and never freed, so pointers handed out stay valid across set() and reset(). */
const std::vector<Feature>*& store()
{
    static const std::vector<Feature>* features = nullptr;
    return features;
}

std::vector<std::string> strings(const json& j, const char* key)
{
    std::vector<std::string> out;
    if (auto it = j.find(key); it != j.end() && it->is_array())
        for (const json& s : *it)
            if (s.is_string())
                out.push_back(s.get<std::string>());
    return out;
}

std::vector<Feature>* parse(const json& roadmap, const json& map)
{
    auto* out = new std::vector<Feature>;
    if (!roadmap.is_object() || !roadmap.contains("sections") || !roadmap["sections"].is_array())
        return out;
    for (const json& section : roadmap["sections"])
    {
        if (!section.is_object() || !section.contains("cards") || !section["cards"].is_array())
            continue;
        for (const json& card : section["cards"])
        {
            if (!card.is_object())
                continue;
            Feature f;
            f.id = card.value("id", "");
            if (f.id.empty())
                continue;
            f.title = card.value("title", f.id);
            f.docs = card.value("docs", "");
            const std::string status = card.value("status", "ready");
            if (!card.value("note", "").empty())
                f.status = Status::NotReady;
            else if (status == "dev")
                f.status = Status::InDevelopment;
            else if (status == "planned")
                f.status = Status::Planned;
            if (map.is_object())
                if (auto it = map.find(f.id); it != map.end() && it->is_object())
                {
                    f.extensions = strings(*it, "extensions");
                    f.menus = strings(*it, "menus");
                    f.actions = strings(*it, "actions");
                }
            out->push_back(std::move(f));
        }
    }
    return out;
}

json readFile(const std::filesystem::path& file)
{
    std::ifstream in(file);
    return in ? json::parse(in, nullptr, false) : json();
}

const std::vector<Feature>& loaded()
{
    std::scoped_lock lock(storeMutex());
    if (!store())
    {
        const std::filesystem::path dir = paths::resourcesDir() / "roadmap";
        store() = parse(readFile(dir / "roadmap.json"), readFile(dir / "features.json"));
    }
    return *store();
}

bool contains(const std::vector<std::string>& list, std::string_view s)
{
    for (const std::string& x : list)
        if (x == s)
            return true;
    return false;
}
} // namespace

const char* label(Status s)
{
    switch (s)
    {
    case Status::InDevelopment: return "In development";
    case Status::NotReady: return "Not ready for use";
    case Status::Planned: return "Planned";
    case Status::Ready: break;
    }
    return "";
}

const std::vector<Feature>& all()
{
    return loaded();
}

const Feature* find(std::string_view id)
{
    for (const Feature& f : loaded())
        if (f.id == id)
            return &f;
    return nullptr;
}

const Feature* ofExtension(std::string_view extension)
{
    if (extension.empty())
        return nullptr;
    for (const Feature& f : loaded())
        if (contains(f.extensions, extension))
            return &f;
    return nullptr;
}

const Feature* ofAction(std::string_view action, std::string_view owner)
{
    const Feature* best = nullptr;
    size_t length = 0;
    for (const Feature& f : loaded())
        for (const std::string& prefix : f.actions)
            if (prefix.size() > length && action.starts_with(prefix))
            {
                best = &f;
                length = prefix.size();
            }
    return best ? best : ofExtension(owner);
}

const Feature* ofMenu(std::string_view path)
{
    const Feature* best = nullptr;
    size_t length = 0;
    for (const Feature& f : loaded())
        for (const std::string& menu : f.menus)
        {
            const bool under = path == menu || (path.size() > menu.size() && path.starts_with(menu) && path[menu.size()] == '/');
            if (under && menu.size() > length)
            {
                best = &f;
                length = menu.size();
            }
        }
    return best;
}

Status extensionStatus(std::string_view extension)
{
    const Feature* f = ofExtension(extension);
    return f ? f->status : Status::Ready;
}

void set(const json& roadmap, const json& map)
{
    std::scoped_lock lock(storeMutex());
    store() = parse(roadmap, map);
}

void reset()
{
    std::scoped_lock lock(storeMutex());
    store() = nullptr;
}
} // namespace lila::features
