// Placeholder modules: frames, doors, shutters, signs as blank boards with a text-ready face, balconies, railings, AC
// boxes, tanks. Built procedurally at the module's size (front +Z, bottom centre at the origin), each with its own uv0,
// and cached by kind and size. The slot types Buildings registers are listed here too.

#include "Buildings.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace buildings
{
namespace
{
const glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 0.0f};

/** A box from lo to hi with outward faces and uv0 projected per face (cm / 100). */
void box(Soup& s, Vec3 lo, Vec3 hi, int mat)
{
    if (hi.x - lo.x < 0.01 || hi.y - lo.y < 0.01 || hi.z - lo.z < 0.01)
        return;
    const Vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                       {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    // Counter-clockwise seen from outside.
    static const int F[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    static const int A[6][2] = {{0, 1}, {0, 1}, {2, 1}, {2, 1}, {0, 2}, {0, 2}};
    for (int f = 0; f < 6; ++f)
    {
        Vec3 p[4];
        glm::vec2 uv[4];
        for (int k = 0; k < 4; ++k)
        {
            p[k] = c[F[f][k]];
            uv[k] = {float(p[k][A[f][0]] / 100.0), float(p[k][A[f][1]] / 100.0)};
        }
        s.poly(p, uv, 4, mat, TrimNone, -1, PartOther, -1, kWhite);
    }
}

/** A quad facing +Z at depth z, x0..x1 by y0..y1, uv 0 to 1 over it. */
void card(Soup& s, double x0, double x1, double y0, double y1, double z, int mat)
{
    const Vec3 p[4] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
    const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    s.poly(p, uv, 4, mat, TrimNone, -1, PartOther, -1, kWhite);
}

/** An n-sided prism along Y, radius r, centred at (x, z). */
void prism(Soup& s, double x, double z, double r, double y0, double y1, int sides, int mat)
{
    const double k2 = 2.0 * std::numbers::pi / sides;
    for (int k = 0; k < sides; ++k)
    {
        const double a0 = k * k2, a1 = (k + 1) * k2;
        const Vec3 p0{x + r * std::cos(a0), y0, z - r * std::sin(a0)}, p1{x + r * std::cos(a1), y0, z - r * std::sin(a1)};
        const Vec3 p[4] = {p0, p1, p1 + Vec3(0, y1 - y0, 0), p0 + Vec3(0, y1 - y0, 0)};
        const glm::vec2 uv[4] = {{float(k / double(sides)), 0}, {float((k + 1) / double(sides)), 0}, {float((k + 1) / double(sides)), 1},
                                 {float(k / double(sides)), 1}};
        s.poly(p, uv, 4, mat, TrimNone, -1, PartOther, -1, kWhite);
    }
    std::vector<Vec3> top, bottom;
    for (int k = 0; k < sides; ++k)
    {
        top.push_back({x + r * std::cos(k * k2), y1, z - r * std::sin(k * k2)});
        bottom.push_back({x + r * std::cos(-k * k2), y0, z - r * std::sin(-k * k2)});
    }
    std::vector<glm::vec2> uv(size_t(sides), glm::vec2(0.5f));
    s.poly(top.data(), uv.data(), top.size(), mat, TrimNone, -1, PartOther, -1, kWhite);
    s.poly(bottom.data(), uv.data(), bottom.size(), mat, TrimNone, -1, PartOther, -1, kWhite);
}

/** A rectangular frame of bars of width b round x0..x1 by y0..y1, depth z0..z1; top only when `top`, bottom when `bottom`. */
void frame(Soup& s, double x0, double x1, double y0, double y1, double z0, double z1, double b, int mat, bool bottom = true)
{
    box(s, {x0, y0, z0}, {x0 + b, y1, z1}, mat);
    box(s, {x1 - b, y0, z0}, {x1, y1, z1}, mat);
    box(s, {x0 + b, y1 - b, z0}, {x1 - b, y1, z1}, mat);
    if (bottom)
        box(s, {x0 + b, y0, z0}, {x1 - b, y0 + b, z1}, mat);
}

Soup build(const std::string& kind, Vec3 sz, int variant)
{
    Soup s;
    // Variants from 10 up are the same pieces glazed clear (enterable buildings).
    const int glass = variant >= 10 ? MatGlassClear : MatGlass;
    variant %= 10;
    const double w = std::max(sz.x, 1.0), h = std::max(sz.y, 1.0), d = std::max(sz.z, 1.0);
    const double hw = w * 0.5;
    if (kind == "window")
    {
        const double b = std::clamp(w * 0.06, 5.0, 9.0);
        frame(s, -hw, hw, 0.0, h, -4.0, 4.0, b, MatFrame);
        card(s, -hw + b, hw - b, b, h - b, 0.0, glass);
        if (variant == 0 && w > 70.0)
        {
            box(s, {-2.5, b, -2.5}, {2.5, h - b, 2.5}, MatFrame);
            box(s, {-hw + b, h * 0.66 - 2.5, -2.5}, {hw - b, h * 0.66 + 2.5, 2.5}, MatFrame);
        }
        else if (variant == 1 || variant == 2)
        {
            const int n = std::max(1, int(w / 130.0));
            for (int k = 1; k < n + (variant == 1 ? 1 : 0); ++k)
            {
                const double x = -hw + w * k / (n + (variant == 1 ? 1 : 0));
                box(s, {x - 3.0, b, -3.0}, {x + 3.0, h - b, 3.0}, MatFrame);
            }
        }
        else if (variant == 3)
        {
            // Louvres of a bell opening.
            for (double y = b + 15.0; y < h - b - 5.0; y += 35.0)
                box(s, {-hw + b, y, -3.0}, {hw - b, y + 5.0, 6.0}, MatWood);
        }
    }
    else if (kind == "door")
    {
        const double b = 8.0;
        frame(s, -hw, hw, 0.0, h, -4.0, 4.0, b, MatFrame, false);
        if (variant == 1 || variant == 2)
        {
            frame(s, -hw + b, hw - b, 0.0, h - b, -2.0, 2.0, 6.0, MatFrame);
            card(s, -hw + b + 6.0, hw - b - 6.0, 6.0, h - b - 6.0, 0.0, glass);
        }
        else
        {
            box(s, {-hw + b, 0.0, -6.0}, {hw - b, h - b, -2.0}, MatDoor);
            // Panels on the leaf.
            card(s, -hw + b + 10.0, hw - b - 10.0, 20.0, h * 0.45, -1.5, MatWood);
            card(s, -hw + b + 10.0, hw - b - 10.0, h * 0.5, h - b - 15.0, -1.5, MatWood);
        }
    }
    else if (kind == "rollup")
    {
        frame(s, -hw, hw, 0.0, h, -4.0, 4.0, 10.0, MatMetal, false);
        box(s, {-hw + 10.0, 0.0, -5.0}, {hw - 10.0, h - 10.0, -3.0}, MatMetal);
        for (double y = 24.0; y < h - 15.0; y += 48.0)
            box(s, {-hw + 10.0, y, -3.0}, {hw - 10.0, y + 4.0, -1.5}, MatMetal);
    }
    else if (kind == "shopfront")
    {
        const double b = 8.0, kick = std::min(45.0, h * 0.15), transom = h * 0.78;
        frame(s, -hw, hw, 0.0, h, -5.0, 5.0, b, MatFrame);
        box(s, {-hw + b, b, -4.0}, {hw - b, kick, 2.0}, MatFrame);
        box(s, {-hw + b, transom - 3.0, -4.0}, {hw - b, transom + 3.0, 4.0}, MatFrame);
        card(s, -hw + b, hw - b, kick, h - b, 0.0, glass);
        const int n = std::max(1, int(w / 160.0));
        for (int k = 1; k < n; ++k)
        {
            const double x = -hw + w * k / n;
            box(s, {x - 3.0, kick, -3.0}, {x + 3.0, h - b, 3.0}, MatFrame);
        }
    }
    else if (kind == "balcony")
    {
        const double slab = 14.0, rail = std::max(80.0, h - slab);
        box(s, {-hw, 0.0, 0.0}, {hw, slab, d}, MatConcrete);
        if (variant == 1)
        {
            box(s, {-hw, slab, d - 10.0}, {hw, slab + rail, d}, MatConcrete);
            box(s, {-hw, slab, 0.0}, {-hw + 10.0, slab + rail, d - 10.0}, MatConcrete);
            box(s, {hw - 10.0, slab, 0.0}, {hw, slab + rail, d - 10.0}, MatConcrete);
        }
        else
        {
            const int mat = variant == 2 ? MatFrame : MatMetal;
            box(s, {-hw, slab + rail - 5.0, d - 6.0}, {hw, slab + rail, d}, mat);
            box(s, {-hw, slab + rail - 5.0, 0.0}, {-hw + 5.0, slab + rail, d - 6.0}, mat);
            box(s, {hw - 5.0, slab + rail - 5.0, 0.0}, {hw, slab + rail, d - 6.0}, mat);
            if (variant == 2)
            {
                const Vec3 p[4] = {{-hw + 3.0, slab, d - 3.0}, {hw - 3.0, slab, d - 3.0}, {hw - 3.0, slab + rail - 5.0, d - 3.0}, {-hw + 3.0, slab + rail - 5.0, d - 3.0}};
                const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                s.poly(p, uv, 4, MatGlass, TrimNone, -1, PartOther, -1, kWhite);
            }
            else
            {
                for (double x = -hw + 15.0; x < hw - 5.0; x += 32.0)
                    box(s, {x, slab, d - 5.0}, {x + 2.5, slab + rail - 5.0, d - 2.5}, mat);
                box(s, {-hw, slab + 12.0, d - 5.0}, {hw, slab + 15.0, d - 2.5}, mat);
            }
        }
    }
    else if (kind == "arcade")
    {
        // Column at the origin, the beam and haunches spanning along -X to the next column.
        const double cs = 45.0, beam = std::min(60.0, h * 0.18);
        box(s, {-cs * 0.5, 0.0, -cs}, {cs * 0.5, h - beam, 0.0}, MatTrim);
        box(s, {-cs * 0.7, 0.0, -cs * 1.2}, {cs * 0.7, 20.0, 0.0}, MatTrim);
        // The beam stops a centimetre short of both columns' centres, inside them, so neighbouring bays' beams never meet face to face.
        box(s, {-w + 1.0, h - beam, -cs}, {-1.0, h, 0.0}, MatRender);
        // Haunches stand just clear of the columns and inside the beam's faces, never in a plane with either.
        box(s, {-cs * 0.5 - 50.0, h - beam - 35.0, -cs + 1.0}, {-cs * 0.5 - 0.5, h - beam, -1.0}, MatRender);
        box(s, {-w + cs * 0.5 + 0.5, h - beam - 35.0, -cs + 1.0}, {-w + cs * 0.5 + 50.0, h - beam, -1.0}, MatRender);
    }
    else if (kind == "column")
    {
        const double r = std::min(w, d) * 0.5;
        box(s, {-r * 1.25, 0.0, -r * 1.25}, {r * 1.25, std::min(25.0, h * 0.06), r * 1.25}, MatTrim);
        prism(s, 0.0, 0.0, r, std::min(25.0, h * 0.06), h - std::min(30.0, h * 0.07), 8, MatTrim);
        box(s, {-r * 1.3, h - std::min(30.0, h * 0.07), -r * 1.3}, {r * 1.3, h, r * 1.3}, MatTrim);
    }
    else if (kind == "awning")
    {
        // Slab sloping from the wall (high) out to the valance (low).
        const double t = 4.0;
        const Vec3 top[4] = {{-hw, h, 0.0}, {-hw, h * 0.35, d}, {hw, h * 0.35, d}, {hw, h, 0.0}};
        const glm::vec2 uv[4] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
        s.poly(top, uv, 4, MatAwning, TrimNone, -1, PartOther, -1, kWhite);
        const Vec3 under[4] = {{hw, h - t, 0.0}, {hw, h * 0.35 - t, d}, {-hw, h * 0.35 - t, d}, {-hw, h - t, 0.0}};
        s.poly(under, uv, 4, MatAwning, TrimNone, -1, PartOther, -1, kWhite);
        box(s, {-hw, h * 0.35 - 22.0, d - 1.0}, {hw, h * 0.35, d + 1.0}, MatAwning);
        box(s, {-hw, h * 0.35 - t, 0.0}, {-hw + 2.0, h, d}, MatAwning);
        box(s, {hw - 2.0, h * 0.35 - t, 0.0}, {hw, h, d}, MatAwning);
    }
    else if (kind == "sign")
    {
        box(s, {-hw, 0.0, 0.0}, {hw, h, d}, MatSign);
        // The text-ready face: uv 0 to 1 over the board's front, its own material.
        card(s, -hw + 4.0, hw - 4.0, 4.0, h - 4.0, d + 0.3, MatSignFace);
    }
    else if (kind == "ac")
    {
        if (variant == 1)
        {
            box(s, {-hw, 10.0, -d * 0.5}, {hw, h, d * 0.5}, MatMetal);
            box(s, {-hw, 0.0, -d * 0.5}, {-hw + 5.0, 10.0, d * 0.5}, MatMetal);
            box(s, {hw - 5.0, 0.0, -d * 0.5}, {hw, 10.0, d * 0.5}, MatMetal);
            card(s, -hw + 8.0, hw - 8.0, 18.0, h - 8.0, d * 0.5 + 0.3, MatDark);
        }
        else
        {
            box(s, {-hw, 0.0, 0.0}, {hw, h, d}, MatMetal);
            card(s, -hw + 6.0, hw * 0.3, 6.0, h - 6.0, d + 0.3, MatDark);
            box(s, {-hw + 5.0, -6.0, 0.0}, {-hw + 9.0, 0.0, d}, MatMetal);
            box(s, {hw - 9.0, -6.0, 0.0}, {hw - 5.0, 0.0, d}, MatMetal);
        }
    }
    else if (kind == "drainpipe")
    {
        box(s, {-w * 0.5, 0.0, 0.0}, {w * 0.5, h, d}, MatMetal);
        box(s, {-w * 0.5, 0.0, d}, {w * 0.5, 8.0, d + 20.0}, MatMetal);
    }
    else if (kind == "grille")
    {
        const double bar = 2.0;
        frame(s, -hw, hw, 0.0, h, 0.0, d, 3.0, MatMetal);
        const double top = variant == 1 ? h * 0.45 : h;
        for (double x = -hw + 18.0; x < hw - 8.0; x += 22.0)
            box(s, {x - bar * 0.5, 0.0, d * 0.4}, {x + bar * 0.5, top, d * 0.6}, MatMetal);
        box(s, {-hw, top * 0.5 - bar * 0.5, d * 0.4}, {hw, top * 0.5 + bar * 0.5, d * 0.6}, MatMetal);
    }
    else if (kind == "shutter")
    {
        box(s, {-hw, h - 25.0, 0.0}, {hw, h, d}, MatMetal);
        const double down = variant == 0 ? 0.0 : variant == 1 ? 0.5 : 1.0;
        if (down > 0.0)
        {
            const double y0 = (h - 25.0) * (1.0 - down);
            box(s, {-hw + 4.0, y0, d * 0.4}, {hw - 4.0, h - 25.0, d * 0.5}, MatMetal);
            for (double y = y0 + 20.0; y < h - 30.0; y += 40.0)
                box(s, {-hw + 4.0, y, d * 0.5}, {hw - 4.0, y + 3.0, d * 0.6}, MatMetal);
        }
    }
    else if (kind == "tank")
    {
        box(s, {-hw * 0.9, 0.0, -d * 0.45}, {hw * 0.9, h * 0.12, d * 0.45}, MatConcrete);
        prism(s, 0.0, 0.0, hw * 0.92, h * 0.12, h * 0.92, 10, MatDark);
        prism(s, 0.0, 0.0, hw * 0.35, h * 0.92, h, 8, MatDark);
    }
    else if (kind == "vent")
    {
        box(s, {-hw * 0.6, 0.0, -d * 0.3}, {hw * 0.6, h * 0.8, d * 0.3}, MatMetal);
        box(s, {-hw, h * 0.8, -d * 0.5}, {hw, h, d * 0.5}, MatMetal);
    }
    else if (kind == "antenna")
    {
        box(s, {-2.0, 0.0, -2.0}, {2.0, h, 2.0}, MatMetal);
        box(s, {-hw * 2.0, h * 0.75, -1.5}, {hw * 2.0, h * 0.75 + 3.0, 1.5}, MatMetal);
        box(s, {-hw * 1.4, h * 0.88, -1.5}, {hw * 1.4, h * 0.88 + 3.0, 1.5}, MatMetal);
    }
    else if (kind == "rebar")
    {
        for (int k = 0; k < 4; ++k)
        {
            const double x = (k % 2 ? 1 : -1) * w * 0.25, z = (k / 2 ? 1 : -1) * d * 0.25;
            box(s, {x - 0.8, 0.0, z - 0.8}, {x + 0.8, h * (0.8 + 0.1 * k), z + 0.8}, MatMetal);
        }
    }
    else if (kind == "steps")
    {
        const int n = std::max(1, int(std::ceil(h / 18.0)));
        for (int k = 0; k < n; ++k)
            box(s, {-hw, 0.0, 0.0}, {hw, h * (k + 1) / n, d * (n - k) / n}, MatStone);
    }
    else if (kind == "portico")
    {
        box(s, {-hw, 0.0, -d * 0.5}, {hw, h * 0.6, d * 0.5}, MatTrim);
        // A low pediment over the slab.
        const Vec3 f[3] = {{-hw, h * 0.6, d * 0.5}, {hw, h * 0.6, d * 0.5}, {0.0, h * 0.6 + w * 0.12, d * 0.5}};
        const glm::vec2 uv[3] = {{0, 0}, {1, 0}, {0.5f, 1}};
        s.poly(f, uv, 3, MatTrim, TrimNone, -1, PartOther, -1, kWhite);
    }
    else if (kind == "perimeter")
    {
        box(s, {-hw, 0.0, -d * 0.5}, {hw, h, d * 0.5}, MatConcrete);
        box(s, {-hw, h, -d * 0.5 - 3.0}, {hw, h + 8.0, d * 0.5 + 3.0}, MatTrim);
    }
    else if (kind == "gate")
    {
        box(s, {-hw - 30.0, 0.0, -25.0}, {-hw, h + 20.0, 25.0}, MatConcrete);
        box(s, {hw, 0.0, -25.0}, {hw + 30.0, h + 20.0, 25.0}, MatConcrete);
        frame(s, -hw, hw, 0.0, h, -3.0, 3.0, 6.0, MatMetal);
        for (double x = -hw + 20.0; x < hw - 6.0; x += 20.0)
            box(s, {x - 1.0, 6.0, -1.0}, {x + 1.0, h - 6.0, 1.0}, MatMetal);
    }
    else if (kind == "pool")
    {
        const double c = 30.0;
        box(s, {-hw - c, -2.0, -d * 0.5 - c}, {hw + c, 4.0, -d * 0.5}, MatStone);
        box(s, {-hw - c, -2.0, d * 0.5}, {hw + c, 4.0, d * 0.5 + c}, MatStone);
        box(s, {-hw - c, -2.0, -d * 0.5}, {-hw, 4.0, d * 0.5}, MatStone);
        box(s, {hw, -2.0, -d * 0.5}, {hw + c, 4.0, d * 0.5}, MatStone);
        const Vec3 p[4] = {{-hw, -12.0, d * 0.5}, {hw, -12.0, d * 0.5}, {hw, -12.0, -d * 0.5}, {-hw, -12.0, -d * 0.5}};
        const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        s.poly(p, uv, 4, MatWater, TrimNone, -1, PartOther, -1, kWhite);
    }
    else if (kind == "dome")
    {
        prism(s, 0.0, 0.0, hw, 0.0, h * 0.3, 12, MatRender);
        prism(s, 0.0, 0.0, hw * 0.7, h * 0.3, h * 0.75, 12, MatRoof);
        prism(s, 0.0, 0.0, hw * 0.3, h * 0.75, h, 12, MatRoof);
    }
    else
    {
        // wall, pilaster, cornice, corner, band, plinth, roof: a plain block standing out of the wall.
        box(s, {-hw, 0.0, 0.0}, {hw, h, std::max(d, 2.0)}, kind == "roof" ? MatRoof : MatRender);
    }
    return s;
}

std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, std::unique_ptr<Soup>>& cache()
{
    static std::unordered_map<std::string, std::unique_ptr<Soup>> c;
    return c;
}
} // namespace

Vec3 placeholderSize(const Vec3& size)
{
    return {std::max(5.0, std::floor(size.x / 5.0 + 1e-6) * 5.0), std::max(5.0, std::floor(size.y / 5.0 + 1e-6) * 5.0), std::round(size.z)};
}

const Soup& placeholderSoup(const std::string& kind, const Vec3& size, int variant)
{
    const Vec3 q = placeholderSize(size);
    const std::string key = kind + "/" + std::to_string(int(q.x)) + "/" + std::to_string(int(q.y)) + "/" + std::to_string(int(q.z)) + "/" +
                            std::to_string(variant);
    std::lock_guard lock(cacheMutex());
    auto& c = cache();
    auto it = c.find(key);
    if (it != c.end())
        return *it->second;
    auto s = std::make_unique<Soup>(build(kind, q, variant));
    const Soup& ref = *s;
    c.emplace(key, std::move(s));
    return ref;
}

const std::vector<std::string>& placeholderKinds()
{
    static const std::vector<std::string> k = {"window", "door",    "rollup", "shopfront", "balcony", "arcade",    "column", "awning",
                                               "sign",   "ac",      "drainpipe", "grille", "shutter", "tank",      "vent",   "antenna",
                                               "rebar",  "steps",   "portico", "perimeter", "gate",   "pool",      "dome",   "wall",
                                               "pilaster", "cornice", "corner", "band",    "plinth",  "roof"};
    return k;
}

const json& slotTypeRows()
{
    // id, label, footprint, attach, placeholder, tags, doc
    static const json rows = json::parse(R"([
 ["wall", "Wall", [300, 300, 30], "wall", "panel", ["facade", "shell"], "A wall piece of a building shell."],
 ["wall.bay", "Wall bay", [300, 320, 30], "wall", "panel", ["facade", "shell", "building"], "One bay of a facade on one floor, round its opening; drawn in the shell until replaced."],
 ["wall.party", "Party wall", [600, 600, 30], "wall", "panel", ["facade", "shell", "building"], "A blank wall where a neighbour touches."],
 ["wall.perimeter", "Perimeter wall", [600, 250, 30], "ground", "box", ["site", "building"], "A wall round a lot: mansions, police stations, compounds."],
 ["shopfront", "Shopfront", [250, 280, 15], "wall", "window", ["opening", "facade", "commercial", "building"], "A shop's glazing and frame on a ground floor."],
 ["door.balcony", "Balcony door", [120, 230, 8], "wall", "door", ["opening", "facade", "building"], "A glazed door onto a balcony."],
 ["window.glazing", "Glazing", [300, 280, 12], "wall", "window", ["opening", "facade", "building"], "Bay-wide glazing of a glass facade."],
 ["window.bell", "Bell opening", [150, 250, 12], "wall", "window", ["opening", "facade", "building"], "A tower's louvred opening."],
 ["balcony", "Balcony", [180, 105, 100], "wall", "box", ["facade", "building"], "A balcony slab and railing."],
 ["balcony.iron", "Iron balcony", [180, 105, 70], "wall", "box", ["facade", "building"], "Wrought-iron balconette."],
 ["balcony.concrete", "Concrete balcony", [200, 105, 120], "wall", "box", ["facade", "building"], "A concrete balcony with a solid parapet."],
 ["balcony.glass", "Glass balcony", [200, 105, 120], "wall", "box", ["facade", "building"], "A balcony with glass railings."],
 ["arcade.bay", "Arcade bay", [320, 450, 300], "wall", "box", ["facade", "building"], "A column and arch of a ground-floor arcade (portales)."],
 ["column", "Column", [50, 450, 50], "ground", "cylinder", ["building"], "A free-standing column (portico)."],
 ["portico", "Portico", [600, 70, 300], "ground", "box", ["building"], "The slab and pediment over portico columns."],
 ["pilaster", "Pilaster", [36, 600, 8], "wall", "panel", ["facade", "shell", "building"], "A flat column on the facade between bays."],
 ["cornice", "Cornice", [600, 45, 30], "wall", "panel", ["facade", "shell", "building"], "The moulding along a facade's top."],
 ["corner", "Corner", [40, 600, 6], "wall", "panel", ["facade", "shell", "building"], "Quoins or a corner strip."],
 ["band", "String course", [600, 24, 6], "wall", "panel", ["facade", "shell", "building"], "A band between floors."],
 ["plinth", "Plinth", [600, 60, 6], "wall", "panel", ["facade", "shell", "building"], "The base band under the ground floor."],
 ["parapet", "Parapet", [600, 90, 25], "roof", "panel", ["roof", "shell", "building"], "A low wall round a flat roof."],
 ["drainpipe", "Drainpipe", [12, 600, 12], "wall", "pole", ["facade", "building"], "A downpipe from the roof."],
 ["grille", "Grille", [120, 150, 6], "wall", "panel", ["facade", "building"], "Bars over an opening."],
 ["grille.window", "Window grille", [120, 150, 6], "wall", "panel", ["facade", "building"], "Security bars over a window."],
 ["shutter", "Shutter", [250, 280, 22], "wall", "panel", ["facade", "building"], "A shutter over an opening."],
 ["shutter.rollup", "Roll-up shutter", [250, 280, 22], "wall", "panel", ["facade", "commercial", "building"], "A metal roll-up shutter over a shopfront."],
 ["roof", "Roof", [800, 300, 800], "roof", "box", ["roof", "shell", "building"], "A building's roof."],
 ["roof.flat", "Flat roof", [800, 10, 800], "roof", "box", ["roof", "shell", "building"], "A flat roof top with its parapet."],
 ["roof.hip", "Hip roof", [800, 300, 800], "roof", "box", ["roof", "shell", "building"], "A hipped roof."],
 ["roof.gable", "Gable roof", [800, 300, 800], "roof", "box", ["roof", "shell", "building"], "A gabled roof."],
 ["roof.tiled", "Tiled roof", [800, 300, 800], "roof", "box", ["roof", "shell", "building"], "A pitched roof in clay tiles."],
 ["roof.sheet", "Sheet roof", [800, 200, 800], "roof", "box", ["roof", "shell", "building"], "A low pitched sheet metal roof."],
 ["roof.mansard", "Mansard roof", [800, 300, 800], "roof", "box", ["roof", "shell", "building"], "A mansard roof."],
 ["roof.clutter", "Roof clutter", [60, 60, 60], "roof", "box", ["roof", "building"], "Things standing on a flat roof."],
 ["roof.clutter.vent", "Roof vent", [50, 60, 50], "roof", "box", ["roof", "building"], "A vent on a flat roof."],
 ["roof.clutter.antenna", "Antenna", [30, 300, 30], "roof", "pole", ["roof", "building"], "An antenna mast."],
 ["tank", "Tank", [110, 130, 110], "roof", "cylinder", ["roof", "building"], "A tank."],
 ["tank.water", "Water tank", [110, 130, 110], "roof", "cylinder", ["roof", "building"], "A rooftop water tank (tinaco)."],
 ["rebar", "Rebar", [20, 90, 20], "roof", "pole", ["roof", "building"], "Rebar left standing for a next floor."],
 ["steps", "Steps", [150, 40, 80], "ground", "box", ["building"], "A stoop up to a door."],
 ["dome", "Dome", [500, 400, 500], "roof", "box", ["roof", "shell", "building"], "A dome with its drum."],
 ["gate", "Gate", [420, 290, 20], "ground", "panel", ["site", "building"], "A gate in a perimeter wall."],
 ["pool", "Pool", [700, 10, 400], "ground", "box", ["site", "building"], "A swimming pool."]
])");
    return rows;
}
} // namespace buildings
