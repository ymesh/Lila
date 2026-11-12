#include "gn/NodesCommon.h"

#include <algorithm>
#include <map>

namespace lila::gn::nodes::fieldstats
{
enum class Stat : uint8_t
{
    Mean,
    Median,
    Min,
    Max,
    Deviation,
    Variance,
};

FieldContext statsContextOn(const FieldContext& ctx, Domain d)
{
    switch (ctx.component)
    {
    case Component::Mesh: return FieldContext::forMesh(*ctx.mesh, d > Domain::Corner ? Domain::Point : d);
    case Component::Curve: return FieldContext::forCurves(*ctx.curves, d == Domain::Curve ? Domain::Curve : Domain::Point);
    default: return ctx;
    }
}

Column statsToContext(const FieldContext& ctx, const Column& c, Domain from)
{
    if (ctx.component == Component::Mesh)
        return adaptDomain(*ctx.mesh, c, from, ctx.domain);
    if (ctx.component == Component::Curve)
        return adaptCurveDomain(*ctx.curves, c, from, ctx.domain);
    return c;
}

/** One statistic of a group's values, per component. */
D3 groupStatistic(std::vector<D3>& v, Stat s)
{
    if (v.empty())
        return D3(0.0);
    D3 mean(0.0);
    for (const D3& x : v)
        mean += x;
    mean /= double(v.size());
    switch (s)
    {
    case Stat::Mean: return mean;
    case Stat::Min:
    case Stat::Max:
    {
        D3 r = v[0];
        for (const D3& x : v)
            r = s == Stat::Min ? glm::min(r, x) : glm::max(r, x);
        return r;
    }
    case Stat::Median:
    {
        D3 r;
        std::vector<double> col(v.size());
        for (int k = 0; k < 3; ++k)
        {
            for (size_t i = 0; i < v.size(); ++i)
                col[i] = v[i][k];
            std::sort(col.begin(), col.end());
            size_t h = col.size() / 2;
            r[k] = col.size() % 2 ? col[h] : 0.5 * (col[h - 1] + col[h]);
        }
        return r;
    }
    case Stat::Deviation:
    case Stat::Variance:
    {
        D3 var(0.0);
        for (const D3& x : v)
            var += (x - mean) * (x - mean);
        var /= double(v.size());
        return s == Stat::Variance ? var : glm::sqrt(var);
    }
    }
    return D3(0.0);
}

class GroupStatField final : public Field
{
public:
    GroupStatField(SocketType t, FieldPtr value, FieldPtr group, Domain d, Stat s)
        : Field(t), mValue(std::move(value)), mGroup(std::move(group)), mDomain(d), mStat(s)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        FieldContext src = statsContextOn(ctx, mDomain);
        size_t n = src.size;
        std::vector<D3> v = evaluateAll<D3>(convertField(mValue, T::Vector), src);
        if (type() != T::Vector)
        {
            std::vector<double> f = evaluateAll<double>(mValue, src);
            for (size_t k = 0; k < n; ++k)
                v[k] = D3(f[k]);
        }
        std::vector<int64_t> groups = evaluateAll<int64_t>(mGroup, src);
        std::map<int64_t, std::vector<D3>> members;
        for (size_t k = 0; k < n; ++k)
            members[groups[k]].push_back(v[k]);
        std::map<int64_t, D3> result;
        for (auto& [g, vals] : members)
            result[g] = groupStatistic(vals, mStat);
        Column out;
        if (type() == T::Vector)
        {
            std::vector<D3> r(n);
            for (size_t k = 0; k < n; ++k)
                r[k] = result[groups[k]];
            out = Column::of(std::move(r));
        }
        else
        {
            std::vector<double> r(n);
            for (size_t k = 0; k < n; ++k)
                r[k] = result[groups[k]].x;
            out = Column::of(std::move(r));
        }
        return statsToContext(ctx, out, src.domain);
    }

private:
    FieldPtr mValue, mGroup;
    Domain mDomain;
    Stat mStat;
};

void statNode(const char* type, const char* label, const char* doc, std::vector<std::pair<const char*, Stat>> outputs)
{
    std::string t = type;
    registerDecl({
        .type = type,
        .label = label,
        .category = "Utilities",
        .doc = doc,
        .props = {prop("dataType", {"float", "vector"}, "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [t, outputs](const Group&, const GNode& n) {
                SocketType st = dataTypeOf(findDecl(t)->propValue(n, "dataType"));
                Sockets s{{fin("value", st, zeroJson(st)), fin("groupId", T::Int, 0, Implicit::None, "Elements with equal ids are measured separately.")}, {}};
                for (auto& [id, stat] : outputs)
                    s.outputs.push_back(fout(id, st));
                return s;
            },
        .exec =
            [outputs](ExecContext& c) {
                SocketType st = dataTypeOf(c.prop("dataType"));
                Domain d = domainOf(c.prop("domain"));
                for (auto& [id, stat] : outputs)
                    c.out(id, std::make_shared<GroupStatField>(st, c.field("value"), c.field("groupId"), d, stat));
            },
    });
}
} // namespace lila::gn::nodes::fieldstats

namespace lila::gn::nodes
{
void registerFieldStatsNodes()
{
    using fieldstats::Stat;
    fieldstats::statNode("FieldAverage", "Field Average", "Mean and median of a field over a domain, per group of equal group ids, given to every element of the group.",
                         {{"mean", Stat::Mean}, {"median", Stat::Median}});
    fieldstats::statNode("FieldMinMax", "Field Min & Max", "Smallest and largest value of a field over a domain, per group of equal group ids. Vectors compare per component.",
                         {{"min", Stat::Min}, {"max", Stat::Max}});
    fieldstats::statNode("FieldVariance", "Field Variance", "Standard deviation and variance of a field over a domain, per group of equal group ids. Vectors measure per component.",
                         {{"standardDeviation", Stat::Deviation}, {"variance", Stat::Variance}});
}
} // namespace lila::gn::nodes
