#include "Template.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace rigt
{
namespace
{
constexpr double kEps = 1e-9;

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
json quat(const glm::dquat& q) { return json::array({q.x, q.y, q.z, q.w}); }

glm::dvec3 checkedVec(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument(std::string(what) + " must be [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

glm::dquat quatOf(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 4)
        throw std::invalid_argument(std::string(what) + " must be [x, y, z, w]");
    glm::dquat q(j[3].get<double>(), j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
    // Hand-edited values get normalized; written ones load bit for bit.
    return std::abs(glm::length(q) - 1.0) > 1e-9 ? glm::normalize(q) : q;
}

template <class E> struct Names
{
    E value;
    const char* name;
};
constexpr Names<Placement> kPlacements[] = {{Placement::Free, "free"}, {Placement::Parent, "parent"}, {Placement::Follow, "follow"}, {Placement::Segment, "segment"}};
constexpr Names<Orient> kOrients[] = {{Orient::Aim, "aim"}, {Orient::Parent, "parent"}, {Orient::Follow, "follow"}, {Orient::Fixed, "fixed"}};
constexpr Names<UpType> kUpTypes[] = {{UpType::World, "world"}, {UpType::Plane, "plane"}, {UpType::Parent, "parent"}};

template <class E, size_t N> const char* nameOf(const Names<E> (&table)[N], E v)
{
    for (const auto& n : table)
        if (n.value == v)
            return n.name;
    return table[0].name;
}

template <class E, size_t N> E valueOf(const Names<E> (&table)[N], const std::string& s, const char* what)
{
    for (const auto& n : table)
        if (s == n.name)
            return n.value;
    throw std::invalid_argument(std::string("unknown ") + what + " '" + s + "'");
}

/** rig limits: marker -> {type, axis, min, max, cone, stiffness, preferredAngle}, with ikLimit's names and units. */
void validateLimits(const Template& t)
{
    if (!t.rig.contains("limits"))
        return;
    const json& limits = t.rig["limits"];
    if (!limits.is_object())
        throw std::invalid_argument("rig limits should map markers to limits");
    auto oneOf = [](const json& v, std::initializer_list<const char*> names) {
        if (!v.is_string())
            return false;
        for (const char* n : names)
            if (v.get<std::string>() == n)
                return true;
        return false;
    };
    for (const auto& [m, l] : limits.items())
    {
        const std::string where = "rig limits of " + m;
        if (!t.marker(m))
            throw std::invalid_argument("rig limits names unknown marker " + m);
        if (!l.is_object())
            throw std::invalid_argument(where + " should be an object");
        if (l.contains("type") && !oneOf(l["type"], {"none", "hinge", "cone"}))
            throw std::invalid_argument(where + ": type should be none, hinge or cone");
        if (l.contains("axis") && !oneOf(l["axis"], {"x", "y", "z"}))
            throw std::invalid_argument(where + ": axis should be x, y or z");
        for (const char* k : {"min", "max", "cone", "stiffness"})
            if (l.contains(k) && !l[k].is_number())
                throw std::invalid_argument(where + ": " + k + " should be a number");
        if (l.contains("preferredAngle"))
            checkedVec(l["preferredAngle"], (where + " preferredAngle").c_str());
        for (const auto& [k, v] : l.items())
            if (k != "type" && k != "axis" && k != "min" && k != "max" && k != "cone" && k != "stiffness" && k != "preferredAngle")
                throw std::invalid_argument(where + ": unknown key " + k);
    }
}

std::string stripNamespace(const std::string& s)
{
    size_t c = s.rfind(':');
    return c == std::string::npos ? s : s.substr(c + 1);
}

/** Rotation taking the signed local axes primary and secondary onto aim and (the part of up across aim). */
std::optional<glm::dquat> frameFor(const glm::dvec3& primary, const glm::dvec3& secondary, glm::dvec3 aim, glm::dvec3 up)
{
    if (glm::length(aim) < kEps)
        return std::nullopt;
    aim = glm::normalize(aim);
    up -= aim * glm::dot(up, aim);
    if (glm::length(up) < 1e-6)
        return std::nullopt;
    up = glm::normalize(up);
    glm::dmat3 world(aim, up, glm::cross(aim, up));
    glm::dmat3 local(primary, secondary, glm::cross(primary, secondary));
    return glm::normalize(glm::quat_cast(world * glm::transpose(local)));
}

glm::dvec3 planeNormal(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) { return glm::cross(b - a, c - b); }

// Labels in UE's vocabulary and names other tools give the same joints.
struct AliasRow
{
    std::string label;
    std::vector<std::string> names;
};

const std::vector<AliasRow>& aliasTable()
{
    static const std::vector<AliasRow> table = [] {
        std::vector<AliasRow> t = {
            {"root", {"Root", "Reference", "root_jnt"}},
            {"pelvis", {"Hips", "Pelvis", "Bip01_Pelvis", "hip", "COG"}},
            {"spine_01", {"Spine", "Spine01", "Bip01_Spine", "spine1", "spine_1"}},
            {"spine_02", {"Spine1", "Spine02", "Bip01_Spine1", "spine2", "spine_2"}},
            {"spine_03", {"Spine2", "Spine03", "Bip01_Spine2", "spine3", "spine_3", "Chest"}},
            {"spine_04", {"Spine3", "Spine04", "spine4", "spine_4", "UpperChest"}},
            {"spine_05", {"Spine4", "Spine05", "spine5", "spine_5"}},
            {"neck_01", {"Neck", "Neck1", "Bip01_Neck", "neck"}},
            {"neck_02", {"Neck2", "Neck_02"}},
            {"head", {"Head", "Bip01_Head"}},
        };
        struct Side
        {
            const char* ue;
            const char* word;
            const char* letter;
        };
        const Side sides[] = {{"l", "Left", "L"}, {"r", "Right", "R"}};
        for (const Side& s : sides)
        {
            std::string W = s.word, L = s.letter, u = s.ue;
            auto add = [&](const std::string& label, std::vector<std::string> names) { t.push_back({label + "_" + u, std::move(names)}); };
            add("clavicle", {W + "Shoulder", W + "Clavicle", "Bip01_" + L + "_Clavicle", "clavicle_" + L, L + "_clavicle"});
            add("upperarm", {W + "Arm", W + "UpperArm", "Bip01_" + L + "_UpperArm", "shoulder_" + L, L + "_shoulder", "upperArm_" + L});
            add("lowerarm", {W + "ForeArm", W + "LowerArm", "Bip01_" + L + "_Forearm", "elbow_" + L, L + "_elbow", "forearm_" + L});
            add("hand", {W + "Hand", "Bip01_" + L + "_Hand", "wrist_" + L, L + "_wrist", "hand_" + L});
            add("thigh", {W + "UpLeg", W + "UpperLeg", W + "Thigh", "Bip01_" + L + "_Thigh", "hip_" + L, L + "_hip"});
            add("calf", {W + "Leg", W + "LowerLeg", W + "Shin", W + "Calf", "Bip01_" + L + "_Calf", "knee_" + L, L + "_knee"});
            add("foot", {W + "Foot", "Bip01_" + L + "_Foot", "ankle_" + L, L + "_ankle"});
            add("ball", {W + "ToeBase", W + "Toes", W + "Toe", "Bip01_" + L + "_Toe0", "toe_" + L, L + "_toe"});
            const char* fingers[] = {"thumb", "index", "middle", "ring", "pinky"};
            const char* mixamo[] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
            for (int f = 0; f < 5; ++f)
                for (int k = 1; k <= 3; ++k)
                {
                    std::string label = std::string(fingers[f]) + "_0" + std::to_string(k);
                    add(label, {W + "Hand" + mixamo[f] + std::to_string(k), W + mixamo[f] + std::to_string(k), std::string(fingers[f]) + std::to_string(k) + "_" + L});
                }
            for (int f = 1; f < 5; ++f)
                add(std::string(fingers[f]) + "_metacarpal", {W + "Hand" + mixamo[f] + "0", W + "In" + mixamo[f]});
        }
        return t;
    }();
    return table;
}

/** Joint names that are deformation helpers rather than the skeleton's main chain. */
bool looksHelper(const std::string& name)
{
    std::string n = lower(stripNamespace(name));
    for (const char* key : {"twist", "corrective", "twistcor", "_in_", "_out_", "_fwd", "_bck", "bicep", "tricep", "knee", "scap", "_pec",
                            "latissimus", "weapon", "wrist_inner", "wrist_outer", "interaction", "center_of_mass", "_end", "end_", "_tip",
                            "_helper", "roll"})
        if (n.find(key) != std::string::npos)
            return true;
    return n.ends_with("_in_l") || n.ends_with("_in_r") || n.ends_with("_out_l") || n.ends_with("_out_r");
}

/** UE ik bones: ik_hand_l follows hand_l, ik_hand_gun the right hand. */
std::string followFor(const std::string& name, const std::set<std::string>& names)
{
    std::string n = stripNamespace(name);
    if (!lower(n).starts_with("ik_"))
        return {};
    std::string rest = n.substr(3);
    if (names.contains(rest))
        return rest;
    if (lower(rest) == "hand_gun" && names.contains("hand_r"))
        return "hand_r";
    return {};
}

std::string limbFor(const std::string& label, const std::string& name)
{
    const std::string l = label.empty() ? lower(stripNamespace(name)) : label;
    std::string side = l.ends_with("_l") ? "_l" : l.ends_with("_r") ? "_r" : "";
    auto has = [&](const char* k) { return l.find(k) != std::string::npos; };
    if (has("pelvis") || has("spine") || has("neck") || has("head"))
        return "spine";
    if (has("clavicle") || has("upperarm") || has("lowerarm") || has("hand"))
        return "arm" + side;
    if (has("thigh") || has("calf") || has("foot") || has("ball"))
        return "leg" + side;
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky"})
        if (has(f))
            return std::string(f) + side;
    return {};
}

/** The signed local axis (of rotation r) closest to world direction d. */
std::string closestAxis(const glm::dquat& r, const glm::dvec3& d, const std::string& except = {})
{
    const char* names[] = {"x", "y", "z"};
    std::string best;
    double bestDot = -2.0;
    for (int a = 0; a < 3; ++a)
    {
        if (!except.empty() && except.back() == names[a][0])
            continue;
        glm::dvec3 e(0.0);
        e[a] = 1.0;
        double dd = glm::dot(r * e, glm::normalize(d));
        if (std::abs(dd) > bestDot)
        {
            bestDot = std::abs(dd);
            best = dd < 0 ? std::string("-") + names[a] : std::string(names[a]);
        }
    }
    return best;
}
} // namespace

glm::dvec3 axisOf(const std::string& axis)
{
    bool neg = !axis.empty() && axis[0] == '-';
    std::string a = lower(neg ? axis.substr(1) : axis);
    glm::dvec3 v(0.0);
    if (a == "x")
        v.x = 1;
    else if (a == "y")
        v.y = 1;
    else if (a == "z")
        v.z = 1;
    else
        throw std::invalid_argument("axis must be x, y or z (optionally signed), got '" + axis + "'");
    return neg ? -v : v;
}

std::string sideOf(const std::string& name)
{
    std::string n = lower(stripNamespace(name));
    if (n.ends_with("_l") || n.starts_with("l_") || n.starts_with("left") || n.find("_l_") != std::string::npos || n.find("left") != std::string::npos)
        return "left";
    if (n.ends_with("_r") || n.starts_with("r_") || n.starts_with("right") || n.find("_r_") != std::string::npos || n.find("right") != std::string::npos)
        return "right";
    return "center";
}

std::string labelFor(const std::string& jointName)
{
    const std::string n = lower(stripNamespace(jointName));
    for (const AliasRow& r : aliasTable())
    {
        if (n == lower(r.label))
            return r.label;
        for (const std::string& a : r.names)
            if (n == lower(a))
                return r.label;
    }
    return {};
}

std::vector<std::string> aliasesFor(const std::string& label)
{
    for (const AliasRow& r : aliasTable())
        if (label == r.label)
            return r.names;
    return {};
}

bool Marker::hasFlag(const std::string& f) const { return std::find(flags.begin(), flags.end(), f) != flags.end(); }

const Marker* Template::marker(const std::string& n) const
{
    for (const Marker& m : markers)
        if (m.name == n)
            return &m;
    return nullptr;
}

Marker* Template::marker(const std::string& n) { return const_cast<Marker*>(std::as_const(*this).marker(n)); }

const Limb* Template::limb(const std::string& n) const
{
    for (const Limb& l : limbs)
        if (l.name == n)
            return &l;
    return nullptr;
}

json Template::toJson() const
{
    json j = extra.is_object() ? extra : json::object();
    j["format"] = "lilarig";
    j["version"] = version;
    j["id"] = id;
    j["name"] = name;
    j["description"] = description;
    j["source"] = source;
    j["lateral"] = vec(lateral);
    json ms = json::array();
    for (const Marker& m : markers)
    {
        json o = {{"name", m.name}, {"parent", m.parent}, {"joint", m.joint}, {"label", m.label}, {"side", m.side},
                  {"position", vec(m.position)}, {"orientation", quat(m.orientation)}, {"radius", m.radius},
                  {"centerInMesh", m.centerInMesh}, {"placement", nameOf(kPlacements, m.placement)}, {"orient", nameOf(kOrients, m.orient)},
                  {"offset", quat(m.offset)}};
        if (!m.mirror.empty())
            o["mirror"] = m.mirror;
        if (!m.limb.empty())
            o["limb"] = m.limb;
        if (!m.flags.empty())
            o["flags"] = m.flags;
        if (!m.follow.empty())
            o["follow"] = m.follow;
        if (m.placement == Placement::Segment)
        {
            o["to"] = m.to;
            o["t"] = m.t;
        }
        if (m.placement != Placement::Free)
            o["localOffset"] = vec(m.localOffset);
        if (m.orient == Orient::Aim)
        {
            o["aimAt"] = m.aimAt;
            o["aim"] = m.aim;
            o["up"] = m.up;
            o["upType"] = nameOf(kUpTypes, m.upType);
            o["upVector"] = vec(m.upVector);
        }
        ms.push_back(std::move(o));
    }
    j["markers"] = std::move(ms);
    json ls = json::array();
    for (const Limb& l : limbs)
        ls.push_back({{"name", l.name}, {"side", l.side}, {"markers", l.markers}, {"rule", {{"upType", nameOf(kUpTypes, l.upType)}}}});
    j["limbs"] = std::move(ls);
    j["picker"] = picker;
    j["poses"] = poses;
    j["aliases"] = aliases;
    if (!spaces.empty())
        j["spaces"] = spaces;
    if (!rig.empty())
        j["rig"] = rig;
    return j;
}

Template Template::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a rig template is a JSON object");
    if (j.value("format", "lilarig") != "lilarig")
        throw std::invalid_argument("format must be 'lilarig'");
    Template t;
    t.id = j.value("id", "");
    if (t.id.empty())
        throw std::invalid_argument("'id' is missing");
    t.name = j.value("name", t.id);
    t.description = j.value("description", "");
    t.version = j.value("version", 1);
    t.source = j.value("source", json::object());
    if (j.contains("lateral"))
        t.lateral = checkedVec(j["lateral"], "lateral");
    if (!j.contains("markers") || !j["markers"].is_array() || j["markers"].empty())
        throw std::invalid_argument("'markers' must be a non-empty array");
    for (const json& o : j["markers"])
    {
        Marker m;
        m.name = o.value("name", "");
        if (m.name.empty())
            throw std::invalid_argument("a marker has no name");
        auto ctx = [&](const char* key) { return "marker " + m.name + ": " + key; };
        m.parent = o.value("parent", "");
        m.joint = o.value("joint", m.name);
        m.label = o.value("label", "");
        m.side = o.value("side", sideOf(m.name));
        m.mirror = o.value("mirror", "");
        m.limb = o.value("limb", "");
        m.flags = o.value("flags", std::vector<std::string>{});
        m.position = checkedVec(o.value("position", json::array({0.0, 0.0, 0.0})), ctx("position").c_str());
        if (o.contains("orientation"))
            m.orientation = quatOf(o["orientation"], ctx("orientation").c_str());
        m.radius = o.value("radius", 2.0);
        m.centerInMesh = o.value("centerInMesh", false);
        m.placement = valueOf(kPlacements, o.value("placement", "free"), ctx("placement").c_str());
        m.follow = o.value("follow", "");
        m.to = o.value("to", "");
        m.t = o.value("t", 0.0);
        if (o.contains("localOffset"))
            m.localOffset = checkedVec(o["localOffset"], ctx("localOffset").c_str());
        m.orient = valueOf(kOrients, o.value("orient", "aim"), ctx("orient").c_str());
        m.aimAt = o.value("aimAt", "");
        m.aim = o.value("aim", "z");
        m.up = o.value("up", "y");
        m.upType = valueOf(kUpTypes, o.value("upType", "world"), ctx("upType").c_str());
        if (o.contains("upVector"))
            m.upVector = checkedVec(o["upVector"], ctx("upVector").c_str());
        if (o.contains("offset"))
            m.offset = quatOf(o["offset"], ctx("offset").c_str());
        if (m.orient == Orient::Aim)
        {
            if (std::abs(glm::dot(axisOf(m.aim), axisOf(m.up))) > 0.5)
                throw std::invalid_argument(ctx("aim and up must be different axes"));
        }
        if (t.marker(m.name))
            throw std::invalid_argument("marker " + m.name + " is defined twice");
        t.markers.push_back(std::move(m));
    }
    for (const json& o : j.value("limbs", json::array()))
    {
        Limb l;
        l.name = o.value("name", "");
        l.side = o.value("side", "center");
        l.markers = o.value("markers", std::vector<std::string>{});
        if (o.contains("rule"))
            l.upType = valueOf(kUpTypes, o["rule"].value("upType", "world"), "limb upType");
        for (const std::string& m : l.markers)
            if (!t.marker(m))
                throw std::invalid_argument("limb " + l.name + " names unknown marker " + m);
        t.limbs.push_back(std::move(l));
    }
    t.picker = j.value("picker", json::object());
    t.poses = j.value("poses", json::object());
    t.aliases = j.value("aliases", json::object());
    t.spaces = j.value("spaces", json::object());
    for (auto& [k, v] : t.spaces.items())
        if (!v.is_array())
            throw std::invalid_argument("spaces of " + k + " should be a list");
    t.rig = j.value("rig", json::object());
    if (!t.rig.is_object())
        throw std::invalid_argument("rig should be an object");
    auto needMarker = [&](const json& entry, const char* key, const char* section) {
        if (!entry.contains(key))
            return;
        const std::string n = entry[key].get<std::string>();
        if (!n.empty() && !t.marker(n))
            throw std::invalid_argument(std::string("rig ") + section + " names unknown marker " + n);
    };
    for (const char* section : {"ikChains", "reverseFeet", "lookers", "twists"})
        if (t.rig.contains(section))
        {
            if (!t.rig[section].is_array())
                throw std::invalid_argument(std::string("rig ") + section + " should be a list");
            for (const json& e : t.rig[section])
                for (const char* key : {"start", "end", "ball", "toe", "joint", "source"})
                    needMarker(e, key, section);
        }
    for (const json& c : t.rig.value("ikChains", json::array()))
        if (c.contains("hose"))
        {
            const json& h = c["hose"];
            if (!h.is_object() || (h.contains("enabled") && !h["enabled"].is_boolean()))
                throw std::invalid_argument("rig ikChains hose should be {enabled, joints, amount, autoArc}");
            for (const char* k : {"joints", "amount", "autoArc"})
                if (h.contains(k) && !h[k].is_number())
                    throw std::invalid_argument(std::string("rig ikChains hose ") + k + " should be a number");
        }
    validateLimits(t);
    if (const json fb = t.rig.value("fullBody", json::object()); fb.contains("stiffness"))
    {
        if (!fb["stiffness"].is_object())
            throw std::invalid_argument("rig fullBody stiffness should map markers to numbers");
        for (const auto& [m, v] : fb["stiffness"].items())
            if (!v.is_number() || !t.marker(m))
                throw std::invalid_argument("rig fullBody stiffness names unknown marker " + m + " or is not a number");
    }
    if (const json st = t.rig.value("strings", json::object()); st.contains("contacts"))
    {
        if (!st["contacts"].is_array())
            throw std::invalid_argument("rig strings contacts should be a list of markers");
        for (const json& m : st["contacts"])
            if (!m.is_string() || !t.marker(m.get<std::string>()))
                throw std::invalid_argument("rig strings contacts names unknown marker " + m.dump());
    }
    if (const json st = t.rig.value("strings", json::object()); st.contains("lines"))
    {
        if (!st["lines"].is_object())
            throw std::invalid_argument("rig strings lines should map names to [top marker, end marker]");
        for (const auto& [name, ends] : st["lines"].items())
            if (!ends.is_array() || ends.size() != 2 || !ends[0].is_string() || !ends[1].is_string() || !t.marker(ends[0].get<std::string>()) ||
                !t.marker(ends[1].get<std::string>()))
                throw std::invalid_argument("rig strings line " + name + " should be [top marker, end marker] of known markers");
    }
    for (auto& [k, v] : j.items())
        if (k != "format" && k != "version" && k != "id" && k != "name" && k != "description" && k != "source" && k != "lateral" &&
            k != "markers" && k != "limbs" && k != "picker" && k != "poses" && k != "aliases" && k != "spaces" && k != "rig")
            t.extra[k] = v;
    t.sortMarkers();
    return t;
}

std::vector<std::string> Template::spacesFor(const Marker& m) const
{
    for (const std::string& key : {m.name, m.label})
        if (!key.empty() && spaces.contains(key))
            return spaces[key].get<std::vector<std::string>>();
    return {};
}

const Marker* Template::markerFor(const std::string& nameOrLabel) const
{
    if (const Marker* m = marker(nameOrLabel))
        return m;
    for (const Marker& m : markers)
        if (!m.label.empty() && m.label == nameOrLabel)
            return &m;
    return nullptr;
}

void Template::sortMarkers()
{
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < markers.size(); ++i)
        index[markers[i].name] = i;
    auto deps = [&](const Marker& m) {
        std::vector<std::string> d;
        for (const std::string* s : {&m.parent, &m.follow, &m.to})
            if (!s->empty())
            {
                if (!index.contains(*s))
                    throw std::invalid_argument("marker " + m.name + " refers to unknown marker " + *s);
                d.push_back(*s);
            }
        if (m.placement == Placement::Follow && m.follow.empty())
            throw std::invalid_argument("marker " + m.name + " follows nothing");
        if (m.orient == Orient::Aim && !m.aimAt.empty() && !index.contains(m.aimAt))
            throw std::invalid_argument("marker " + m.name + " aims at unknown marker " + m.aimAt);
        return d;
    };
    std::vector<Marker> out;
    std::vector<int> state(markers.size(), 0); // 0 new, 1 visiting, 2 done
    std::function<void(size_t)> visit = [&](size_t i) {
        if (state[i] == 2)
            return;
        if (state[i] == 1)
            throw std::invalid_argument("markers depend on each other in a cycle at " + markers[i].name);
        state[i] = 1;
        for (const std::string& d : deps(markers[i]))
            visit(index[d]);
        state[i] = 2;
        out.push_back(markers[i]);
    };
    for (size_t i = 0; i < markers.size(); ++i)
        visit(i);
    markers = std::move(out);
}

Template readFile(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("can't read " + file.string());
    json j;
    try
    {
        j = json::parse(in);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(file.string() + ": " + e.what());
    }
    try
    {
        return Template::fromJson(j);
    }
    catch (const std::invalid_argument& e)
    {
        throw std::runtime_error(file.string() + ": " + e.what());
    }
}

void writeFile(const Template& t, const std::filesystem::path& file)
{
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path());
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out)
            throw std::runtime_error("can't write " + file.string());
        out << t.toJson().dump(1, '\t') << '\n';
    }
    std::filesystem::rename(tmp, file);
}

std::optional<glm::dquat> aimFrame(const Template& t, const Marker& m, const std::map<std::string, glm::dvec3>& pos,
                                   const std::map<std::string, glm::dquat>& rot)
{
    auto here = pos.find(m.name);
    auto at = pos.find(m.aimAt);
    if (m.aimAt.empty() || here == pos.end() || at == pos.end())
        return std::nullopt;
    glm::dvec3 aim = at->second - here->second;
    glm::dvec3 up = m.upVector;
    if (m.upType == UpType::Parent)
    {
        auto pr = rot.find(m.parent);
        up = pr == rot.end() ? m.upVector : pr->second * m.upVector;
    }
    else if (m.upType == UpType::Plane)
    {
        auto pp = pos.find(m.parent);
        if (pp != pos.end())
            if (glm::dvec3 n = planeNormal(pp->second, here->second, at->second); glm::length(n) > 1e-6 * glm::length(aim) * glm::length(aim))
                up = glm::dot(n, m.upVector) < 0 ? -n : n;
    }
    (void)t;
    return frameFor(axisOf(m.aim), axisOf(m.up), aim, up);
}

Solved solve(const Template& t, const std::map<std::string, glm::dvec3>& known, bool fillFree)
{
    Solved s;
    for (const Marker& m : t.markers)
        if (m.isFree())
        {
            auto k = known.find(m.name);
            if (k != known.end() || fillFree)
                s.position[m.name] = k != known.end() ? k->second : m.position;
        }
        else if (auto k = known.find(m.name); k != known.end())
            s.position[m.name] = k->second;

    auto rotOf = [&](const std::string& n) { return n.empty() || !s.rotation.contains(n) ? glm::dquat(1, 0, 0, 0) : s.rotation[n]; };
    auto posOf = [&](const std::string& n) { return n.empty() || !s.position.contains(n) ? glm::dvec3(0.0) : s.position[n]; };
    for (const Marker& m : t.markers)
    {
        if (!s.position.contains(m.name) && !m.isFree())
        {
            switch (m.placement)
            {
            case Placement::Parent:
                s.position[m.name] = posOf(m.parent) + rotOf(m.parent) * m.localOffset;
                break;
            case Placement::Follow:
                s.position[m.name] = posOf(m.follow) + rotOf(m.follow) * m.localOffset;
                break;
            case Placement::Segment:
                s.position[m.name] = glm::mix(posOf(m.parent), posOf(m.to), m.t) + rotOf(m.parent) * m.localOffset;
                break;
            case Placement::Free:
                break;
            }
        }
        glm::dquat r = m.offset;
        switch (m.orient)
        {
        case Orient::Aim:
            if (std::optional<glm::dquat> f = aimFrame(t, m, s.position, s.rotation))
                r = *f * m.offset;
            else if (const Marker* p = t.marker(m.parent))
                r = rotOf(m.parent) * glm::inverse(p->orientation) * m.orientation; // nothing to aim at: ride the parent
            else
                r = m.orientation;
            break;
        case Orient::Parent:
            r = rotOf(m.parent) * m.offset;
            break;
        case Orient::Follow:
            r = rotOf(m.follow) * m.offset;
            break;
        case Orient::Fixed:
            break;
        }
        s.rotation[m.name] = glm::normalize(r);
    }
    return s;
}

Template fromSkeleton(const std::vector<SourceJoint>& joints, const FromSkeletonOptions& opts)
{
    if (joints.empty())
        throw std::invalid_argument("the skeleton has no joints");
    Template t;
    t.id = opts.id;
    t.name = opts.name.empty() ? opts.id : opts.name;
    std::set<std::string> names;
    std::map<std::string, const SourceJoint*> byName;
    std::map<std::string, std::vector<std::string>> kids;
    for (const SourceJoint& j : joints)
    {
        names.insert(j.name);
        byName[j.name] = &j;
    }
    for (const SourceJoint& j : joints)
        if (!j.parent.empty() && byName.contains(j.parent))
            kids[j.parent].push_back(j.name);

    // Classify: placement and flags.
    const std::string top = joints.front().name;
    for (const SourceJoint& j : joints)
    {
        Marker m;
        m.name = m.joint = j.name;
        m.parent = byName.contains(j.parent) ? j.parent : "";
        m.label = labelFor(j.name);
        m.side = sideOf(j.name);
        m.position = j.position;
        m.orientation = glm::normalize(j.rotation);
        m.limb = limbFor(m.label, j.name);
        std::string follow = followFor(j.name, names);
        bool isIk = lower(stripNamespace(j.name)).starts_with("ik_");
        if (isIk && !opts.includeIk)
            continue;
        if (m.parent.empty())
            m.flags.push_back("root");
        if (opts.free.contains(j.name) || (m.parent.empty() && !isIk))
            m.placement = Placement::Free;
        else if (isIk)
        {
            m.flags.push_back("ik");
            m.placement = follow.empty() ? Placement::Parent : Placement::Follow;
            m.follow = follow;
        }
        else if (m.label.empty() && looksHelper(j.name))
        {
            m.flags.push_back("helper");
            if (lower(j.name).find("twist") != std::string::npos)
                m.flags.push_back("twist");
            m.placement = Placement::Parent;
        }
        m.centerInMesh = m.isFree() && !m.parent.empty() && m.limb != "" && !isIk;
        t.markers.push_back(std::move(m));
    }
    if (!t.markers.empty())
        t.markers.front().placement = Placement::Free;
    (void)top;

    // Mirror partners, by side naming.
    auto otherSide = [&](const std::string& n) {
        static const std::pair<const char*, const char*> swaps[] = {{"_l", "_r"}, {"_r", "_l"}, {"Left", "Right"}, {"Right", "Left"}};
        for (auto [a, b] : swaps)
        {
            std::string s(a);
            if (n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0)
                return n.substr(0, n.size() - s.size()) + b;
            if (size_t p = n.find(s); p != std::string::npos && (s[0] != '_' || p + s.size() == n.size() || n[p + s.size()] == '_'))
                return n.substr(0, p) + b + n.substr(p + s.size());
        }
        return std::string();
    };
    for (Marker& m : t.markers)
        if (m.side != "center")
            if (std::string o = otherSide(m.name); !o.empty() && t.marker(o))
                m.mirror = o;

    // Twist joints ride their parent's bone.
    auto freeChild = [&](const std::string& parent) -> std::vector<const Marker*> {
        std::vector<const Marker*> out;
        for (const Marker& c : t.markers)
            if (c.parent == parent && c.isFree())
                out.push_back(&c);
        return out;
    };

    // Orientation rules: aim at the free child best lined up with an axis, preferring the same limb.
    for (Marker& m : t.markers)
    {
        if (m.placement == Placement::Follow)
        {
            m.orient = Orient::Follow;
            continue;
        }
        if (m.hasFlag("root"))
        {
            m.orient = Orient::Fixed;
            continue;
        }
        if (!m.isFree())
        {
            m.orient = Orient::Parent;
            continue;
        }
        const Marker* best = nullptr;
        double bestScore = -1.0;
        for (const Marker* c : freeChild(m.name))
        {
            glm::dvec3 d = c->position - m.position;
            if (glm::length(d) < 1e-4)
                continue;
            std::string ax = closestAxis(m.orientation, d);
            double score = std::abs(glm::dot(m.orientation * axisOf(ax), glm::normalize(d))) + (c->limb == m.limb && !m.limb.empty() ? 1.0 : 0.0);
            if (score > bestScore)
                bestScore = score, best = c;
        }
        if (!best)
        {
            m.orient = m.parent.empty() ? Orient::Fixed : Orient::Parent;
            continue;
        }
        m.orient = Orient::Aim;
        m.aimAt = best->name;
        glm::dvec3 d = best->position - m.position;
        m.aim = closestAxis(m.orientation, d);
        const Marker* parent = t.marker(m.parent);
        const bool plane = parent && parent->isFree() && parent->limb == m.limb &&
                           (m.limb.starts_with("arm") || m.limb.starts_with("leg") || m.limb.starts_with("thumb") || m.limb.starts_with("index") ||
                            m.limb.starts_with("middle") || m.limb.starts_with("ring") || m.limb.starts_with("pinky"));
        glm::dvec3 n = parent ? planeNormal(parent->position, m.position, best->position) : glm::dvec3(0.0);
        if (plane && glm::length(n) > 1e-3 * glm::length(d) * glm::length(parent->position - m.position))
        {
            m.upType = UpType::Plane;
            m.up = closestAxis(m.orientation, n, m.aim);
            m.upVector = glm::normalize(n);
            // Keep the sign rule pointing the way the recorded up axis does.
            if (glm::dot(m.orientation * axisOf(m.up), m.upVector) < 0)
                m.upVector = -m.upVector;
        }
        else
        {
            // The world axis and the local (non aim) axis most nearly lined up.
            double bestDot = -1.0;
            for (int w = 0; w < 3; ++w)
            {
                glm::dvec3 wv(0.0);
                wv[w] = 1.0;
                std::string ax = closestAxis(m.orientation, wv, m.aim);
                double dd = glm::dot(m.orientation * axisOf(ax), wv);
                if (std::abs(dd) > bestDot)
                {
                    bestDot = std::abs(dd);
                    m.up = ax;
                    m.upVector = dd < 0 ? -wv : wv;
                }
            }
            m.upType = UpType::World;
        }
    }

    // Segment placement for twist helpers whose parent aims down a bone.
    for (Marker& m : t.markers)
        if (m.hasFlag("twist"))
            if (const Marker* p = t.marker(m.parent); p && p->orient == Orient::Aim && !p->aimAt.empty())
            {
                const Marker* end = t.marker(p->aimAt);
                glm::dvec3 bone = end->position - p->position;
                double len2 = glm::dot(bone, bone);
                if (len2 > 1e-8)
                {
                    m.placement = Placement::Segment;
                    m.to = end->name;
                    m.t = glm::dot(m.position - p->position, bone) / len2;
                }
            }

    t.sortMarkers();

    // Offsets: solve at the defaults with identity offsets, marker by marker, and keep what reproduces the recording.
    std::map<std::string, glm::dvec3> pos;
    std::map<std::string, glm::dquat> rot;
    for (Marker& m : t.markers)
    {
        pos[m.name] = m.position;
        const glm::dvec3 parentPos = m.parent.empty() ? glm::dvec3(0.0) : pos[m.parent];
        const glm::dquat parentRot = m.parent.empty() ? glm::dquat(1, 0, 0, 0) : rot[m.parent];
        switch (m.placement)
        {
        case Placement::Parent:
            m.localOffset = glm::inverse(parentRot) * (m.position - parentPos);
            break;
        case Placement::Follow:
            m.localOffset = glm::inverse(rot[m.follow]) * (m.position - pos[m.follow]);
            break;
        case Placement::Segment:
            m.localOffset = glm::inverse(parentRot) * (m.position - glm::mix(parentPos, pos[m.to], m.t));
            break;
        case Placement::Free:
            break;
        }
        switch (m.orient)
        {
        case Orient::Aim:
        {
            // aimAt comes later in order; its recorded position is what the solve will see at the defaults.
            std::map<std::string, glm::dvec3> p = pos;
            p[m.aimAt] = t.marker(m.aimAt)->position;
            std::optional<glm::dquat> f = aimFrame(t, m, p, rot);
            m.offset = f ? glm::normalize(glm::inverse(*f) * m.orientation) : m.orientation;
            break;
        }
        case Orient::Parent:
            m.offset = glm::normalize(glm::inverse(parentRot) * m.orientation);
            break;
        case Orient::Follow:
            m.offset = glm::normalize(glm::inverse(rot[m.follow]) * m.orientation);
            break;
        case Orient::Fixed:
            m.offset = m.orientation;
            break;
        }
        rot[m.name] = m.orientation;
    }

    // Limbs, the character's lateral direction and the alias table.
    std::map<std::string, Limb> limbs;
    std::vector<std::string> limbOrder;
    for (const Marker& m : t.markers)
        if (!m.limb.empty() && m.isFree())
        {
            if (!limbs.contains(m.limb))
            {
                limbOrder.push_back(m.limb);
                Limb l;
                l.name = m.limb;
                l.side = m.limb.ends_with("_l") ? "left" : m.limb.ends_with("_r") ? "right" : "center";
                l.upType = m.limb == "spine" ? UpType::World : UpType::Plane;
                limbs[m.limb] = l;
            }
            limbs[m.limb].markers.push_back(m.name);
        }
    for (const std::string& n : limbOrder)
        t.limbs.push_back(limbs[n]);

    glm::dvec3 lateral(0.0);
    for (const Marker& m : t.markers)
        if (m.side == "left" && !m.mirror.empty())
            lateral += m.position - t.marker(m.mirror)->position;
    if (glm::length(lateral) > 1e-6)
    {
        // Snap to the nearest world axis when it is that close, which it is for any symmetric rig.
        lateral = glm::normalize(lateral);
        for (int a = 0; a < 3; ++a)
            if (std::abs(lateral[a]) > 0.999)
            {
                glm::dvec3 w(0.0);
                w[a] = lateral[a] > 0 ? 1.0 : -1.0;
                lateral = w;
            }
        t.lateral = lateral;
    }

    for (const Marker& m : t.markers)
        if (!m.label.empty())
        {
            json other = json::array();
            if (m.label != m.joint)
                other.push_back(m.label);
            for (const std::string& a : aliasesFor(m.label))
                if (a != m.joint)
                    other.push_back(a);
            if (!other.empty())
                t.aliases[m.joint] = other;
        }
    return t;
}
json pickerLayout(const Template& t)
{
    std::vector<const Marker*> shown;
    for (const Marker& m : t.markers)
        if (m.isFree() && !m.label.empty() && !m.hasFlag("root") && !m.hasFlag("ik"))
            shown.push_back(&m);
    if (shown.empty())
        return json::object();
    // Front view: the character's left on the page's right, head at the top.
    const glm::dvec3 lateral = glm::normalize(t.lateral);
    double lo = 1e30, hi = -1e30, top = -1e30, bottom = 1e30;
    for (const Marker* m : shown)
    {
        double x = glm::dot(m->position, lateral);
        lo = std::min(lo, x), hi = std::max(hi, x);
        top = std::max(top, m->position.y), bottom = std::min(bottom, m->position.y);
    }
    const double width = 400.0, height = 600.0, margin = 30.0;
    const double scale = std::min((width - 2 * margin) / std::max(hi - lo, 1e-6), (height - 2 * margin) / std::max(top - bottom, 1e-6));
    const double mid = (lo + hi) * 0.5;
    json items = json::array();
    for (const Marker* m : shown)
    {
        const bool finger = m->limb.starts_with("thumb") || m->limb.starts_with("index") || m->limb.starts_with("middle") ||
                            m->limb.starts_with("ring") || m->limb.starts_with("pinky");
        json it = {{"target", m->joint},
                   {"x", std::round((width * 0.5 + (glm::dot(m->position, lateral) - mid) * scale) * 10.0) / 10.0},
                   {"y", std::round((margin + (top - m->position.y) * scale) * 10.0) / 10.0},
                   {"size", finger ? 7 : 14},
                   {"shape", finger ? "circle" : "rect"}};
        if (!finger)
            it["label"] = m->label;
        items.push_back(std::move(it));
    }
    return {{"name", t.id}, {"character", t.id}, {"template", t.id},
            {"pages", json::array({{{"name", "Body"}, {"width", width}, {"height", height}, {"items", items}}})}};
}

json defaultPoses(const Template& t)
{
    auto poseOf = [&](const std::string& name, json nodes) {
        return json{{"name", name}, {"character", t.id}, {"template", t.id}, {"tags", json::array({"template"})}, {"nodes", std::move(nodes)}};
    };
    auto q = [](const glm::dquat& r) { return json::array({r.x, r.y, r.z, r.w}); };
    json bind = json::object();
    for (const Marker& m : t.markers)
        bind[m.joint] = {{"rotate", q(glm::dquat(1, 0, 0, 0))}};

    const glm::dvec3 up(0.0, 1.0, 0.0);
    const glm::dvec3 lateral = glm::normalize(t.lateral - up * glm::dot(t.lateral, up));
    auto armsTo = [&](double downDegrees) {
        json nodes = json::object();
        for (const char* side : {"l", "r"})
        {
            const Marker* upper = nullptr;
            const Marker* lower = nullptr;
            const Marker* hand = nullptr;
            for (const Marker& m : t.markers)
            {
                if (m.label == std::string("upperarm_") + side)
                    upper = &m;
                if (m.label == std::string("lowerarm_") + side)
                    lower = &m;
                if (m.label == std::string("hand_") + side)
                    hand = &m;
            }
            if (!upper || !lower || !hand)
                continue;
            const double a = glm::radians(downDegrees);
            const glm::dvec3 out = (side[0] == 'l' ? lateral : -lateral) * std::cos(a) - up * std::sin(a);
            // R_local = O^-1 * delta * O: the world turn expressed in the joint's own frame, after its jointOrient.
            const glm::dquat du = glm::rotation(glm::normalize(lower->position - upper->position), out);
            nodes[upper->joint] = {{"rotate", q(glm::normalize(glm::inverse(upper->orientation) * du * upper->orientation))}};
            const glm::dquat lowerNow = du * lower->orientation;
            const glm::dquat dl = glm::rotation(glm::normalize(du * (hand->position - lower->position)), out);
            nodes[lower->joint] = {{"rotate", q(glm::normalize(glm::inverse(lowerNow) * dl * lowerNow))}};
        }
        return nodes;
    };
    return {{"bind", poseOf("bind", bind)}, {"T", poseOf("T", armsTo(0.0))}, {"A", poseOf("A", armsTo(45.0))}};
}
} // namespace rigt
