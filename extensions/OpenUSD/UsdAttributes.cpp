#include "UsdAttributes.h"

#include <lila/Names.h>
#include <lila/Node.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/timeCode.h>
#include <pxr/usd/usd/apiSchemaBase.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <algorithm>
#include <format>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
using scene::AttrDef;
using scene::PlugType;
using scene::PlugValue;

namespace
{
const Sdf_ValueTypeNamesType& T() { return *SdfValueTypeNames; }

bool oneOf(const SdfValueTypeName& t, std::initializer_list<SdfValueTypeName> names)
{
    return std::find(names.begin(), names.end(), t) != names.end();
}

bool isInt(const SdfValueTypeName& t) { return oneOf(t, {T().Int, T().UInt, T().Int64, T().UInt64, T().UChar}); }
bool isFloat(const SdfValueTypeName& t) { return oneOf(t, {T().Half, T().Float, T().Double, T().TimeCode}); }
bool isVec3(const SdfValueTypeName& t)
{
    return oneOf(t, {T().Float3, T().Double3, T().Half3, T().Color3f, T().Color3d, T().Color3h, T().Point3f, T().Point3d, T().Point3h,
                     T().Vector3f, T().Vector3d, T().Vector3h, T().Normal3f, T().Normal3d, T().Normal3h});
}
bool isText(const SdfValueTypeName& t) { return oneOf(t, {T().String, T().Token, T().Asset}); }

std::optional<double> number(const VtValue& v)
{
    if (v.IsHolding<bool>())
        return v.UncheckedGet<bool>() ? 1.0 : 0.0;
    if (v.IsHolding<int>())
        return double(v.UncheckedGet<int>());
    if (v.IsHolding<unsigned int>())
        return double(v.UncheckedGet<unsigned int>());
    if (v.IsHolding<int64_t>())
        return double(v.UncheckedGet<int64_t>());
    if (v.IsHolding<uint64_t>())
        return double(v.UncheckedGet<uint64_t>());
    if (v.IsHolding<unsigned char>())
        return double(v.UncheckedGet<unsigned char>());
    if (v.IsHolding<GfHalf>())
        return double(float(v.UncheckedGet<GfHalf>()));
    if (v.IsHolding<float>())
        return double(v.UncheckedGet<float>());
    if (v.IsHolding<double>())
        return v.UncheckedGet<double>();
    if (v.IsHolding<SdfTimeCode>())
        return v.UncheckedGet<SdfTimeCode>().GetValue();
    return std::nullopt;
}

std::optional<glm::dvec3> vec3(const VtValue& v)
{
    if (v.IsHolding<GfVec3f>())
    {
        const GfVec3f& g = v.UncheckedGet<GfVec3f>();
        return glm::dvec3(g[0], g[1], g[2]);
    }
    if (v.IsHolding<GfVec3d>())
    {
        const GfVec3d& g = v.UncheckedGet<GfVec3d>();
        return glm::dvec3(g[0], g[1], g[2]);
    }
    if (v.IsHolding<GfVec3h>())
    {
        const GfVec3h& g = v.UncheckedGet<GfVec3h>();
        return glm::dvec3(float(g[0]), float(g[1]), float(g[2]));
    }
    return std::nullopt;
}

std::optional<std::string> text(const VtValue& v)
{
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<SdfAssetPath>())
        return v.UncheckedGet<SdfAssetPath>().GetAssetPath();
    return std::nullopt;
}

/** One scalar USD value as a value of the Lila attribute. */
std::optional<PlugValue> plugValue(const VtValue& v, const AttrDef& def)
{
    switch (def.type)
    {
    case PlugType::Bool:
        if (auto n = number(v))
            return PlugValue(*n != 0.0);
        break;
    case PlugType::Int:
        if (auto n = number(v))
            return PlugValue(int64_t(*n));
        break;
    case PlugType::Float:
        if (auto n = number(v))
            return PlugValue(*n);
        break;
    case PlugType::Vec3:
        if (auto g = vec3(v))
            return PlugValue(*g);
        break;
    case PlugType::Matrix:
        if (v.IsHolding<GfMatrix4d>())
        {
            const GfMatrix4d& m = v.UncheckedGet<GfMatrix4d>();
            glm::dmat4 out;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    out[r][c] = m[r][c];
            return PlugValue(out);
        }
        break;
    case PlugType::String:
        if (auto s = text(v))
            return PlugValue(*s);
        break;
    case PlugType::Enum:
        if (auto s = text(v))
        {
            auto it = std::find(def.enumNames.begin(), def.enumNames.end(), *s);
            return PlugValue(int64_t(it == def.enumNames.end() ? 0 : it - def.enumNames.begin()));
        }
        break;
    default: break;
    }
    return std::nullopt;
}

template <class A> std::optional<std::vector<PlugValue>> arrayValues(const VtValue& v, const AttrDef& def)
{
    if (!v.IsHolding<A>())
        return std::nullopt;
    std::vector<PlugValue> out;
    for (const auto& item : v.UncheckedGet<A>())
    {
        std::optional<PlugValue> p = plugValue(VtValue(item), def);
        if (!p)
            return std::nullopt;
        out.push_back(std::move(*p));
    }
    return out;
}

double asNumber(const PlugValue& p)
{
    if (const bool* b = std::get_if<bool>(&p))
        return *b ? 1.0 : 0.0;
    if (const int64_t* i = std::get_if<int64_t>(&p))
        return double(*i);
    if (const double* d = std::get_if<double>(&p))
        return *d;
    return 0.0;
}

std::string asText(const PlugValue& p, const AttrDef& def)
{
    if (const std::string* s = std::get_if<std::string>(&p))
        return *s;
    if (const int64_t* i = std::get_if<int64_t>(&p); i && *i >= 0 && size_t(*i) < def.enumNames.size())
        return def.enumNames[size_t(*i)];
    return {};
}

glm::dvec3 asVec3(const PlugValue& p)
{
    const glm::dvec3* v = std::get_if<glm::dvec3>(&p);
    return v ? *v : glm::dvec3(0.0);
}

/** One Lila value as a scalar of USD type t. */
VtValue scalar(const PlugValue& p, const AttrDef& def, const SdfValueTypeName& t)
{
    if (t == T().Bool)
        return VtValue(asNumber(p) != 0.0);
    if (t == T().Int)
        return VtValue(int(asNumber(p)));
    if (t == T().UInt)
        return VtValue((unsigned int)(std::max(0.0, asNumber(p))));
    if (t == T().Int64)
        return VtValue(int64_t(asNumber(p)));
    if (t == T().UInt64)
        return VtValue(uint64_t(std::max(0.0, asNumber(p))));
    if (t == T().UChar)
        return VtValue((unsigned char)(std::clamp(asNumber(p), 0.0, 255.0)));
    if (t == T().Half)
        return VtValue(GfHalf(float(asNumber(p))));
    if (t == T().Float)
        return VtValue(float(asNumber(p)));
    if (t == T().Double)
        return VtValue(asNumber(p));
    if (t == T().TimeCode)
        return VtValue(SdfTimeCode(asNumber(p)));
    if (t == T().String)
        return VtValue(asText(p, def));
    if (t == T().Token)
        return VtValue(TfToken(asText(p, def)));
    if (t == T().Asset)
        return VtValue(SdfAssetPath(asText(p, def)));
    if (isVec3(t))
    {
        const glm::dvec3 v = asVec3(p);
        if (t.GetCPPTypeName() == "GfVec3f")
            return VtValue(GfVec3f(float(v.x), float(v.y), float(v.z)));
        if (t.GetCPPTypeName() == "GfVec3h")
            return VtValue(GfVec3h(float(v.x), float(v.y), float(v.z)));
        return VtValue(GfVec3d(v.x, v.y, v.z));
    }
    if (t == T().Matrix4d)
    {
        const glm::dmat4* m = std::get_if<glm::dmat4>(&p);
        GfMatrix4d out(1.0);
        if (m)
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    out[r][c] = (*m)[r][c];
        return VtValue(out);
    }
    return {};
}

template <class E> VtValue array(const std::vector<PlugValue>& values, const AttrDef& def, const SdfValueTypeName& scalarType)
{
    VtArray<E> out;
    out.reserve(values.size());
    for (const PlugValue& p : values)
    {
        VtValue v = scalar(p, def, scalarType);
        if (!v.IsHolding<E>())
            return {};
        out.push_back(v.UncheckedGet<E>());
    }
    return VtValue(out);
}

/** The applied schema's definition and its instance name ("" for single-apply). */
std::pair<const UsdPrimDefinition*, TfToken> definitionOf(const TfToken& applied)
{
    const auto [typeName, instance] = UsdSchemaRegistry::GetTypeNameAndInstance(applied);
    return {UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition(typeName), instance};
}

TfToken instanced(const TfToken& name, const TfToken& instance)
{
    return instance.IsEmpty() ? name : UsdSchemaRegistry::MakeMultipleApplyNameInstance(name, instance);
}

void setValues(scene::Graph& g, scene::NodeId id, int attr, const AttrDef& def, const std::vector<PlugValue>& values)
{
    if (def.flags & scene::AttrArray)
        g.setElements({id, attr, -1}, values);
    else if (!values.empty())
        g.setValue({id, attr, -1}, values.front());
}

/** The node's attr for an imported one: added, or its own when it already holds that kind of value. -1 when it can't. */
int componentAttr(scene::Graph& g, scene::NodeId id, const AttrDef& want, std::string& why)
{
    const scene::Node* n = g.node(id);
    const int have = n->type().attrIndex(want.name);
    if (have < 0)
        return g.addAttr(id, want);
    const AttrDef& d = n->type().attrs[size_t(have)];
    if (!(d.flags & scene::AttrUser))
    {
        why = "Lila's own attribute of that name";
        return -1;
    }
    if (d.type != want.type || (d.flags & scene::AttrArray) != (want.flags & scene::AttrArray))
    {
        why = "an attribute of that name holding another type";
        return -1;
    }
    return have;
}
} // namespace

bool lilaSchema(const TfToken& applied)
{
    static const std::set<std::string> kOwn{"MaterialBindingAPI", "SkelBindingAPI", "LightAPI",      "ShadowAPI",      "ShapingAPI",
                                            "LightListAPI",       "ListAPI",        "MeshLightAPI",  "VolumeLightAPI", "GeomModelAPI",
                                            "MotionAPI",          "PrimvarsAPI",    "XformCommonAPI", "VisibilityAPI", "CollectionAPI",
                                            "ConnectableAPI",     "CoordSysAPI",    "NodeDefAPI",    "ModelAPI",       "ClipsAPI"};
    return kOwn.count(UsdSchemaRegistry::GetTypeNameAndInstance(applied).first.GetString()) > 0;
}

TfTokenVector componentSchemas(const TfTokenVector& applied)
{
    TfTokenVector out;
    for (const TfToken& t : applied)
        if (!lilaSchema(t))
            out.push_back(t);
    return out;
}

nlohmann::json componentSchemaList()
{
    nlohmann::json out = nlohmann::json::array();
    std::set<TfType> types;
    TfType::Find<UsdAPISchemaBase>().GetAllDerivedTypes(&types);
    std::vector<std::pair<std::string, TfType>> named;
    for (const TfType& t : types)
        if (UsdSchemaRegistry::GetSchemaKind(t) == UsdSchemaKind::SingleApplyAPI)
            if (const TfToken name = UsdSchemaRegistry::GetSchemaTypeName(t); !name.IsEmpty() && !lilaSchema(name))
                named.emplace_back(name.GetString(), t);
    std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [name, type] : named)
    {
        const UsdPrimDefinition* def = UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition(TfToken(name));
        if (!def)
            continue;
        nlohmann::json attrs = nlohmann::json::array();
        for (const TfToken& n : def->GetPropertyNames())
        {
            const UsdPrimDefinition::Attribute ad = def->GetAttributeDefinition(n);
            if (!ad)
                continue;
            VtTokenArray allowed;
            ad.GetMetadata(SdfFieldKeys->AllowedTokens, &allowed);
            std::optional<AttrDef> a = attrDefFor(n.GetString(), ad.GetTypeName(), allowed);
            if (!a)
                continue;
            VtValue fallback;
            if (ad.GetFallbackValue(&fallback))
                if (std::optional<std::vector<PlugValue>> v = plugValues(fallback, *a); v && v->size() == 1 && !(a->flags & scene::AttrArray))
                    a->defaultValue = v->front();
            a->doc = ad.GetDocumentation();
            if (std::string nice; ad.GetMetadata(SdfFieldKeys->DisplayName, &nice))
                a->niceName = nice;
            attrs.push_back(scene::attrDefToJson(*a));
        }
        nlohmann::json schema = {{"name", name}, {"doc", def->GetDocumentation()}, {"attrs", std::move(attrs)}};
        if (std::string label; def->GetMetadata(SdfFieldKeys->DisplayName, &label) && !label.empty())
            schema["label"] = label;
        out.push_back(std::move(schema));
    }
    return out;
}

UsdPrimDefinition::Attribute schemaAttribute(const TfToken& applied, const TfToken& name)
{
    const auto [def, instance] = definitionOf(applied);
    if (!def)
        return {};
    for (const TfToken& n : def->GetPropertyNames())
        if (instanced(n, instance) == name)
            return def->GetAttributeDefinition(n);
    return {};
}

std::set<TfToken> schemaAttributeNames(const TfTokenVector& schemas)
{
    std::set<TfToken> out;
    for (const TfToken& s : schemas)
    {
        const auto [def, instance] = definitionOf(s);
        if (!def)
            continue;
        for (const TfToken& n : def->GetPropertyNames())
            if (def->GetAttributeDefinition(n))
                out.insert(instanced(n, instance));
    }
    return out;
}

std::optional<AttrDef> attrDefFor(const std::string& name, const SdfValueTypeName& type, const VtTokenArray& allowed)
{
    if (!scene::Graph::validName(name))
        return std::nullopt;
    const SdfValueTypeName s = type.IsArray() ? type.GetScalarType() : type;
    AttrDef def;
    def.name = name;
    if (s == T().Bool && !type.IsArray())
        def.type = PlugType::Bool;
    else if (isInt(s))
        def.type = PlugType::Int;
    else if (isFloat(s))
        def.type = PlugType::Float;
    else if (isVec3(s))
    {
        def.type = PlugType::Vec3;
        if (s.GetRole() == SdfValueRoleNames->Color)
            def.flags |= scene::AttrColor;
    }
    else if (s == T().Matrix4d && !type.IsArray())
        def.type = PlugType::Matrix;
    else if (s == T().Token && !type.IsArray() && !allowed.empty())
    {
        def.type = PlugType::Enum;
        for (const TfToken& a : allowed)
            def.enumNames.push_back(a.GetString());
    }
    else if (isText(s))
    {
        def.type = PlugType::String;
        if (s == T().Asset && !type.IsArray())
            def.flags |= scene::AttrFile;
    }
    else
        return std::nullopt;
    if (type.IsArray())
        def.flags |= scene::AttrArray;
    def.defaultValue = scene::defaultFor(def.type);
    return def;
}

std::optional<std::vector<PlugValue>> plugValues(const VtValue& v, const AttrDef& def)
{
    if (v.IsEmpty())
        return std::vector<PlugValue>{};
    if (!v.IsArrayValued())
    {
        std::optional<PlugValue> p = plugValue(v, def);
        if (!p)
            return std::nullopt;
        return std::vector<PlugValue>{std::move(*p)};
    }
    for (auto r : {arrayValues<VtIntArray>(v, def), arrayValues<VtUIntArray>(v, def), arrayValues<VtInt64Array>(v, def),
                   arrayValues<VtUInt64Array>(v, def), arrayValues<VtUCharArray>(v, def), arrayValues<VtHalfArray>(v, def),
                   arrayValues<VtFloatArray>(v, def), arrayValues<VtDoubleArray>(v, def), arrayValues<VtVec3fArray>(v, def),
                   arrayValues<VtVec3dArray>(v, def), arrayValues<VtVec3hArray>(v, def), arrayValues<VtStringArray>(v, def),
                   arrayValues<VtTokenArray>(v, def), arrayValues<VtArray<SdfAssetPath>>(v, def), arrayValues<VtArray<SdfTimeCode>>(v, def)})
        if (r)
            return r;
    return std::nullopt;
}

VtValue usdValue(const std::vector<PlugValue>& values, const AttrDef& def, const SdfValueTypeName& type)
{
    if (!type.IsArray())
        return values.empty() ? scalar(def.defaultValue, def, type) : scalar(values.front(), def, type);
    const SdfValueTypeName s = type.GetScalarType();
    if (s == T().Int)
        return array<int>(values, def, s);
    if (s == T().UInt)
        return array<unsigned int>(values, def, s);
    if (s == T().Int64)
        return array<int64_t>(values, def, s);
    if (s == T().UInt64)
        return array<uint64_t>(values, def, s);
    if (s == T().UChar)
        return array<unsigned char>(values, def, s);
    if (s == T().Half)
        return array<GfHalf>(values, def, s);
    if (s == T().Float)
        return array<float>(values, def, s);
    if (s == T().Double)
        return array<double>(values, def, s);
    if (s == T().TimeCode)
        return array<SdfTimeCode>(values, def, s);
    if (s == T().String)
        return array<std::string>(values, def, s);
    if (s == T().Token)
        return array<TfToken>(values, def, s);
    if (s == T().Asset)
        return array<SdfAssetPath>(values, def, s);
    if (isVec3(s))
    {
        if (s.GetCPPTypeName() == "GfVec3f")
            return array<GfVec3f>(values, def, s);
        if (s.GetCPPTypeName() == "GfVec3h")
            return array<GfVec3h>(values, def, s);
        return array<GfVec3d>(values, def, s);
    }
    return {};
}

SdfValueTypeName usdTypeFor(const AttrDef& def)
{
    const bool arr = def.flags & scene::AttrArray;
    SdfValueTypeName s;
    switch (def.type)
    {
    case PlugType::Bool: s = T().Bool; break;
    case PlugType::Int: s = T().Int; break;
    case PlugType::Float: s = T().Double; break;
    case PlugType::Vec3: s = (def.flags & scene::AttrColor) ? T().Color3f : T().Double3; break;
    case PlugType::Matrix: s = T().Matrix4d; break;
    case PlugType::Enum: s = T().Token; break;
    case PlugType::String: s = (def.flags & scene::AttrFile) ? T().Asset : T().String; break;
    default: return {};
    }
    return arr ? s.GetArrayType() : s;
}

std::vector<PlugValue> nodeValues(const scene::Node& n, int attr)
{
    if (!(n.type().attrs[size_t(attr)].flags & scene::AttrArray))
        return {n.value(attr)};
    std::vector<PlugValue> out;
    for (const auto& [index, value] : n.elements(attr))
        out.push_back(value);
    return out;
}

void importComponents(scene::Graph& g, scene::NodeId id, const UsdPrim& prim, UsdTimeCode t, std::vector<std::string>& skipped)
{
    const TfTokenVector schemas = componentSchemas(prim.GetAppliedSchemas());
    const UsdPrimDefinition& primDef = prim.GetPrimDefinition();

    auto bring = [&](const UsdAttribute& a, const SdfValueTypeName& type, const VtTokenArray& allowed, const std::string& category, const std::string& doc) {
        std::optional<AttrDef> def = attrDefFor(a.GetName().GetString(), type, allowed);
        VtValue v;
        a.Get(&v, t);
        std::optional<std::vector<PlugValue>> values = def ? plugValues(v, *def) : std::nullopt;
        if (!values)
        {
            skipped.push_back(std::format("{} ({})", a.GetPath().GetString(), type.GetAsToken().GetString()));
            return;
        }
        def->category = category;
        def->doc = doc;
        if (std::string nice; a.GetMetadata(SdfFieldKeys->DisplayName, &nice))
            def->niceName = nice;
        std::string why;
        const int attr = componentAttr(g, id, *def, why);
        if (attr < 0)
        {
            skipped.push_back(std::format("{}: the node has {}", a.GetPath().GetString(), why));
            return;
        }
        setValues(g, id, attr, g.node(id)->type().attrs[size_t(attr)], *values);
    };

    std::set<TfToken> done;
    for (const TfToken& s : schemas)
    {
        const auto [def, instance] = definitionOf(s);
        if (!def)
            continue;
        for (const TfToken& n : def->GetPropertyNames())
        {
            const UsdPrimDefinition::Attribute ad = def->GetAttributeDefinition(n);
            const TfToken name = instanced(n, instance);
            if (!ad || !done.insert(name).second)
                continue;
            VtTokenArray allowed;
            ad.GetMetadata(SdfFieldKeys->AllowedTokens, &allowed);
            const std::string category = UsdSchemaRegistry::GetTypeNameAndInstance(s).first.GetString() +
                                         (instance.IsEmpty() ? std::string() : ":" + instance.GetString());
            bring(prim.GetAttribute(name), ad.GetTypeName(), allowed, category, ad.GetDocumentation());
        }
    }
    // Authored attributes nothing on the prim defines: custom ones, and those of schemas this USD doesn't know.
    for (const UsdAttribute& a : prim.GetAuthoredAttributes())
    {
        if (done.count(a.GetName()) || primDef.GetAttributeDefinition(a.GetName()) || UsdGeomPrimvar::IsPrimvar(a) || UsdGeomXformOp::IsXformOp(a))
            continue;
        VtTokenArray allowed;
        a.GetMetadata(SdfFieldKeys->AllowedTokens, &allowed);
        bring(a, a.GetTypeName(), allowed, {}, a.GetDocumentation());
    }

    const scene::Node* n = g.node(id);
    if (schemas.empty() && n->type().attrIndex(kSchemasAttr) < 0)
        return;
    AttrDef list;
    list.name = kSchemasAttr;
    list.type = PlugType::String;
    list.defaultValue = std::string();
    list.flags |= scene::AttrArray;
    list.niceName = "USD API Schemas";
    list.doc = "The API schemas applied to this object's prim that Lila keeps as attributes; export applies them again.";
    std::string why;
    const int attr = componentAttr(g, id, list, why);
    if (attr < 0)
        return;
    std::vector<PlugValue> names;
    for (const TfToken& s : schemas)
        names.push_back(s.GetString());
    g.setElements({id, attr, -1}, names);
}

void exportComponents(const scene::Graph& g, scene::NodeId id, const UsdPrim& prim)
{
    const scene::Node* n = g.node(id);
    if (!n)
        return;
    TfTokenVector schemas;
    if (const int list = n->type().attrIndex(kSchemasAttr); list >= 0)
        for (const PlugValue& p : nodeValues(*n, list))
            if (const std::string* s = std::get_if<std::string>(&p); s && !s->empty())
                schemas.push_back(TfToken(*s));
    for (const TfToken& s : schemas)
        prim.AddAppliedSchema(s);
    const std::vector<AttrDef>& attrs = n->type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
    {
        const AttrDef& def = attrs[size_t(i)];
        // A compound's children ("group.child") have no USD name.
        // The real names an import replaced stay in the scene for the real-name check and never leave it.
        if (!(def.flags & scene::AttrUser) || def.name == kSchemasAttr || def.name == "lilaComponentExport" || def.name == lila::names::kSourceAttr ||
            def.name.find('.') != std::string::npos)
            continue;
        const TfToken name(def.name);
        UsdPrimDefinition::Attribute schemaDef;
        for (const TfToken& s : schemas)
            if ((schemaDef = schemaAttribute(s, name)))
                break;
        // Names the prim's type already defines belong to what Lila writes itself.
        if (!schemaDef && prim.GetPrimDefinition().GetAttributeDefinition(name))
            continue;
        const SdfValueTypeName type = schemaDef ? schemaDef.GetTypeName() : usdTypeFor(def);
        if (!type)
            continue;
        const VtValue v = usdValue(nodeValues(*n, i), def, type);
        if (v.IsEmpty())
            continue;
        if (schemaDef)
        {
            VtValue fallback;
            if (schemaDef.GetFallbackValue(&fallback) && fallback == v)
                continue;
        }
        prim.CreateAttribute(name, type, !schemaDef).Set(v);
    }
}

TfTokenVector specComponentSchemas(const SdfPrimSpecHandle& spec)
{
    if (!spec)
        return {};
    const VtValue v = spec->GetInfo(UsdTokens->apiSchemas);
    if (!v.IsHolding<SdfTokenListOp>())
        return {};
    TfTokenVector applied;
    v.UncheckedGet<SdfTokenListOp>().ApplyOperations(&applied);
    return componentSchemas(applied);
}

std::set<std::string> componentAttributes(const SdfPrimSpecHandle& spec)
{
    std::set<std::string> out;
    if (!spec)
        return out;
    for (const TfToken& n : schemaAttributeNames(specComponentSchemas(spec)))
        out.insert(n.GetString());
    for (const SdfAttributeSpecHandle& a : spec->GetAttributes())
        if (a->IsCustom())
            out.insert(a->GetName());
    return out;
}
} // namespace lila::usd
