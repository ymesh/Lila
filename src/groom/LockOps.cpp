#include "groom/LockOps.h"

#include "groom/Attrs.h"
#include "groom/Groom.h"
#include "groom/GuideData.h"
#include "groom/Style.h"

#include "curve/Pipe.h"

#include <groomlab/graph.h>
#include <groomlab/guide_set.h>
#include <groomlab/node_catalog.h>
#include <groomlab/scalp_mesh.h>

#include <map>

namespace lila::groom
{
namespace lockOps
{
using groomlab::EvalContext;
using groomlab::GuidesPtr;
using groomlab::Operator;
using groomlab::ParamSpec;
using groomlab::PortSpec;
using groomlab::PortType;
using groomlab::Value;

/** One param of a lock style: its StyleOptions field and its spec. */
struct StyleParam
{
    ParamSpec spec;
    double StyleOptions::* field = nullptr;
    int StyleOptions::* intField = nullptr;
};

ParamSpec number(const char* name, double def, const char* doc, std::optional<double> lo = std::nullopt,
                 std::optional<double> hi = std::nullopt)
{
    ParamSpec p{name, def, doc};
    p.min = lo;
    p.max = hi;
    return p;
}

ParamSpec integer(const char* name, std::int64_t def, const char* doc, std::optional<double> lo = std::nullopt)
{
    ParamSpec p{name, def, doc};
    p.min = lo;
    return p;
}

/** Each style's params, as the style stack's nodes had them. */
std::vector<StyleParam> styleParams(Style k)
{
    std::vector<StyleParam> a;
    auto num = [&](ParamSpec p, double StyleOptions::* f) { a.push_back({std::move(p), f, nullptr}); };
    auto cnt = [&](ParamSpec p, int StyleOptions::* f) { a.push_back({std::move(p), nullptr, f}); };
    switch (k)
    {
    case Style::Split:
        cnt(integer("count", 3, "Sub-locks per lock.", 1.0), &StyleOptions::count);
        num(number("spread", 1.0, "How far the sub-locks fan out, cm.", 0.0), &StyleOptions::spread);
        num(number("widthScale", 0.55, "Sub-lock width as a fraction of its lock's.", 0.01), &StyleOptions::widthScale);
        break;
    case Style::Clump:
        num(number("amount", 0.6, "How far locks pull toward their clump's middle, 0 to 1; painted clump adds to it.", 0.0, 1.0),
            &StyleOptions::amount);
        cnt(integer("count", 0, "Clumps; 0 groups by the clump attribute (each split lock's sub-locks).", 0.0), &StyleOptions::count);
        break;
    case Style::Curl:
        num(number("amount", 1.0, "Strength, 0 to 1; painted curl adds to it.", 0.0), &StyleOptions::amount);
        num(number("radius", 1.0, "Curl radius, cm.", 0.0), &StyleOptions::radius);
        num(number("frequency", 0.25, "Turns per cm.", 0.0), &StyleOptions::frequency);
        break;
    case Style::Wave:
        num(number("amount", 1.0, "Wave height, cm; painted wave adds to it."), &StyleOptions::amount);
        num(number("frequency", 0.15, "Waves per cm.", 0.0), &StyleOptions::frequency);
        break;
    case Style::Frizz:
        num(number("amount", 0.3, "Kink size, cm; painted frizz adds to it."), &StyleOptions::amount);
        num(number("frequency", 1.5, "Kinks per cm.", 0.0), &StyleOptions::frequency);
        break;
    case Style::Noise:
        num(number("amount", 1.0, "Displacement, cm."), &StyleOptions::amount);
        num(number("frequency", 0.08, "Features per cm: low bends whole locks together.", 0.0), &StyleOptions::frequency);
        break;
    case Style::Length:
        num(number("amount", 0.3, "Most a lock is shortened, as a fraction of its length.", 0.0, 0.95), &StyleOptions::amount);
        break;
    case Style::Cut:
        num(number("length", 20.0, "Longest a lock may be, cm.", 0.0), &StyleOptions::length);
        break;
    case Style::Flyaways:
        num(number("amount", 0.5, "Strays per lock.", 0.0), &StyleOptions::amount);
        num(number("spread", 2.0, "How far strays stray from their lock, cm.", 0.0), &StyleOptions::spread);
        num(number("length", 0.6, "Stray length, fraction of its lock.", 0.0, 1.0), &StyleOptions::length);
        break;
    default: break;
    }
    if (k != Style::Cut && k != Style::Noise && k != Style::Length)
        num(number("randomness", 0.5, "Variation between locks, 0 to 1.", 0.0, 1.0), &StyleOptions::randomness);
    return a;
}

const char* styleDoc(Style k)
{
    switch (k)
    {
    case Style::Split: return "Splits each lock into sub-locks fanned out around it: chunks of chunks.";
    case Style::Clump: return "Pulls locks toward the middle of their clump, so sub-locks gather into locks.";
    case Style::Curl: return "Winds locks into a helix: curls, ringlets and coils.";
    case Style::Wave: return "Waves locks side to side in their flat plane.";
    case Style::Frizz: return "Fine random kinks along each lock, stronger toward the tips.";
    case Style::Noise: return "Smooth noise in space, so neighbouring locks bend together.";
    case Style::Length: return "Shortens locks at random for an uneven hem.";
    case Style::Cut: return "Trims locks to a length.";
    case Style::Flyaways: return "Adds stray strands off locks.";
    default: return "";
    }
}

/** The scalp as a Lila surface, for the styles that orient by it. */
std::unique_ptr<Surface> surfaceOf(const groomlab::ScalpMesh& scalp)
{
    geo::Mesh m;
    for (const groomlab::Vec3f& p : scalp.points())
        m.addPoint({p.x, p.y, p.z});
    for (const groomlab::ScalpMesh::Triangle& t : scalp.triangles())
        m.addFace({geo::Index(t[0]), geo::Index(t[1]), geo::Index(t[2])});
    return std::make_unique<Surface>(std::move(m));
}

/** Guides through one of Lila's lock styles: each guide a lock spline, styled, back as a guide with its tube. */
class LockStyleOp final : public Operator
{
public:
    explicit LockStyleOp(Style kind) : mKind(kind) {}
    std::string typeName() const override { return lockStyleType(mKind); }
    std::string category() const override { return "guides"; }
    std::vector<PortSpec> inputs() const override { return {{"guides", PortType::Guides}, {"scalp", PortType::Mesh, true}}; }
    PortSpec output() const override { return {"out", PortType::Guides}; }
    std::vector<ParamSpec> paramSpecs() const override
    {
        std::vector<ParamSpec> out;
        for (const StyleParam& p : styleParams(mKind))
            out.push_back(p.spec);
        if (mKind != Style::Cut && mKind != Style::Length)
            out.push_back(ParamSpec{"ramp", std::string(), "Strength from root to tip: keys \"t:value\"; empty uses the style's own."});
        if (mKind != Style::Cut)
            out.push_back(ParamSpec{"seed", std::int64_t(1), "Random seed."});
        return out;
    }

    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override
    {
        const GuidesPtr in = groomlab::valueAs<GuidesPtr>(inputs.at(0));
        if (!in)
            return Value(std::make_shared<groomlab::GuideSet>());
        const groomlab::MeshPtr scalp = inputs.size() > 1 ? groomlab::valueAs<groomlab::MeshPtr>(inputs[1]) : nullptr;
        StyleOptions o;
        o.kind = mKind;
        for (const StyleParam& p : styleParams(mKind))
        {
            if (p.field)
                o.*p.field = params().getDouble(p.spec.name, o.*p.field);
            else
                o.*p.intField = int(params().getInt(p.spec.name, o.*p.intField));
        }
        if (mKind != Style::Cut && mKind != Style::Length)
            o.ramp = curve::parseRamp(params().getString("ramp"));
        o.seed = std::uint32_t(params().getInt("seed", 1));

        const LockOptions lock;
        std::map<std::uint64_t, const groomlab::Guide*> was;
        Splines locks;
        locks.reserve(in->size());
        for (const groomlab::Guide& g : in->guides())
        {
            was[g.id] = &g;
            locks.push_back(lockSplineOf(g, lock));
        }
        const std::unique_ptr<Surface> surface = scalp && !scalp->triangles().empty() ? surfaceOf(*scalp) : nullptr;
        attr::ensureIds(locks);
        applyStyle(locks, o, surface.get());
        auto out = std::make_shared<groomlab::GuideSet>();
        for (const Spline& s : locks)
        {
            if (s.points.size() < 2)
                continue;
            const auto it = was.find(std::uint64_t(std::max(0.0, attr::get(s, attr::kId, 0.0))));
            groomlab::Guide g = guideOfLock(s, lock, it != was.end() ? it->second : nullptr);
            out->add(g.id, g.points) = std::move(g);
        }
        if (scalp && !scalp->triangles().empty())
            out->rootOnMesh(*scalp);
        return Value(GuidesPtr(out));
    }

private:
    Style mKind;
};

/** Marks the guides a groom makes its locks of; hands them on unchanged. */
class LocksOp final : public Operator
{
public:
    std::string typeName() const override { return kLocksType; }
    std::string category() const override { return "guides"; }
    std::vector<PortSpec> inputs() const override { return {{"guides", PortType::Guides}}; }
    PortSpec output() const override { return {"out", PortType::Guides}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override { return inputs.at(0); }
};
} // namespace lockOps

std::vector<LockStyleParam> lockStyleParams(Style kind)
{
    std::vector<LockStyleParam> out;
    for (const groomlab::ParamSpec& p : lockOps::LockStyleOp(kind).paramSpecs())
    {
        LockStyleParam q{p.name, p.description};
        if (const double* d = std::get_if<double>(&p.defaultValue))
            q.defaultValue = *d;
        else if (const std::int64_t* i = std::get_if<std::int64_t>(&p.defaultValue))
        {
            q.kind = 'i';
            q.defaultValue = double(*i);
        }
        else
            q.kind = 's';
        q.min = p.min;
        q.max = p.max;
        out.push_back(std::move(q));
    }
    return out;
}

std::string lockStyleType(Style kind)
{
    std::string name = styleName(kind);
    if (!name.empty())
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    return "Lock" + name;
}

void registerLockOps()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    auto& reg = groomlab::OperatorRegistry::instance();
    groomlab::NodeInfo locks;
    locks.category = "guides";
    locks.description = "The guides a groom makes its locks of: what reaches it is swept into the locks mesh.";
    reg.registerType(kLocksType, locks, [] { return std::make_unique<lockOps::LocksOp>(); });
    for (int k = 0; k < int(Style::Count); ++k)
    {
        const Style kind = Style(k);
        groomlab::NodeInfo info;
        info.category = "guides";
        info.description = lockOps::styleDoc(kind);
        reg.registerType(lockStyleType(kind), info, [kind] { return std::make_unique<lockOps::LockStyleOp>(kind); });
    }
}
} // namespace lila::groom
