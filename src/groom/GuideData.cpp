#include "groom/GuideData.h"

#include "groom/Attrs.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <set>

namespace lila::groom
{
namespace guideData
{
using groomlab::GuideHierarchy;
using groomlab::HierarchyGuide;
using groomlab::TubeProfile;
using groomlab::TubeShape;

/** Library shapes standing for the lock profiles GroomLab has no built-in of. */
constexpr const char* kCrescentShape = "crescent";
constexpr const char* kCustomShape = "custom";

/** The lock attributes the tube and the hierarchy hold themselves: never a guide attribute. */
bool tubeAttr(const std::string& name)
{
    return name == attr::kId || name == attr::kGroup || name == attr::kWidth || name == attr::kThickness || name == attr::kProfile;
}

/** Each point's share of the curve's length, 0 at the root to 1 at the tip. */
std::vector<double> along(const std::vector<glm::dvec3>& p)
{
    std::vector<double> t(p.size(), 0.0);
    for (size_t i = 1; i < p.size(); ++i)
        t[i] = t[i - 1] + glm::length(p[i] - p[i - 1]);
    const double total = t.empty() ? 0.0 : t.back();
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = total > 1e-12 ? t[i] / total : (t.size() > 1 ? double(i) / double(t.size() - 1) : 0.0);
    return t;
}

/** Per-point values at even steps root to tip, `count` of them, as a tube's lists are spaced. */
std::vector<float> evenly(const std::vector<double>& t, const std::vector<double>& v, size_t count)
{
    std::vector<float> out(count, 0.0f);
    for (size_t k = 0; k < count; ++k)
    {
        const double at = count > 1 ? double(k) / double(count - 1) : 0.0;
        const size_t hi = size_t(std::lower_bound(t.begin(), t.end(), at) - t.begin());
        if (hi == 0)
            out[k] = float(v.front());
        else if (hi >= t.size())
            out[k] = float(v.back());
        else
        {
            const double span = t[hi] - t[hi - 1];
            const double w = span > 1e-12 ? (at - t[hi - 1]) / span : 0.0;
            out[k] = float(v[hi - 1] + (v[hi] - v[hi - 1]) * w);
        }
    }
    return out;
}

std::vector<groomlab::SectionPoint> sectionPoints(const std::vector<glm::dvec2>& p)
{
    std::vector<groomlab::SectionPoint> out;
    for (const glm::dvec2& q : p)
        out.push_back({float(q.x), float(q.y)});
    return out;
}

LockProfile profileOf(const TubeProfile& tube)
{
    if (tube.custom)
        return tube.custom->name == kCrescentShape ? LockProfile::Crescent : LockProfile::Custom;
    switch (tube.shape)
    {
    case TubeShape::Round: return LockProfile::Tube;
    case TubeShape::Teardrop: return LockProfile::Teardrop;
    default: return LockProfile::Ribbon;
    }
}

template <class G> Spline lockSplineOf(const G& g, const TubeProfile& tube, double group, const LockOptions& o)
{
    Spline s;
    s.kind = curve::Kind::Poly;
    s.points.reserve(g.points.size());
    for (const groomlab::Vec3f& p : g.points)
        s.points.push_back({p.x, p.y, p.z});
    const std::vector<double> t = along(s.points);
    const double width = o.width > 1e-12 ? o.width : 1.0, thickness = o.thickness > 1e-12 ? o.thickness : 1.0;
    s.radius.resize(s.points.size());
    s.tilt.resize(s.points.size());
    double aspect = 0.0;
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        s.radius[i] = tube.widthAt(float(t[i])) / width;
        s.tilt[i] = tube.twistAt(float(t[i]));
        aspect += tube.aspectAt(float(t[i]));
    }
    s.attrs[std::string(attr::kId)] = double(g.id);
    s.attrs[std::string(attr::kGroup)] = group;
    s.attrs[std::string(attr::kWidth)] = 1.0;
    s.attrs[std::string(attr::kThickness)] = s.points.empty() ? 1.0 : aspect / double(s.points.size()) / thickness;
    s.attrs[std::string(attr::kProfile)] = double(int(profileOf(tube)));
    for (const auto& [name, value] : g.attributes)
        if (!tubeAttr(name))
            s.attrs[name] = value;
    for (const auto& [name, values] : g.pointAttributes)
        if (values.size() == s.points.size())
            s.pointAttrs[name].assign(values.begin(), values.end());
    return s;
}

Spline lockSpline(const HierarchyGuide& g, const LockOptions& o)
{
    return lockSplineOf(g, g.tube, double(g.group), o);
}

bool sameSpline(const Spline& a, const Spline& b)
{
    return a.points == b.points && a.radius == b.radius && a.tilt == b.tilt && a.attrs == b.attrs && a.pointAttrs == b.pointAttrs;
}

/** The tube a lock spline describes; `shape` gives the library shape of a name and points (made when missing). */
TubeProfile tubeOf(const Spline& s, const LockOptions& o,
                   const std::function<groomlab::CustomTubeShapePtr(const std::string&, const std::vector<glm::dvec2>&)>& shape)
{
    TubeProfile tube;
    const size_t n = std::max<size_t>(s.points.size(), 2);
    const std::vector<double> t = along(s.points.size() >= 2 ? s.points : std::vector<glm::dvec3>{D3(0.0), D3(0.0, 1.0, 0.0)});
    const double scale = o.width * attr::get(s, attr::kWidth, 1.0);
    std::vector<double> width(t.size()), twist(t.size());
    for (size_t i = 0; i < t.size(); ++i)
    {
        width[i] = scale * (i < s.radius.size() ? s.radius[i] : 1.0);
        twist[i] = i < s.tilt.size() ? s.tilt[i] : 0.0;
    }
    tube.width = evenly(t, width, n);
    tube.twist = evenly(t, twist, n);
    tube.aspect = {float(std::clamp(o.thickness * attr::get(s, attr::kThickness, 1.0), double(groomlab::kMinTubeAspect),
                                    double(groomlab::kMaxTubeAspect)))};
    const LockProfile profile = LockProfile(std::clamp(int(attr::get(s, attr::kProfile, double(int(o.profile)))), 0, int(LockProfile::Count) - 1));
    switch (profile)
    {
    case LockProfile::Tube: tube.shape = TubeShape::Round; break;
    case LockProfile::Teardrop: tube.shape = TubeShape::Teardrop; break;
    case LockProfile::Crescent:
        tube.custom = shape(kCrescentShape, profilePoints(LockProfile::Crescent, 24));
        break;
    case LockProfile::Custom:
        if (o.customProfile.size() >= 3)
        {
            try
            {
                tube.custom = shape(kCustomShape, o.customProfile);
                break;
            }
            catch (const std::exception&)
            {
            }
        }
        tube.shape = TubeShape::Ribbon;
        break;
    default: tube.shape = TubeShape::Ribbon; break;
    }
    return tube;
}
/** Library shapes from the hierarchy's library, added to it when missing. */
auto hierarchyShapes(GuideHierarchy& h)
{
    return [&h](const std::string& name, const std::vector<glm::dvec2>& points) {
        if (groomlab::CustomTubeShapePtr had = h.tubeShape(name); had && name == kCrescentShape)
            return had;
        return h.setTubeShape(name, sectionPoints(points), false);
    };
}
} // namespace guideData

Spline lockSplineOf(const groomlab::Guide& g, const LockOptions& o)
{
    groomlab::TubeProfile tube;
    if (g.hasTube)
        tube = g.tube;
    else
        tube.width = {float(o.width), float(o.width)};
    Spline s = guideData::lockSplineOf(g, tube, 0.0, o);
    s.attrs.erase(std::string(attr::kGroup));
    return s;
}

groomlab::Guide guideOfLock(const Spline& s, const LockOptions& o, const groomlab::Guide* was)
{
    using namespace guideData;
    groomlab::Guide g;
    g.id = groomlab::GuideId(std::max(0.0, attr::get(s, attr::kId, 0.0)));
    for (const glm::dvec3& p : s.points)
        g.points.push_back({float(p.x), float(p.y), float(p.z)});
    g.hasTube = true;
    g.tube = tubeOf(s, o, [was](const std::string& name, const std::vector<glm::dvec2>& points) {
        if (was && was->tube.custom && was->tube.custom->name == name)
            return was->tube.custom;
        return groomlab::makeCustomTubeShape(name, sectionPoints(points), false);
    });
    if (was)
    {
        g.frameUp = was->frameUp;
        g.depth = was->depth;
        g.root = was->root;
    }
    for (const auto& [name, value] : s.attrs)
        if (!tubeAttr(name))
            g.attributes[name] = float(value);
    for (const auto& [name, values] : s.pointAttrs)
        if (values.size() == s.points.size())
        {
            std::vector<float>& layer = g.pointAttributes[name];
            layer.resize(values.size());
            std::transform(values.begin(), values.end(), layer.begin(), [](double v) { return float(v); });
        }
    return g;
}

Splines lockSplines(const groomlab::GuideHierarchy& h, const LockOptions& o)
{
    Splines out;
    out.reserve(h.guides().size());
    for (const groomlab::HierarchyGuide& g : h.guides())
        if (g.points.size() >= 2)
            out.push_back(guideData::lockSpline(g, o));
    return out;
}

groomlab::TubeProfile tubeOfLock(const Spline& s, const LockOptions& o, groomlab::GuideHierarchy& h)
{
    return guideData::tubeOf(s, o, guideData::hierarchyShapes(h));
}

bool applyLockSplines(groomlab::GuideHierarchy& h, const Splines& s, const LockOptions& o)
{
    using namespace guideData;
    std::set<groomlab::GuideId> kept;
    groomlab::GuideId next = 1;
    for (const groomlab::HierarchyGuide& g : h.guides())
        next = std::max(next, g.id + 1);
    for (const Spline& sp : s)
        next = std::max(next, groomlab::GuideId(std::max(0.0, attr::get(sp, attr::kId, 0.0))) + 1);
    bool changed = false;
    std::vector<std::pair<groomlab::GuideId, const Spline*>> todo;
    for (const Spline& sp : s)
    {
        if (sp.points.size() < 2)
            continue;
        const double idValue = attr::get(sp, attr::kId, 0.0);
        groomlab::GuideId id = idValue >= 1.0 ? groomlab::GuideId(idValue) : next++;
        if (kept.count(id))
            id = next++; // a duplicated curve becomes a guide of its own
        kept.insert(id);
        todo.emplace_back(id, &sp);
    }
    std::vector<groomlab::GuideId> gone;
    for (const groomlab::HierarchyGuide& g : h.guides())
        if (!kept.count(g.id))
            gone.push_back(g.id);
    if (!gone.empty())
    {
        h.removeGuides(gone);
        changed = true;
    }
    for (const auto& [id, sp] : todo)
    {
        const groomlab::HierarchyGuide* g = h.guide(id);
        Spline stored = *sp;
        stored.attrs[std::string(attr::kId)] = double(id);
        if (g)
        {
            stored.attrs[std::string(attr::kGroup)] = double(g->group);
            if (sameSpline(lockSpline(*g, o), stored))
                continue;
        }
        std::vector<groomlab::Vec3f> points;
        points.reserve(sp->points.size());
        for (const glm::dvec3& p : sp->points)
            points.push_back({float(p.x), float(p.y), float(p.z)});
        const groomlab::TubeProfile tube = tubeOf(*sp, o, hierarchyShapes(h));
        if (g)
        {
            h.setPoints(id, std::move(points));
            h.setTube(id, tube);
        }
        else
        {
            groomlab::GroupId group = groomlab::GroupId(std::max(0.0, attr::get(*sp, attr::kGroup, 0.0)));
            const auto& groups = h.groups();
            if (std::none_of(groups.begin(), groups.end(), [&](const groomlab::GuideGroup& gr) { return gr.id == group; }))
                group = groups.empty() ? h.addGroup("locks") : groups.front().id;
            h.addGuide(group, std::move(points), tube, id);
        }
        for (const auto& [name, value] : sp->attrs)
            if (!tubeAttr(name))
                h.setAttribute(id, name, float(value));
        for (const auto& [name, values] : sp->pointAttrs)
            if (values.size() == sp->points.size())
            {
                std::vector<float> layer(values.size());
                std::transform(values.begin(), values.end(), layer.begin(), [](double v) { return float(v); });
                h.setPointAttribute(id, name, std::move(layer));
            }
        changed = true;
    }
    return changed;
}
} // namespace lila::groom
