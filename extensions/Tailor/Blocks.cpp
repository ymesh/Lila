#include "Blocks.h"
#include "Pattern.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace tailor
{
namespace
{

constexpr float kStep = 0.01f; // curve sampling
constexpr float kGap = 0.025f; // body to panel at placement
constexpr float kStuffedBend = 3000.f; // at tailStiffness 1: holds out from the back, the tip drooping

Vec3 add3(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul3(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 cross3(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 norm3(const Vec3& a)
{
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 0.f ? mul3(a, 1.f / l) : a;
}

float dist2d(const Vec2& a, const Vec2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

struct Builder
{
    std::vector<Vec2> pts;

    int add(float x, float y)
    {
        pts.push_back({x, y});
        return int(pts.size()) - 1;
    }

    /** Elliptical arc about c from angle t0 to t1, without its start point. */
    int arc(Vec2 c, float a, float b, float t0, float t1, bool includeEnd)
    {
        const float len = std::abs(t1 - t0) * std::sqrt((a * a + b * b) * 0.5f);
        const int n = std::max(2, int(std::ceil(len / kStep)));
        for (int i = 1; i <= n; ++i)
        {
            if (i == n && !includeEnd)
                break;
            const float t = t0 + (t1 - t0) * float(i) / float(n);
            add(c[0] + a * std::cos(t), c[1] + b * std::sin(t));
        }
        return int(pts.size()) - 1;
    }
};

std::vector<Vec2> mirrorX(const std::vector<Vec2>& p)
{
    const size_t n = p.size();
    std::vector<Vec2> m(n);
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2& q = p[(n - k) % n];
        m[k] = {-q[0], q[1]};
    }
    return m;
}

int mirrorIndex(int i, int n) { return (n - i) % n; }

/** The run on a mirrored outline that matches run `s` on the original. */
SeamSide mirrorRun(const SeamSide& s, int n, int panel) { return {panel, mirrorIndex(s.to, n), mirrorIndex(s.from, n)}; }

float runLength(const std::vector<Vec2>& o, int from, int to)
{
    const int n = int(o.size());
    float len = 0.f;
    for (int i = from; i != to; i = (i + 1) % n)
        len += dist2d(o[i], o[(i + 1) % n]);
    return len;
}

Seam seam(std::vector<SeamSide> a, std::vector<SeamSide> b, bool reversed = true)
{
    Seam s;
    s.a = std::move(a);
    s.b = std::move(b);
    s.reversed = reversed;
    return s;
}

/**
 * Symmetric panel from its +x half: `half` runs from the hem up the side to the neck point, the neck
 * (half-ellipse of half width neckA, depth neckB) joins it to its mirror. plus/minus give outline
 * indices of each half point and its mirror image.
 */
struct Symmetric
{
    std::vector<Vec2> outline;
    std::vector<int> plus, minus;

    SeamSide plusRun(int panel, int r0, int r1) const { return {panel, plus[r0], plus[r1]}; }
    SeamSide minusRun(int panel, int r0, int r1) const { return {panel, minus[r1], minus[r0]}; }
};

Symmetric symmetric(const std::vector<Vec2>& half, float neckY, float neckA, float neckB)
{
    Symmetric s;
    Builder b;
    const int h = int(half.size());
    b.add(-half[0][0], half[0][1]);
    for (const Vec2& p : half)
        s.plus.push_back(b.add(p[0], p[1]));
    if (neckB > 0.f)
        b.arc({0.f, neckY}, neckA, neckB, 0.f, -std::numbers::pi_v<float>, false);
    s.minus.assign(h, 0);
    for (int r = h - 1; r >= 1; --r)
        s.minus[r] = b.add(-half[r][0], half[r][1]);
    s.outline = std::move(b.pts);
    return s;
}

/**
 * Wrap radius for a panel whose centre line sits z0 in front of the body axis. Narrow panels wrap the
 * body cylinder; wider ones get a larger radius so their edges stop short of the side plane and front
 * and back panels never cross.
 */
float lensRadius(float halfWidth, float z0)
{
    if (halfWidth <= z0 * std::numbers::pi_v<float> * 0.5f)
        return z0;
    const float target = z0 - 0.005f;
    float lo = z0, hi = halfWidth * halfWidth / target + z0;
    for (int i = 0; i < 40; ++i)
    {
        const float r = 0.5f * (lo + hi);
        (r * (1.f - std::cos(halfWidth / r)) > target ? lo : hi) = r;
    }
    return hi;
}

void place(Panel& p, const Vec3& origin, const Vec3& up, const Vec3& normal, float wrap)
{
    p.origin = origin;
    p.up = norm3(up);
    p.right = norm3(cross3(p.up, norm3(normal)));
    p.wrapRadius = wrap;
}

/** Front (+Z) or back (-Z) panel centred on the body axis, pattern origin at the hem. */
void placeAround(Panel& p, float hemY, float z0, float halfWidth, bool front)
{
    const float s = front ? 1.f : -1.f;
    place(p, {0.f, hemY, s * z0}, {0.f, 1.f, 0.f}, {0.f, 0.f, s}, lensRadius(halfWidth, z0));
}

Panel makePanel(std::string name, std::vector<Vec2> outline)
{
    Panel p;
    p.name = std::move(name);
    p.outline = std::move(outline);
    return p;
}

Fabric fabric(const std::string& name)
{
    Fabric f;
    f.name = name;
    if (name == "Linen")
    {
        f.stretchWarp = 5000.f, f.stretchWeft = 4000.f, f.bendWarp = f.bendWeft = 3e-5f, f.density = 0.18f;
    }
    else if (name == "Wool")
    {
        f.stretchWarp = 3000.f, f.stretchWeft = 2500.f, f.bendWarp = f.bendWeft = 8e-5f, f.bendBias = 6e-5f;
        f.density = 0.35f, f.thickness = 0.002f, f.friction = 0.5f;
    }
    else if (name == "Leather")
    {
        f.stretchWarp = f.stretchWeft = 20000.f, f.shear = 2000.f, f.bendWarp = f.bendWeft = f.bendBias = 2e-3f;
        f.strainLimit = 0.03f, f.density = 0.6f, f.thickness = 0.003f, f.friction = 0.6f;
    }
    else if (name == "Canvas")
    {
        f.stretchWarp = f.stretchWeft = 12000.f, f.bendWarp = f.bendWeft = 3e-4f, f.density = 0.3f;
    }
    else if (name == "Banner")
    {
        f.stretchWarp = f.stretchWeft = 6000.f, f.bendWarp = f.bendWeft = 1e-5f, f.density = 0.12f;
    }
    return f;
}

Garment withFabric(const std::string& name)
{
    Garment g;
    g.fabrics = {fabric(name)};
    return g;
}

struct HoodPanels
{
    int p = -1, m = -1;
    SeamSide neckP, neckM; // front to back on +X, back to front on -X
};

/** Hood side panels at +X and -X with the crown seam; the neck edges are left for the caller. */
HoodPanels addHood(Garment& g, const Body& body, float neckLength, float depth, float height)
{
    // Room for the head over its widest section and up to its crown, however big it is.
    const float d0 = std::max(neckLength, 0.1f);
    const float d = std::max({depth, d0, body.head / std::numbers::pi_v<float> * 1.25f});
    const float h = std::max({height, 0.15f, (body.height - body.neckY()) * 1.15f});
    const float f = std::min(0.12f, d0 * 0.45f);
    const float yc = std::min(0.12f, h * 0.3f);
    Builder b;
    b.add(-f, 0.f);
    b.add(d0 - f, 0.f);
    b.add(d - f, yc);
    b.arc({-f, yc}, d, h - yc, 0.f, std::numbers::pi_v<float> * 0.5f, true);
    const int n = int(b.pts.size());

    HoodPanels hp;
    hp.p = int(g.panels.size());
    hp.m = hp.p + 1;
    Panel pp = makePanel("Hood L", b.pts);
    Panel pm = makePanel("Hood R", mirrorX(b.pts));
    pp.particleDistance = pm.particleDistance = 0.015f;
    const float hx = body.head / (2.f * std::numbers::pi_v<float>) + 0.03f;
    place(pp, {hx, body.neckY(), 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, 0.f);
    place(pm, {-hx, body.neckY(), 0.f}, {0.f, 1.f, 0.f}, {-1.f, 0.f, 0.f}, 0.f);
    g.panels.push_back(std::move(pp));
    g.panels.push_back(std::move(pm));

    const SeamSide crown{hp.p, 1, n - 1};
    g.seams.push_back(seam({crown}, {mirrorRun(crown, n, hp.m)}));
    hp.neckP = {hp.p, 0, 1};
    hp.neckM = mirrorRun(hp.neckP, n, hp.m);
    return hp;
}

/** Widest |x| of a closed outline along the horizontal line at y. */
float halfWidthAt(const std::vector<Vec2>& o, float y)
{
    float w = 0.f;
    for (size_t i = 0; i < o.size(); ++i)
    {
        const Vec2& a = o[i];
        const Vec2& b = o[(i + 1) % o.size()];
        if ((a[1] - y) * (b[1] - y) > 0.f || a[1] == b[1])
            continue;
        const float t = (y - a[1]) / (b[1] - a[1]);
        w = std::max(w, std::abs(a[0] + (b[0] - a[0]) * t));
    }
    return w;
}

} // namespace

float Body::chestRadius() const { return chest / (2.f * std::numbers::pi_v<float>); }

const char* sleeveStyleName(SleeveStyle s)
{
    static const char* names[] = {"set-in", "cuffed", "bishop", "bell",    "puff",   "cap",    "flutter", "leg-of-mutton", "poet",
                                  "drop-shoulder", "batwing", "raglan", "marie", "flounce", "slit", "tiered", "petal", "cape"};
    const int i = int(s);
    return i >= 0 && i < int(SleeveStyle::Count) ? names[i] : "set-in";
}

namespace
{
/** What a sleeve style drafts: cap height as a share of armhole depth, cap ease, wrist and cuff, default length. */
struct SleeveShape
{
    float capHeight = 0.65f;
    float capEase = 0.04f; // cap seam longer than the armhole by this fraction; more gathers
    float wrist = 0.75f;   // wrist width over bicep width (before sleeveFlare)
    float cuff = 0.f;      // cuff band height, 0 for none
    float length = -1.f;   // default sleeve length as a share of the arm; -1 keeps the block's
    float drop = 0.f;      // default dropped shoulder, metres
};

SleeveShape sleeveShape(SleeveStyle s)
{
    switch (s)
    {
    case SleeveStyle::Cuffed: return {0.55f, 0.04f, 0.7f, 0.06f};
    case SleeveStyle::Bishop: return {0.6f, 0.05f, 1.25f, 0.05f, 0.95f};
    case SleeveStyle::Bell: return {0.6f, 0.04f, 2.2f, 0.f, 0.95f};
    case SleeveStyle::Puff: return {0.85f, 0.4f, 1.3f, 0.04f, 0.2f};
    case SleeveStyle::Cap: return {0.6f, 0.03f, 1.1f, 0.f, 0.1f};
    case SleeveStyle::Flutter: return {0.55f, 0.03f, 2.4f, 0.f, 0.25f};
    case SleeveStyle::LegOfMutton: return {0.9f, 0.5f, 0.6f, 0.f, 0.95f};
    case SleeveStyle::Poet: return {0.7f, 0.25f, 1.3f, 0.05f, 0.95f};
    case SleeveStyle::DropShoulder: return {0.25f, 0.f, 0.8f, 0.f, -1.f, 0.08f};
    case SleeveStyle::Raglan: return {0.f, 0.f, 0.75f};
    case SleeveStyle::Marie: return {0.6f, 0.05f, 1.3f, 0.04f, 0.95f};
    case SleeveStyle::Flounce: return {0.6f, 0.04f, 0.85f, 0.f, 0.5f};
    case SleeveStyle::Slit: return {0.6f, 0.04f, 0.9f, 0.f, 0.95f};
    case SleeveStyle::Tiered: return {0.6f, 0.04f, 1.f, 0.f, 0.6f};
    case SleeveStyle::Petal: return {0.6f, 0.04f, 1.1f, 0.f, 0.16f};
    case SleeveStyle::Cape: return {0.65f, 0.f, 3.2f, 0.f, 0.45f};
    default: return {};
    }
}

/** Bell-shaped sleeve cap from (-w/2, 0) over (0, -h) to (w/2, 0), y towards the shoulder negative. Excludes the ends. */
std::vector<Vec2> capCurve(float w, float h)
{
    std::vector<Vec2> out;
    const int n = std::max(8, int(std::ceil(w / kStep)));
    for (int i = 1; i < n; ++i)
    {
        const float t = -1.f + 2.f * float(i) / float(n);
        out.push_back({0.5f * w * t, -h * 0.5f * (1.f + std::cos(std::numbers::pi_v<float> * t))});
    }
    return out;
}

float capLength(float w, float h)
{
    std::vector<Vec2> c = capCurve(w, h);
    c.insert(c.begin(), Vec2{-0.5f * w, 0.f});
    c.push_back({0.5f * w, 0.f});
    float len = 0.f;
    for (size_t i = 0; i + 1 < c.size(); ++i)
        len += dist2d(c[i], c[i + 1]);
    return len;
}

/** Quadratic Bezier points from a towards b around control c, without a and b. */
std::vector<Vec2> bezier(const Vec2& a, const Vec2& c, const Vec2& b)
{
    std::vector<Vec2> out;
    const int n = std::max(3, int(std::ceil((dist2d(a, c) + dist2d(c, b)) / kStep)));
    for (int i = 1; i < n; ++i)
    {
        const float t = float(i) / float(n), u = 1.f - t;
        out.push_back({u * u * a[0] + 2.f * u * t * c[0] + t * t * b[0], u * u * a[1] + 2.f * u * t * c[1] + t * t * b[1]});
    }
    return out;
}

/** The edges of a drafted top that other pieces join. Neck and centre front are filled for open fronts only. */
struct TopRuns
{
    std::vector<SeamSide> hemFront; // -X to +X
    SeamSide hemBack;               // +X to -X
    std::vector<SeamSide> neck;     // centre front on -X, round the back, to centre front on +X
    SeamSide centreL, centreR;      // the front panels' centre edges, top to bottom on L
};

Garment draftTop(const Body& body, const TopParams& p, TopRuns* runs)
{
    Garment g = withFabric(p.openFront ? "Wool" : "Linen");
    const SleeveStyle style = SleeveStyle(std::clamp(p.sleeveStyle, 0, int(SleeveStyle::Count) - 1));
    const SleeveShape shape = sleeveShape(style);
    const bool batwing = style == SleeveStyle::Batwing && p.sleeveLength > 0.02f;
    const bool raglan = style == SleeveStyle::Raglan && p.sleeveLength > 0.02f;
    const float shoulderY = body.shoulderY();
    const float len = std::clamp(p.length, 0.25f, shoulderY - 0.02f);
    const float hemY = shoulderY - len;
    const float ease = std::max(p.ease, 0.f);
    const float wc = body.chest * (1.f + ease) * 0.25f;
    float wh = wc;
    if (hemY < body.hipY())
        wh = std::max(wh, body.hip * (1.f + ease) * 0.25f);
    wh += std::max(p.hemFlare, 0.f);
    const float sx = body.shoulderWidth * 0.5f;
    const float nx = std::min(body.neck * (1.f + ease) / std::numbers::pi_v<float> * 0.58f + 0.01f, sx - 0.04f);
    const float yT = len;
    const float yN = len + 0.035f;

    // Armhole depth from the bicep the sleeve needs, then the shoulder carried past the shoulder point when dropped.
    const float dx = std::abs(wc - sx);
    const float bicepHalf = body.chest * 0.33f * (1.f + std::max(p.sleeveEase, 0.f)) * 0.5f;
    const float drop = std::max(p.dropShoulder > 0.f ? p.dropShoulder : shape.drop, 0.f);
    float ya = std::max(yT - std::sqrt(std::max(bicepHalf * bicepHalf - dx * dx, 0.0036f)), 0.06f);
    ya = std::max(ya - 0.5f * drop - (batwing ? 0.14f : 0.f), 0.06f);
    // A raglan line ends on the neckline, a little down and in from the neck point.
    const Vec2 neckPt = raglan ? Vec2{nx * 0.85f, yN - 0.03f} : Vec2{nx, yN};
    const Vec2 shoulderDir{sx - nx, yT - yN};
    const float sl = std::hypot(shoulderDir[0], shoulderDir[1]);
    const Vec2 tip{sx + shoulderDir[0] / sl * drop, yT + shoulderDir[1] / sl * drop};
    const float slit = std::clamp(p.slit, 0.f, ya - 0.05f);

    std::vector<Vec2> half{{wh, 0.f}};
    int rSide = 0;
    if (slit > 0.005f)
    {
        half.push_back({wh + (wc - wh) * slit / ya, slit});
        rSide = 1;
    }
    const int rU = int(half.size());
    half.push_back({wc, ya});
    int rS, rWrist0 = -1, rWrist1 = -1;
    const float th = body.armAngle;
    const float ls = std::min(shape.length > 0.f ? shape.length * body.armLength : p.sleeveLength, body.armLength);
    if (batwing)
    {
        // Cut on: the underarm sweeps out along the arm, the top runs from the wrist back up to the neck.
        const Vec2 armDir{std::cos(th), -std::sin(th)};
        const float wristHalf = std::max(0.07f, bicepHalf * 0.45f) * (1.f + std::max(p.sleeveFlare, 0.f));
        const Vec2 shoulderEnd = tip;
        const Vec2 wristTop{shoulderEnd[0] + armDir[0] * ls, shoulderEnd[1] + armDir[1] * ls};
        const Vec2 n{armDir[1], -armDir[0]}; // below the arm line
        const Vec2 wristBottom{wristTop[0] + n[0] * 2.f * wristHalf, wristTop[1] + n[1] * 2.f * wristHalf};
        for (const Vec2& q : bezier({wc, ya}, {wc + 0.05f, ya + 0.02f}, wristBottom))
            half.push_back(q);
        rWrist0 = int(half.size());
        half.push_back(wristBottom);
        rWrist1 = int(half.size());
        half.push_back(wristTop);
        rS = int(half.size());
        half.push_back(shoulderEnd);
    }
    else if (raglan)
    {
        // The raglan line: up from the underarm, curving slightly in, to the neckline. There is no shoulder seam.
        for (const Vec2& q : bezier({wc, ya}, {wc - 0.03f, ya + 0.6f * (neckPt[1] - ya)}, neckPt))
            half.push_back(q);
        rS = int(half.size());
    }
    else
    {
        // Set-in armholes scoop: down from the shoulder, then out to the underarm. Dropped ones run nearly straight.
        const float straight = std::min(drop / 0.08f, 1.f);
        const Vec2 control{tip[0] + (wc - tip[0]) * 0.5f * straight, ya + (tip[1] - ya) * (0.25f + 0.25f * straight)};
        std::vector<Vec2> curve = bezier({wc, ya}, control, tip);
        for (const Vec2& q : curve)
            half.push_back(q);
        rS = int(half.size());
        half.push_back(tip);
    }
    const int rN = int(half.size());
    half.push_back(neckPt);

    struct Runs { SeamSide side, arm, shoulder, wrist; };
    Runs fPlus, fMinus, bPlus, bMinus;
    const float z0 = 2.f * wc / std::numbers::pi_v<float> + kGap;
    // Batwing fronts lie flat: wrapped round the torso, the sleeves would curl round it too.
    auto placeBody = [&](Panel& panel, float z, bool front) {
        if (batwing)
            place(panel, {0.f, hemY, front ? z : -z}, {0.f, 1.f, 0.f}, {0.f, 0.f, front ? 1.f : -1.f}, 0.f);
        else
            placeAround(panel, hemY, z, wh, front);
    };

    if (!p.openFront)
    {
        const Symmetric f = symmetric(half, neckPt[1], neckPt[0], std::clamp(p.neckDepth, 0.01f, neckPt[1] * 0.8f));
        Panel front = makePanel("Front", f.outline);
        placeBody(front, z0, true);
        g.panels.push_back(std::move(front));
        fPlus = {f.plusRun(0, rSide, rU), f.plusRun(0, rU, rS), f.plusRun(0, rS, rN), {}};
        fMinus = {f.minusRun(0, rSide, rU), f.minusRun(0, rU, rS), f.minusRun(0, rS, rN), {}};
        if (batwing)
        {
            fPlus = {f.plusRun(0, rSide, rWrist0), {}, f.plusRun(0, rWrist1, rN), {}};
            fMinus = {f.minusRun(0, rSide, rWrist0), {}, f.minusRun(0, rWrist1, rN), {}};
        }
    }
    else
    {
        const float ov = std::clamp(p.overlap, 0.f, nx - 0.01f);
        Builder b;
        b.add(-ov, 0.f);
        for (const Vec2& q : half)
            b.add(q[0], q[1]);
        b.add(-ov, yN - std::clamp(p.neckDepth, 0.02f, yN - 0.05f));
        const int n = int(b.pts.size());
        Panel fl = makePanel("Front L", b.pts);
        Panel fr = makePanel("Front R", mirrorX(b.pts));
        // The wearer's left laps over the right, so it sits a little further out.
        placeBody(fl, z0 + 0.012f, true);
        placeBody(fr, z0, true);
        g.panels.push_back(std::move(fl));
        g.panels.push_back(std::move(fr));
        auto run = [](int r0, int r1) { return SeamSide{0, 1 + r0, 1 + r1}; };
        if (batwing)
            fPlus = {run(rSide, rWrist0), {}, run(rWrist1, rN), {}};
        else
            fPlus = {run(rSide, rU), run(rU, rS), run(rS, rN), {}};
        fMinus = {mirrorRun(fPlus.side, n, 1), batwing ? SeamSide{} : mirrorRun(fPlus.arm, n, 1), mirrorRun(fPlus.shoulder, n, 1), {}};
    }

    const int bk = int(g.panels.size());
    const Symmetric bs = symmetric(half, neckPt[1], neckPt[0], 0.02f);
    Panel back = makePanel("Back", bs.outline);
    placeBody(back, z0, false);
    g.panels.push_back(std::move(back));
    if (batwing)
    {
        bPlus = {bs.plusRun(bk, rSide, rWrist0), {}, bs.plusRun(bk, rWrist1, rN), {}};
        bMinus = {bs.minusRun(bk, rSide, rWrist0), {}, bs.minusRun(bk, rWrist1, rN), {}};
    }
    else
    {
        bPlus = {bs.plusRun(bk, rSide, rU), bs.plusRun(bk, rU, rS), bs.plusRun(bk, rS, rN), {}};
        bMinus = {bs.minusRun(bk, rSide, rU), bs.minusRun(bk, rU, rS), bs.minusRun(bk, rS, rN), {}};
    }

    SeamSide sleeveNeck[2]; // raglan sleeves' neck edges, L then R
    if (runs)
    {
        runs->hemBack = {bk, 0, 1};
        if (p.openFront)
        {
            const int n = int(g.panels[0].outline.size());
            const SeamSide flHem{0, 0, 1}, flNeck{0, 1 + rN, n - 1}, flCentre{0, n - 1, 0};
            runs->hemFront = {mirrorRun(flHem, n, 1), flHem};
            runs->neck = {mirrorRun(flNeck, n, 1), {bk, bs.plus[size_t(rN)], bs.minus[size_t(rN)]}, flNeck};
            runs->centreL = flCentre;
            runs->centreR = mirrorRun(flCentre, n, 1);
        }
        else
            runs->hemFront = {{0, 0, 1}};
    }

    // The back faces -Z, so its +x half lies on the wearer's right (-X).
    g.seams.push_back(seam({fPlus.side}, {bMinus.side}));
    g.seams.push_back(seam({fMinus.side}, {bPlus.side}));
    if (!raglan)
    {
        g.seams.push_back(seam({fPlus.shoulder}, {bMinus.shoulder}));
        g.seams.push_back(seam({fMinus.shoulder}, {bPlus.shoulder}));
    }

    if (batwing || p.sleeveLength <= 0.02f || ls <= 0.02f)
        return g;

    // Sleeve: cap length matched to the armhole (front + back) plus the style's ease, solved for bicep width at the
    // style's cap height. Pattern y runs from the bicep line (0) down the arm; the cap rises to -capHeight.
    const float armhole = runLength(g.panels[size_t(fPlus.arm.panel)].outline, fPlus.arm.from, fPlus.arm.to);
    const float depth = std::max(tip[1] - ya, 0.05f);
    float h = shape.capHeight * depth;
    float w;
    if (raglan)
    {
        w = body.chest * 0.36f * (1.f + std::max(p.sleeveEase, 0.f));
        h = 0.f;
    }
    else
    {
        const float target = 2.f * armhole * (1.f + shape.capEase);
        float lo = 0.05f, hi = target;
        for (int i = 0; i < 40; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            (capLength(mid, h) < target ? lo : hi) = mid;
        }
        w = lo;
    }
    const float flare = 1.f + std::max(p.sleeveFlare, 0.f);
    // Pieces below the main sleeve: tiers, puffs and bands share the sleeve's length with it.
    const bool marie = style == SleeveStyle::Marie, tiered = style == SleeveStyle::Tiered, flounce = style == SleeveStyle::Flounce;
    const float cuffH = shape.cuff > 0.f ? std::min(shape.cuff, ls * 0.5f) : 0.f;
    float mainLen = ls - cuffH;
    if (marie || tiered)
        mainLen = (ls - cuffH) / 3.f;
    else if (flounce)
        mainLen = ls * 0.55f;
    const float wristW = (marie || tiered) ? w * (marie ? 1.3f : 1.f) : w * shape.wrist * flare;

    Builder sb;
    int capEnd = 0, hemFrom = 0, hemTo = 0, backTop = -1, frontTop = -1;
    if (raglan)
    {
        // Back raglan edge, the neck edge over the shoulder, front raglan edge; straight, as long as the body's line.
        const float nw = 0.08f;
        const float span = std::max(0.5f * (w - nw), 0.01f);
        const float hr = std::sqrt(std::max(armhole * armhole - span * span, 0.0025f));
        sb.add(-0.5f * w, 0.f);
        backTop = sb.add(-0.5f * nw, -hr);
        frontTop = sb.add(0.5f * nw, -hr);
        capEnd = sb.add(0.5f * w, 0.f);
        h = 0.5f * hr;
    }
    else
    {
        sb.add(-0.5f * w, 0.f);
        for (const Vec2& q : capCurve(w, h))
            sb.add(q[0], q[1]);
        capEnd = sb.add(0.5f * w, 0.f);
    }
    hemFrom = sb.add(0.5f * wristW, mainLen);
    if (style == SleeveStyle::Petal)
    {
        // The hem rises in the middle, up over the top of the arm.
        const int n = std::max(6, int(std::ceil(wristW / kStep)));
        for (int i = 1; i < n; ++i)
        {
            const float t = float(i) / float(n);
            sb.add(0.5f * wristW * (1.f - 2.f * t), mainLen * (1.f - 0.65f * std::sin(std::numbers::pi_v<float> * t)));
        }
    }
    hemTo = sb.add(-0.5f * wristW, mainLen);
    const float rs = w / (2.f * std::numbers::pi_v<float>);
    // Short sleeves band the upper arm, long ones the wrist.
    const float armGirth = ls < 0.4f * body.armLength ? 0.8f * w : std::max(body.armLength * 0.27f, 0.16f);

    for (float s : {1.f, -1.f})
    {
        const std::string side = s > 0.f ? " L" : " R";
        const Vec3 up{s * std::cos(th), -std::sin(th), 0.f};
        const Vec3 nrm{s * std::sin(th), std::cos(th), 0.f};
        // On the arm's axis: the shoulder joint sits a little inside and below the shoulder point.
        const Vec3 joint{s * (sx - 0.03f + drop * std::cos(th)), shoulderY - 0.04f - drop * std::sin(th), 0.f};
        const Vec3 bicep = add3(joint, mul3(up, h));
        // The body's armhole as the sleeve meets it: front run, back run, in the order this side's cap walks.
        const std::vector<SeamSide> armChain = s > 0.f ? std::vector<SeamSide>{fPlus.arm, bMinus.arm} : std::vector<SeamSide>{bPlus.arm, fMinus.arm};

        SeamSide hem;
        if (style == SleeveStyle::Slit)
        {
            // Two halves split along the top of the arm; the split stays open.
            Builder backHalf, frontHalf;
            const int mid = capEnd / 2;
            for (int i = 0; i <= mid; ++i)
                backHalf.add(sb.pts[size_t(i)][0], sb.pts[size_t(i)][1]);
            backHalf.add(sb.pts[size_t(mid)][0], mainLen);
            const int backHemEnd = backHalf.add(-0.5f * wristW, mainLen);
            for (int i = mid; i <= capEnd; ++i)
                frontHalf.add(sb.pts[size_t(i)][0], sb.pts[size_t(i)][1]);
            const int frontCap = int(frontHalf.pts.size()) - 1;
            frontHalf.add(0.5f * wristW, mainLen);
            frontHalf.add(sb.pts[size_t(mid)][0], mainLen);
            const int bp = int(g.panels.size());
            Panel bpanel = makePanel("Sleeve Back" + side, backHalf.pts), fpanel = makePanel("Sleeve Front" + side, frontHalf.pts);
            place(bpanel, add3(bicep, mul3(nrm, rs)), up, nrm, rs);
            place(fpanel, add3(bicep, mul3(nrm, rs + 0.005f)), up, nrm, rs + 0.005f);
            g.panels.push_back(std::move(bpanel));
            g.panels.push_back(std::move(fpanel));
            g.seams.push_back(seam(armChain, {{bp, 0, mid}, {bp + 1, 0, frontCap}}));
            g.seams.push_back(seam({{bp + 1, frontCap, frontCap + 1}}, {{bp, backHemEnd, 0}}));
            continue;
        }

        const int sp = int(g.panels.size());
        Panel sleeve = makePanel("Sleeve" + side, sb.pts);
        place(sleeve, add3(bicep, mul3(nrm, rs)), up, nrm, rs);
        g.panels.push_back(std::move(sleeve));
        if (raglan)
        {
            // This side's +x edge faces front on the left arm and back on the right, as with a set-in cap.
            const SeamSide toFront = s > 0.f ? fPlus.arm : bPlus.arm, toBack = s > 0.f ? bMinus.arm : fMinus.arm;
            g.seams.push_back(seam({toFront}, {{sp, frontTop, capEnd}}));
            g.seams.push_back(seam({toBack}, {{sp, 0, backTop}}));
            // Front to back on the right arm, back to front on the left, as the neck chain walks.
            sleeveNeck[s > 0.f ? 0 : 1] = {sp, backTop, frontTop};
        }
        else
            g.seams.push_back(seam(armChain, {{sp, 0, capEnd}}));
        g.seams.push_back(seam({{sp, capEnd, hemFrom}}, {{sp, hemTo, 0}}));
        hem = {sp, hemFrom, hemTo};

        // A closed tube below the last piece: top edge sewn to its hem (gathered when the hem is longer), side seamed.
        float along = mainLen;
        auto tube = [&](const std::string& name, float top, float bottom, float height) {
            const int tp = int(g.panels.size());
            const float r = std::max(top, bottom) / (2.f * std::numbers::pi_v<float>);
            Panel t = makePanel(name + side, {{-0.5f * top, 0.f}, {0.5f * top, 0.f}, {0.5f * bottom, height}, {-0.5f * bottom, height}});
            place(t, add3(add3(bicep, mul3(up, along)), mul3(nrm, r)), up, nrm, r);
            g.panels.push_back(std::move(t));
            g.seams.push_back(seam({hem}, {{tp, 0, 1}}));
            g.seams.push_back(seam({{tp, 1, 2}}, {{tp, 3, 0}}));
            hem = {tp, 2, 3};
            along += height;
        };
        const float third = (ls - cuffH) / 3.f;
        if (marie)
        {
            const float band = 0.04f;
            tube("Band A", 0.85f * w, 0.85f * w, band);
            tube("Puff B", 1.3f * w, 1.3f * w, third - band);
            tube("Band B", 0.75f * w, 0.75f * w, band);
            tube("Puff C", 1.3f * w, 1.3f * w, third - band);
        }
        else if (tiered)
        {
            float width = wristW;
            for (const char* name : {"Tier B", "Tier C"})
            {
                width *= 1.45f;
                tube(name, width, width * 1.15f, third);
                width *= 1.15f;
            }
        }
        if (cuffH > 0.f)
        {
            const float cw = std::min(marie ? 1.3f * w : wristW, armGirth);
            tube("Cuff", cw, cw, cuffH);
        }
        if (flounce)
        {
            // A three-quarter ring: its inner edge is the elbow hem, its outer edge falls in a circle.
            const float sweep = 1.5f * std::numbers::pi_v<float>, r1 = wristW / sweep, r2 = r1 + std::max(ls - mainLen, 0.08f);
            Builder fb;
            const int n = std::max(12, int(std::ceil(sweep * r2 / kStep)));
            for (int i = 0; i <= n; ++i)
            {
                const float a = -0.5f * sweep + sweep * float(i) / float(n);
                fb.add(r2 * std::sin(a), -r2 * std::cos(a));
            }
            const int innerStart = int(fb.pts.size());
            for (int i = n; i >= 0; --i)
            {
                const float a = -0.5f * sweep + sweep * float(i) / float(n);
                fb.add(r1 * std::sin(a), -r1 * std::cos(a));
            }
            const int innerEnd = int(fb.pts.size()) - 1;
            const int fp = int(g.panels.size());
            Panel fl = makePanel("Flounce" + side, fb.pts);
            place(fl, add3(add3(bicep, mul3(up, mainLen + r1)), mul3(nrm, wristW / (2.f * std::numbers::pi_v<float>))), up, nrm, wristW / (2.f * std::numbers::pi_v<float>));
            g.panels.push_back(std::move(fl));
            // Both run right to left already.
            g.seams.push_back(seam({hem}, {{fp, innerStart, innerEnd}}, false));
            // Close the ring: the two cut ends sewn together.
            g.seams.push_back(seam({{fp, n, innerStart}}, {{fp, innerEnd, 0}}));
        }
    }
    if (runs && raglan && runs->neck.size() == 3 && sleeveNeck[0].panel >= 0 && sleeveNeck[1].panel >= 0)
        runs->neck = {runs->neck[0], sleeveNeck[1], runs->neck[1], sleeveNeck[0], runs->neck[2]};
    return g;
}
} // namespace

Garment top(const Body& body, const TopParams& p) { return draftTop(body, p, nullptr); }

Garment tee(const Body& body, const TeeParams& p) { return top(body, p); }
Garment tunic(const Body& body, const TunicParams& p) { return top(body, p); }
Garment robe(const Body& body, const RobeParams& p) { return top(body, p); }

Garment trousers(const Body& body, const TrousersParams& p)
{
    Garment g = withFabric("Cotton");
    const float waistY = body.waistY();
    const float len = std::clamp(p.length, waistY - body.crotchY() + 0.08f, waistY - 0.02f);
    const float hemY = waistY - len;
    const float yc = body.crotchY() - hemY;
    const float yw = len;
    float yh = body.hipY() - hemY;
    if (yh < yc + 0.03f || yh > yw - 0.03f)
        yh = 0.5f * (yc + yw);
    const float ease = std::max(p.ease, 0.f);
    const float tx = body.hip * 0.6f * (1.f + ease) * 0.25f;
    const float hx = std::clamp(p.hemCircumference, 0.2f, 1.2f) * 0.25f;
    const float cf = -tx;
    const float hipQ = body.hip * (1.f + ease) * 0.25f;
    const float waistQ = (p.waist > 0.f ? p.waist : body.waist * (1.f + ease)) * 0.25f;

    struct Shape { std::vector<Vec2> o; int crotch; };
    // The inner hem corner keeps the same offset from the crotch point front and back, so inseams match.
    const float innerGap = -hx - (cf - 0.045f);
    auto shape = [&](float ext, float rise) {
        Builder b;
        b.add(cf - ext + innerGap, 0.f);
        b.add(hx, 0.f);
        b.add(tx, yc);
        b.add(cf + hipQ, yh);
        b.add(cf + 0.01f + waistQ, yw);
        b.add(cf + 0.01f, yw + rise);
        b.add(cf, yh);
        // Quarter ellipse down and in to the crotch point.
        const int c = b.arc({cf - ext, yh}, ext, yh - yc, 0.f, -std::numbers::pi_v<float> * 0.5f, true);
        return Shape{b.pts, c};
    };
    const Shape f = shape(0.045f, 0.f);
    const Shape bk = shape(0.09f, std::clamp(p.backRise, 0.f, 0.1f));
    const int nf = int(f.o.size()), nb = int(bk.o.size());

    const float legX = std::max(body.hip * 0.095f, tx - 0.02f);
    const float z0 = body.hip * 0.6f / (2.f * std::numbers::pi_v<float>) + kGap + 0.03f;
    const float halfW = tx + 0.09f;
    const Vec3 up{0.f, 1.f, 0.f};
    auto leg = [&](const char* name, std::vector<Vec2> o, float x, float zs) {
        Panel pn = makePanel(name, std::move(o));
        place(pn, {x, hemY, zs * z0}, up, {0.f, 0.f, zs}, lensRadius(halfW, z0));
        g.panels.push_back(std::move(pn));
    };
    leg("Front L", f.o, legX, 1.f);                // 0
    leg("Back L", mirrorX(bk.o), legX, -1.f);      // 1
    leg("Front R", mirrorX(f.o), -legX, 1.f);      // 2
    leg("Back R", bk.o, -legX, -1.f);              // 3

    const SeamSide fOut{0, 1, 4}, fIn{0, f.crotch, 0}, fCentre{0, 5, f.crotch};
    const SeamSide bOut{3, 1, 4}, bIn{3, bk.crotch, 0}, bCentre{3, 5, bk.crotch};
    g.seams.push_back(seam({fOut}, {mirrorRun(bOut, nb, 1)}));
    g.seams.push_back(seam({fIn}, {mirrorRun(bIn, nb, 1)}));
    g.seams.push_back(seam({mirrorRun(fOut, nf, 2)}, {bOut}));
    g.seams.push_back(seam({mirrorRun(fIn, nf, 2)}, {bIn}));
    g.seams.push_back(seam({fCentre}, {mirrorRun(fCentre, nf, 2)}));
    g.seams.push_back(seam({mirrorRun(bCentre, nb, 1)}, {bCentre}));
    return g;
}

Garment skirt(const Body& body, const SkirtParams& p)
{
    Garment g = withFabric("Cotton");
    const float waistY = body.waistY();
    const float drop = waistY - body.hipY();
    const float len = std::clamp(p.length, drop + 0.05f, waistY - 0.02f);
    const float hemY = waistY - len;
    const float ease = std::max(p.ease, 0.f);
    const float hipX = body.hip * (1.f + ease) * 0.25f;
    const float waistX = body.waist * (1.f + ease) * 0.25f;
    const float hemX = hipX + std::max(p.flare, 0.f);
    const std::vector<Vec2> half{{hemX, 0.f}, {hipX, len - drop}, {waistX, len}};
    const float z0 = 2.f * hipX / std::numbers::pi_v<float> + kGap;
    for (int i = 0; i < 2; ++i)
    {
        const Symmetric s = symmetric(half, len, waistX, 0.f);
        Panel pn = makePanel(i == 0 ? "Front" : "Back", s.outline);
        placeAround(pn, hemY, z0, hemX, i == 0);
        g.panels.push_back(std::move(pn));
    }
    const Symmetric s = symmetric(half, len, waistX, 0.f);
    g.seams.push_back(seam({s.plusRun(0, 0, 2)}, {s.minusRun(1, 0, 2)}));
    g.seams.push_back(seam({s.minusRun(0, 0, 2)}, {s.plusRun(1, 0, 2)}));
    return g;
}

Garment hood(const Body& body, const HoodParams& p)
{
    Garment g = withFabric("Wool");
    addHood(g, body, body.neck * 0.62f, p.depth, p.height);
    return g;
}

Garment drape(const Body& body, const DrapeParams& p)
{
    Garment g = withFabric("Wool");
    const float neckY = body.neckY();
    const float len = std::clamp(p.length, 0.25f, neckY - 0.02f);
    const float hemY = neckY - len;
    const float nx = body.neck / std::numbers::pi_v<float> * 0.6f + 0.01f;
    const float sx = body.shoulderWidth * 0.5f + 0.04f;
    const float yS = len - (neckY - body.shoulderY()) - 0.01f;
    const float hw = sx + std::max(p.flare, 0.f);
    const std::vector<Vec2> half{{hw, 0.f}, {sx, yS}, {nx, len}};

    const float cov = std::clamp(p.frontCoverage, 0.f, 1.f);
    const float ov = std::clamp(p.overlap, 0.f, nx - 0.01f);
    const float xin = -ov + (nx - 0.03f + ov) * (1.f - cov);
    Builder b;
    b.add(xin, 0.f);
    for (const Vec2& q : half)
        b.add(q[0], q[1]);
    const int fLast = b.arc({xin, len}, nx - xin, 0.06f, 0.f, -std::numbers::pi_v<float> * 0.5f, true);
    const int nf = int(b.pts.size());

    const float z0 = p.clearance > 0.f ? p.clearance : body.chestRadius() + 0.08f;
    Panel fl = makePanel("Front L", b.pts);
    Panel fr = makePanel("Front R", mirrorX(b.pts));
    placeAround(fl, hemY, z0 + 0.012f, hw, true);
    placeAround(fr, hemY, z0, hw, true);
    const Symmetric bs = symmetric(half, len, nx, 0.015f);
    Panel back = makePanel("Back", bs.outline);
    placeAround(back, hemY, z0, hw, false);
    g.panels = {std::move(fl), std::move(fr), std::move(back)};

    const int r0 = p.closedSides ? 0 : 1;
    const SeamSide flSide{0, 1 + r0, 3};
    g.seams.push_back(seam({flSide}, {bs.minusRun(2, r0, 2)}));
    g.seams.push_back(seam({mirrorRun(flSide, nf, 1)}, {bs.plusRun(2, r0, 2)}));

    if (p.hood)
    {
        const SeamSide flNeck{0, 3, fLast};
        const SeamSide frNeck = mirrorRun(flNeck, nf, 1);
        const SeamSide bkNeck{2, bs.plus[2], bs.minus[2]};
        const float neckLen = runLength(g.panels[0].outline, flNeck.from, flNeck.to)
                              + runLength(g.panels[1].outline, frNeck.from, frNeck.to)
                              + runLength(g.panels[2].outline, bkNeck.from, bkNeck.to);
        const HoodPanels hp = addHood(g, body, neckLen * 0.5f, 0.32f, p.hoodHeight);
        g.seams.push_back(seam({frNeck, bkNeck, flNeck}, {hp.neckP, hp.neckM}));
    }
    return g;
}

Garment cloak(const Body& body, const CloakParams& p) { return drape(body, p); }
Garment cape(const Body& body, const CapeParams& p) { return drape(body, p); }

Garment overShoulder(const Body& body, const OverShoulderParams& p)
{
    Garment g = withFabric("Wool");
    const float topY = body.shoulderY() + 0.03f;
    const float len = std::clamp(p.length, 0.2f, topY - 0.02f);
    const float hemY = topY - len;
    const float nx0 = std::max(p.neckWidth * 0.5f, 0.07f);
    const float w2 = std::max(p.width * 0.5f, nx0 + 0.05f);
    const float nx = std::min(nx0, w2 - 0.05f);

    std::vector<Vec2> half{{w2, 0.f}};
    int rT0 = -1, rT1 = -1;
    const float tie = std::clamp(p.sideTie, 0.f, len * 0.5f);
    if (tie > 0.005f)
    {
        const float tc = std::clamp(body.waistY() - hemY, 0.02f + tie * 0.5f, len - 0.1f - tie * 0.5f);
        rT0 = int(half.size());
        half.push_back({w2, tc - tie * 0.5f});
        rT1 = int(half.size());
        half.push_back({w2, tc + tie * 0.5f});
    }
    const int rT = int(half.size());
    half.push_back({w2, len});
    const int rN = int(half.size());
    half.push_back({nx, len});

    const float z0 = body.chestRadius() + kGap + 0.03f;
    const Symmetric f = symmetric(half, len, nx, std::min(0.11f, len * 0.5f));
    const Symmetric bk = symmetric(half, len, nx, 0.03f);
    Panel front = makePanel("Front", f.outline);
    Panel back = makePanel("Back", bk.outline);
    placeAround(front, hemY, z0, w2, true);
    placeAround(back, hemY, z0, w2, false);
    g.panels = {std::move(front), std::move(back)};

    g.seams.push_back(seam({f.plusRun(0, rT, rN)}, {bk.minusRun(1, rT, rN)}));
    g.seams.push_back(seam({f.minusRun(0, rT, rN)}, {bk.plusRun(1, rT, rN)}));
    if (rT0 >= 0)
    {
        g.seams.push_back(seam({f.plusRun(0, rT0, rT1)}, {bk.minusRun(1, rT0, rT1)}));
        g.seams.push_back(seam({f.minusRun(0, rT0, rT1)}, {bk.plusRun(1, rT0, rT1)}));
    }
    return g;
}

Garment poncho(const Body& body, const PonchoParams& p) { return overShoulder(body, p); }
Garment tabard(const Body& body, const TabardParams& p) { return overShoulder(body, p); }

Garment belt(const Body& body, const BeltParams& p, std::vector<BlockAnchor>* anchors)
{
    Garment g = withFabric("Leather");
    const float target = body.waist * (1.f + std::max(p.ease, 0.f));
    const float fitR = p.fitRadius > 0.f ? p.fitRadius : body.waist / (2.f * std::numbers::pi_v<float>) + 0.04f;
    // Cut to fit loosely around the placement radius; warp shrink pulls it in to the target length.
    const float cut = 2.f * std::numbers::pi_v<float> * fitR;
    g.fabrics[0].shrinkWarp = std::min(1.f, target / cut);
    const float h = std::clamp(p.width, 0.01f, 0.2f);
    Panel pn = makePanel("Belt", {{-cut * 0.5f, 0.f}, {cut * 0.5f, 0.f}, {cut * 0.5f, h}, {-cut * 0.5f, h}});
    pn.particleDistance = std::min(0.01f, h * 0.25f);
    const float y = body.waistY() + p.height - h * 0.5f;
    place(pn, {0.f, y, -fitR}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f}, fitR);
    g.panels.push_back(std::move(pn));
    g.seams.push_back(seam({{0, 1, 2}}, {{0, 3, 0}}));
    if (anchors && p.buckle)
        anchors->push_back({"buckle", 0, {cut * 0.5f, h * 0.5f}, {0.f, y + h * 0.5f, fitR}});
    return g;
}

Garment sack(const Body& body, const SackParams& p)
{
    Garment g = withFabric("Canvas");
    const float w = std::clamp(p.width, 0.1f, 2.f), h = std::clamp(p.height, 0.1f, 2.f);
    const std::vector<Vec2> o{{-w * 0.5f, 0.f}, {w * 0.5f, 0.f}, {w * 0.5f, h}, {-w * 0.5f, h}};
    const float x = body.hip / std::numbers::pi_v<float> * 0.5f + w * 0.5f + 0.15f;
    const float y = std::max(0.05f, body.hipY() - h * 0.5f);
    for (float s : {1.f, -1.f})
    {
        Panel pn = makePanel(s > 0.f ? "Front" : "Back", o);
        place(pn, {x, y, s * 0.02f}, {0.f, 1.f, 0.f}, {0.f, 0.f, s}, 0.f);
        g.panels.push_back(std::move(pn));
    }
    g.seams.push_back(seam({{0, 0, 1}}, {{1, 0, 1}}));
    g.seams.push_back(seam({{0, 1, 2}}, {{1, 3, 0}}));
    g.seams.push_back(seam({{0, 3, 0}}, {{1, 1, 2}}));
    return g;
}

Garment banner(const Body& body, const BannerParams& p)
{
    Garment g = withFabric("Banner");
    const float w = std::clamp(p.width, 0.1f, 5.f), h = std::clamp(p.height, 0.1f, 5.f);
    Builder b;
    Vec3 origin;
    if (p.vertical)
    {
        const float tail = std::clamp(p.tail, 0.f, h * 0.45f);
        b.add(0.f, 0.f);
        if (tail > 0.f)
            b.add(w * 0.5f, tail);
        b.add(w, 0.f);
        b.add(w, h);
        b.add(0.f, h);
        origin = {body.shoulderWidth * 0.5f + 0.4f, std::max(0.05f, body.height + 0.5f - h), 0.f};
    }
    else
    {
        const float tail = std::clamp(p.tail, 0.f, w * 0.45f);
        b.add(0.f, 0.f);
        b.add(w, 0.f);
        if (tail > 0.f)
            b.add(w - tail, h * 0.5f);
        b.add(w, h);
        b.add(0.f, h);
        origin = {body.shoulderWidth * 0.5f + 0.4f, body.height * 0.9f, 0.f};
    }
    Panel pn = makePanel(p.vertical ? "Banner" : "Flag", b.pts);
    place(pn, origin, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, 0.f);
    g.panels.push_back(std::move(pn));
    return g;
}

Garment tent(const Body&, const TentParams& p)
{
    Garment g = withFabric("Canvas");
    const int n = std::clamp(p.sides, 3, 16);
    const float r = std::max(p.radius, 0.3f);
    const float h = std::max(p.height, 0.3f);
    const float apothem = r * std::cos(std::numbers::pi_v<float> / float(n));
    const float hb = r * std::sin(std::numbers::pi_v<float> / float(n));
    const float slant = std::hypot(apothem, h);
    for (int i = 0; i < n; ++i)
    {
        const float phi = 2.f * std::numbers::pi_v<float> * float(i) / float(n);
        const float sp = std::sin(phi), cp = std::cos(phi);
        Panel pn = makePanel("Side " + std::to_string(i + 1), {{-hb, 0.f}, {hb, 0.f}, {0.f, slant}});
        place(pn, {sp * apothem, 0.f, cp * apothem}, {-apothem * sp, h, -apothem * cp}, {h * sp, apothem, h * cp}, 0.f);
        g.panels.push_back(std::move(pn));
    }
    for (int i = 0; i < n; ++i)
    {
        if (p.door && i == n - 1)
            continue;
        g.seams.push_back(seam({{i, 1, 2}}, {{(i + 1) % n, 2, 0}}));
    }
    return g;
}

int appendGarment(Garment& dst, const Garment& src, int layer)
{
    const int p0 = int(dst.panels.size());
    const int f0 = int(dst.fabrics.size());
    dst.fabrics.insert(dst.fabrics.end(), src.fabrics.begin(), src.fabrics.end());
    for (Panel pn : src.panels)
    {
        pn.fabric += f0;
        pn.layer += layer;
        dst.panels.push_back(std::move(pn));
    }
    for (Seam s : src.seams)
    {
        for (SeamSide& a : s.a)
            a.panel += p0;
        for (SeamSide& b : s.b)
            b.panel += p0;
        dst.seams.push_back(std::move(s));
    }
    return p0;
}

Garment cloakTunicBelt(const Body& body, const OutfitParams& p, std::vector<BlockAnchor>* anchors)
{
    TunicParams tp;
    tp.length = p.tunicLength;
    const Garment t = tunic(body, tp);

    // Belt loose around the tunic's widest point at the waist, cinched by its fabric.
    const Panel& front = t.panels[0];
    const float hx = halfWidthAt(front.outline, body.waistY() - front.origin[1]);
    const float z0 = front.origin[2];
    const float r = front.wrapRadius;
    const float th = r > 0.f ? hx / r : 0.f;
    const float reach = std::max(z0, std::hypot(r * std::sin(th), z0 - r * (1.f - std::cos(th))));
    BeltParams bp;
    bp.ease = p.beltEase;
    bp.fitRadius = reach + 0.015f;
    std::vector<BlockAnchor> beltAnchors;
    const Garment bt = belt(body, bp, &beltAnchors);

    CloakParams cp;
    cp.length = p.cloakLength;
    cp.hood = p.hood;
    cp.clearance = std::max(z0, bp.fitRadius) + 0.06f;
    const Garment ck = cloak(body, cp);

    Garment out;
    out.fabrics.clear();
    appendGarment(out, t, 0);
    const int beltPanel = appendGarment(out, bt, 1);
    appendGarment(out, ck, 2);
    if (anchors)
        for (BlockAnchor a : beltAnchors)
        {
            a.panel += beltPanel;
            anchors->push_back(std::move(a));
        }
    return out;
}

namespace
{
/** A bootie's opening, split where it meets the leg's side seams: A front and back halves, M's mirrored. */
struct Bootie
{
    SeamSide af, ab, mf, mb;
};

/**
 * One foot cover centred on x = cx: a side profile (A, facing -X) and its mirror (M, facing +X), sewn round all but the
 * top. The opening is frontHalf + backHalf long on each profile, split between the two at its mid point.
 */
Bootie addBootie(Garment& g, const Body& body, float cx, float top, float frontHalf, float backHalf, float length, const char* side)
{
    const float s = body.torso() / 1.8f;
    const float rH = 0.035f * s, toeH = 0.085f * s;
    const float w = frontHalf + backHalf;
    const float xh = -0.085f * s, xb = xh + 0.005f, xf = xb + w, xm = xf - frontHalf;
    const float rT = std::min(0.05f * s, 0.5f * toeH);
    const float xt = std::max(xh + length, xf + rT + 0.01f);
    top = std::max(top, toeH + 0.06f * s);

    // Counter-clockwise from the heel: sole forward, round the toe, up the instep, back along the opening, down the heel.
    Builder b;
    b.add(xh + rH, 0.f);
    b.add(xt - rT, 0.f);
    b.arc({xt - rT, 0.5f * toeH}, rT, 0.5f * toeH, -0.5f * std::numbers::pi_v<float>, 0.5f * std::numbers::pi_v<float>, true);
    const Vec2 instep{xf, std::min(top - 0.03f, toeH + 0.04f * s)};
    for (const Vec2& q : bezier({xt - rT, toeH}, {xf, toeH}, instep))
        b.add(q[0], q[1]);
    b.add(instep[0], instep[1]);
    const int topF = b.add(xf, top);
    const int topM = b.add(xm, top);
    const int topB = b.add(xb, top);
    b.add(xh, rH);
    b.arc({xh + rH, rH}, rH, rH, std::numbers::pi_v<float>, 1.5f * std::numbers::pi_v<float>, false);
    const int n = int(b.pts.size());

    const float gap = 0.055f * s;
    const int a = int(g.panels.size()), m = a + 1;
    const bool left = cx > 0.f;
    Panel pa = makePanel(std::string(left ? "Foot Inner" : "Foot Outer") + side, b.pts);
    Panel pm = makePanel(std::string(left ? "Foot Outer" : "Foot Inner") + side, mirrorX(b.pts));
    pa.particleDistance = pm.particleDistance = 0.012f;
    place(pa, {cx - gap, 0.005f, 0.f}, {0.f, 1.f, 0.f}, {-1.f, 0.f, 0.f}, 0.f);
    place(pm, {cx + gap, 0.005f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, 0.f);
    g.panels.push_back(std::move(pa));
    g.panels.push_back(std::move(pm));

    const SeamSide round{a, topB, topF};
    g.seams.push_back(seam({round}, {mirrorRun(round, n, m)}));
    Bootie out;
    out.af = {a, topF, topM};
    out.ab = {a, topM, topB};
    out.mf = mirrorRun(out.af, n, m);
    out.mb = mirrorRun(out.ab, n, m);
    return out;
}

/**
 * A tail out of the back panel `host`: two mirrored tapering profiles sewn along their outer edges, their mouths tacked
 * round an oval on host centred at its pattern point `at`. It points straight back and droops under its own weight.
 */
void addTail(Garment& g, int host, const Vec2& at, float length, float girth, float scale)
{
    const float pi = std::numbers::pi_v<float>;
    const float a = girth / (pi * 1.85f), b = 0.85f * a;
    // Each profile's mouth is half the oval's perimeter (Ramanujan), so the two mouths go round it once.
    const float mouth = 0.5f * pi * (3.f * (a + b) - std::sqrt((3.f * a + b) * (a + 3.f * b)));
    const float tipY = -0.1f * mouth, tipH = 0.03f * scale;

    // Counter-clockwise, pattern +x out of the back: mouth bottom, along the underside to the tip, back along the top to
    // the mouth top. The closing edge is the mouth.
    Builder bd;
    bd.add(0.f, -0.5f * mouth);
    for (const Vec2& q : bezier({0.f, -0.5f * mouth}, {0.45f * length, -0.3f * mouth}, {length, tipY}))
        bd.add(q[0], q[1]);
    bd.add(length, tipY);
    bd.add(length, tipY + tipH);
    for (const Vec2& q : bezier({length, tipY + tipH}, {0.4f * length, 0.45f * mouth}, {0.f, 0.5f * mouth}))
        bd.add(q[0], q[1]);
    bd.add(0.f, 0.5f * mouth);
    const int n = int(bd.pts.size());

    const Panel& hp = g.panels[size_t(host)];
    const Vec3 outward = norm3(cross3(hp.right, hp.up));
    const Vec3 side = norm3(cross3({0.f, 1.f, 0.f}, outward));
    const Vec3 base = add3(placePatternPoint(hp, at), mul3(outward, 0.01f));
    const float gap = 0.25f * a;
    // Which way the host's pattern x runs against the tail's +side decides which half of the oval each profile takes.
    const float hostX = hp.right[0] * side[0] + hp.right[1] * side[1] + hp.right[2] * side[2] >= 0.f ? 1.f : -1.f;

    // A on the -side facing out, M its mirror on the +side; both have pattern +x running out of the back.
    const int pa = int(g.panels.size()), pm = pa + 1;
    Panel A = makePanel("Tail A", bd.pts), M = makePanel("Tail B", mirrorX(bd.pts));
    A.particleDistance = M.particleDistance = 0.015f;
    place(A, add3(base, mul3(side, -gap)), {0.f, 1.f, 0.f}, mul3(side, -1.f), 0.f);
    place(M, add3(base, mul3(side, gap)), {0.f, 1.f, 0.f}, side, 0.f);
    g.panels.push_back(std::move(A));
    g.panels.push_back(std::move(M));

    const SeamSide outer{pa, 0, n - 1};
    g.seams.push_back(seam({outer}, {mirrorRun(outer, n, pm)}));

    const int count = std::max(8, int(mouth / 0.012f));
    for (int k = 0; k <= count; ++k)
    {
        const float t = float(k) / float(count);
        const Vec2 mouthPt{0.f, 0.5f * mouth - t * mouth};
        const float ox = a * std::sin(pi * t), oy = b * std::cos(pi * t);
        g.tacks.push_back({pa, mouthPt, host, {at[0] - hostX * ox, at[1] + oy}});
        g.tacks.push_back({pm, mouthPt, host, {at[0] + hostX * ox, at[1] + oy}});
    }
}

/** The bootie's opening as one chain that, walked backwards, starts at M's mid point and goes round the back first. */
std::vector<SeamSide> bootieOpening(const Bootie& b) { return {b.mf, b.af, b.ab, b.mb}; }

float chainLength(const Garment& g, const std::vector<SeamSide>& chain)
{
    float len = 0.f;
    for (const SeamSide& s : chain)
        len += runLength(g.panels[size_t(s.panel)].outline, s.from, s.to);
    return len;
}

Garment cutFrom(Garment g, const char* fabricName)
{
    if (const Fabric* f = findFabric(fabricName))
        g.fabrics = {*f};
    for (Panel& pn : g.panels)
        pn.fabric = 0;
    return g;
}
} // namespace

Garment booties(const Body& body, const BootieParams& p)
{
    Garment g = withFabric("Wool");
    const float quarter = std::clamp(p.opening, 0.2f, 1.f) * 0.25f;
    const float length = std::clamp(p.length, 0.15f, 0.6f), height = std::clamp(p.height, 0.1f, 0.6f);
    addBootie(g, body, body.hip * 0.1f, height, quarter, quarter, length, " L");
    addBootie(g, body, -body.hip * 0.1f, height, quarter, quarter, length, " R");
    return cutFrom(std::move(g), "Plush");
}

Garment onesie(const Body& body, const OnesieParams& p)
{
    TopParams tp;
    tp.length = body.shoulderY() - body.waistY();
    tp.ease = std::max(p.ease, 0.f);
    tp.sleeveLength = p.sleeveLength;
    tp.sleeveEase = p.sleeveEase;
    tp.sleeveStyle = p.sleeveStyle;
    tp.neckDepth = 0.06f;
    tp.openFront = true;
    tp.overlap = 0.f;
    TopRuns tr;
    Garment g = draftTop(body, tp, &tr);
    // Centre front first, so the zip goes in seam 0.
    g.seams.insert(g.seams.begin(), seam({tr.centreL}, {tr.centreR}));

    const float floorToFoot = p.feet ? 0.16f * body.torso() / 1.8f : 0.f;
    TrousersParams tq;
    tq.ease = std::max(p.legEase, 0.f);
    tq.hemCircumference = p.legOpening;
    tq.backRise = 0.f;
    tq.waist = chainLength(g, tr.hemFront) + chainLength(g, {tr.hemBack});
    tq.length = body.waistY() - (p.feet ? floorToFoot : 0.03f);
    const int t0 = appendGarment(g, trousers(body, tq), 0);
    const int fl = t0, bl = t0 + 1, fr = t0 + 2, br = t0 + 3;
    const int nf = int(g.panels[size_t(fl)].outline.size()), nb = int(g.panels[size_t(br)].outline.size());

    // Top hem to trouser waist: fronts -X to +X, back +X to -X.
    g.seams.push_back(seam(tr.hemFront, {{fl, 4, 5}, {fr, nf - 5, nf - 4}}));
    g.seams.push_back(seam({tr.hemBack}, {{br, 4, 5}, {bl, nb - 5, nb - 4}}));

    if (p.hood && !tr.neck.empty())
    {
        const HoodPanels hp = addHood(g, body, chainLength(g, tr.neck) * 0.5f, p.hoodDepth, p.hoodHeight);
        g.seams.push_back(seam(tr.neck, {hp.neckP, hp.neckM}));
    }

    if (p.feet)
    {
        // Each leg's hem starts at the side seam on the back, walks the back, then the front.
        const std::vector<SeamSide> hemL{{bl, nb - 1, 0}, {fl, 0, 1}}, hemR{{br, 0, 1}, {fr, nf - 1, 0}};
        for (const auto& [hem, sign, side] : {std::tuple{hemL, 1.f, " L"}, std::tuple{hemR, -1.f, " R"}})
        {
            const float back = runLength(g.panels[size_t(hem[0].panel)].outline, hem[0].from, hem[0].to);
            const float front = runLength(g.panels[size_t(hem[1].panel)].outline, hem[1].from, hem[1].to);
            const float cx = g.panels[size_t(sign > 0.f ? fl : fr)].origin[0];
            const Bootie bt = addBootie(g, body, cx, floorToFoot, 0.5f * front, 0.5f * back, std::max(p.footLength, 0.15f), side);
            g.seams.push_back(seam(hem, bootieOpening(bt)));
        }
    }
    g = cutFrom(std::move(g), "Plush");
    if (p.tail)
    {
        // At the small of the back, the oval's bottom just above the waist seam.
        const float girth = std::clamp(p.tailGirth, 0.15f, 1.2f);
        const float b = 0.85f * girth / (std::numbers::pi_v<float> * 1.85f);
        const int first = int(g.panels.size());
        addTail(g, tr.hemBack.panel, {0.f, b + 0.02f}, std::clamp(p.tailLength, 0.1f, 2.f), girth, body.torso() / 1.8f);
        // Stuffing has no sim of its own: the tail's plush bends like foam instead, so it holds out from the back.
        Fabric stuffed = g.fabrics[0];
        stuffed.name = "Plush Stuffed";
        const float bend = kStuffedBend * std::clamp(p.tailStiffness, 0.f, 3.f);
        stuffed.bendWarp *= bend, stuffed.bendWeft *= bend, stuffed.bendBias *= bend;
        stuffed.density *= 0.6f;
        g.fabrics.push_back(stuffed);
        for (size_t k = size_t(first); k < g.panels.size(); ++k)
            g.panels[k].fabric = 1;
    }
    return g;
}

namespace
{
/** Leftmost and rightmost x of a closed outline along the horizontal line at y. */
std::pair<float, float> spanAt(const std::vector<Vec2>& o, float y)
{
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < o.size(); ++i)
    {
        const Vec2& a = o[i];
        const Vec2& b = o[(i + 1) % o.size()];
        if ((a[1] - y) * (b[1] - y) > 0.f || a[1] == b[1])
            continue;
        const float x = a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]);
        lo = std::min(lo, x), hi = std::max(hi, x);
    }
    return {lo, hi};
}

float spanWidth(const std::vector<Vec2>& o, float y)
{
    const auto [lo, hi] = spanAt(o, y);
    return hi > lo ? hi - lo : 0.f;
}

Vec3 chainPoint3(const Garment& g, const std::vector<SeamSide>& chain, float s)
{
    for (const SeamSide& side : chain)
    {
        const auto& o = g.panels[size_t(side.panel)].outline;
        const int n = int(o.size());
        for (int v = side.from; v != side.to; v = (v + 1) % n)
        {
            const Vec2 &a = o[size_t(v)], &b = o[size_t((v + 1) % n)];
            const float l = dist2d(a, b);
            if (s <= l && l > 0.f)
                return placePatternPoint(g.panels[size_t(side.panel)], {a[0] + (b[0] - a[0]) * s / l, a[1] + (b[1] - a[1]) * s / l});
            s -= l;
        }
    }
    const SeamSide& last = chain.back();
    return placePatternPoint(g.panels[size_t(last.panel)], g.panels[size_t(last.panel)].outline[size_t(last.to)]);
}

/** Sews a to b, the way round that puts their quarter points nearest each other. */
Seam sewFacing(const Garment& g, std::vector<SeamSide> a, std::vector<SeamSide> b)
{
    const float la = chainLength(g, a), lb = chainLength(g, b);
    const Vec3 a1 = chainPoint3(g, a, 0.25f * la), b1 = chainPoint3(g, b, 0.25f * lb), b3 = chainPoint3(g, b, 0.75f * lb);
    auto dist = [](const Vec3& x, const Vec3& y) { return std::hypot(x[0] - y[0], x[1] - y[1], x[2] - y[2]); };
    const bool rev = dist(a1, b3) < dist(a1, b1);
    return seam(std::move(a), std::move(b), rev);
}

/** A band round a leg hem hanging below it, its side seam at the chain's start (the outer side seam). */
void addAnkleCuff(Garment& g, const std::vector<SeamSide>& hem, float cx, float hemY, float height, float band, float inward, const char* side)
{
    const float r = band / (2.f * std::numbers::pi_v<float>);
    const int tp = int(g.panels.size());
    Panel t = makePanel(std::string("Ankle Cuff") + side, {{-0.5f * band, 0.f}, {0.5f * band, 0.f}, {0.5f * band, height}, {-0.5f * band, height}});
    t.particleDistance = std::min(t.particleDistance, 0.3f * height);
    // Pattern x = 0 on the inside of the leg, so both ends meet on the outside.
    const Vec3 nrm{inward, 0.f, 0.f};
    place(t, {cx + inward * r, hemY, 0.f}, {0.f, -1.f, 0.f}, nrm, r);
    g.panels.push_back(std::move(t));
    g.seams.push_back(sewFacing(g, {{tp, 0, 1}}, hem));
    g.seams.push_back(seam({{tp, 1, 2}}, {{tp, 3, 0}}));
}

/** Knee patches on the two trouser fronts, at knee height. */
void addKneePatches(Garment& g, const Body& body, int fl, int fr)
{
    for (int panel : {fl, fr})
    {
        const Panel& p = g.panels[size_t(panel)];
        const float y = 0.285f * body.torso() - p.origin[1];
        const auto [lo, hi] = spanAt(p.outline, y);
        if (hi - lo < 0.08f)
            continue;
        addPatch(g, panel, {0.5f * (lo + hi), y}, std::min(0.15f, 0.6f * (hi - lo)), 0.17f * body.torso() / 1.8f, "Knee");
    }
}
} // namespace

Garment suit(const Body& body, const SuitParams& p)
{
    const float s = body.torso() / 1.8f;
    TopParams tp;
    tp.length = body.shoulderY() - body.waistY();
    tp.ease = std::max(p.ease, 0.f);
    tp.sleeveLength = p.sleeveLength;
    tp.sleeveEase = p.sleeveEase;
    tp.sleeveStyle = p.sleeveStyle;
    tp.neckDepth = 0.06f * s;
    tp.openFront = true;
    tp.overlap = 0.f;
    TopRuns tr;
    Garment g = draftTop(body, tp, &tr);
    // Centre front first, so the zip goes in seam 0.
    g.seams.insert(g.seams.begin(), seam({tr.centreL}, {tr.centreR}));

    const float ankle = 0.06f * s;
    const float cuffH = p.ankleCuffs ? std::clamp(p.cuffHeight, 0.02f, 0.15f) : 0.f;
    TrousersParams tq;
    tq.ease = std::max(p.legEase, 0.f);
    tq.hemCircumference = p.legOpening;
    tq.backRise = 0.f;
    tq.waist = chainLength(g, tr.hemFront) + chainLength(g, {tr.hemBack});
    tq.length = body.waistY() - ankle - cuffH;
    const int t0 = appendGarment(g, trousers(body, tq), 0);
    const int fl = t0, bl = t0 + 1, fr = t0 + 2, br = t0 + 3;
    const int nf = int(g.panels[size_t(fl)].outline.size()), nb = int(g.panels[size_t(br)].outline.size());
    g.seams.push_back(seam(tr.hemFront, {{fl, 4, 5}, {fr, nf - 5, nf - 4}}));
    g.seams.push_back(seam({tr.hemBack}, {{br, 4, 5}, {bl, nb - 5, nb - 4}}));
    g = cutFrom(std::move(g), p.fabric);

    const CollarStyle collar = CollarStyle(std::clamp(p.collar, 0, int(CollarStyle::Count) - 1));
    if (collar != CollarStyle::None && !tr.neck.empty())
    {
        const int c = addCollar(g, tr.neck, std::clamp(p.collarHeight, 0.015f, 0.15f), collar == CollarStyle::Shirt);
        // A shirt collar's points stay apart at the front; a stand closes with the zip.
        const Seam& last = g.seams.back();
        if (collar == CollarStyle::Shirt && c >= 0 && last.a.size() == 1 && last.a[0].panel == c && last.a[0].from == 1)
            g.seams.pop_back();
    }

    if (p.ankleCuffs)
    {
        const std::vector<SeamSide> hemL{{bl, nb - 1, 0}, {fl, 0, 1}}, hemR{{br, 0, 1}, {fr, nf - 1, 0}};
        const float hemY = g.panels[size_t(fl)].origin[1];
        for (const auto& [hem, panel, inward, side] : {std::tuple{hemL, fl, -1.f, " L"}, std::tuple{hemR, fr, 1.f, " R"}})
        {
            // Gathered a little into the band, as an elastic ankle is.
            const float band = std::max(0.85f * chainLength(g, hem), 0.24f * s);
            addAnkleCuff(g, hem, g.panels[size_t(panel)].origin[0], hemY, cuffH, band, inward, side);
        }
    }

    if (p.chestPockets)
    {
        // Front L runs from the centre (x = 0) out to +x, Front R mirrors it.
        const float wc = g.panels[0].outline.size() > 1 ? spanWidth(g.panels[0].outline, 0.02f) : 0.2f;
        const float w = std::min(0.13f * s, 0.42f * wc), h = 0.14f * s;
        const float y = body.chestY() - body.waistY() - 0.5f * h + 0.01f;
        addPocket(g, 0, {0.5f * wc - 0.5f * w, y}, w, h, true);
        addPocket(g, 1, {-0.5f * wc - 0.5f * w, y}, w, h, true);
    }

    if (p.legPockets)
    {
        for (const auto& [panel, outward] : {std::pair{fl, 1.f}, std::pair{fr, -1.f}})
        {
            const Panel& leg = g.panels[size_t(panel)];
            const float h = 0.17f * s;
            const float top = body.crotchY() - leg.origin[1] - 0.1f * s, bottom = top - h;
            if (bottom < 0.05f)
                continue;
            const auto [lo, hi] = spanAt(leg.outline, 0.5f * (top + bottom));
            const float span = std::min({hi - lo, spanWidth(leg.outline, top), spanWidth(leg.outline, bottom)});
            const float w = std::min(0.15f * s, 0.45f * span);
            const float cx = 0.5f * (lo + hi) + outward * 0.2f * span;
            addPocket(g, panel, {cx - 0.5f * w, bottom}, w, h, true);
        }
    }

    if (p.kneePatches)
        addKneePatches(g, body, fl, fr);

    if (p.elbowPatches)
    {
        for (size_t k = 0; k < g.panels.size(); ++k)
        {
            const Panel& sl = g.panels[k];
            if (sl.name != "Sleeve L" && sl.name != "Sleeve R")
                continue;
            // The elbow faces back: pattern -x on the left sleeve, +x on the right.
            const float back = sl.name == "Sleeve L" ? -1.f : 1.f;
            const float y = 0.4f * body.armLength;
            float maxY = 0.f;
            for (const Vec2& q : sl.outline)
                maxY = std::max(maxY, q[1]);
            const float h = 0.13f * s;
            if (maxY < y + 0.5f * h + 0.01f)
                continue;
            const auto [lo, hi] = spanAt(sl.outline, y);
            const float w = std::min(0.11f * s, 0.4f * (hi - lo));
            addPatch(g, int(k), {0.5f * (lo + hi) + back * 0.25f * (hi - lo), y}, w, h, "Elbow");
        }
    }
    return g;
}

Garment overalls(const Body& body, const OverallsParams& p, std::vector<BlockAnchor>* anchors)
{
    const float s = body.torso() / 1.8f;
    const float ease = std::max(p.ease, 0.f);
    TrousersParams tq;
    tq.ease = ease;
    tq.hemCircumference = p.legOpening;
    tq.length = p.length > 0.f ? p.length : body.waistY() - 0.03f * s;
    Garment g = trousers(body, tq);
    const int fl = 0, bl = 1, fr = 2, br = 3;
    const int nf = int(g.panels[size_t(fl)].outline.size()), nb = int(g.panels[size_t(br)].outline.size());

    const float waistY = body.waistY();
    const float yS = body.shoulderY() - waistY;
    const float sw = std::clamp(p.strapWidth, 0.02f, 0.1f);
    const float z0 = body.chestRadius() * (1.f + ease) + kGap;
    struct Piece { int panel; Symmetric shape; int outerTop, innerTop, bibTop; };
    // Bib or back: bottom as wide as the trousers' waist, sides curving in to the top, straps rising from its corners.
    auto piece = [&](const char* name, float halfBottom, float height, bool front) {
        const float top = std::clamp(height, 0.06f, yS - 0.08f);
        const float bw = std::clamp(0.5f * p.bibWidth, sw + 0.03f, halfBottom);
        std::vector<Vec2> half{{halfBottom, 0.f}};
        for (const Vec2& q : bezier({halfBottom, 0.f}, {bw, 0.45f * top}, {bw, top}))
            half.push_back(q);
        Piece out{int(g.panels.size()), {}, 0, 0, 0};
        out.bibTop = int(half.size());
        half.push_back({bw, top});
        out.outerTop = int(half.size());
        half.push_back({bw, yS});
        out.innerTop = int(half.size());
        half.push_back({bw - sw, yS});
        half.push_back({bw - sw, top});
        out.shape = symmetric(half, top, bw - sw, 0.f);
        Panel pn = makePanel(name, out.shape.outline);
        pn.particleDistance = std::min(pn.particleDistance, sw / 3.f);
        placeAround(pn, waistY, z0, halfBottom, front);
        g.panels.push_back(std::move(pn));
        return out;
    };
    const Piece bib = piece("Bib", runLength(g.panels[size_t(fl)].outline, 4, 5), p.bibHeight, true);
    const Piece back = piece("Back Bib", runLength(g.panels[size_t(br)].outline, 4, 5), p.backHeight, false);

    g.seams.push_back(seam({{bib.panel, 0, 1}}, {{fl, 4, 5}, {fr, nf - 5, nf - 4}}));
    g.seams.push_back(seam({{back.panel, 0, 1}}, {{br, 4, 5}, {bl, nb - 5, nb - 4}}));
    // The back faces -Z, so its +x straps lie on the wearer's right.
    g.seams.push_back(seam({bib.shape.plusRun(bib.panel, bib.outerTop, bib.innerTop)}, {back.shape.minusRun(back.panel, back.outerTop, back.innerTop)}));
    g.seams.push_back(seam({bib.shape.minusRun(bib.panel, bib.outerTop, bib.innerTop)}, {back.shape.plusRun(back.panel, back.outerTop, back.innerTop)}));
    g = cutFrom(std::move(g), "Denim");

    const Panel& bp = g.panels[size_t(bib.panel)];
    const Vec2 corner = bp.outline[size_t(bib.shape.plus[size_t(bib.bibTop)])];
    if (anchors)
        for (const auto& [name, x] : {std::pair{"buckle L", 1.f}, std::pair{"buckle R", -1.f}})
        {
            const Vec2 at{x * (corner[0] - 0.5f * sw), corner[1]};
            anchors->push_back({name, bib.panel, at, placePatternPoint(bp, at)});
        }
    if (p.bibPocket)
    {
        const float w = std::min(0.16f * s, 2.f * corner[0] - 0.06f), h = std::min(0.14f * s, 0.5f * corner[1]);
        if (w > 0.04f)
            addPocket(g, bib.panel, {-0.5f * w, corner[1] - h - 0.03f * s}, w, h, false);
    }
    if (p.kneePatches)
        addKneePatches(g, body, fl, fr);
    return g;
}

// Registry

namespace
{

template <class P>
struct Field
{
    const char* name;
    float P::*f = nullptr;
    int P::*i = nullptr;
    bool P::*b = nullptr;
    float min = 0.f, max = 1.f;

    Field(const char* n, float P::*m, float lo, float hi) : name(n), f(m), min(lo), max(hi) {}
    Field(const char* n, int P::*m, int lo, int hi) : name(n), i(m), min(float(lo)), max(float(hi)) {}
    Field(const char* n, bool P::*m) : name(n), b(m) {}
};

template <class P>
using Generator = std::function<Garment(const Body&, const P&, std::vector<BlockAnchor>*)>;

template <class P>
Block makeBlock(const char* id, const char* label, std::vector<Field<P>> fields, Generator<P> gen)
{
    Block blk{id, label, {}, {}};
    const P defaults{};
    for (const Field<P>& f : fields)
    {
        BlockParam bp{f.name, 0.f, f.min, f.max, BlockParam::Kind::Float};
        if (f.f)
            bp.defaultValue = defaults.*f.f;
        else if (f.i)
            bp.defaultValue = float(defaults.*f.i), bp.kind = BlockParam::Kind::Int;
        else
            bp.defaultValue = defaults.*f.b ? 1.f : 0.f, bp.kind = BlockParam::Kind::Bool;
        blk.params.push_back(std::move(bp));
    }
    blk.build = [fields = std::move(fields), gen = std::move(gen)](const Body& body, const BlockParamValues& values,
                                                                  std::vector<BlockAnchor>* anchors) {
        P p{};
        for (const Field<P>& f : fields)
        {
            const auto it = values.find(f.name);
            if (it == values.end())
                continue;
            const float v = std::clamp(it->second, f.min, f.max);
            if (f.f)
                p.*f.f = v;
            else if (f.i)
                p.*f.i = int(std::lround(v));
            else
                p.*f.b = v >= 0.5f;
        }
        return gen(body, p, anchors);
    };
    return blk;
}

template <class P>
Generator<P> plain(Garment (*fn)(const Body&, const P&))
{
    return [fn](const Body& b, const P& p, std::vector<BlockAnchor>*) { return fn(b, p); };
}

template <class P>
std::vector<Field<P>> topFields()
{
    return {{"length", &P::length, 0.25f, 1.6f},
            {"ease", &P::ease, 0.f, 1.f},
            {"sleeveLength", &P::sleeveLength, 0.f, 0.75f},
            {"sleeveEase", &P::sleeveEase, 0.f, 1.5f},
            {"sleeveFlare", &P::sleeveFlare, 0.f, 2.f},
            {"hemFlare", &P::hemFlare, 0.f, 0.6f},
            {"slit", &P::slit, 0.f, 0.8f},
            {"neckDepth", &P::neckDepth, 0.01f, 0.6f},
            {"openFront", &P::openFront},
            {"overlap", &P::overlap, 0.f, 0.2f},
            {"sleeveStyle", &P::sleeveStyle, 0, int(SleeveStyle::Count) - 1},
            {"dropShoulder", &P::dropShoulder, 0.f, 0.2f}};
}

template <class P>
std::vector<Field<P>> drapeFields()
{
    return {{"length", &P::length, 0.25f, 1.6f},
            {"flare", &P::flare, 0.f, 1.2f},
            {"frontCoverage", &P::frontCoverage, 0.f, 1.f},
            {"overlap", &P::overlap, 0.f, 0.15f},
            {"closedSides", &P::closedSides},
            {"hood", &P::hood},
            {"hoodHeight", &P::hoodHeight, 0.25f, 0.6f}};
}

template <class P>
std::vector<Field<P>> overShoulderFields()
{
    return {{"width", &P::width, 0.3f, 2.f},
            {"length", &P::length, 0.2f, 1.6f},
            {"neckWidth", &P::neckWidth, 0.14f, 0.5f},
            {"sideTie", &P::sideTie, 0.f, 0.3f}};
}

std::vector<Block> makeBlocks()
{
    std::vector<Block> out;
    out.push_back(makeBlock<TeeParams>("tee", "T-Shirt", topFields<TeeParams>(), plain(&tee)));
    out.push_back(makeBlock<TunicParams>("tunic", "Tunic", topFields<TunicParams>(), plain(&tunic)));
    out.push_back(makeBlock<RobeParams>("robe", "Robe", topFields<RobeParams>(), plain(&robe)));
    out.push_back(makeBlock<RaglanTeeParams>("raglanTee", "Raglan Tee", topFields<RaglanTeeParams>(), [](const Body& b, const RaglanTeeParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<MarieBlouseParams>("marieBlouse", "Marie-Sleeve Blouse", topFields<MarieBlouseParams>(), [](const Body& b, const MarieBlouseParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<FlounceSleeveTopParams>("flounceSleeveTop", "Flounce-Sleeve Top", topFields<FlounceSleeveTopParams>(), [](const Body& b, const FlounceSleeveTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<SlitSleeveTopParams>("slitSleeveTop", "Slit-Sleeve Top", topFields<SlitSleeveTopParams>(), [](const Body& b, const SlitSleeveTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<TieredSleeveBlouseParams>("tieredSleeveBlouse", "Tiered-Sleeve Blouse", topFields<TieredSleeveBlouseParams>(), [](const Body& b, const TieredSleeveBlouseParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PetalSleeveTopParams>("petalSleeveTop", "Petal-Sleeve Top", topFields<PetalSleeveTopParams>(), [](const Body& b, const PetalSleeveTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<CapeSleeveTopParams>("capeSleeveTop", "Cape-Sleeve Top", topFields<CapeSleeveTopParams>(), [](const Body& b, const CapeSleeveTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<DropShoulderTopParams>("dropShoulderTop", "Drop-Shoulder Top", topFields<DropShoulderTopParams>(), [](const Body& b, const DropShoulderTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<ShirtParams>("shirt", "Shirt", topFields<ShirtParams>(), [](const Body& b, const ShirtParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PeasantBlouseParams>("peasantBlouse", "Peasant Blouse", topFields<PeasantBlouseParams>(), [](const Body& b, const PeasantBlouseParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PuffBlouseParams>("puffBlouse", "Puff-Sleeve Blouse", topFields<PuffBlouseParams>(), [](const Body& b, const PuffBlouseParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PoetShirtParams>("poetShirt", "Poet Shirt", topFields<PoetShirtParams>(), [](const Body& b, const PoetShirtParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<BellSleeveDressParams>("bellSleeveDress", "Bell-Sleeve Dress", topFields<BellSleeveDressParams>(), [](const Body& b, const BellSleeveDressParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<FlutterTopParams>("flutterTop", "Flutter-Sleeve Top", topFields<FlutterTopParams>(), [](const Body& b, const FlutterTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<CapSleeveTopParams>("capSleeveTop", "Cap-Sleeve Top", topFields<CapSleeveTopParams>(), [](const Body& b, const CapSleeveTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<BatwingTopParams>("batwingTop", "Batwing Top", topFields<BatwingTopParams>(), [](const Body& b, const BatwingTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<LegOfMuttonParams>("legOfMuttonBlouse", "Leg-of-Mutton Blouse", topFields<LegOfMuttonParams>(), [](const Body& b, const LegOfMuttonParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<TrousersParams>("trousers", "Trousers",
                                            {{"length", &TrousersParams::length, 0.3f, 1.2f},
                                             {"ease", &TrousersParams::ease, 0.f, 0.6f},
                                             {"hemCircumference", &TrousersParams::hemCircumference, 0.2f, 1.2f},
                                             {"backRise", &TrousersParams::backRise, 0.f, 0.1f}},
                                            plain(&trousers)));
    out.push_back(makeBlock<SkirtParams>("skirt", "Skirt",
                                         {{"length", &SkirtParams::length, 0.15f, 1.1f},
                                          {"ease", &SkirtParams::ease, 0.f, 0.5f},
                                          {"flare", &SkirtParams::flare, 0.f, 1.f}},
                                         plain(&skirt)));
    out.push_back(makeBlock<HoodParams>("hood", "Hood",
                                        {{"depth", &HoodParams::depth, 0.2f, 0.5f}, {"height", &HoodParams::height, 0.25f, 0.6f}},
                                        plain(&hood)));
    out.push_back(makeBlock<CloakParams>("cloak", "Cloak", drapeFields<CloakParams>(), plain(&cloak)));
    out.push_back(makeBlock<CapeParams>("cape", "Cape", drapeFields<CapeParams>(), plain(&cape)));
    out.push_back(makeBlock<PonchoParams>("poncho", "Poncho", overShoulderFields<PonchoParams>(), plain(&poncho)));
    out.push_back(makeBlock<TabardParams>("tabard", "Tabard", overShoulderFields<TabardParams>(), plain(&tabard)));
    out.push_back(makeBlock<BeltParams>("belt", "Belt",
                                        {{"height", &BeltParams::height, -0.3f, 0.2f},
                                         {"width", &BeltParams::width, 0.01f, 0.2f},
                                         {"ease", &BeltParams::ease, 0.f, 0.5f},
                                         {"fitRadius", &BeltParams::fitRadius, 0.f, 0.6f},
                                         {"buckle", &BeltParams::buckle}},
                                        Generator<BeltParams>(&belt)));
    out.push_back(makeBlock<SackParams>("sack", "Sack",
                                        {{"width", &SackParams::width, 0.1f, 2.f}, {"height", &SackParams::height, 0.1f, 2.f}},
                                        plain(&sack)));
    out.push_back(makeBlock<BannerParams>("banner", "Banner / Flag",
                                          {{"width", &BannerParams::width, 0.1f, 5.f},
                                           {"height", &BannerParams::height, 0.1f, 5.f},
                                           {"vertical", &BannerParams::vertical},
                                           {"tail", &BannerParams::tail, 0.f, 2.f}},
                                          plain(&banner)));
    out.push_back(makeBlock<TentParams>("tent", "Tent",
                                        {{"radius", &TentParams::radius, 0.3f, 5.f},
                                         {"height", &TentParams::height, 0.3f, 5.f},
                                         {"sides", &TentParams::sides, 3, 16},
                                         {"door", &TentParams::door}},
                                        plain(&tent)));
    out.push_back(makeBlock<OutfitParams>("cloakTunicBelt", "Tunic, Belt and Hooded Cloak",
                                          {{"tunicLength", &OutfitParams::tunicLength, 0.5f, 1.3f},
                                           {"cloakLength", &OutfitParams::cloakLength, 0.4f, 1.5f},
                                           {"beltEase", &OutfitParams::beltEase, 0.f, 0.3f},
                                           {"hood", &OutfitParams::hood}},
                                          Generator<OutfitParams>(&cloakTunicBelt)));
    out.push_back(makeBlock<OnesieParams>("onesie", "Onesie",
                                          {{"ease", &OnesieParams::ease, 0.f, 1.f},
                                           {"sleeveLength", &OnesieParams::sleeveLength, 0.f, 0.75f},
                                           {"sleeveEase", &OnesieParams::sleeveEase, 0.f, 1.5f},
                                           {"sleeveStyle", &OnesieParams::sleeveStyle, 0, int(SleeveStyle::Count) - 1},
                                           {"legEase", &OnesieParams::legEase, 0.f, 0.8f},
                                           {"legOpening", &OnesieParams::legOpening, 0.3f, 1.f},
                                           {"hood", &OnesieParams::hood},
                                           {"hoodDepth", &OnesieParams::hoodDepth, 0.2f, 0.5f},
                                           {"hoodHeight", &OnesieParams::hoodHeight, 0.25f, 0.6f},
                                           {"feet", &OnesieParams::feet},
                                           {"footLength", &OnesieParams::footLength, 0.15f, 0.6f},
                                           {"tail", &OnesieParams::tail},
                                           {"tailLength", &OnesieParams::tailLength, 0.1f, 2.f},
                                           {"tailGirth", &OnesieParams::tailGirth, 0.15f, 1.2f},
                                           {"tailStiffness", &OnesieParams::tailStiffness, 0.f, 3.f}},
                                          plain(&onesie)));
    auto suitBlock = [&]<class P>(const char* id, const char* label, P*) {
        out.push_back(makeBlock<P>(id, label,
                                   {{"ease", &P::ease, 0.f, 1.f},
                                    {"sleeveLength", &P::sleeveLength, 0.f, 0.75f},
                                    {"sleeveEase", &P::sleeveEase, 0.f, 1.5f},
                                    {"sleeveStyle", &P::sleeveStyle, 0, int(SleeveStyle::Count) - 1},
                                    {"legEase", &P::legEase, 0.f, 0.8f},
                                    {"legOpening", &P::legOpening, 0.2f, 1.f},
                                    {"collar", &P::collar, 0, int(CollarStyle::Count) - 1},
                                    {"collarHeight", &P::collarHeight, 0.015f, 0.15f},
                                    {"ankleCuffs", &P::ankleCuffs},
                                    {"cuffHeight", &P::cuffHeight, 0.02f, 0.15f},
                                    {"chestPockets", &P::chestPockets},
                                    {"legPockets", &P::legPockets},
                                    {"kneePatches", &P::kneePatches},
                                    {"elbowPatches", &P::elbowPatches}},
                                   [](const Body& b, const P& p, std::vector<BlockAnchor>*) { return suit(b, p); }));
    };
    suitBlock("jumpsuit", "Jumpsuit", static_cast<JumpsuitParams*>(nullptr));
    suitBlock("flightSuit", "Flight Suit", static_cast<FlightSuitParams*>(nullptr));
    suitBlock("coverall", "Coverall", static_cast<CoverallParams*>(nullptr));
    out.push_back(makeBlock<OverallsParams>("overalls", "Overalls",
                                            {{"ease", &OverallsParams::ease, 0.f, 0.8f},
                                             {"legOpening", &OverallsParams::legOpening, 0.25f, 1.2f},
                                             {"length", &OverallsParams::length, 0.f, 1.3f},
                                             {"bibHeight", &OverallsParams::bibHeight, 0.06f, 0.4f},
                                             {"bibWidth", &OverallsParams::bibWidth, 0.12f, 0.5f},
                                             {"backHeight", &OverallsParams::backHeight, 0.06f, 0.35f},
                                             {"strapWidth", &OverallsParams::strapWidth, 0.02f, 0.1f},
                                             {"bibPocket", &OverallsParams::bibPocket},
                                             {"kneePatches", &OverallsParams::kneePatches}},
                                            Generator<OverallsParams>(&overalls)));
    out.push_back(makeBlock<BootieParams>("booties", "Booties",
                                          {{"length", &BootieParams::length, 0.15f, 0.6f},
                                           {"height", &BootieParams::height, 0.1f, 0.6f},
                                           {"opening", &BootieParams::opening, 0.2f, 1.f}},
                                          plain(&booties)));
    return out;
}

} // namespace

const std::vector<Block>& blocks()
{
    static const std::vector<Block> all = makeBlocks();
    return all;
}

const Block* findBlock(const std::string& id)
{
    for (const Block& b : blocks())
        if (b.id == id)
            return &b;
    return nullptr;
}

std::vector<std::string> blockNames()
{
    std::vector<std::string> out;
    for (const Block& b : blocks())
        out.push_back(b.id);
    return out;
}

bool buildBlock(const std::string& id, Garment& out, std::string* error, const Body& body, const BlockParamValues& values,
                std::vector<BlockAnchor>* anchors)
{
    const Block* b = findBlock(id);
    if (!b)
    {
        if (error)
            *error = "unknown block '" + id + "'";
        return false;
    }
    out = b->build(body, values, anchors);
    return true;
}

} // namespace tailor
