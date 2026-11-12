#include "gn/Group.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace lila::gn
{
bool validIdentifier(std::string_view s)
{
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!alpha && !(digit && i > 0))
            return false;
    }
    return true;
}

bool validInputName(std::string_view s)
{
    static const std::set<std::string, std::less<>> reserved = {"inMesh", "enabled", "groupDefinition", "outMesh", "geometry", "message", "caching"};
    return validIdentifier(s) && !reserved.contains(s) && !s.starts_with("obj_");
}

namespace
{
void putOpt(json& o, const char* key, const std::optional<double>& v)
{
    if (v)
        o[key] = *v;
}

std::optional<double> getOpt(const json& o, const char* key)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_number())
        return std::nullopt;
    return it->get<double>();
}
} // namespace

void putNodeFlags(const GNode& n, json& o) { putNodeFlagsTo(n, o); }

void getNodeFlags(GNode& n, const json& o)
{
    n.muted = o.value("muted", false);
    n.collapsed = o.value("collapsed", false);
    n.hideUnused = o.value("hideUnused", false);
}

json tiesToJson(const std::vector<GTie>& ties)
{
    json out = json::array();
    for (const GTie& t : ties)
    {
        json links = json::array();
        for (const GLink& l : t.links)
            links.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
        json o = {{"links", links}, {"position", {t.position.x, t.position.y}}, {"angle", t.angle}};
        if (!t.color.empty())
            o["color"] = t.color;
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<GTie> tiesFromJson(const json& j)
{
    std::vector<GTie> out;
    if (!j.is_array())
        return out;
    for (const json& t : j)
    {
        if (!t.is_object())
            continue;
        GTie tie;
        for (const json& l : t.value("links", json::array()))
            if (l.is_array() && l.size() == 4 && l[0].is_string() && l[1].is_string() && l[2].is_string() && l[3].is_string())
                tie.links.push_back({l[0].get<std::string>(), l[1].get<std::string>(), l[2].get<std::string>(), l[3].get<std::string>()});
        if (const json& p = t.value("position", json()); p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number())
            tie.position = {p[0].get<double>(), p[1].get<double>()};
        if (t.contains("angle") && t["angle"].is_number())
            tie.angle = t["angle"].get<double>();
        if (t.contains("color") && t["color"].is_string())
            tie.color = t["color"].get<std::string>();
        out.push_back(std::move(tie));
    }
    return out;
}

void pruneTies(std::vector<GTie>& ties, const std::vector<GLink>& links)
{
    std::set<GLink> live(links.begin(), links.end());
    for (GTie& t : ties)
    {
        std::set<GLink> seen;
        std::erase_if(t.links, [&](const GLink& l) { return !live.contains(l) || !seen.insert(l).second; });
    }
    std::erase_if(ties, [](const GTie& t) { return t.links.size() < 2; });
}

std::vector<GTie> tiesAmong(const std::vector<GTie>& ties, const std::set<std::string>& nodes, const std::map<std::string, std::string>& renamed,
                            glm::dvec2 offset)
{
    auto name = [&](const std::string& n) {
        auto it = renamed.find(n);
        return it == renamed.end() ? n : it->second;
    };
    std::vector<GTie> out;
    for (const GTie& t : ties)
    {
        GTie copy = t;
        copy.links.clear();
        copy.position += offset;
        for (const GLink& l : t.links)
            if (nodes.contains(l.fromNode) && nodes.contains(l.toNode))
                copy.links.push_back({name(l.fromNode), l.fromSocket, name(l.toNode), l.toSocket});
        if (copy.links.size() >= 2)
            out.push_back(std::move(copy));
    }
    return out;
}

json cleanBookmarks(const json& j)
{
    json out = json::object();
    if (!j.is_object())
        return out;
    for (auto& [slot, v] : j.items())
        if (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number() && v[2].get<double>() > 0.0)
            out[slot] = v;
    return out;
}

json framesToJson(const std::vector<GFrame>& frames)
{
    json out = json::array();
    for (const GFrame& f : frames)
    {
        json o = {{"label", f.label}, {"position", {f.position.x, f.position.y}}, {"size", {f.size.x, f.size.y}}, {"nodes", f.nodes}};
        if (!f.color.empty())
            o["color"] = f.color;
        if (f.collapsed)
            o["collapsed"] = true;
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<GFrame> framesFromJson(const json& j)
{
    std::vector<GFrame> out;
    if (!j.is_array())
        return out;
    auto vec2 = [](const json& v, glm::dvec2& to) {
        if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
            to = {v[0].get<double>(), v[1].get<double>()};
    };
    for (const json& f : j)
    {
        if (!f.is_object())
            continue;
        GFrame frame;
        if (f.contains("label") && f["label"].is_string())
            frame.label = f["label"].get<std::string>();
        vec2(f.value("position", json()), frame.position);
        vec2(f.value("size", json()), frame.size);
        if (f.contains("color") && f["color"].is_string())
            frame.color = f["color"].get<std::string>();
        frame.collapsed = f.value("collapsed", false);
        for (const json& n : f.value("nodes", json::array()))
            if (n.is_string())
                frame.nodes.push_back(n.get<std::string>());
        out.push_back(std::move(frame));
    }
    return out;
}

void pruneFrames(std::vector<GFrame>& frames, const std::vector<std::string>& nodes)
{
    std::set<std::string> live(nodes.begin(), nodes.end());
    for (GFrame& f : frames)
    {
        std::set<std::string> seen;
        std::erase_if(f.nodes, [&](const std::string& n) { return !live.contains(n) || !seen.insert(n).second; });
    }
}

std::vector<GFrame> framesAmong(const std::vector<GFrame>& frames, const std::set<std::string>& nodes, const std::map<std::string, std::string>& renamed,
                                glm::dvec2 offset)
{
    std::vector<GFrame> out;
    for (const GFrame& f : frames)
    {
        GFrame copy = f;
        copy.nodes.clear();
        copy.position += offset;
        for (const std::string& n : f.nodes)
            if (nodes.contains(n))
            {
                auto it = renamed.find(n);
                copy.nodes.push_back(it == renamed.end() ? n : it->second);
            }
        if (!copy.nodes.empty())
            out.push_back(std::move(copy));
    }
    return out;
}

std::string GInput::displayName() const
{
    if (!label.empty())
        return label;
    std::string out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (c == '_')
        {
            out += ' ';
            continue;
        }
        bool upper = c >= 'A' && c <= 'Z';
        if (i > 0 && upper && name[i - 1] >= 'a' && name[i - 1] <= 'z')
            out += ' ';
        out += i == 0 && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    }
    return out;
}

json Group::toJson() const
{
    json jin = json::array();
    for (const GInput& in : inputs)
    {
        json o = {{"name", in.name}, {"type", socketTypeName(in.type)}, {"default", in.defaultValue}};
        for (auto [key, val] : {std::pair{"label", &in.label}, {"description", &in.description}, {"subtype", &in.subtype}, {"panel", &in.panel}})
            if (!val->empty())
                o[key] = *val;
        putOpt(o, "min", in.min);
        putOpt(o, "max", in.max);
        putOpt(o, "softMin", in.softMin);
        putOpt(o, "softMax", in.softMax);
        putOpt(o, "step", in.step);
        jin.push_back(std::move(o));
    }
    json jn = json::array();
    for (const GNode& n : nodes)
    {
        json o = {{"name", n.name}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
        if (!n.values.empty())
            o["values"] = n.values;
        putNodeFlags(n, o);
        jn.push_back(std::move(o));
    }
    json jl = json::array();
    for (const GLink& l : links)
        jl.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
    json out = {{"name", name}, {"typeVersion", typeVersion}, {"inputs", jin}, {"nodes", jn}, {"links", jl}};
    if (!ties.empty())
        out["ties"] = tiesToJson(ties);
    if (!frames.empty())
        out["frames"] = framesToJson(frames);
    if (!bookmarks.empty())
        out["bookmarks"] = bookmarks;
    if (!outputs.empty())
    {
        json jo = json::array();
        for (const GOutput& o : outputs)
        {
            json e = {{"name", o.name}, {"type", socketTypeName(o.type)}};
            if (!o.description.empty())
                e["description"] = o.description;
            jo.push_back(std::move(e));
        }
        out["outputs"] = std::move(jo);
    }
    if (!description.empty())
        out["description"] = description;
    if (tool)
        out["tool"] = true;
    if (!subgroups.empty())
        out["subgroups"] = subgroups;
    return out;
}

Group Group::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("group definition must be an object");
    Group g;
    try
    {
        g.name = j.at("name").get<std::string>();
        g.typeVersion = j.value("typeVersion", 1);
        g.description = j.value("description", std::string());
        g.tool = j.value("tool", false);
        for (const json& in : j.value("inputs", json::array()))
        {
            GInput gi;
            gi.name = in.at("name").get<std::string>();
            if (!parseSocketType(in.value("type", "float"), &gi.type))
                throw std::invalid_argument("input " + gi.name + ": unknown type " + in.value("type", ""));
            gi.defaultValue = in.value("default", json());
            gi.label = in.value("label", std::string());
            gi.description = in.value("description", in.value("tooltip", std::string()));
            gi.subtype = in.value("subtype", std::string());
            gi.panel = in.value("panel", std::string());
            gi.min = getOpt(in, "min");
            gi.max = getOpt(in, "max");
            gi.softMin = getOpt(in, "softMin");
            gi.softMax = getOpt(in, "softMax");
            gi.step = getOpt(in, "step");
            g.inputs.push_back(std::move(gi));
        }
        for (const json& o : j.value("outputs", json::array()))
        {
            GOutput go;
            go.name = o.at("name").get<std::string>();
            if (!parseSocketType(o.value("type", "float"), &go.type))
                throw std::invalid_argument("output " + go.name + ": unknown type " + o.value("type", ""));
            go.description = o.value("description", std::string());
            g.outputs.push_back(std::move(go));
        }
        for (const json& n : j.value("nodes", json::array()))
        {
            GNode gn;
            gn.name = n.at("name").get<std::string>();
            gn.type = n.at("type").get<std::string>();
            if (n.contains("position") && n["position"].is_array() && n["position"].size() == 2)
                gn.position = {n["position"][0].get<double>(), n["position"][1].get<double>()};
            gn.values = n.value("values", json::object());
            getNodeFlags(gn, n);
            g.nodes.push_back(std::move(gn));
        }
        for (const json& l : j.value("links", json::array()))
            g.links.push_back({l.at(0).get<std::string>(), l.at(1).get<std::string>(), l.at(2).get<std::string>(), l.at(3).get<std::string>()});
        g.ties = tiesFromJson(j.value("ties", json::array()));
        g.frames = framesFromJson(j.value("frames", json::array()));
        g.bookmarks = cleanBookmarks(j.value("bookmarks", json::object()));
        if (auto it = j.find("subgroups"); it != j.end() && it->is_object())
            for (auto& [k, v] : it->items())
                g.subgroups[k] = v;
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad group definition: ") + e.what());
    }
    return g;
}

GNode* Group::node(std::string_view n)
{
    for (GNode& x : nodes)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GNode* Group::node(std::string_view n) const
{
    for (const GNode& x : nodes)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GInput* Group::input(std::string_view n) const
{
    for (const GInput& x : inputs)
        if (x.name == n)
            return &x;
    return nullptr;
}

GInput* Group::input(std::string_view n)
{
    for (GInput& x : inputs)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GOutput* Group::output(std::string_view n) const
{
    for (const GOutput& x : outputs)
        if (x.name == n)
            return &x;
    return nullptr;
}

std::vector<std::string> Group::panels() const
{
    std::vector<std::string> out;
    for (const GInput& in : inputs)
        if (!in.panel.empty() && std::find(out.begin(), out.end(), in.panel) == out.end())
            out.push_back(in.panel);
    return out;
}

const GNode* Group::firstOfType(std::string_view type) const
{
    for (const GNode& x : nodes)
        if (x.type == type)
            return &x;
    return nullptr;
}

std::string Group::uniqueNodeName(std::string_view base) const
{
    std::string b(base);
    if (!node(b))
        return b;
    for (int i = 1;; ++i)
    {
        std::string c = b + std::to_string(i);
        if (!node(c))
            return c;
    }
}

std::vector<GLink> Group::linksInto(std::string_view n, std::string_view socket) const
{
    std::vector<GLink> out;
    for (const GLink& l : links)
        if (l.toNode == n && l.toSocket == socket)
            out.push_back(l);
    return out;
}

bool runsGroup(std::string_view type)
{
    return type == kGroupNodeDeclType || type == "RepeatGroup" || type == "ForEachElement" || type == "SimulationZone" || type == "MakeClosure";
}

std::vector<std::string> referencedGroups(const Group& g)
{
    std::vector<std::string> out;
    for (const GNode& n : g.nodes)
        if (runsGroup(n.type))
            if (auto it = n.values.find("group"); it != n.values.end() && it->is_string() && !it->get<std::string>().empty())
                if (std::find(out.begin(), out.end(), it->get<std::string>()) == out.end())
                    out.push_back(it->get<std::string>());
    return out;
}

namespace
{
bool usesNodeType(const Group& g, std::initializer_list<std::string_view> types)
{
    for (const GNode& n : g.nodes)
        if (std::find(types.begin(), types.end(), n.type) != types.end())
            return true;
    for (const auto& [name, def] : g.subgroups)
    {
        try
        {
            if (usesNodeType(Group::fromJson(def), types))
                return true;
        }
        catch (const std::exception&)
        {
        }
    }
    return false;
}
} // namespace

bool usesSceneTime(const Group& g) { return usesNodeType(g, {"SceneTime"}); }

bool usesViewState(const Group& g) { return usesNodeType(g, {"IsViewport", "ActiveCamera"}); }

bool dependsOn(const Group& g, std::string_view group)
{
    if (g.name == group)
        return true;
    for (const std::string& r : referencedGroups(g))
    {
        if (r == group)
            return true;
        auto it = g.subgroups.find(r);
        if (it == g.subgroups.end())
            continue;
        try
        {
            if (dependsOn(Group::fromJson(it->second), group))
                return true;
        }
        catch (const std::exception&)
        {
        }
    }
    return false;
}

Group Group::makeDefault(std::string name)
{
    Group g;
    g.name = std::move(name);
    g.nodes.push_back({"GroupInput", "GroupInput", {-300.0, 0.0}, json::object()});
    g.nodes.push_back({"GroupOutput", "GroupOutput", {300.0, 0.0}, json::object()});
    g.links.push_back({"GroupInput", "geometry", "GroupOutput", "geometry"});
    return g;
}
} // namespace lila::gn
