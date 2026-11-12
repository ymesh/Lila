#include "gn/NodesCommon.h"

#include "gn/Tool.h"

namespace lila::gn::nodes
{
namespace tool_nodes
{
const std::vector<std::string>& selectionDomains()
{
    static const std::vector<std::string> v = {"point", "edge", "face"};
    return v;
}

int selectionDomain(const std::string& s) { return s == "edge" ? 1 : s == "face" ? 2 : 0; }

/** The tool's component selection on whatever domain it is read: its own layer, else the points' adapted, else everything. */
class ToolSelectionField final : public Field
{
public:
    ToolSelectionField() : Field(SocketType::Bool) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (!ctx.mesh)
            return Column::of(uint8_t(1));
        const int d = int(ctx.domain);
        if (d <= 2 && ctx.mesh->attributes(geo::Domain(d)).find(toolSelectionAttr(d)))
            return attributeField(toolSelectionAttr(d), SocketType::Bool)->evaluate(ctx);
        if (ctx.mesh->pointAttrs().find(toolSelectionAttr(0)))
            return attributeField(toolSelectionAttr(0), SocketType::Bool)->evaluate(ctx);
        return Column::of(uint8_t(1));
    }
};

void selectionNode()
{
    registerDecl({
        .type = "ToolSelection",
        .label = "Selection",
        .category = "Tool",
        .doc = "The selected elements of the mesh a tool runs on, on whatever domain it is read; everything in object mode and in "
               "modifiers.",
        .sockets = {.outputs = {fout("selection", T::Bool)}},
        .exec = [](ExecContext& c) { c.out("selection", std::make_shared<ToolSelectionField>()); },
    });
}

void setSelectionNode()
{
    registerDecl({
        .type = "ToolSetSelection",
        .label = "Set Selection",
        .category = "Tool",
        .doc = "The component selection a tool leaves behind on this domain once it has run. No effect in modifiers.",
        .props = {prop("domain", selectionDomains(), "face")},
        .sockets = {.inputs = {geoIn(), fin("selection", T::Bool, true)}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                const int d = selectionDomain(c.prop("domain"));
                FieldPtr selection = c.field("selection");
                c.out("geometry", editComponents(c.geometry("geometry"), {.mesh = [&](geo::Mesh& m) {
                                                     std::vector<uint8_t> v = evaluateAll<uint8_t>(selection, FieldContext::forMesh(m, Domain(d)));
                                                     auto out = m.attributes(geo::Domain(d)).getOrAdd<bool>(toolSetSelectionAttr(d));
                                                     for (size_t k = 0; k < out.size() && k < v.size(); ++k)
                                                         out[k] = v[k];
                                                 }}));
            },
    });
}

void mousePositionNode()
{
    registerDecl({
        .type = "MousePosition",
        .label = "Mouse Position",
        .category = "Tool",
        .doc = "Where the pointer was when the tool ran: viewport pixels from the top-left, the viewport's size, and the pointer's ray "
               "in the mesh's own space. Zero in modifiers.",
        .sockets = {.outputs = {out("x", T::Int), out("y", T::Int), out("regionWidth", T::Int), out("regionHeight", T::Int),
                                out("rayOrigin", T::Vector), out("rayDirection", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                ToolContext none;
                const ToolContext& t = c.env.tool ? *c.env.tool : none;
                c.out("x", constantInt(int64_t(std::lround(t.mouse.x))));
                c.out("y", constantInt(int64_t(std::lround(t.mouse.y))));
                c.out("regionWidth", constantInt(int64_t(std::lround(t.region.x))));
                c.out("regionHeight", constantInt(int64_t(std::lround(t.region.y))));
                c.out("rayOrigin", constantVector(t.rayOrigin));
                c.out("rayDirection", constantVector(t.rayDirection));
            },
    });
}

void viewportTransformNode()
{
    registerDecl({
        .type = "ViewportTransform",
        .label = "Viewport Transform",
        .category = "Tool",
        .doc = "The view the tool ran in: world to camera (view) and camera to clip (projection). Identity in modifiers.",
        .sockets = {.outputs = {out("view", T::Matrix), out("projection", T::Matrix), out("isOrthographic", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                ToolContext none;
                const ToolContext& t = c.env.tool ? *c.env.tool : none;
                c.out("view", constantMatrix(t.view));
                c.out("projection", constantMatrix(t.projection));
                c.out("isOrthographic", constantBool(t.orthographic));
            },
    });
}

void activeElementNode()
{
    registerDecl({
        .type = "ActiveElement",
        .label = "Active Element",
        .category = "Tool",
        .doc = "The last picked point, edge or face of the mesh the tool runs on. Exists is off when none is picked, and in modifiers.",
        .props = {prop("domain", selectionDomains(), "face")},
        .sockets = {.outputs = {out("index", T::Int), out("exists", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                const int64_t index = c.env.tool ? c.env.tool->active[size_t(selectionDomain(c.prop("domain")))] : -1;
                c.out("index", constantInt(std::max<int64_t>(index, 0)));
                c.out("exists", constantBool(index >= 0));
            },
    });
}

/** The group input linked straight into the gizmo's value, or null when the value comes from anywhere else. */
const GInput* drivenInput(const ExecContext& c)
{
    for (const GLink& l : c.group.linksInto(c.node.name, "value"))
        if (const GNode* from = c.group.node(l.fromNode); from && from->type == "GroupInput")
            return c.group.input(l.fromSocket);
    return nullptr;
}

/** A spec for the gizmo's driven input, or null when nothing collects gizmos or no group input drives it. */
GizmoSpec* beginGizmo(ExecContext& c, GizmoSpec::Kind kind)
{
    const GInput* in = drivenInput(c);
    if (!c.env.gizmos || !in)
        return nullptr;
    GizmoSpec s;
    s.kind = kind;
    s.node = c.node.name;
    s.param = in->name;
    s.paramType = in->type;
    s.subtype = in->subtype;
    if (auto it = c.env.params.find(in->name); it != c.env.params.end() && it->second.field)
    {
        if (in->type == SocketType::Matrix)
            s.matrix = evaluateSingle<glm::dmat4>(it->second.field);
        else if (in->type == SocketType::Float || in->type == SocketType::Int)
            s.value = evaluateSingle<double>(it->second.field);
    }
    c.env.gizmos->push_back(std::move(s));
    return &c.env.gizmos->back();
}

const char* kGizmoLink = " Link a group input into value; dragging the handle edits that input.";

void linearGizmo()
{
    registerDecl({
        .type = "LinearGizmo",
        .label = "Linear Gizmo",
        .category = "Gizmo",
        .doc = std::string("An arrow in the viewport at position, dragged along direction: one unit of value per unit moved, "
                           "scaled by direction's length.") + kGizmoLink,
        .props = {prop("style", {"arrow", "box", "cross"}, "arrow")},
        .sockets = {.inputs = {in("value", T::Float, 0.0), in("position", T::Vector, vec(0, 0, 0)), in("direction", T::Vector, vec(0, 1, 0))}},
        .exec =
            [](ExecContext& c) {
                if (GizmoSpec* s = beginGizmo(c, GizmoSpec::Kind::Linear))
                {
                    s->position = c.v("position");
                    s->direction = c.v("direction");
                    s->style = c.prop("style");
                }
            },
    });
}

void dialGizmo()
{
    registerDecl({
        .type = "DialGizmo",
        .label = "Dial Gizmo",
        .category = "Gizmo",
        .doc = std::string("A ring in the viewport at position around the up axis, turned to change value by the angle in radians "
                           "(an angle input shows degrees).") + kGizmoLink,
        .sockets = {.inputs = {in("value", T::Float, 0.0), in("position", T::Vector, vec(0, 0, 0)), in("up", T::Vector, vec(0, 1, 0)),
                               bounded(in("radius", T::Float, 1.0, "Ring size; 1 is the default screen size."), 0.1, 10.0)}},
        .exec =
            [](ExecContext& c) {
                if (GizmoSpec* s = beginGizmo(c, GizmoSpec::Kind::Dial))
                {
                    s->position = c.v("position");
                    s->direction = c.v("up");
                    s->radius = std::clamp(c.f("radius"), 0.1, 10.0);
                }
            },
    });
}

void transformGizmo()
{
    registerDecl({
        .type = "TransformGizmo",
        .label = "Transform Gizmo",
        .category = "Gizmo",
        .doc = std::string("Move arrows, rotate rings and scale boxes at a matrix's origin, editing the matrix.") + kGizmoLink,
        .sockets = {.inputs = {in("value", T::Matrix), in("translate", T::Bool, true), in("rotate", T::Bool, true), in("scale", T::Bool, false)}},
        .exec =
            [](ExecContext& c) {
                if (GizmoSpec* s = beginGizmo(c, GizmoSpec::Kind::Transform))
                {
                    s->position = glm::dvec3(s->matrix[3]);
                    s->translate = c.b("translate");
                    s->rotate = c.b("rotate");
                    s->scale = c.b("scale");
                }
            },
    });
}
} // namespace tool_nodes

void registerToolNodes()
{
    using namespace tool_nodes;
    selectionNode();
    setSelectionNode();
    mousePositionNode();
    viewportTransformNode();
    activeElementNode();
    linearGizmo();
    dialGizmo();
    transformGizmo();
}
} // namespace lila::gn::nodes
