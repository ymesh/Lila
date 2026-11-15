#include "bake/BakeNodes.h"

#include "bake/Targets.h"

#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <stdexcept>

namespace lila::bake
{
namespace
{
using scene::AttrInput;
using scene::AttrStorable;
using scene::AttrHidden;
using scene::PlugType;

constexpr const char* kProjectKind = "bakeProjectSettings";
constexpr const char* kGroupKind = "bakeGroupSettings";

/** Settings JSON stored as scene Data: {"kind", "value"}. */
struct SettingsData final : scene::Data
{
    const char* kindName = kProjectKind;
    json value;

    const char* kind() const override { return kindName; }
    json toJson() const override { return {{"kind", kindName}, {"value", value}}; }
    size_t bytes() const override { return 256 + value.dump().size(); }
};

scene::DataPtr makeData(const char* kind, json value)
{
    auto d = std::make_shared<SettingsData>();
    d->kindName = kind;
    d->value = std::move(value);
    return d;
}

/** Brings older settings up to kSettingsVersion. Version 1 named group meshes the way version 2 files still do. */
json upgrade(json v)
{
    if (v.is_object())
        v["version"] = kSettingsVersion;
    return v;
}

/**
 * A node a group refers to. The id is authoritative while the node lives, so renames follow; the name binds references
 * read from a file and rebinds referenced nodes after their reference reloads (as object set members do).
 */
struct Ref
{
    mutable scene::NodeId id = scene::kNoNode;
    mutable std::string name;
    mutable bool referenced = false;
};

/** A group's settings with its lows, highs, cage and project held as Refs. */
struct GroupData final : scene::Data
{
    GroupSettings settings;
    /** Parallel to settings.lows and settings.highs (self and file highs leave theirs empty). */
    std::vector<Ref> lows, highs;
    /** Per unlink: its lows' refs, then its meshes'. */
    std::vector<std::pair<std::vector<Ref>, std::vector<Ref>>> unlinks;
    /** Per unlink: its materials' nodes, so a renamed material keeps its unlink. A slot without a node stays a name. */
    std::vector<std::vector<Ref>> unlinkMaterials;
    Ref cage, project;

    const char* kind() const override { return kGroupKind; }
    json toJson() const override;
    size_t bytes() const override { return 512 + settings.toJson().dump().size(); }
};

const scene::Node* findByName(const scene::Graph& g, const std::string& ns, const std::string& name)
{
    if (name.empty())
        return nullptr;
    try
    {
        if (!ns.empty())
            if (const scene::Node* n = g.find(ns + ":" + name))
                return n;
        return g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** The live node a ref means, or null. Read only, so it is safe from the parallel scene serializer. */
const scene::Node* lookup(const scene::Graph& g, const std::string& ns, const Ref& r)
{
    if (r.id != scene::kNoNode)
        if (const scene::Node* n = g.node(r.id))
            return n;
    // A deleted scene node stays deleted (undo brings it back by id); only file refs and reloaded references bind by name.
    if (r.id != scene::kNoNode && !r.referenced)
        return nullptr;
    const scene::Node* n = findByName(g, ns, r.name);
    if (n && r.id != scene::kNoNode && n->referenceId() == 0)
        return nullptr;
    return n;
}

const scene::Node* bindRef(const scene::Graph& g, const std::string& ns, const Ref& r)
{
    const scene::Node* n = lookup(g, ns, r);
    if (n)
    {
        r.id = n->id();
        r.name = n->path();
        r.referenced = n->referenceId() != 0;
    }
    return n;
}

/** Whether the ref's node was deleted from the scene (as opposed to never bound). */
bool deleted(const scene::Graph& g, const Ref& r) { return r.id != scene::kNoNode && !r.referenced && !g.node(r.id); }

/**
 * The settings with every reference as its node's current path. References to deleted nodes drop out; unbound ones keep
 * their name so a bake can say what is missing. refresh binds (main thread); without it the call only reads.
 */
GroupSettings resolved(const scene::Graph& g, const std::string& ns, const GroupData& d, bool refresh)
{
    GroupSettings s = d.settings;
    auto name = [&](const Ref& r, std::string& out) {
        const scene::Node* n = refresh ? bindRef(g, ns, r) : lookup(g, ns, r);
        if (n)
            out = n->path();
        else if (deleted(g, r))
            return false;
        return true;
    };
    s.lows.clear();
    for (size_t i = 0; i < d.settings.lows.size(); ++i)
    {
        std::string l = d.settings.lows[i];
        if (i < d.lows.size() && !name(d.lows[i], l))
            continue;
        s.lows.push_back(std::move(l));
    }
    s.highs.clear();
    for (size_t i = 0; i < d.settings.highs.size(); ++i)
    {
        HighEntry h = d.settings.highs[i];
        if (!h.self && h.file.empty() && i < d.highs.size() && !name(d.highs[i], h.node))
            continue;
        s.highs.push_back(std::move(h));
    }
    if (!s.cage.empty() && !name(d.cage, s.cage))
        s.cage.clear();
    auto names = [&](const std::vector<std::string>& in, const std::vector<Ref>* refs) {
        std::vector<std::string> out;
        for (size_t i = 0; i < in.size(); ++i)
        {
            std::string n = in[i];
            if (refs && i < refs->size() && !name((*refs)[i], n))
                continue;
            out.push_back(std::move(n));
        }
        return out;
    };
    s.unlinks.clear();
    for (size_t i = 0; i < d.settings.unlinks.size(); ++i)
    {
        Unlink u = d.settings.unlinks[i];
        const bool has = i < d.unlinks.size();
        u.lows = names(u.lows, has ? &d.unlinks[i].first : nullptr);
        u.meshes = names(u.meshes, has ? &d.unlinks[i].second : nullptr);
        if (i < d.unlinkMaterials.size())
            for (size_t m = 0; m < u.materials.size() && m < d.unlinkMaterials[i].size(); ++m)
            {
                const Ref& r = d.unlinkMaterials[i][m];
                const scene::Node* n = r.id != scene::kNoNode ? g.node(r.id) : findByName(g, {}, r.name);
                if (!n || !n->type().isA("material"))
                    continue;
                if (refresh)
                    r.id = n->id();
                u.materials[m] = n->name();
            }
        if (!u.empty())
            s.unlinks.push_back(std::move(u));
    }
    return s;
}

json GroupData::toJson() const
{
    const scene::Graph& g = scene::Graph::current();
    json v = resolved(g, {}, *this, false).toJson();
    std::string p = project.name;
    if (const scene::Node* n = lookup(g, {}, project))
        p = n->path();
    v["project"] = p;
    return {{"kind", kGroupKind}, {"value", std::move(v)}};
}

Ref refFor(const scene::Graph& g, const std::string& ns, const std::string& name)
{
    Ref r;
    r.name = name;
    if (const scene::Node* n = findByName(g, ns, name))
    {
        r.id = n->id();
        r.name = n->path();
        r.referenced = n->referenceId() != 0;
    }
    return r;
}

/** Unbound data for settings read from a file or an older blob; refs bind by name on first use. */
std::shared_ptr<GroupData> unbound(const GroupSettings& s, const std::string& project)
{
    auto d = std::make_shared<GroupData>();
    d->settings = s;
    for (const std::string& l : s.lows)
        d->lows.push_back(Ref{.name = l});
    for (const HighEntry& h : s.highs)
        d->highs.push_back(Ref{.name = h.self || !h.file.empty() ? std::string() : h.node});
    d->cage.name = s.cage;
    for (const Unlink& u : s.unlinks)
    {
        auto& refs = d->unlinks.emplace_back();
        for (const std::string& l : u.lows)
            refs.first.push_back(Ref{.name = l});
        for (const std::string& m : u.meshes)
            refs.second.push_back(Ref{.name = m});
        auto& mats = d->unlinkMaterials.emplace_back();
        for (const std::string& m : u.materials)
            mats.push_back(Ref{.name = m});
    }
    d->project.name = project;
    return d;
}

scene::DataPtr groupFromJson(const json& j)
{
    const json v = upgrade(j.value("value", json::object()));
    GroupSettings s;
    try
    {
        s = GroupSettings::fromJson(v);
    }
    catch (const std::exception& e)
    {
        log::warn("bad bake group settings ({}); using defaults", e.what());
    }
    return unbound(s, v.is_object() ? v.value("project", std::string()) : std::string());
}

int attr(const scene::Node& n, const char* name)
{
    const int a = n.type().attrIndex(name);
    if (a < 0)
        throw std::logic_error(std::string("bake node without ") + name);
    return a;
}

json stored(const scene::Node& n)
{
    const scene::PlugValue& v = n.value(attr(n, "settings"));
    if (!std::holds_alternative<scene::DataPtr>(v))
        return nullptr;
    const scene::DataPtr& d = std::get<scene::DataPtr>(v);
    if (!d)
        return nullptr;
    const json j = d->toJson();
    return j.contains("value") ? j["value"] : json();
}
} // namespace

void registerTypes()
{
    if (scene::types::find(kProjectType))
        return;
    scene::data::registerKind(kProjectKind, [](const json& j) -> scene::DataPtr { return makeData(kProjectKind, upgrade(j.value("value", json::object()))); });
    scene::data::registerKind(kGroupKind, &groupFromJson);

    scene::NodeType p;
    p.name = kProjectType;
    p.icon = "texture";
    p.doc = "A texture bake project: output folder, file naming, resolution and the maps to bake. Its bakeGroup nodes say what "
            "projects onto what. Bake with the bake command, or bakeForTarget for a tool such as Substance Painter or Unreal.";
    p.ownAttrs = {
        {.name = "settings", .shortName = "bs", .type = PlugType::Data, .defaultValue = scene::DataPtr(),
         .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Output, maps and target, as versioned JSON (setBakeOutput, setBakeMap)."},
    };
    p.create = [] { return std::make_unique<scene::Node>(); };
    scene::types::registerType(std::move(p));

    scene::NodeType g;
    g.name = kGroupType;
    g.icon = "stack-front";
    g.doc = "A bake group: low meshes that receive maps and the highs projected onto them, each high with its own bake-only "
            "modifier stack. Groups only ever trace their own highs, so overlapping groups never affect each other.";
    g.ownAttrs = {
        {.name = "project", .shortName = "pj", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "The bakeProject this group belongs to."},
        {.name = "settings", .shortName = "bs", .type = PlugType::Data, .defaultValue = scene::DataPtr(),
         .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Lows, highs, cage and distances, as versioned JSON (bakeGroup)."},
    };
    g.create = [] { return std::make_unique<scene::Node>(); };
    scene::types::registerType(std::move(g));

    // Scene loads, imports and reference reloads end in one Reset: bind what they read by name before anything is renamed.
    scene::Graph::current().addListener([](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Reset)
            return;
        scene::Graph& graph = scene::Graph::current();
        for (scene::NodeId id : graph.nodes())
            if (const scene::Node* n = graph.node(id); n && isGroup(*n))
            {
                groupSettings(*n);
                groupProject(graph, *n);
            }
    });
}

bool isProject(const scene::Node& n) { return n.type().isA(kProjectType); }
bool isGroup(const scene::Node& n) { return n.type().isA(kGroupType); }

ProjectSettings projectSettings(const scene::Node& project)
{
    const json j = stored(project);
    if (j.is_null())
        return ProjectSettings::defaults();
    try
    {
        return ProjectSettings::fromJson(j);
    }
    catch (const std::exception& e)
    {
        log::warn("{}: bad bake settings ({}); using defaults", project.path(), e.what());
        return ProjectSettings::defaults();
    }
}

void setProjectSettings(scene::Graph& g, scene::NodeId project, const ProjectSettings& s)
{
    scene::Node* n = g.node(project);
    g.setValue({project, attr(*n, "settings"), -1}, makeData(kProjectKind, s.toJson()));
}

namespace
{
const GroupData* groupData(const scene::Node& group)
{
    const scene::PlugValue& v = group.value(attr(group, "settings"));
    if (!std::holds_alternative<scene::DataPtr>(v))
        return nullptr;
    return dynamic_cast<const GroupData*>(std::get<scene::DataPtr>(v).get());
}
} // namespace

GroupSettings groupSettings(const scene::Node& group)
{
    const GroupData* d = groupData(group);
    return d ? resolved(scene::Graph::current(), group.nameSpace(), *d, true) : GroupSettings{};
}

void setGroupSettings(scene::Graph& g, scene::NodeId group, const GroupSettings& s)
{
    scene::Node* n = g.node(group);
    const std::string& ns = n->nameSpace();
    auto d = std::make_shared<GroupData>();
    d->settings = s;
    for (const std::string& l : s.lows)
        d->lows.push_back(refFor(g, ns, l));
    for (const HighEntry& h : s.highs)
        d->highs.push_back(h.self || !h.file.empty() ? Ref{} : refFor(g, ns, h.node));
    if (!s.cage.empty())
        d->cage = refFor(g, ns, s.cage);
    for (const Unlink& u : s.unlinks)
    {
        auto& refs = d->unlinks.emplace_back();
        for (const std::string& l : u.lows)
            refs.first.push_back(refFor(g, ns, l));
        for (const std::string& m : u.meshes)
            refs.second.push_back(refFor(g, ns, m));
        auto& mats = d->unlinkMaterials.emplace_back();
        for (const std::string& m : u.materials)
        {
            const scene::Node* material = scene::materials::find(g, m);
            mats.push_back(Ref{.id = material ? material->id() : scene::kNoNode, .name = m});
        }
    }
    if (const GroupData* old = groupData(*n))
        d->project = old->project;
    else
        d->project.name = n->as<std::string>(attr(*n, "project"));
    g.setValue({group, attr(*n, "settings"), -1}, scene::DataPtr(std::move(d)));
}

scene::Node* groupProject(scene::Graph& g, const scene::Node& group)
{
    const GroupData* d = groupData(group);
    Ref legacy;
    const Ref* r = d ? &d->project : &legacy;
    if (r->id == scene::kNoNode && r->name.empty())
    {
        // Version 1 groups named their project only in the project attr.
        legacy.name = group.as<std::string>(attr(group, "project"));
        r = &legacy;
    }
    const scene::Node* p = bindRef(g, group.nameSpace(), *r);
    return p && isProject(*p) ? g.node(p->id()) : nullptr;
}

void setGroupProject(scene::Graph& g, scene::NodeId group, const std::string& project)
{
    scene::Node* n = g.node(group);
    std::shared_ptr<GroupData> d;
    if (const GroupData* old = groupData(*n))
        d = std::make_shared<GroupData>(*old);
    else
        d = unbound(GroupSettings{}, {});
    d->project = refFor(g, n->nameSpace(), project);
    g.setValue({group, attr(*n, "project"), -1}, d->project.name);
    g.setValue({group, attr(*n, "settings"), -1}, scene::DataPtr(std::move(d)));
}

std::vector<scene::NodeId> projects(const scene::Graph& g)
{
    std::vector<scene::NodeId> out;
    for (scene::NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && isProject(*n))
            out.push_back(id);
    return out;
}

std::vector<scene::NodeId> groupsOf(scene::Graph& g, scene::NodeId project)
{
    std::vector<scene::NodeId> out;
    for (scene::NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && isGroup(*n))
            if (scene::Node* p = groupProject(g, *n); p && p->id() == project)
                out.push_back(id);
    return out;
}

scene::Node* createProject(scene::Graph& g, const std::string& name, const ProjectSettings& s)
{
    scene::Node* n = g.createNode(*scene::types::find(kProjectType), name.empty() ? std::string_view(kProjectType) : std::string_view(name));
    setProjectSettings(g, n->id(), s);
    return n;
}

scene::Node* createGroup(scene::Graph& g, scene::NodeId project, const std::string& name, const GroupSettings& s)
{
    scene::Node* n = g.createNode(*scene::types::find(kGroupType), name.empty() ? std::string_view(kGroupType) : std::string_view(name));
    setGroupProject(g, n->id(), g.node(project)->name());
    setGroupSettings(g, n->id(), withGroupDefaults(s, projectSettings(*g.node(project)).groupDefaults));
    return n;
}
} // namespace lila::bake
