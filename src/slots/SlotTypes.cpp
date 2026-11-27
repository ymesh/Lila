#include "slots/Slots.h"

#include "core/Events.h"
#include "core/Profile.h"
#include "scene/AttrBuilders.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneData.h"

namespace lila::slots
{
namespace detail
{
void registerGnNode();
void registerCommands();
void servicePending(scene::Graph& g);
void forgetSet(NodeId id);
void clearTagCache();
/** Loads the library items an opened scene names, when any are missing (runs slotLoadItems). True when it ran. */
bool scheduleItemLoad();
} // namespace detail

namespace
{
SlotType type(std::string id, std::string label, glm::dvec3 footprint, Attach attach, std::string placeholder, std::vector<std::string> tags,
              std::string doc)
{
    return {.id = std::move(id),
            .label = std::move(label),
            .tags = std::move(tags),
            .footprint = footprint,
            .attach = attach,
            .placeholder = std::move(placeholder),
            .doc = std::move(doc)};
}

void registerShippedTypes()
{
    using A = Attach;
    const std::vector<SlotType> shipped = {
        type("prop", "Prop", {100, 100, 100}, A::Ground, "box", {}, "Any placed part without a more specific type."),

        type("door", "Door", {100, 220, 10}, A::Wall, "door", {"opening", "facade"}, "A door in a wall; origin at the sill centre, +Z out."),
        type("door.shop", "Shop door", {120, 240, 10}, A::Wall, "door", {"opening", "facade", "commercial"}, "A shopfront entrance."),
        type("door.house", "House door", {95, 210, 10}, A::Wall, "door", {"opening", "facade", "residential"}, "A front or back door of a home."),
        type("door.garage", "Garage door", {250, 220, 10}, A::Wall, "panel", {"opening", "facade"}, "A roller or swing garage door."),
        type("door.service", "Service door", {100, 210, 10}, A::Wall, "door", {"opening", "facade"}, "A back-of-house or utility door."),

        type("window", "Window", {100, 140, 15}, A::Wall, "window", {"opening", "facade"}, "A window in a wall; origin at the sill centre."),
        type("window.residential", "Residential window", {100, 140, 15}, A::Wall, "window", {"opening", "facade", "residential"}, "A home's window."),
        type("window.shop", "Shop window", {250, 220, 15}, A::Wall, "window", {"opening", "facade", "commercial"}, "A shopfront display window."),

        type("sign", "Sign", {100, 60, 8}, A::Wall, "sign", {"signage"}, "Any sign."),
        type("sign.storefront", "Storefront sign", {300, 80, 10}, A::Wall, "sign", {"signage", "commercial"}, "The name board over a shop."),
        type("sign.billboard", "Billboard", {1200, 400, 60}, A::Ground, "sign", {"signage", "advertising"}, "A billboard face."),
        type("sign.street", "Street sign", {60, 300, 10}, A::Ground, "pole", {"signage", "street"}, "A street name sign or plaque."),
        type("sign.traffic", "Traffic sign", {60, 250, 10}, A::Ground, "pole", {"signage", "street", "traffic"}, "A stop, yield or signal post."),

        type("awning", "Awning", {300, 60, 120}, A::Wall, "awning", {"facade", "commercial"}, "A canopy over a door or window."),
        type("ac", "AC", {80, 60, 40}, A::Wall, "box", {"facade", "utility"}, "Air conditioning hardware."),
        type("ac.unit", "AC unit", {80, 60, 40}, A::Wall, "box", {"facade", "utility"}, "A wall or window air conditioner."),

        type("lamp", "Lamp", {40, 300, 40}, A::Ground, "lamp", {"lighting"}, "Any lamp."),
        type("lamp.street", "Street lamp", {150, 800, 40}, A::Ground, "lamp", {"lighting", "street"}, "A street light; +Z the arm."),
        type("lamp.wall", "Wall lamp", {30, 40, 30}, A::Wall, "box", {"lighting", "facade"}, "A wall-mounted light."),

        type("tree", "Tree", {500, 800, 500}, A::Ground, "tree", {"vegetation"}, "Any tree."),
        type("tree.palm", "Palm", {400, 1200, 400}, A::Ground, "palm", {"vegetation", "tropical"}, "A palm tree."),
        type("tree.street", "Street tree", {400, 700, 400}, A::Ground, "tree", {"vegetation", "street"}, "A tree planted along a street."),
        type("tree.broadleaf", "Broadleaf tree", {700, 1000, 700}, A::Ground, "tree", {"vegetation"}, "A broadleaf tree."),
        type("tree.conifer", "Conifer", {400, 1200, 400}, A::Ground, "cone", {"vegetation"}, "A conifer."),
        type("plant", "Plant", {80, 80, 80}, A::Ground, "bush", {"vegetation"}, "A shrub, grass clump or flower."),
        type("plant.shrub", "Shrub", {150, 120, 150}, A::Ground, "bush", {"vegetation"}, "A shrub."),
        type("plant.grass", "Grass", {50, 40, 50}, A::Ground, "plant", {"vegetation", "groundcover"}, "A grass clump."),
        type("rock", "Rock", {100, 60, 100}, A::Ground, "bush", {"groundcover"}, "A rock or boulder."),

        type("crop", "Crop", {100, 80, 100}, A::Ground, "plant", {"vegetation", "farm"}, "A crop plant."),
        type("crop.row", "Crop row", {100, 80, 1000}, A::Ground, "row", {"vegetation", "farm"}, "A row of crops along +Z."),

        type("container", "Container", {244, 259, 606}, A::Ground, "box", {"industrial", "port"}, "A shipping container."),
        type("container.20ft", "20 ft container", {244, 259, 606}, A::Ground, "box", {"industrial", "port"}, "A 20 ft ISO container."),
        type("container.40ft", "40 ft container", {244, 259, 1219}, A::Ground, "box", {"industrial", "port"}, "A 40 ft ISO container."),

        type("furniture", "Street furniture", {100, 100, 60}, A::Ground, "box", {"street"}, "Benches, bins, bollards and the like."),
        type("furniture.bench", "Bench", {180, 80, 60}, A::Ground, "box", {"street"}, "A bench."),
        type("furniture.bin", "Bin", {50, 100, 50}, A::Ground, "cylinder", {"street"}, "A litter bin."),
        type("furniture.bollard", "Bollard", {25, 100, 25}, A::Ground, "cylinder", {"street"}, "A bollard."),
        type("furniture.hydrant", "Hydrant", {40, 80, 40}, A::Ground, "cylinder", {"street"}, "A fire hydrant."),
        type("furniture.stall", "Market stall", {300, 250, 200}, A::Ground, "box", {"street", "commercial"}, "A market stall."),
        type("furniture.cart", "Cart", {200, 150, 120}, A::Ground, "box", {"street"}, "A cart."),
        type("furniture.well", "Well", {150, 120, 150}, A::Ground, "cylinder", {"street"}, "A well."),
    };
    for (const SlotType& t : shipped)
        registerType(t);
}

void registerSetType()
{
    using namespace scene::attrdefs;
    const scene::NodeType* transform = scene::types::find("transform");
    if (!transform || scene::types::find(kSetType))
        return;
    scene::NodeType t{.name = kSetType,
                      .base = transform,
                      .dag = true,
                      .doc = "Slots a generator wrote (doors, signs, lamps, trees), drawn as their resolved payloads: rules and "
                             "overrides from the Slots panel swap what each one shows without regenerating.",
                      .icon = "layout-grid-add",
                      .create = transform->create};
    t.ownAttrs = {
        {.name = "slots", .type = scene::PlugType::String, .defaultValue = std::string(), .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden,
         .doc = "The slots, local to this node, as JSON."},
        {.name = "source", .type = scene::PlugType::String, .defaultValue = std::string(), .flags = scene::AttrInput | scene::AttrStorable,
         .doc = "The generator that wrote the slots."},
        boolAttr("drawSlots", true, "Draw the slots' resolved payloads."),
    };
    scene::types::registerType(std::move(t));
}

void registerMeshProvider()
{
    meshProviders().put("mesh", [](scene::Graph& g, const std::string& ref, bool, std::string& why) -> std::shared_ptr<const geo::Mesh> {
        const scene::Node* n = nullptr;
        try
        {
            n = g.find(ref);
        }
        catch (const std::exception& e)
        {
            why = e.what();
            return nullptr;
        }
        if (!n)
        {
            why = "no node " + ref;
            return nullptr;
        }
        std::shared_ptr<const geo::Mesh> m = combinedMesh(g, n->id());
        if (!m)
            why = ref + " has no mesh";
        return m;
    });
}
} // namespace

void registerAll()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerShippedTypes();
    registerSetType();
    registerMeshProvider();
    detail::registerGnNode();
    detail::registerCommands();
    scene::Graph::addEvaluateHook([](scene::Graph& g, bool after) {
        if (after)
            detail::servicePending(g);
    });
    for (events::Event e : {events::Event::SceneOpened, events::Event::SceneNew})
        events::subscribe(e, [](const events::EventData&) {
            LILA_ZONE_N("slots.sceneOpened");
            unpublishGone(scene::Graph::current());
            clearVisuals();
            detail::clearTagCache();
            refresh();
        });
    // The rules, overrides and pins as soon as the scene's data is read, so publishers evaluated during the rest of the load
    // see them and need no rerun when the scene has opened.
    scene::data::onLoaded([] {
        unpublishGone(scene::Graph::current());
        refresh();
    });
    // After the visuals are dropped: library items the opened scene names come in, so its slots draw them straight away.
    events::subscribe(events::Event::SceneOpened, [](const events::EventData&) {
        LILA_ZONE_N("slots.itemLoad");
        detail::scheduleItemLoad();
    });
    events::subscribe(events::Event::NodeDeleted, [](const events::EventData& d) {
        detail::forgetSet(NodeId(d.node));
        unpublish(NodeId(d.node));
    });
}
} // namespace lila::slots
