#pragma once

#include "curve/CurveNodes.h"
#include "curve/Spline.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * Attr, plug and command helpers the curve node and command files share. Internal to src/curve. The attr builders come
 * with and without a category (the Attribute Editor section); every input is stored, the numeric ones keyable.
 */
namespace lila::curve::helpers
{
using scene::AttrDef;
using scene::PlugType;

inline constexpr uint32_t kIn = scene::AttrInput | scene::AttrStorable;
inline constexpr uint32_t kKey = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable;

inline AttrDef fAttr(std::string name, double def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

inline AttrDef fAttr(std::string name, double def, std::string doc, std::string cat, std::optional<double> lo = std::nullopt,
                     std::optional<double> hi = std::nullopt)
{
    AttrDef a = fAttr(std::move(name), def, std::move(doc), lo, hi);
    a.category = std::move(cat);
    return a;
}

inline AttrDef iAttr(std::string name, int64_t def, std::string doc, double lo = 0.0, std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

inline AttrDef iAttr(std::string name, int64_t def, std::string doc, std::string cat, double lo = 0.0)
{
    AttrDef a = iAttr(std::move(name), def, std::move(doc), lo);
    a.category = std::move(cat);
    return a;
}

inline AttrDef bAttr(std::string name, bool def, std::string doc, std::string cat = {})
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kKey, .doc = std::move(doc), .category = std::move(cat)};
}

inline AttrDef eAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc, std::string cat = {})
{
    return {.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kIn, .enumNames = std::move(names), .doc = std::move(doc),
            .category = std::move(cat)};
}

inline AttrDef vAttr(std::string name, const glm::dvec3& def, std::string doc, std::string cat = {})
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = kKey, .doc = std::move(doc), .category = std::move(cat)};
    for (int k = 0; k < 3; ++k)
        a.childNames[size_t(k)] = name + char('X' + k);
    return a;
}

/** A string input that starts empty; extra adds flags such as AttrHidden or AttrMultiline. */
inline AttrDef sAttr(std::string name, std::string doc, std::string cat = {}, uint32_t extra = 0)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = kIn | extra, .doc = std::move(doc),
            .category = std::move(cat)};
}

inline AttrDef sAttr(std::string name, std::string doc, uint32_t extra) { return sAttr(std::move(name), std::move(doc), {}, extra); }

/** A string input with a starting value. */
inline AttrDef sAttrDefault(std::string name, std::string def, std::string doc, std::string cat = {}, uint32_t extra = 0)
{
    AttrDef a = sAttr(std::move(name), std::move(doc), std::move(cat), extra);
    a.defaultValue = std::move(def);
    return a;
}

inline AttrDef meshIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrInput | scene::AttrHidden, .doc = std::move(doc)};
}

inline AttrDef matrixIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = scene::AttrInput | scene::AttrHidden, .doc = std::move(doc)};
}

/** A hidden data input; stored keeps an unconnected value in the file. */
inline AttrDef dataIn(std::string name, std::string doc, bool stored = true)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = scene::DataPtr(), .flags = (stored ? kIn : scene::AttrInput) | scene::AttrHidden,
            .doc = std::move(doc)};
}

/** Curves in any number, one per element; fileAlias reads the numbered attrs it replaced. */
inline AttrDef dataArray(std::string name, std::string doc, std::string fileAlias)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = scene::DataPtr(), .flags = scene::AttrInput | scene::AttrHidden | scene::AttrArray,
            .doc = std::move(doc), .fileAlias = std::move(fileAlias)};
}

inline AttrDef dataOut(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = scene::DataPtr(), .flags = scene::AttrOutput | scene::AttrHidden, .doc = std::move(doc)};
}

inline AttrDef stringOut(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = scene::AttrOutput, .doc = std::move(doc)};
}

inline AttrDef curveOut() { return dataOut("outCurve", "The curves it makes."); }

inline AttrDef meshOut()
{
    return {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrOutput | scene::AttrHidden,
            .doc = "The mesh it makes."};
}

inline scene::PlugValue meshValue(geo::Mesh m) { return scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))); }

inline int idx(const scene::Node& n, const char* a) { return n.type().attrIndex(a); }
inline double f(const scene::Node& n, const char* a) { return n.as<double>(idx(n, a)); }
inline int64_t i(const scene::Node& n, const char* a) { return n.as<int64_t>(idx(n, a)); }
inline bool b(const scene::Node& n, const char* a) { return n.as<bool>(idx(n, a)); }
inline glm::dvec3 v(const scene::Node& n, const char* a) { return n.as<glm::dvec3>(idx(n, a)); }
inline const std::string& s(const scene::Node& n, const char* a) { return n.as<std::string>(idx(n, a)); }
inline const Splines& in(const scene::Node& n, const char* a) { return splinesOf(n.value(idx(n, a))); }

inline scene::Graph& graph() { return scene::Graph::current(); }

/** Option flags for every editable attr of a node type (enums by name), with no defaults so only given ones apply. */
/** A flag for an editable attr (enums by name) with no default, so it applies only when given; shownDefault carries the attr's. */
inline FlagDef flagFromAttr(const AttrDef& d)
{
    FlagDef fd{d.name, "", ArgType::Float, nullptr, d.doc};
    fd.min = d.minValue;
    fd.max = d.maxValue;
    switch (d.type)
    {
    case PlugType::Bool: fd.type = ArgType::Bool; break;
    case PlugType::Int: fd.type = ArgType::Int; break;
    case PlugType::Vec3: fd.type = ArgType::Vec3; break;
    case PlugType::String: fd.type = ArgType::String; break;
    case PlugType::Enum:
        fd.type = ArgType::String;
        fd.choices = d.enumNames;
        break;
    default: break;
    }
    if (d.type == PlugType::Enum)
    {
        if (const int64_t* k = std::get_if<int64_t>(&d.defaultValue); k && *k >= 0 && size_t(*k) < d.enumNames.size())
            fd.shownDefault = d.enumNames[size_t(*k)];
    }
    else if (d.type == PlugType::Float || d.type == PlugType::Bool || d.type == PlugType::Int || d.type == PlugType::String ||
             d.type == PlugType::Vec3)
        fd.shownDefault = scene::toJson(d.defaultValue);
    return fd;
}

inline std::vector<FlagDef> attrFlags(const std::string& type)
{
    std::vector<FlagDef> out;
    for (const AttrDef& d : scene::types::find(type)->ownAttrs)
        if ((d.flags & scene::AttrInput) && !(d.flags & scene::AttrHidden))
            out.push_back(flagFromAttr(d));
    return out;
}

/** Sets the node's editable attrs from the flags given (enum values by name). */
inline void applyFlags(scene::Graph& g, scene::Node& n, const Args& a, const char* cmd)
{
    for (const AttrDef& d : n.type().ownAttrs)
    {
        if (!(d.flags & scene::AttrInput) || (d.flags & scene::AttrHidden) || !a.has(d.name))
            continue;
        const int at = n.type().attrIndex(d.name);
        const json& val = a.flags[d.name];
        if (d.type == PlugType::Enum && val.is_string())
        {
            auto it = std::ranges::find(d.enumNames, val.get<std::string>());
            if (it == d.enumNames.end())
                throw CommandError(std::string(cmd) + ": " + d.name + ": unknown value '" + val.get<std::string>() + "'");
            g.setValue({n.id(), at, -1}, int64_t(it - d.enumNames.begin()));
            continue;
        }
        try
        {
            g.setValue({n.id(), at, -1}, scene::fromJson(d, val));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + d.name + ": " + e.what());
        }
    }
}

inline std::vector<FlagDef> withFlags(std::vector<FlagDef> base, const std::vector<FlagDef>& extra)
{
    base.insert(base.end(), extra.begin(), extra.end());
    return base;
}

inline FlagDef chFlag() { return {"constructionHistory", "ch", ArgType::Bool, true, "Keep the history node so its settings stay live."}; }

/** Bakes what a history node makes into its destination and drops the node. */
inline void bake(scene::Graph& g, scene::Node& op, const scene::PlugRef& dst)
{
    g.evaluate();
    scene::PlugValue val = g.value(dst);
    g.disconnect(dst);
    g.setValue(dst, val);
    g.deleteNode(op.id());
}

inline scene::Node& curveNamed(scene::Graph& g, const std::string& name, const char* cmd)
{
    scene::Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a curve");
    return *n;
}

inline scene::Node& meshNamed(scene::Graph& g, const std::string& name, const char* cmd)
{
    scene::Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** The given curves, or the selected ones in selection order. */
inline std::vector<scene::Node*> givenOrSelectedCurves(scene::Graph& g, const Args& a, const char* cmd)
{
    std::vector<scene::Node*> out;
    for (const json& p : a.positional)
        out.push_back(&curveNamed(g, p.get<std::string>(), cmd));
    if (out.empty())
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
    return out;
}

/** givenOrSelectedCurves that throws when there are none. */
inline std::vector<scene::Node*> curvesArg(scene::Graph& g, const Args& a, const char* cmd)
{
    std::vector<scene::Node*> out = givenOrSelectedCurves(g, a, cmd);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select a curve");
    return out;
}

/** A new curve node holding s; throws when a spline is invalid. */
inline scene::Node* createCurve(scene::Graph& g, Splines s, const std::string& name)
{
    for (const Spline& sp : s)
    {
        std::string why;
        if (!isValid(sp, &why))
            throw CommandError("curve: " + why);
    }
    scene::Node* n = g.createNode(*scene::types::find("curve"), name.empty() ? "Curve" : name, scene::kNoNode, true);
    g.setValue({n->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(s)));
    return n;
}

/** The curve's own splines (local space) as evaluated now. */
inline Splines storedSplines(scene::Graph& g, scene::Node& n)
{
    g.evaluate();
    return splinesOf(g.value({n.id(), CurveShape::kLocal, -1}));
}
} // namespace lila::curve::helpers
