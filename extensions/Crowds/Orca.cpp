#include "Orca.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace crowds::orca
{
namespace
{
constexpr double kEps = 1e-9;

struct Line
{
    glm::dvec2 point{0.0};
    glm::dvec2 direction{1.0, 0.0};
};

double det(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

bool program1(const std::vector<Line>& lines, size_t no, double radius, glm::dvec2 opt, bool directionOpt, glm::dvec2& result)
{
    const double dot = glm::dot(lines[no].point, lines[no].direction);
    const double disc = dot * dot + radius * radius - glm::dot(lines[no].point, lines[no].point);
    if (disc < 0.0)
        return false;
    const double s = std::sqrt(disc);
    double left = -dot - s, right = -dot + s;
    for (size_t i = 0; i < no; ++i)
    {
        const double den = det(lines[no].direction, lines[i].direction);
        const double num = det(lines[i].direction, lines[no].point - lines[i].point);
        if (std::abs(den) <= kEps)
        {
            if (num < 0.0)
                return false;
            continue;
        }
        const double t = num / den;
        if (den >= 0.0)
            right = std::min(right, t);
        else
            left = std::max(left, t);
        if (left > right)
            return false;
    }
    if (directionOpt)
        result = lines[no].point + (glm::dot(opt, lines[no].direction) > 0.0 ? right : left) * lines[no].direction;
    else
        result = lines[no].point + std::clamp(glm::dot(lines[no].direction, opt - lines[no].point), left, right) * lines[no].direction;
    return true;
}

size_t program2(const std::vector<Line>& lines, double radius, glm::dvec2 opt, bool directionOpt, glm::dvec2& result)
{
    if (directionOpt)
        result = opt * radius;
    else if (glm::dot(opt, opt) > radius * radius)
        result = glm::normalize(opt) * radius;
    else
        result = opt;
    for (size_t i = 0; i < lines.size(); ++i)
        if (det(lines[i].direction, lines[i].point - result) > 0.0)
        {
            const glm::dvec2 keep = result;
            if (!program1(lines, i, radius, opt, directionOpt, result))
            {
                result = keep;
                return i;
            }
        }
    return lines.size();
}

/** When the constraints can't all hold: the velocity that breaks the agent lines least, obstacle lines kept. */
void program3(const std::vector<Line>& lines, size_t obstacleLines, size_t begin, double radius, glm::dvec2& result)
{
    double distance = 0.0;
    for (size_t i = begin; i < lines.size(); ++i)
    {
        if (det(lines[i].direction, lines[i].point - result) <= distance)
            continue;
        std::vector<Line> proj(lines.begin(), lines.begin() + std::ptrdiff_t(obstacleLines));
        for (size_t j = obstacleLines; j < i; ++j)
        {
            Line line;
            const double d = det(lines[i].direction, lines[j].direction);
            if (std::abs(d) <= kEps)
            {
                if (glm::dot(lines[i].direction, lines[j].direction) > 0.0)
                    continue;
                line.point = 0.5 * (lines[i].point + lines[j].point);
            }
            else
                line.point = lines[i].point + (det(lines[j].direction, lines[i].point - lines[j].point) / d) * lines[i].direction;
            const glm::dvec2 dir = lines[j].direction - lines[i].direction;
            if (glm::length(dir) <= kEps)
                continue;
            line.direction = glm::normalize(dir);
            proj.push_back(line);
        }
        const glm::dvec2 keep = result;
        if (program2(proj, radius, {-lines[i].direction.y, lines[i].direction.x}, true, result) < proj.size())
            result = keep;
        distance = det(lines[i].direction, lines[i].point - result);
    }
}

/** The ORCA half-plane against something at relative position rel moving at relative velocity relVel. */
Line avoidLine(glm::dvec2 rel, glm::dvec2 relVel, double combined, double horizon, double responsibility, glm::dvec2 velocity, double dt)
{
    Line line;
    glm::dvec2 u;
    const double distSq = glm::dot(rel, rel);
    const double rSq = combined * combined;
    if (distSq > rSq)
    {
        const glm::dvec2 w = relVel - rel / horizon;
        const double wLenSq = glm::dot(w, w);
        const double dot1 = glm::dot(w, rel);
        if (dot1 < 0.0 && dot1 * dot1 > rSq * wLenSq)
        {
            const double wLen = std::sqrt(std::max(wLenSq, kEps));
            const glm::dvec2 unit = w / wLen;
            line.direction = {unit.y, -unit.x};
            u = (combined / horizon - wLen) * unit;
        }
        else
        {
            const double leg = std::sqrt(std::max(distSq - rSq, 0.0));
            if (det(rel, w) > 0.0)
                line.direction = glm::dvec2(rel.x * leg - rel.y * combined, rel.x * combined + rel.y * leg) / distSq;
            else
                line.direction = -glm::dvec2(rel.x * leg + rel.y * combined, -rel.x * combined + rel.y * leg) / distSq;
            u = glm::dot(relVel, line.direction) * line.direction - relVel;
        }
    }
    else
    {
        // Already overlapping: resolve within one step.
        const glm::dvec2 w = relVel - rel / dt;
        const double wLen = std::max(glm::length(w), kEps);
        const glm::dvec2 unit = w / wLen;
        line.direction = {unit.y, -unit.x};
        u = (combined / dt - wLen) * unit;
    }
    line.point = velocity + responsibility * u;
    return line;
}

int64_t cellKey(int64_t x, int64_t y) { return (x << 32) ^ (y & 0xffffffff); }
} // namespace

double polygonDistance(const Obstacle& o, glm::dvec2 p, glm::dvec2* nearest)
{
    const size_t n = o.points.size();
    if (n == 0)
        return 1e30;
    double best = 1e30;
    glm::dvec2 at = o.points[0];
    bool inside = n >= 3;
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 a = o.points[i], b = o.points[(i + 1) % n];
        const glm::dvec2 d = b - a;
        const double t = std::clamp(glm::dot(p - a, d) / std::max(glm::dot(d, d), kEps), 0.0, 1.0);
        const glm::dvec2 q = a + d * t;
        const double dist = glm::length(p - q);
        if (dist < best)
            best = dist, at = q;
        if (det(d, p - a) < 0.0)
            inside = false;
    }
    if (nearest)
        *nearest = at;
    return inside ? -best : best;
}

std::vector<glm::dvec2> solve(const std::vector<Agent>& agents, const std::vector<Obstacle>& obstacles, const Options& o)
{
    const size_t n = agents.size();
    std::vector<glm::dvec2> out(n);
    const double cell = std::max(o.neighbourDistance, 0.5);
    std::unordered_map<int64_t, std::vector<size_t>> grid;
    grid.reserve(n);
    for (size_t i = 0; i < n; ++i)
        grid[cellKey(int64_t(std::floor(agents[i].position.x / cell)), int64_t(std::floor(agents[i].position.y / cell)))].push_back(i);

    constexpr double kDt = 1.0 / 30.0;
    std::vector<std::pair<double, size_t>> near;
    std::vector<Line> lines;
    for (size_t i = 0; i < n; ++i)
    {
        const Agent& a = agents[i];
        if (!a.active)
        {
            out[i] = a.preferred;
            continue;
        }
        lines.clear();
        // ORCA alone stalls an agent heading straight into an obstacle: aim past its nearer silhouette corner instead.
        glm::dvec2 preferred = a.preferred;
        const double prefSpeed = glm::length(preferred);
        if (prefSpeed > kEps)
            for (const Obstacle& ob : obstacles)
            {
                const glm::dvec2 dir = preferred / prefSpeed;
                constexpr double kLook = 3.0;
                if (ob.points.size() < 2 || polygonDistance(ob, a.position) > kLook + a.radius)
                    continue;
                glm::dvec2 centre{0.0};
                for (const glm::dvec2& p : ob.points)
                    centre += p / double(ob.points.size());
                if (glm::dot(centre - a.position, dir) <= 0.0)
                    continue;
                double lo = 1e9, hi = -1e9;
                glm::dvec2 loAt{0.0}, hiAt{0.0};
                for (const glm::dvec2& p : ob.points)
                {
                    const glm::dvec2 away = glm::length(p - centre) > kEps ? glm::normalize(p - centre) : glm::dvec2(0.0);
                    const glm::dvec2 v = p + away * (a.radius + 0.15) - a.position;
                    const double th = std::atan2(det(dir, v), glm::dot(dir, v));
                    if (th < lo)
                        lo = th, loAt = v;
                    if (th > hi)
                        hi = th, hiAt = v;
                }
                if (!(lo < 0.0 && hi > 0.0))
                    continue;
                const glm::dvec2 aim = -lo < hi ? loAt : hiAt;
                if (glm::length(aim) > kEps)
                    preferred = glm::normalize(aim) * prefSpeed;
            }
        for (const Obstacle& ob : obstacles)
        {
            glm::dvec2 q;
            const double d = polygonDistance(ob, a.position, &q);
            if (d > a.maxSpeed * o.obstacleHorizon + a.radius)
                continue;
            if (d <= 0.0)
            {
                // Inside: the only allowed way is out, across the nearest edge.
                const glm::dvec2 outward = glm::length(a.position - q) > kEps ? glm::normalize(q - a.position) : glm::dvec2(1.0, 0.0);
                Line l;
                l.direction = {outward.y, -outward.x};
                l.point = outward * std::min((-d + a.radius) / std::max(o.obstacleHorizon, kDt), a.maxSpeed);
                lines.push_back(l);
                continue;
            }
            lines.push_back(avoidLine(q - a.position, a.velocity, a.radius, o.obstacleHorizon, 1.0, a.velocity, kDt));
        }
        const size_t obstacleLines = lines.size();

        near.clear();
        const int64_t cx = int64_t(std::floor(a.position.x / cell)), cy = int64_t(std::floor(a.position.y / cell));
        const double range2 = o.neighbourDistance * o.neighbourDistance;
        for (int64_t gx = cx - 1; gx <= cx + 1; ++gx)
            for (int64_t gy = cy - 1; gy <= cy + 1; ++gy)
            {
                auto it = grid.find(cellKey(gx, gy));
                if (it == grid.end())
                    continue;
                for (size_t j : it->second)
                {
                    if (j == i)
                        continue;
                    const glm::dvec2 d = agents[j].position - a.position;
                    const double d2 = glm::dot(d, d);
                    if (d2 < range2)
                        near.push_back({d2, j});
                }
            }
        std::sort(near.begin(), near.end());
        if (near.size() > size_t(std::max(o.maxNeighbours, 0)))
            near.resize(size_t(std::max(o.maxNeighbours, 0)));
        for (const auto& [d2, j] : near)
        {
            const Agent& b = agents[j];
            lines.push_back(avoidLine(b.position - a.position, a.velocity - b.velocity, a.radius + b.radius, o.timeHorizon, b.active ? 0.5 : 1.0,
                                      a.velocity, kDt));
        }
        glm::dvec2 v{0.0};
        const size_t fail = program2(lines, a.maxSpeed, preferred, false, v);
        if (fail < lines.size())
            program3(lines, obstacleLines, fail, a.maxSpeed, v);
        out[i] = v;
    }
    return out;
}
} // namespace crowds::orca
