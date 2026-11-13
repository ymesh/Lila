// Architecture from a spec: walls with openings, buttresses, bands and cornices, plus columns, stairs, vaults, domes,
// freestanding arches and floor slabs, each a named region an agent can address.

#include "MasonryBuilding.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace masonry
{
using json = nlohmann::json;

namespace
{
glm::dvec3 flatUnit(glm::dvec3 d)
{
    d.y = 0.0;
    const double len = glm::length(d);
    return len > 1e-12 ? d / len : glm::dvec3(1, 0, 0);
}

/** A frame with x along the given horizontal direction and y up. */
glm::dmat3 uprightFrame(const glm::dvec3& along)
{
    const glm::dvec3 x = flatUnit(along);
    return glm::dmat3(x, kUp, glm::cross(x, kUp));
}

Stone block(const glm::dvec3& centre, const glm::dmat3& frame, const glm::dvec3& size, Role role)
{
    Stone s;
    s.center = centre;
    s.frame = frame;
    s.size = size;
    s.role = role;
    s.face = Face::Through;
    return s;
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("masonryBuilding: " + what); }

glm::dvec3 point(const json& j, const char* what, double scale)
{
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        fail(std::string(what) + " is [x, y, z]");
    return glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) * scale;
}

double num(const json& o, const char* key, double fallback)
{
    if (!o.contains(key))
        return fallback;
    if (!o[key].is_number())
        fail(std::string(key) + " is a number");
    return o[key].get<double>();
}

void checkKeys(const json& o, const char* what, std::initializer_list<const char*> allowed)
{
    if (!o.is_object())
        fail(std::string(what) + " is an object");
    for (const auto& [key, value] : o.items())
        if (std::find_if(allowed.begin(), allowed.end(), [&](const char* a) { return key == a; }) == allowed.end())
        {
            std::string list;
            for (const char* a : allowed)
                list += (list.empty() ? "" : ", ") + std::string(a);
            fail(std::string(what) + " has no key '" + key + "' (" + list + ")");
        }
}

std::string nameOf(const json& o, const std::string& fallback) { return o.value("name", fallback); }

ArchKind archOf(const json& o, const char* key, ArchKind fallback)
{
    if (!o.contains(key))
        return fallback;
    ArchKind k = fallback;
    if (!o[key].is_string() || !parseArch(o[key].get<std::string>(), &k))
        fail(std::string(key) + " is lintel, round, pointed, segmental or flat");
    return k;
}

double polygonArea(const std::vector<glm::dvec3>& pts)
{
    double a = 0.0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const glm::dvec3& p = pts[i];
        const glm::dvec3& q = pts[(i + 1) % pts.size()];
        a += p.x * q.z - q.x * p.z;
    }
    return a * 0.5;
}
} // namespace

std::vector<Stone> columnStones(const ColumnSpec& c, double joint, uint64_t seed)
{
    std::vector<Stone> out;
    const double d = std::max(c.diameter, 1.0), j = std::max(0.0, joint);
    const glm::dmat3 upright(1.0);
    double y = c.base.y;
    const double plinthH = c.plinth ? d * 0.18 : 0.0, baseH = c.plinth ? d * 0.12 : 0.0;
    const double echinusH = c.capital ? d * 0.14 : 0.0, abacusH = c.capital ? d * 0.12 : 0.0;
    const double shaftH = c.height - plinthH - baseH - echinusH - abacusH - j * (c.plinth ? 2 : 0) - j * (c.capital ? 2 : 0);
    if (shaftH <= j)
        return out;
    auto at = [&](double h) { return glm::dvec3(c.base.x, y + h * 0.5, c.base.z); };
    if (c.plinth)
    {
        Stone p = block(at(plinthH), upright, {d * 1.4, plinthH, d * 1.4}, Role::Column);
        p.grounded = true;
        out.push_back(p);
        y += plinthH + j;
        Stone b = block(at(baseH), upright, {d * 1.2, baseH, d * 1.2}, Role::Column);
        if (!c.square)
            b.shape = Shape::Drum;
        out.push_back(b);
        y += baseH + j;
    }
    const int drums = c.drums > 0 ? c.drums : std::max(1, int(std::lround(shaftH / std::max(d * 1.1, 1.0))));
    const double drumH = (shaftH - j * (drums - 1)) / drums;
    const double e = std::clamp(c.entasis, 0.0, 0.5);
    for (int k = 0; k < drums; ++k)
    {
        const double ta = double(k) / drums, tb = double(k + 1) / drums;
        const double bottom = d * (1.0 - e * ta), top = d * (1.0 - e * tb);
        Stone s = block(at(drumH), upright, {bottom, drumH, bottom}, Role::Column);
        // The narrow end of a shaped stone is its -Y end: turn the drum over so the shaft narrows upward.
        s.frame = glm::dmat3(glm::dvec3(-1, 0, 0), glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1));
        s.shape = c.square ? Shape::Wedge : Shape::Drum;
        s.taper = float(top / bottom);
        if (c.square)
            s.shape = e > 0.0 ? Shape::Wedge : Shape::Box;
        // Joints never quite line up: a little turn and slip per drum.
        const double turn = (unitRandom(mixKey(seed, 7, k)) - 0.5) * (c.square ? 0.02 : 6.0);
        const double cs = std::cos(turn), sn = std::sin(turn);
        const glm::dmat3 yaw(glm::dvec3(cs, 0, -sn), glm::dvec3(0, 1, 0), glm::dvec3(sn, 0, cs));
        s.frame = yaw * s.frame;
        s.center.x += (unitRandom(mixKey(seed, 8, k)) - 0.5) * j * 0.3;
        s.center.z += (unitRandom(mixKey(seed, 9, k)) - 0.5) * j * 0.3;
        s.grounded = !c.plinth && k == 0;
        out.push_back(s);
        y += drumH + j;
    }
    if (c.capital)
    {
        const double topD = d * (1.0 - e);
        Stone ech = block(at(echinusH), upright, {topD * 1.3, echinusH, topD * 1.3}, Role::Column);
        ech.shape = c.square ? Shape::Wedge : Shape::Drum;
        ech.taper = float(1.0 / 1.3);
        out.push_back(ech);
        y += echinusH + j;
        out.push_back(block(at(abacusH), upright, {topD * 1.45, abacusH, topD * 1.45}, Role::Column));
    }
    for (Stone& s : out)
        s.course = int(std::lround((s.center.y - c.base.y) / std::max(c.height, 1.0) * 100.0));
    return out;
}

std::vector<Stone> stairStones(const StairSpec& st, double stoneLength, double joint, std::vector<geo::Mesh>* fill)
{
    std::vector<Stone> out;
    const glm::dvec3 d = flatUnit(st.direction);
    const glm::dvec3 across = glm::cross(d, kUp); // right of the flight
    const glm::dmat3 frame(across, kUp, glm::cross(across, kUp));
    const double j = std::max(0.0, joint);
    const int perStep = std::max(1, int(std::ceil(st.width / std::max(stoneLength * 2.0, 1.0))));
    for (int k = 0; k < std::max(1, st.steps); ++k)
    {
        const double y0 = st.start.y + k * st.rise;
        const glm::dvec3 front = st.start + d * (k * st.going);
        const glm::dvec3 mid = front + d * (st.going * 0.5);
        // Stones across the step, every other step shifted by half a stone.
        const double unit = st.width / perStep;
        std::vector<double> cuts = {-st.width * 0.5};
        for (int i = 1; i < perStep; ++i)
            cuts.push_back(-st.width * 0.5 + unit * (i - ((k & 1) ? 0.5 : 0.0)));
        if (k & 1)
            cuts.push_back(st.width * 0.5 - unit * 0.5);
        cuts.push_back(st.width * 0.5);
        std::sort(cuts.begin(), cuts.end());
        for (size_t i = 0; i + 1 < cuts.size(); ++i)
        {
            const double a = cuts[i] + (i > 0 ? j * 0.5 : 0.0), b = cuts[i + 1] - (i + 2 < cuts.size() ? j * 0.5 : 0.0);
            if (b - a < 1.0)
                continue;
            Stone s = block(mid + across * ((a + b) * 0.5) + kUp * (st.rise * 0.5 + j * 0.5), frame,
                            {b - a, std::max(0.5, st.rise - j), st.going - j}, Role::Step);
            s.course = k;
            s.grounded = true;
            out.push_back(s);
        }
        if (fill && k > 0)
        {
            const glm::dvec3 a0 = front - across * (st.width * 0.5), a1 = front + across * (st.width * 0.5);
            const glm::dvec3 b0 = a0 + d * st.going, b1 = a1 + d * st.going;
            fill->push_back(prismMesh({a0, a1, b1, b0}, st.start.y, y0));
        }
    }
    return out;
}

std::vector<Stone> archRing(const ArchSpec& a, const glm::dvec3& origin, const glm::dvec3& direction, double depth)
{
    std::vector<Stone> out;
    const glm::dvec3 along = flatUnit(direction);
    for (const ArchStone& p : archStones(a))
    {
        const glm::dvec3 x3 = along * p.axisX.x + kUp * p.axisX.y;
        const glm::dvec3 y3 = along * p.axisY.x + kUp * p.axisY.y;
        Stone s;
        s.frame = glm::dmat3(x3, y3, glm::cross(x3, y3));
        s.center = origin + along * p.center.x + kUp * p.center.y;
        s.size = {p.top, p.height, depth};
        s.taper = float(std::clamp(p.bottom / std::max(p.top, 1e-9), 0.02, 1.0));
        s.shape = s.taper < 0.999f ? Shape::Wedge : Shape::Box;
        s.role = p.role;
        s.face = Face::Through;
        // Springers stand on whatever is under the springing line.
        s.grounded = false;
        out.push_back(s);
    }
    return out;
}

std::vector<Stone> vaultStones(const VaultSpec& v, const Units& stone, uint64_t seed)
{
    std::vector<Stone> out;
    const double j = std::max(0.0, stone.joint);
    if (v.groin)
    {
        // Each barrel keeps its web where it is the higher of the two: across its own axis nearer the middle than along it.
        const double half = v.profile.span * 0.5;
        VaultSpec x = v, z = v;
        x.groin = z.groin = false;
        x.from = v.centre - glm::dvec3(half, 0.0, 0.0);
        x.to = v.centre + glm::dvec3(half, 0.0, 0.0);
        z.from = v.centre - glm::dvec3(0.0, 0.0, half);
        z.to = v.centre + glm::dvec3(0.0, 0.0, half);
        for (const Stone& s : vaultStones(x, stone, seed))
            if (std::abs(s.center.z - v.centre.z) <= std::abs(s.center.x - v.centre.x))
                out.push_back(s);
        const size_t fromX = out.size();
        for (const Stone& s : vaultStones(z, stone, seed + 1))
        {
            if (std::abs(s.center.x - v.centre.x) > std::abs(s.center.z - v.centre.z))
                continue;
            bool clash = false;
            for (size_t k = 0; k < fromX && !clash; ++k)
                clash = glm::length(out[k].center - s.center) < glm::length(s.size) && overlaps(out[k], s, 0.01);
            if (!clash)
                out.push_back(s);
        }
        return out;
    }
    if (!v.dome)
    {
        const glm::dvec3 axis = v.to - v.from;
        const double length = glm::length(glm::dvec3(axis.x, 0.0, axis.z));
        if (length <= 1.0)
            return out;
        const glm::dvec3 d = flatUnit(axis);
        const glm::dvec3 across = glm::cross(d, kUp);
        const int rings = std::max(1, int(std::lround(length / (stone.length + j))));
        const double ringW = length / rings;
        for (int i = 0; i < rings; ++i)
        {
            ArchSpec a = v.profile;
            a.unit = stone.height + j;
            a.joint = j;
            // Alternate rings take one stone more, so their joints fall between the neighbours'.
            a.keystone = (i % 2) == 0;
            std::vector<Stone> ring = archRing(a, v.from + d * ((i + 0.5) * ringW), across, ringW - j);
            for (Stone& s : ring)
            {
                s.course = i;
                if (s.role == Role::Keystone)
                    s.role = Role::Voussoir;
            }
            out.insert(out.end(), ring.begin(), ring.end());
        }
        return out;
    }
    const double R = std::max(v.radius, 10.0), t = std::max(v.ring, 1.0);
    const double step = (stone.height + j) / R;
    const double top = v.oculus > 0.0 ? std::acos(std::clamp(v.oculus / R, 0.0, 1.0)) : kPi * 0.5 - step * 0.5;
    const int courses = std::max(1, int(std::floor(top / step)));
    const double dPhi = top / courses, gap = j / R;
    for (int c = 0; c < courses; ++c)
    {
        const double pa = c * dPhi + gap * 0.5, pb = (c + 1) * dPhi - gap * 0.5, pm = (pa + pb) * 0.5;
        const double circ = 2.0 * kPi * R * std::cos(pb);
        const int count = std::max(3, int(std::lround(circ / (stone.length + j))));
        const double dl = 2.0 * kPi / count;
        const double offset = (c & 1) ? dl * 0.5 : 0.0;
        for (int k = 0; k < count; ++k)
        {
            const double lm = offset + (k + 0.5) * dl;
            const glm::dvec3 radial(std::cos(pm) * std::cos(lm), std::sin(pm), std::cos(pm) * std::sin(lm));
            const glm::dvec3 east(-std::sin(lm), 0.0, std::cos(lm));
            const glm::dvec3 north = glm::cross(radial, east);
            Stone s;
            s.frame = glm::dmat3(east, north, glm::cross(east, north));
            s.center = v.centre + radial * (R + t * 0.5);
            const double width = 2.0 * R * std::cos(pb) * std::sin(dl * 0.5) - j;
            // A flat stone's ends stand off the course's circle by this much, toward the courses either side.
            const double sag = (width * 0.5) * (width * 0.5) / (2.0 * std::max(R * std::cos(pb), 1.0)) * std::sin(pm);
            s.size = {std::max(1.0, width), std::max(1.0, R * (pb - pa) - 2.0 * sag), t};
            s.role = Role::Voussoir;
            s.face = Face::Through;
            s.course = c;
            s.random = float(unitRandom(mixKey(seed, 31, c, k)));
            out.push_back(s);
        }
    }
    if (v.oculus <= 0.0)
    {
        // A cap stone over the last ring.
        const double rTop = R * std::cos(top + gap);
        Stone cap;
        cap.shape = Shape::Drum;
        cap.size = {std::max(2.0, 2.0 * rTop), t, std::max(2.0, 2.0 * rTop)};
        cap.center = v.centre + kUp * ((R + t) * std::sin(top) + t * 0.5 + j);
        cap.role = Role::Keystone;
        cap.face = Face::Through;
        cap.course = courses;
        out.push_back(cap);
    }
    return out;
}

namespace
{
/** The stretch of the line o + t * dir (seen from above) inside the outline that holds t = 0; lo > hi when o is outside. */
std::pair<double, double> stretchInside(const std::vector<glm::dvec3>& outline, const glm::dvec3& o, const glm::dvec3& dir)
{
    auto cross = [](double ax, double az, double bx, double bz) { return ax * bz - az * bx; };
    std::vector<double> ts;
    for (size_t i = 0, k = outline.size() - 1; i < outline.size(); k = i++)
    {
        const glm::dvec3 e = outline[i] - outline[k], w = outline[k] - o;
        const double den = cross(dir.x, dir.z, e.x, e.z);
        if (std::abs(den) < 1e-12)
            continue;
        const double u = cross(w.x, w.z, dir.x, dir.z) / den;
        if (u >= 0.0 && u < 1.0)
            ts.push_back(cross(w.x, w.z, e.x, e.z) / den);
    }
    std::sort(ts.begin(), ts.end());
    for (size_t i = 0; i + 1 < ts.size(); i += 2)
        if (ts[i] <= 0.0 && ts[i + 1] >= 0.0)
            return {ts[i], ts[i + 1]};
    return {1.0, 0.0};
}

/**
 * Shrinks a flat stone (half extents along d and a about centre) to the outline, trimming each side back to where the
 * outline crosses it. False when too little of it is left inside.
 */
bool trimToOutline(const std::vector<glm::dvec3>& outline, glm::dvec3& centre, const glm::dvec3& d, const glm::dvec3& a, double& length, double& width)
{
    double s0 = -length * 0.5, s1 = length * 0.5, w0 = -width * 0.5, w1 = width * 0.5;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (double off : {w0 * 0.999, w1 * 0.999})
            if (const auto [lo, hi] = stretchInside(outline, centre + a * off, d); lo <= hi)
            {
                s0 = std::max(s0, lo);
                s1 = std::min(s1, hi);
            }
        for (double off : {s0 * 0.999, s1 * 0.999})
            if (const auto [lo, hi] = stretchInside(outline, centre + d * off, a); lo <= hi)
            {
                w0 = std::max(w0, lo);
                w1 = std::min(w1, hi);
            }
    }
    if (s1 - s0 < length * 0.3 || w1 - w0 < width * 0.3)
        return false;
    centre += d * ((s0 + s1) * 0.5) + a * ((w0 + w1) * 0.5);
    length = s1 - s0;
    width = w1 - w0;
    return true;
}
} // namespace

std::vector<Stone> pavingStones(const PavingSpec& p, double joint, uint64_t seed)
{
    std::vector<Stone> out;
    if (p.outline.size() < 3)
        return out;
    const double j = std::max(0.0, joint), size = std::max(1.0, p.size);
    auto inside = [&](double x, double z) {
        bool in = false;
        for (size_t i = 0, k = p.outline.size() - 1; i < p.outline.size(); k = i++)
            if ((p.outline[i].z > z) != (p.outline[k].z > z) &&
                x < (p.outline[k].x - p.outline[i].x) * (z - p.outline[i].z) / (p.outline[k].z - p.outline[i].z) + p.outline[i].x)
                in = !in;
        return in;
    };
    const bool tiles = p.kind == "tiles";
    const glm::dvec3 d = tiles ? flatUnit(p.uphill) : glm::dvec3(1, 0, 0);
    const glm::dvec3 a = glm::cross(kUp, d); // across the rows
    double lo = 1e300, hi = -1e300, alo = 1e300, ahi = -1e300;
    for (const glm::dvec3& q : p.outline)
    {
        lo = std::min(lo, glm::dot(q, d));
        hi = std::max(hi, glm::dot(q, d));
        alo = std::min(alo, glm::dot(q, a));
        ahi = std::max(ahi, glm::dot(q, a));
    }
    if (tiles)
    {
        // Rows up the slope, each overlapping the one below by a third and lifted at its foot by the one below.
        const double theta = glm::radians(std::clamp(p.pitch, 5.0, 75.0));
        const glm::dvec3 up = d * std::cos(theta) + kUp * std::sin(theta), normal = kUp * std::cos(theta) - d * std::sin(theta);
        const double width = size, length = size * 1.5, thick = std::max(0.5, size * 0.08), step = length * (2.0 / 3.0);
        const double lift = std::atan((thick + j) / step);
        const glm::dvec3 tz = up * std::cos(lift) - normal * std::sin(lift), ty = normal * std::cos(lift) + up * std::sin(lift);
        const double run = (hi - lo) / std::cos(theta);
        const int rows = std::max(1, int(std::ceil((run - length) / step)) + 1);
        for (int r = 0; r < rows; ++r)
        {
            const double s0 = r * step;
            const double off = (r & 1) ? (width + j) * 0.5 : 0.0;
            for (double x = alo + off; x < ahi; x += width + j)
            {
                const double mid = s0 + length * 0.5;
                const glm::dvec3 plan = d * (lo + mid * std::cos(theta)) + a * (x + width * 0.5);
                if (!inside(plan.x, plan.z))
                    continue;
                Stone s;
                s.center = glm::dvec3(plan.x, p.y, plan.z) + kUp * (mid * std::sin(theta)) + normal * (length * 0.5 * std::tan(lift) + thick * 0.5);
                s.frame = glm::dmat3(glm::cross(ty, tz), ty, tz);
                s.size = {width, thick, length};
                s.role = Role::Paving;
                s.face = Face::Through;
                s.course = r;
                out.push_back(s);
            }
        }
        return out;
    }
    const bool flags = p.kind == "flags";
    const double rowW = flags ? size : size * 0.7, thick = flags ? std::max(1.0, size * 0.12) : size * 0.6;
    int row = 0;
    for (double z = alo; z < ahi; z += rowW + j, ++row)
    {
        double x = lo + ((row & 1) ? -size * 0.5 : 0.0);
        for (int k = 0; x < hi; ++k)
        {
            const double r = unitRandom(mixKey(seed, uint64_t(row), uint64_t(k), 91));
            const double len = flags ? size * (0.8 + 1.0 * r) : size * (0.85 + 0.3 * r);
            glm::dvec3 plan = d * (x + len * 0.5) + a * (z + rowW * 0.5);
            x += len + j;
            double length = len, width = rowW;
            if (!inside(plan.x, plan.z) || !trimToOutline(p.outline, plan, d, a, length, width))
                continue;
            Stone s;
            s.center = glm::dvec3(plan.x, p.y + thick * 0.5, plan.z);
            s.frame = glm::dmat3(d, kUp, glm::cross(d, kUp));
            s.size = {length, thick, width};
            s.role = Role::Paving;
            s.face = Face::Through;
            s.course = row;
            s.grounded = true;
            out.push_back(s);
        }
    }
    return out;
}

Plan planWalls(std::span<const Footprint> footprints, const BuildOptions& o, const std::string& region)
{
    Plan p;
    p.look = o;
    p.pitch = o.wall.stone.height + o.wall.stone.joint;
    for (size_t w = 0; w < footprints.size(); ++w)
    {
        WallPlan wp;
        wp.footprint = footprints[w];
        wp.params = o.wall;
        wp.region = int(p.regions.size());
        p.regions.push_back(region.empty() ? "wall_" + std::to_string(w) : (w == 0 ? region : region + "_" + std::to_string(w)));
        for (const Opening& op : wp.params.openings)
        {
            wp.openingRegions.push_back(int(p.regions.size()));
            p.regions.push_back(p.regions[size_t(wp.region)] + "/" + (op.name.empty() ? std::string("opening") : op.name));
        }
        p.walls.push_back(std::move(wp));
    }
    return p;
}

Plan planBuilding(const json& spec, const BuildOptions& lookIn)
{
    if (!spec.is_object())
        fail("the spec is a JSON object");
    checkKeys(spec, "the spec", {"scale", "seed", "stone", "bond", "look", "walls", "columns", "colonnades", "stairs", "vaults", "floors", "arches", "name"});
    Plan plan;
    plan.look = lookIn;
    const double scale = num(spec, "scale", 1.0);
    if (scale <= 0.0)
        fail("scale is above 0");
    BuildOptions& look = plan.look;
    look.wall.seed = uint32_t(num(spec, "seed", double(look.wall.seed)));
    if (spec.contains("stone"))
    {
        const json& s = spec["stone"];
        checkKeys(s, "stone", {"length", "height", "depth", "joint"});
        look.wall.stone.length = num(s, "length", look.wall.stone.length);
        look.wall.stone.height = num(s, "height", look.wall.stone.height);
        look.wall.stone.depth = num(s, "depth", look.wall.stone.depth);
        look.wall.stone.joint = num(s, "joint", look.wall.stone.joint);
    }
    look.wall.stone.length *= scale;
    look.wall.stone.height *= scale;
    look.wall.stone.depth *= scale;
    look.wall.stone.joint *= scale;
    if (spec.contains("bond") && (!spec["bond"].is_string() || !parseBond(spec["bond"].get<std::string>(), &look.wall.bond)))
        fail("bond is one of running, stack, english, flemish, header, gardenWall, herringbone, ashlar, rubble, dryStone");
    if (spec.contains("look"))
    {
        const json& l = spec["look"];
        checkKeys(l, "look", {"bevel", "wear", "variants", "tile", "jitter", "protrude"});
        look.bevel = num(l, "bevel", look.bevel);
        look.wear = std::clamp(num(l, "wear", look.wear), 0.0, 1.0);
        look.variants = std::clamp(int(num(l, "variants", look.variants)), 1, 64);
        look.tile = std::max(1.0, num(l, "tile", look.tile));
        look.wall.jitter = std::clamp(num(l, "jitter", look.wall.jitter), 0.0, 1.0);
        look.wall.protrude = std::max(0.0, num(l, "protrude", look.wall.protrude));
    }
    look.bevel *= scale;
    look.tile *= scale;
    look.wall.protrude *= scale;
    const Units& u = look.wall.stone;
    const double j = u.joint;
    plan.pitch = u.height + j;
    std::set<std::string> names;
    auto region = [&](const std::string& name) {
        if (!names.insert(name).second)
            fail("two regions are named '" + name + "'");
        plan.regions.push_back(name);
        return int(plan.regions.size() - 1);
    };
    auto addElements = [&](std::vector<Stone> stones, int reg) {
        for (Stone& s : stones)
        {
            s.region = reg;
            s.wall = -1;
            s.random = float(unitRandom(mixKey(look.wall.seed, 501, plan.elements.size())));
            plan.elements.push_back(s);
        }
    };

    if (spec.contains("walls"))
    {
        if (!spec["walls"].is_array())
            fail("walls is a list");
        for (size_t w = 0; w < spec["walls"].size(); ++w)
        {
            const json& wj = spec["walls"][w];
            checkKeys(wj, "a wall", {"name", "points", "closed", "height", "heights", "thickness", "batter", "bond", "bands", "bandProject",
                                     "cornice", "corniceProject", "openings", "buttresses", "cornerAngle"});
            WallPlan wp;
            wp.params = look.wall;
            const std::string name = nameOf(wj, "wall_" + std::to_string(w));
            if (!wj.contains("points") || !wj["points"].is_array() || wj["points"].size() < 2)
                fail(name + ": points is a list of at least two [x, y, z]");
            for (const json& pt : wj["points"])
                wp.footprint.points.push_back(point(pt, "a wall point", scale));
            wp.footprint.closed = wj.value("closed", false);
            if (wj.contains("heights"))
            {
                if (!wj["heights"].is_array() || wj["heights"].size() != wj["points"].size())
                    fail(name + ": heights has one number per point");
                for (const json& h : wj["heights"])
                    wp.footprint.heights.push_back(h.get<double>() * scale);
            }
            WallParams& p = wp.params;
            p.height = num(wj, "height", p.height / scale) * scale;
            p.thickness = num(wj, "thickness", p.thickness / scale) * scale;
            p.batter = num(wj, "batter", 0.0);
            p.cornerAngle = num(wj, "cornerAngle", p.cornerAngle);
            p.cornice = int(num(wj, "cornice", 0.0));
            p.corniceProject = num(wj, "corniceProject", 0.0) * scale;
            p.bandProject = num(wj, "bandProject", 0.0) * scale;
            p.bands.clear();
            if (wj.contains("bands"))
                for (const json& b : wj["bands"])
                    p.bands.push_back(b.get<double>() * scale);
            if (wj.contains("bond") && (!wj["bond"].is_string() || !parseBond(wj["bond"].get<std::string>(), &p.bond)))
                fail(name + ": unknown bond");
            wp.region = region(name);
            p.openings.clear();
            if (wj.contains("openings"))
                for (size_t k = 0; k < wj["openings"].size(); ++k)
                {
                    const json& oj = wj["openings"][k];
                    checkKeys(oj, "an opening", {"name", "at", "width", "height", "sill", "arch", "rise", "ring", "niche", "sillStone", "kind"});
                    Opening o;
                    const std::string kind = oj.value("kind", std::string(oj.value("sill", 0.0) > 0.0 ? "window" : "door"));
                    if (kind != "door" && kind != "window" && kind != "niche")
                        fail(name + ": an opening's kind is door, window or niche");
                    o.name = nameOf(oj, kind + "_" + std::to_string(k + 1));
                    if (!oj.contains("at"))
                        fail(name + "/" + o.name + ": at (cm along the wall's points) is required");
                    o.at = num(oj, "at", 0.0) * scale;
                    o.width = num(oj, "width", 100.0) * scale;
                    o.height = num(oj, "height", 200.0) * scale;
                    o.sill = num(oj, "sill", kind == "window" ? 100.0 : 0.0) * scale;
                    o.arch = archOf(oj, "arch", ArchKind::Round);
                    o.rise = num(oj, "rise", 0.0) * scale;
                    o.ring = num(oj, "ring", 0.0) * scale;
                    o.through = !(kind == "niche" || oj.value("niche", false));
                    o.sillStone = oj.value("sillStone", true);
                    wp.openingRegions.push_back(region(name + "/" + o.name));
                    p.openings.push_back(o);
                }
            if (wj.contains("buttresses"))
            {
                // Buttresses: short walls standing out of the outer face, sloping down away from it.
                const Footprint& f = wp.footprint;
                double area = 0.0;
                for (size_t i = 0; i < f.points.size(); ++i)
                {
                    const glm::dvec3& a = f.points[i];
                    const glm::dvec3& b = f.points[(i + 1) % f.points.size()];
                    area += a.x * b.z - b.x * a.z;
                }
                const bool leftOuter = !f.closed || area < 0.0;
                std::vector<double> along = {0.0};
                const size_t n = f.points.size();
                const size_t segs = f.closed ? n : n - 1;
                for (size_t s = 0; s < segs; ++s)
                    along.push_back(along.back() + glm::length(glm::dvec3(f.points[(s + 1) % n] - f.points[s]) * glm::dvec3(1, 0, 1)));
                for (size_t k = 0; k < wj["buttresses"].size(); ++k)
                {
                    const json& bj = wj["buttresses"][k];
                    checkKeys(bj, "a buttress", {"name", "at", "length", "width", "height", "top", "batter"});
                    const double at = std::clamp(num(bj, "at", 0.0) * scale, 0.0, along.back());
                    size_t s = std::min(size_t(std::upper_bound(along.begin(), along.end(), at) - along.begin()), along.size() - 1) - 1;
                    const double frac = along[s + 1] > along[s] ? (at - along[s]) / (along[s + 1] - along[s]) : 0.0;
                    const glm::dvec3 a = f.points[s], b = f.points[(s + 1) % n];
                    const glm::dvec3 P = a + (b - a) * frac;
                    const glm::dvec3 d = flatUnit(b - a);
                    const glm::dvec3 out = leftOuter ? glm::dvec3(-d.z, 0.0, d.x) : glm::dvec3(d.z, 0.0, -d.x);
                    WallPlan bw;
                    bw.params = look.wall;
                    bw.params.thickness = num(bj, "width", 2.0 * p.thickness / scale) * scale;
                    bw.params.batter = num(bj, "batter", 0.0);
                    const double length = num(bj, "length", 1.5 * p.thickness / scale) * scale;
                    const double h = num(bj, "height", p.height / scale) * scale;
                    const double top = num(bj, "top", h * 0.5 / scale) * scale;
                    // Clear of string courses and cornice stones standing out of the face.
                    double project = 0.0;
                    if (!p.bands.empty())
                        project = p.bandProject > 0.0 ? p.bandProject : u.depth / 3.0;
                    if (p.cornice > 0)
                        project = std::max(project, p.cornice * (p.corniceProject > 0.0 ? p.corniceProject : u.depth * 0.25));
                    const glm::dvec3 from = P + out * (p.thickness * 0.5 + j + project + p.protrude);
                    bw.footprint.points = {from, from + out * length};
                    bw.footprint.heights = {h, top};
                    bw.region = region(name + "/" + nameOf(bj, "buttress_" + std::to_string(k + 1)));
                    plan.walls.push_back(std::move(bw));
                }
            }
            plan.walls.push_back(std::move(wp));
        }
    }

    auto columnOf = [&](const json& cj, const glm::dvec3& base) {
        ColumnSpec c;
        c.base = base;
        c.height = num(cj, "height", 600.0) * scale;
        c.diameter = num(cj, "diameter", 60.0) * scale;
        c.drums = int(num(cj, "drums", 0.0));
        c.square = cj.value("square", false);
        c.plinth = cj.value("plinth", true);
        c.capital = cj.value("capital", true);
        c.entasis = num(cj, "entasis", 0.08);
        return c;
    };
    if (spec.contains("columns"))
        for (size_t k = 0; k < spec["columns"].size(); ++k)
        {
            const json& cj = spec["columns"][k];
            checkKeys(cj, "a column", {"name", "at", "height", "diameter", "drums", "square", "plinth", "capital", "entasis"});
            const int reg = region(nameOf(cj, "column_" + std::to_string(k + 1)));
            addElements(columnStones(columnOf(cj, point(cj.value("at", json::array({0, 0, 0})), "a column's at", scale)), j, mixKey(look.wall.seed, 11, k)), reg);
        }
    if (spec.contains("colonnades"))
        for (size_t k = 0; k < spec["colonnades"].size(); ++k)
        {
            const json& cj = spec["colonnades"][k];
            checkKeys(cj, "a colonnade", {"name", "from", "to", "count", "height", "diameter", "drums", "square", "plinth", "capital", "entasis"});
            const std::string name = nameOf(cj, "colonnade_" + std::to_string(k + 1));
            const glm::dvec3 a = point(cj.value("from", json::array({0, 0, 0})), "a colonnade's from", scale);
            const glm::dvec3 b = point(cj.value("to", json::array({0, 0, 0})), "a colonnade's to", scale);
            const int count = std::max(1, int(num(cj, "count", 2.0)));
            for (int i = 0; i < count; ++i)
            {
                const double t = count == 1 ? 0.5 : double(i) / (count - 1);
                const int reg = region(name + "/column_" + std::to_string(i + 1));
                addElements(columnStones(columnOf(cj, a + (b - a) * t), j, mixKey(look.wall.seed, 12, k, i)), reg);
            }
        }
    if (spec.contains("stairs"))
        for (size_t k = 0; k < spec["stairs"].size(); ++k)
        {
            const json& sj = spec["stairs"][k];
            checkKeys(sj, "a stair", {"name", "at", "direction", "width", "steps", "rise", "going"});
            StairSpec s;
            s.start = point(sj.value("at", json::array({0, 0, 0})), "a stair's at", scale);
            s.direction = point(sj.value("direction", json::array({1, 0, 0})), "a stair's direction", 1.0);
            s.width = num(sj, "width", 200.0) * scale;
            s.steps = std::max(1, int(num(sj, "steps", 8.0)));
            s.rise = num(sj, "rise", 18.0) * scale;
            s.going = num(sj, "going", 30.0) * scale;
            const int reg = region(nameOf(sj, "stair_" + std::to_string(k + 1)));
            addElements(stairStones(s, u.length, j, &plan.elementCore), reg);
        }
    if (spec.contains("vaults"))
        for (size_t k = 0; k < spec["vaults"].size(); ++k)
        {
            const json& vj = spec["vaults"][k];
            checkKeys(vj, "a vault", {"name", "kind", "from", "to", "span", "profile", "rise", "ring", "at", "radius", "oculus"});
            VaultSpec v;
            const std::string kind = vj.value("kind", std::string("barrel"));
            if (kind != "barrel" && kind != "dome" && kind != "groin")
                fail("a vault's kind is barrel, groin or dome");
            v.dome = kind == "dome";
            v.groin = kind == "groin";
            if (v.groin)
            {
                v.centre = point(vj.value("at", json::array({0, 0, 0})), "a groin vault's at", scale);
                v.profile.kind = archOf(vj, "profile", ArchKind::Round);
                v.profile.span = num(vj, "span", 600.0) * scale;
                v.profile.rise = num(vj, "rise", 0.0) * scale;
                v.profile.ring = num(vj, "ring", std::max(u.depth, v.profile.span * 0.06) / scale) * scale;
            }
            else if (v.dome)
            {
                v.centre = point(vj.value("at", json::array({0, 0, 0})), "a dome's at", scale);
                v.radius = num(vj, "radius", 300.0) * scale;
                v.ring = num(vj, "ring", std::max(u.depth, v.radius * 0.08) / scale) * scale;
                v.oculus = num(vj, "oculus", 0.0) * scale;
            }
            else
            {
                v.from = point(vj.value("from", json::array({0, 0, 0})), "a vault's from", scale);
                v.to = point(vj.value("to", json::array({1000, 0, 0})), "a vault's to", scale);
                v.profile.kind = archOf(vj, "profile", ArchKind::Round);
                if (v.profile.kind == ArchKind::Lintel)
                    fail("a vault's profile is round, pointed, segmental or flat");
                v.profile.span = num(vj, "span", 600.0) * scale;
                v.profile.rise = num(vj, "rise", 0.0) * scale;
                v.profile.ring = num(vj, "ring", std::max(u.depth, v.profile.span * 0.06) / scale) * scale;
            }
            const int reg = region(nameOf(vj, kind + "_" + std::to_string(k + 1)));
            addElements(vaultStones(v, u, mixKey(look.wall.seed, 13, k)), reg);
        }
    if (spec.contains("arches"))
        for (size_t k = 0; k < spec["arches"].size(); ++k)
        {
            const json& aj = spec["arches"][k];
            checkKeys(aj, "an arch", {"name", "at", "direction", "span", "kind", "rise", "ring", "depth"});
            ArchSpec a;
            a.kind = archOf(aj, "kind", ArchKind::Round);
            if (a.kind == ArchKind::Lintel)
                fail("an arch's kind is round, pointed, segmental or flat");
            a.span = num(aj, "span", 300.0) * scale;
            a.rise = num(aj, "rise", 0.0) * scale;
            a.ring = num(aj, "ring", std::max(u.length * 0.6, u.height * 1.5) / scale) * scale;
            a.unit = u.height + j;
            a.joint = j;
            const double depth = num(aj, "depth", u.depth * 2.0 / scale) * scale;
            const int reg = region(nameOf(aj, "arch_" + std::to_string(k + 1)));
            addElements(archRing(a, point(aj.value("at", json::array({0, 0, 0})), "an arch's at", scale),
                                 point(aj.value("direction", json::array({1, 0, 0})), "an arch's direction", 1.0), depth),
                        reg);
        }
    if (spec.contains("floors"))
        for (size_t k = 0; k < spec["floors"].size(); ++k)
        {
            const json& fj = spec["floors"][k];
            checkKeys(fj, "a floor", {"name", "points", "y", "thickness", "surface", "size", "pitch", "uphill"});
            SlabPlan s;
            if (!fj.contains("points") || fj["points"].size() < 3)
                fail("a floor's points are at least three [x, y, z] round its outline");
            for (const json& pt : fj["points"])
                s.outline.push_back(point(pt, "a floor point", scale));
            if (polygonArea(s.outline) < 0.0)
                std::reverse(s.outline.begin(), s.outline.end());
            s.y1 = num(fj, "y", 300.0) * scale;
            s.y0 = s.y1 - std::max(1.0, num(fj, "thickness", 30.0) * scale);
            s.region = region(nameOf(fj, "floor_" + std::to_string(k + 1)));
            plan.slabs.push_back(s);
            if (const std::string surface = fj.value("surface", std::string("slab")); surface != "slab")
            {
                if (surface != "cobbles" && surface != "flags" && surface != "tiles")
                    fail("a floor's surface is slab, cobbles, flags or tiles");
                PavingSpec pv;
                pv.outline = s.outline;
                pv.y = s.y1 + j;
                pv.kind = surface;
                pv.size = num(fj, "size", surface == "flags" ? 60.0 : (surface == "tiles" ? 25.0 : 12.0)) * scale;
                pv.pitch = num(fj, "pitch", 35.0);
                if (fj.contains("uphill"))
                    pv.uphill = point(fj["uphill"], "a floor's uphill", 1.0);
                addElements(pavingStones(pv, j, mixKey(look.wall.seed, 14, k)), s.region);
            }
        }
    return plan;
}

json specSchema()
{
    auto p = [](const char* type, json def, const char* doc) { return json{{"type", type}, {"default", def}, {"description", doc}}; };
    const json opening = {
        {"name", p("string", "door_1", "Region name under the wall: <wall>/<name>.")},
        {"kind", p("string", "door", "door, window (default when sill > 0) or niche (cut into the outer face only).")},
        {"at", p("number", nullptr, "Centre, cm along the wall's points from the first. Required.")},
        {"width", p("number", 100, "Clear width, cm.")},
        {"height", p("number", 200, "Sill to the top of the opening, arch included, cm.")},
        {"sill", p("number", 0, "Height of the sill above the ground, cm; windows default to 100.")},
        {"arch", p("string", "round", "lintel, round, pointed, segmental or flat.")},
        {"rise", p("number", 0, "Arch rise, cm; 0 is the kind's own.")},
        {"ring", p("number", 0, "Voussoir depth, cm; 0 is about a stone and a half.")},
        {"sillStone", p("boolean", true, "A projecting sill stone under windows.")}};
    const json column = {{"name", p("string", "column_1", "Region name.")},
                         {"at", p("array", json::array({0, 0, 0}), "Base centre [x, y, z], cm.")},
                         {"height", p("number", 600, "Plinth to abacus, cm.")},
                         {"diameter", p("number", 60, "Shaft diameter at the bottom, cm.")},
                         {"drums", p("integer", 0, "Shaft drums; 0 is about one per diameter.")},
                         {"square", p("boolean", false, "Square pillar instead of a round column.")},
                         {"plinth", p("boolean", true, "Plinth and base moulding.")},
                         {"capital", p("boolean", true, "Echinus and abacus.")},
                         {"entasis", p("number", 0.08, "How much narrower the shaft is at the top, fraction of the diameter.")}};
    json colonnade = column;
    colonnade.erase("at");
    colonnade["from"] = p("array", json::array({0, 0, 0}), "First column's base.");
    colonnade["to"] = p("array", json::array({0, 0, 0}), "Last column's base.");
    colonnade["count"] = p("integer", 2, "Columns, evenly spaced; regions <name>/column_<k>.");
    return {
        {"description", "One JSON describing a masonry building. Lengths are cm before scale; Y is up. Every wall, opening, column, "
                        "stair, vault, arch and floor is a named region (masonryInfo lists them) that ruin and checks address."},
        {"properties",
         {{"scale", p("number", 1, "Multiplies every length in the spec, stone sizes included: 8 makes a building for creatures eight times human size.")},
          {"seed", p("integer", 1, "Random seed for wander, wear and cut positions.")},
          {"stone", {{"type", "object"}, {"properties", {{"length", p("number", 21.5, "cm")}, {"height", p("number", 6.5, "cm")}, {"depth", p("number", 10.25, "cm")}, {"joint", p("number", 1, "cm")}}}}},
          {"bond", p("string", "running", "running, stack, english, flemish, header, gardenWall, herringbone, ashlar, rubble or dryStone.")},
          {"look", {{"type", "object"}, {"properties", {{"bevel", p("number", 0.5, "cm")}, {"wear", p("number", 0.5, "0 to 1")}, {"variants", p("integer", 6, "1 to 64")},
                                                        {"tile", p("number", 100, "cm of stone per UV unit")}, {"jitter", p("number", 1, "0 to 1")}, {"protrude", p("number", 0.3, "cm")}}}}},
          {"walls",
           {{"type", "array"},
            {"items",
             {{"name", p("string", "wall_1", "Region name, e.g. north_wall.")},
              {"points", p("array", nullptr, "Footprint [[x, y, z], ...]; each point's y is the ground there, so walls follow terrain. Required.")},
              {"closed", p("boolean", false, "Join the last point to the first.")},
              {"height", p("number", 200, "Wall height above the ground, cm.")},
              {"heights", p("array", nullptr, "Height above each point instead, cm: sloping and broken tops, gables.")},
              {"thickness", p("number", 21.5, "At the base, cm.")},
              {"batter", p("number", 0, "Degrees each face leans in.")},
              {"bond", p("string", "running", "Overrides the building's bond.")},
              {"bands", p("array", json::array(), "Heights of string courses above the lowest ground, cm.")},
              {"bandProject", p("number", 0, "String course projection, cm; 0 is a third of a stone depth.")},
              {"cornice", p("integer", 0, "Top courses corbelled out.")},
              {"corniceProject", p("number", 0, "Projection per corbelled course, cm; 0 is a quarter of a stone depth.")},
              {"cornerAngle", p("number", 30, "Turns sharper than this are corners, degrees.")},
              {"openings", {{"type", "array"}, {"items", opening}}},
              {"buttresses",
               {{"type", "array"},
                {"items",
                 {{"name", p("string", "buttress_1", "Region name under the wall.")},
                  {"at", p("number", 0, "cm along the wall's points.")},
                  {"length", p("number", nullptr, "How far it stands out, cm; 1.5 wall thicknesses.")},
                  {"width", p("number", nullptr, "cm; twice the wall's thickness.")},
                  {"height", p("number", nullptr, "At the wall, cm; the wall's height.")},
                  {"top", p("number", nullptr, "At its outer end, cm; half its height.")},
                  {"batter", p("number", 0, "Degrees.")}}}}}}}}},
          {"columns", {{"type", "array"}, {"items", column}}},
          {"colonnades", {{"type", "array"}, {"items", colonnade}}},
          {"stairs",
           {{"type", "array"},
            {"items",
             {{"name", p("string", "stair_1", "Region name.")},
              {"at", p("array", json::array({0, 0, 0}), "Middle of the bottom step's front edge.")},
              {"direction", p("array", json::array({1, 0, 0}), "Uphill, horizontal.")},
              {"width", p("number", 200, "cm")},
              {"steps", p("integer", 8, "")},
              {"rise", p("number", 18, "cm per step")},
              {"going", p("number", 30, "Tread depth, cm.")}}}}},
          {"vaults",
           {{"type", "array"},
            {"items",
             {{"name", p("string", "barrel_1", "Region name.")},
              {"kind", p("string", "barrel", "barrel, groin (two barrels crossing over a square bay at at, span wide) or dome.")},
              {"from", p("array", nullptr, "Barrel: the axis start at springing height.")},
              {"to", p("array", nullptr, "Barrel: the axis end.")},
              {"span", p("number", 600, "Barrel: intrados width, cm.")},
              {"profile", p("string", "round", "Barrel: round, pointed, segmental or flat.")},
              {"rise", p("number", 0, "Barrel: rise, cm; 0 is the profile's own.")},
              {"ring", p("number", nullptr, "Shell thickness, cm.")},
              {"at", p("array", nullptr, "Dome: centre of the springing circle.")},
              {"radius", p("number", 300, "Dome: intrados radius, cm.")},
              {"oculus", p("number", 0, "Dome: radius of the opening at the top, cm; 0 closes it with a cap stone.")}}}}},
          {"arches",
           {{"type", "array"},
            {"items",
             {{"name", p("string", "arch_1", "Region name.")},
              {"at", p("array", json::array({0, 0, 0}), "Middle of the springing line.")},
              {"direction", p("array", json::array({1, 0, 0}), "Across the span.")},
              {"span", p("number", 300, "cm")},
              {"kind", p("string", "round", "round, pointed, segmental or flat.")},
              {"rise", p("number", 0, "cm; 0 is the kind's own.")},
              {"ring", p("number", nullptr, "Voussoir depth, cm.")},
              {"depth", p("number", nullptr, "Through the arch, cm; two stone depths.")}}}}},
          {"floors",
           {{"type", "array"},
            {"items",
             {{"name", p("string", "floor_1", "Region name; roofs are floors at the top.")},
              {"points", p("array", nullptr, "Outline [[x, y, z], ...], y ignored.")},
              {"y", p("number", 300, "Top of the slab, cm.")},
              {"thickness", p("number", 30, "cm")},
              {"surface", p("string", "slab", "slab, cobbles, flags or tiles (a pitched roof of overlapping tiles on the slab).")},
              {"size", p("number", nullptr, "Cobble length, flag row width or tile width, cm.")},
              {"pitch", p("number", 35, "Tiles: roof slope, degrees.")},
              {"uphill", p("array", json::array({0, 0, 1}), "Tiles: the way the roof rises.")}}}}}}}};
}
} // namespace masonry
