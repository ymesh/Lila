// Level > Contacts, the Level Toolkit's Contacts section and Display > Contacts, the debug overlay of joins and blend weights.

#include "Seamwork.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/DebugDraw.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Prefs.h>
#include <lila/Tasks.h>

#include <map>

using namespace lila;
using namespace lila::ui;

namespace seamwork
{
namespace
{
constexpr const char* kOverlayPref = "seamwork/overlay";
constexpr const char* kWeightsPref = "seamwork/overlayWeights";

bool prefOn(const char* key, bool def)
{
    return PrefStore::get().value<bool>("general", key, def);
}

/** Runs a command on the selection from a menu, reporting failure on the status line. */
void onSelection(const char* command, json flags = json::object())
{
    Graph& g = Graph::current();
    json names = json::array();
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            names.push_back(n->path());
    flags["args"] = names;
    try
    {
        ops::execute(command, flags, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}

struct Overlay
{
    debugDraw::Channel channel;
    std::map<NodeId, uint64_t> drawn;
    uint64_t structure = 0;
    bool wasOn = false;
    bool weights = false;
};
Overlay& overlay()
{
    static Overlay o;
    return o;
}

/** Ground joins warm and thick, mesh joins cool, creases thin grey; near contacts dashed, so kind reads without hue too. */
void drawSeam(const SeamData& d, const debugDraw::Channel& ch, bool weights)
{
    for (const Curve& c : d.result->curves)
    {
        debugDraw::Style s;
        s.persistent = true;
        s.channel = ch;
        s.onTop = true;
        switch (c.kind)
        {
        case Kind::Ground:
            s.color = math::LinearColor(1.0f, 0.55f, 0.15f);
            s.thickness = 3.0f;
            break;
        case Kind::Mesh:
            s.color = math::LinearColor(0.25f, 0.75f, 1.0f);
            s.thickness = 2.0f;
            break;
        case Kind::Crease:
            s.color = math::LinearColor(0.7f, 0.7f, 0.7f);
            s.thickness = 1.0f;
            break;
        }
        std::vector<math::Vector> pts(c.points.begin(), c.points.end());
        if (!c.nearContact)
            debugDraw::lines(pts, c.closed, s);
        else
        {
            // Every other segment.
            std::vector<math::Vector> segs;
            const size_t n = pts.size() + (c.closed ? 1 : 0);
            for (size_t k = 0; k + 1 < n; k += 2)
            {
                segs.push_back(pts[k % pts.size()]);
                segs.push_back(pts[(k + 1) % pts.size()]);
            }
            debugDraw::segments(segs, s);
        }
    }
    if (!weights)
        return;
    // Blend weight at each field vertex: brighter and larger where the band is strongest.
    size_t budget = 200000;
    for (size_t i = 0; i < d.objects.size() && budget; ++i)
    {
        const auto& dist = d.distances[i];
        const Object& o = d.objects[i];
        for (geo::Index p = 0; p < dist.size() && budget; ++p)
        {
            if (!o.mesh->pointValid(p))
                continue;
            const glm::dvec3 w(o.world * glm::dvec4(o.mesh->position(p), 1.0));
            const double weight = d.band.weight(dist[p], w);
            if (weight <= 0.01)
                continue;
            --budget;
            debugDraw::point(w, float(3.0 + 5.0 * weight),
                             {.color = math::LinearColor(float(weight), float(weight), float(weight)), .persistent = true, .channel = ch});
        }
    }
}

void refreshOverlay(lila::ext::Context* ctx)
{
    Overlay& o = overlay();
    const bool on = prefOn(kOverlayPref, false);
    const bool weights = prefOn(kWeightsPref, false);
    if (!o.channel && ctx)
        o.channel = debugDraw::channel(*ctx, "Contacts");
    if (!o.channel)
        return;
    if (!on)
    {
        if (o.wasOn)
            o.channel.clear();
        o.wasOn = false;
        o.drawn.clear();
        return;
    }
    Graph& g = Graph::current();
    std::map<NodeId, uint64_t> now;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().name == kSeamType)
            if (const SeamData* d = seamDataOf(*n); d && d->result)
                now[id] = d->revision;
    if (o.wasOn && now == o.drawn && weights == o.weights && g.structure() == o.structure)
        return;
    o.channel.clear();
    for (const auto& [id, rev] : now)
        drawSeam(*seamDataOf(*g.node(id)), o.channel, weights);
    o.drawn = std::move(now);
    o.weights = weights;
    o.structure = g.structure();
    o.wasOn = true;
}

void togglePref(const char* key)
{
    PrefStore::get().set("general", key, !prefOn(key, false));
    refreshOverlay(nullptr);
}
} // namespace

void registerUi(lila::ext::Context& ctx)
{
    lila::ext::Context* c = &ctx;
    ctx.subscribe(events::Event::AppIdle, [c](const events::EventData&) {
        if (tasks::isMainThread())
            refreshOverlay(c);
    });

    struct Entry
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
    };
    const Entry entries[] = {
        {"seamwork.find", "Find Contacts", "seamFind", "Find where the selection meets the ground and each other, live.", "seamFind(['Wall', 'Rock_01'])"},
        {"seamwork.scatter", "Scatter Along Contacts", "seamScatter", "Spawn rubble, grass, moss or any mesh along the contacts.", "seamScatter('contacts', preset='rubble')"},
        {"seamwork.decal", "Contact Strips", "seamDecal", "Grime or moss strips along the contacts, as meshes that work in any renderer.", "seamDecal('contacts', side='both')"},
        {"seamwork.skirt", "Contact Skirt", "seamSkirt", "A blend mesh bridging each wall and the ground, sampling both materials.", "seamSkirt('contacts', width=40)"},
        {"seamwork.check", "Check Contacts", "seamCheck", "Report floating objects and contacts nothing covers.", "seamCheck()"},
        {"seamwork.update", "Update Contacts", "seamUpdate", "Write fields and the terrain mask now.", "seamUpdate()"},
        {"seamwork.send", "Send Contacts to Unreal", "seamSend", "Contact splines, PCG points and materials to Unreal.", "seamSend('contacts')"},
        {"seamwork.export", "Export Contacts", "seamExport", "Contact curves and points as JSON or USD.", "seamExport('contacts', file='contacts.json')"},
    };
    menus::addItem(ctx, {.id = "seamwork.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Contacts",
                         .before = levelToolkit::kAssetsSection});
    for (const Entry& e : entries)
    {
        if (!ops::find(e.command))
            continue;
        actions::add(ctx, {.id = e.id, .label = e.label, .category = "Dress", .doc = e.doc, .python = e.python, .command = e.command, .call = json::object()});
        menus::addItem(ctx, {.menu = "Level/Contacts", .action = e.id});
    }
    menus::addItem(ctx, {.menu = "Level/Contacts", .kind = menus::Kind::Separator});
    actions::add(ctx, {.id = "seamwork.tagIgnore", .label = "Ignore in Contacts", .category = "Dress", .run = [] { onSelection("seamTag", {{"ignore", true}}); },
                       .doc = "Leave the selection out of contacts (intended overlaps)."});
    actions::add(ctx, {.id = "seamwork.tagGround", .label = "Use as Ground", .category = "Dress", .run = [] { onSelection("seamTag", {{"ground", true}}); },
                       .doc = "The selection acts as ground for contacts, like a floor slab."});
    actions::add(ctx, {.id = "seamwork.tagClear", .label = "Clear Contact Tags", .category = "Dress",
                       .run = [] { onSelection("seamTag", {{"ignore", false}, {"ground", false}, {"group", 0}}); },
                       .doc = "The selection takes part in contacts as an ordinary object again."});
    for (const char* id : {"seamwork.tagIgnore", "seamwork.tagGround", "seamwork.tagClear"})
        menus::addItem(ctx, {.menu = "Level/Contacts", .action = id});

    actions::add(ctx, {.id = "seamwork.overlay", .label = "Contacts", .category = "Display", .run = [] { togglePref(kOverlayPref); },
                       .doc = "Draw contacts over the scene: ground joins thick and orange, joins between objects blue, creases thin grey; "
                              "near contacts dashed.",
                       .checked = [] { return prefOn(kOverlayPref, false); }, .icon = "layers-intersect"});
    actions::add(ctx, {.id = "seamwork.overlayWeights", .label = "Contact Blend Weights", .category = "Display",
                       .run = [] { togglePref(kWeightsPref); }, .doc = "With Contacts on, also draw each mesh's blend weight as points, brighter where stronger.",
                       .checked = [] { return prefOn(kWeightsPref, false); }});
    menus::addItem(ctx, {.menu = "Display", .action = "seamwork.overlay"});
    menus::addItem(ctx, {.menu = "Display", .action = "seamwork.overlayWeights"});

    // Find is the daily tile; the covers next, then check and ship; the toggle and tags go to the overflow past six.
    std::vector<levelToolkit::Item> items = {{"seamwork.find", "layers-intersect", "Find", "Contacts"}, {}};
    const levelToolkit::Item buttons[] = {{"seamwork.scatter", "grain", "Scatter"}, {"seamwork.decal", "line-dashed", "Strips"},     {"seamwork.skirt", "wave-saw-tool", "Skirt"},
                                          {"seamwork.check", "checklist", "Check"},     {"seamwork.send", "send", "To Unreal"},   {"seamwork.export", "file-export", "Export"},
                                          {"seamwork.update", "refresh", "Update"},   {"seamwork.overlay", "eye", "Overlay"}, {"seamwork.tagIgnore", "eye-off", "Ignore"},
                                          {"seamwork.tagGround", "map", "Ground"}};
    for (const auto& b : buttons)
        items.push_back(b);
    levelToolkit::add(ctx, {.id = "seamwork.contacts", .title = "Contacts", .items = items, .order = 43, .docs = "seamwork"});
}
} // namespace seamwork
