// namingCultures, fictionalName and realNameCheck.

#include "names/Names.h"

#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <map>

namespace lila::names
{
void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "namingCultures",
        .doc = "The naming cultures imports and settlements name things in, each with the vocabulary its businesses and places "
               "are named from, the kinds of named things, and the business programs with patterns of their own.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json c = json::array();
            for (const std::string& n : cultures())
                c.push_back({{"name", n}, {"vocabulary", vocabularyOf(n)}});
            bool found = false;
            const std::string path = brandBlocklistPath(&found);
            return {{"cultures", c}, {"kinds", kindNames()}, {"programs", programs()},
                    {"blocklist", {{"file", path}, {"found", found}, {"names", brandBlocklist().size()}}}};
        },
        .examples = {"namingCultures()['cultures']"},
    });

    ops::registerCommand({
        .name = "fictionalName",
        .doc = "A fictional name from a naming culture, the same every time for the same culture, kind, program and key. Never "
               "one that matches the brand blocklist or a real name an import in the scene replaced. Returns the name and the key "
               "used.",
        .flags = {{"culture", "cu", T::String, "mexican", "Naming culture (namingCultures)."},
                  {"kind", "k", T::String, "business", "What is named: street, district, town, river, lake, reservoir, dam, island, "
                                                       "beach, port, airport, cemetery, school, hospital, government, business, ..."},
                  {"program", "pg", T::String, "", "For businesses and buildings: what they are (pharmacy, taqueria, carParts, ...)."},
                  {"key", "ky", T::String, "1", "Stable key: a number or any text (an id)."},
                  {"streetClass", "sc", T::Int, 2, "Streets: 0 arterial, 1 collector, 2 local, 3 alley, 4 path.", {}, {}, 0.0, 4.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string culture = a.get<std::string>("culture", "mexican");
            Kind kind = Kind::Business;
            const std::string kindName = a.get<std::string>("kind", "business");
            if (!parseKind(kindName, &kind))
                throw CommandError("fictionalName: unknown kind '" + kindName + "'");
            const std::string keyText = a.get<std::string>("key", "1");
            char* end = nullptr;
            const unsigned long long n = std::strtoull(keyText.c_str(), &end, 10);
            const uint64_t key = end && *end == '\0' && !keyText.empty() ? uint64_t(n) : textKey(keyText);
            try
            {
                NameBook book(culture, std::make_shared<RealNames>(sceneRealNames(scene::Graph::current())));
                const std::string name = kind == Kind::Street ? book.street(a.get<int>("streetClass", 2), key)
                                                              : book.place(kind, a.get<std::string>("program", ""), key);
                return {{"name", name}, {"culture", culture}, {"kind", kindName}, {"key", key}};
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("fictionalName: ") + e.what());
            }
        },
        .examples = {"fictionalName(culture='mexican', kind='business', program='taqueria', key='node/123')['name']",
                     "fictionalName(kind='river', key=42)"},
    });

    ops::registerCommand({
        .name = "realNameCheck",
        .doc = "Finds output text that matches a real name: names written by imports and generators (osmFeatures, osmBuildings, "
               "osmPlaces, sign text, labels), node names, and any texts given, compared with the real names imports replaced "
               "(kept in hidden sourceNames attributes) and the brand blocklist (OpenStreetMap name-suggestion-index). Node "
               "names are compared with imported names only. report=True lists each leak in the Validation panel on its "
               "node. Returns ok, the leaks (where, text, the real name, where that came from) and how many real names were "
               "compared.",
        .flags = {{"texts", "tx", T::List, nullptr, "Extra texts to check: strings, or {where, text}."},
                  {"scene", "sn", T::Bool, true, "Check the scene's names and name attributes too."},
                  {"report", "rp", T::Bool, false, "Also list leaks in the Validation panel (replacing this check's earlier ones)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            scene::Graph& g = scene::Graph::current();
            std::vector<Text> extra;
            if (a.has("texts"))
                for (const json& t : a.flags["texts"])
                {
                    if (t.is_string())
                        extra.push_back({"texts", t.get<std::string>()});
                    else if (t.is_object())
                        extra.push_back({t.value("where", std::string("texts")), t.value("text", std::string())});
                }
            const std::vector<Leak> leaks = findRealNames(g, extra, a.get<bool>("scene", true));
            json list = json::array();
            for (const Leak& l : leaks)
                list.push_back({{"where", l.where}, {"text", l.text}, {"real", l.real}, {"source", l.source}});
            json out = {{"ok", leaks.empty()}, {"leaks", list}, {"realNames", sceneRealNames(g).size()}, {"blocklist", brandBlocklist().size()}};
            if (a.get<bool>("report", false))
            {
                if (!ops::find("reportIssue") || !ops::find("clearIssues"))
                    throw CommandError("realNameCheck: the Validation commands (reportIssue, clearIssues) are not available");
                std::map<std::string, std::vector<const Leak*>> byNode;
                for (const Leak& l : leaks)
                    byNode[l.where.substr(0, l.where.find('.'))].push_back(&l);
                int reported = 0;
                for (const auto& [node, ls] : byNode)
                {
                    if (!g.find(node))
                        continue;
                    ops::execute("clearIssues", {{"args", {node}}, {"source", "realNameCheck"}});
                    for (const Leak* l : ls)
                        if (reported++ < 200)
                            ops::execute("reportIssue", {{"args", {node}},
                                                         {"message", "real name '" + l->real + "' in \"" + l->text + "\""},
                                                         {"severity", "error"},
                                                         {"source", "realNameCheck"}});
                }
                out["reported"] = std::min(reported, 200);
            }
            return out;
        },
        .examples = {"c = realNameCheck()\nc['ok'], c['leaks']", "realNameCheck(texts=['Farmacias Similares'], scene=False)"},
    });
}
} // namespace lila::names
