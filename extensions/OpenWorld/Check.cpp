// remixCheck's findings: what a remix broke, each with where it is so the overlay and an agent can go to it.

#include "Remix.h"

#include <lila/Profile.h>

namespace openworld
{
json checkRemix(const RemixSpec& spec, const RemixResult& r)
{
    LILA_ZONE();

    json issues = json::array();
    json counts = {{"riverNoDrain", 0}, {"danglingRoad", 0}, {"coastGap", 0}, {"overlap", 0}, {"fold", 0}, {"source", 0}};
    auto add = [&](const char* kind, std::string id, Vec2 at, std::string what) {
        issues.push_back({{"kind", kind}, {"id", std::move(id)}, {"at", {at.x, at.y}}, {"what", std::move(what)}});
        counts[kind] = counts[kind].get<int>() + 1;
    };

    for (const std::string& e : r.errors)
        add("source", "", Vec2(0.0), e);

    for (const RiverState& s : r.rivers)
        if (!s.drains && s.drainedBefore)
            add("riverNoDrain", std::to_string(s.way), s.end, (s.name.empty() ? std::string("a river") : s.name) + " ends in a basin the remix made");

    // Ends off the output map (a source's leftovers outside a cropped map) or on its edge can't be joined on it and
    // aren't on the map anyone builds; they are counted, not reported.
    const double edge = 2.0 * std::max(r.after.cell, 1.0);
    auto onMap = [&](Vec2 p) { return p.x > r.lo.x + edge && p.x < r.hi.x - edge && p.y > r.lo.y + edge && p.y < r.hi.y - edge; };
    int offMap = 0;
    for (const CutEnd& e : r.roadEnds)
        if (!e.joined)
        {
            if (!onMap(e.at))
                ++offMap;
            else
                add("danglingRoad", std::to_string(e.node), e.at, (e.cls.empty() ? std::string("road") : e.cls) + " cut by a region and not joined again");
        }

    for (const CutEnd& e : r.coastEnds)
        if (!e.joined)
        {
            if (!onMap(e.at))
                ++offMap;
            else
                add("coastGap", std::to_string(e.node), e.at, "coastline cut by a region and not re-merged");
        }

    for (size_t i = 0; i < spec.regions.size(); ++i)
        for (size_t k = i + 1; k < spec.regions.size(); ++k)
        {
            const Region& a = spec.regions[i];
            const Region& b = spec.regions[k];
            const Polygon ta = a.target(), tb = b.target();
            if (overlaps(ta, tb))
                add("overlap", a.id + "," + b.id, (centroid(ta) + centroid(tb)) * 0.5, a.id + " and " + b.id + " end up on top of each other");
            // A rigid region inside the land another region lifts out moves twice.
            else if ((warps(a.op) && punches(b.op) && overlaps(a.polygon, b.polygon)) || (warps(b.op) && punches(a.op) && overlaps(a.polygon, b.polygon)))
                add("overlap", a.id + "," + b.id, (centroid(a.polygon) + centroid(b.polygon)) * 0.5, a.id + " and " + b.id + " claim the same land");
        }

    int folded = 0;
    double minJacobian = 1.0;
    if (r.warp && !r.warp->identity())
    {
        const Warp::Folds f = r.warp->folds(r.lo, r.hi, Warp::kFoldGrid);
        folded = f.folded;
        minJacobian = f.minJacobian;
        for (size_t i = 0; i < f.where.size() && i < 16; ++i)
            add("fold", "", f.where[i], "the warp folds the land over itself here; move the pins or regions apart or raise warp.maxSteps");
        if (f.folded > int(f.where.size()))
            counts["fold"] = f.folded;
    }

    bool ok = true;
    for (const auto& [k, v] : counts.items())
        ok = ok && v.get<int>() == 0;
    return {{"ok", ok}, {"counts", counts}, {"issues", issues}, {"warp", {{"foldedCells", folded}, {"minJacobian", minJacobian}}}, {"offMapEnds", offMap}};
}
} // namespace openworld
