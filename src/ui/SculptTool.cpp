#include "ui/SculptTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "brush/BrushUi.h"
#include "brush/Presets.h"
#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"
#include "core/Paths.h"
#include "ui/MeshCache.h"
#include "ui/TextureCache.h"
#include "ui/SculptPanel.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportTool.h"
#include "ui/Tools.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QWidget>
#include <QTimer>

#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{

std::map<int, std::function<void()>>& settingsListeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}

SculptSettings loadSettings()
{
    SculptSettings s;
    const PrefStore& p = PrefStore::get();
    const std::string brushName = p.value<std::string>("general", "/sculpt/brush", "draw");
    if (auto b = sculpt::brushFromName(brushName))
        s.brush = *b;
    else if (!brushName.empty())
    {
        // Extensions may register the brush after the settings first load.
        s.brush = sculpt::Brush::Custom;
        s.custom = brushName;
    }
    s.preset = p.value<std::string>("general", "/sculpt/preset", s.preset);
    s.paint = p.value<bool>("general", "/sculpt/paint", s.paint);
    s.cavityPaint = p.value<bool>("general", "/sculpt/cavityPaint", s.cavityPaint);
    if (json c = p.value("general", "/sculpt/color"); c.is_array() && c.size() == 3 && c[0].is_number() && c[1].is_number() && c[2].is_number())
        s.color = glm::vec3(c[0].get<float>(), c[1].get<float>(), c[2].get<float>());
    s.stroke.loadPrefs("/sculpt");
    if (!brush::Falloff::find(s.stroke.falloff))
        s.stroke.falloff = "smooth";
    s.stroke.radius = std::clamp(s.stroke.radius, 2.0, 2000.0);
    s.stroke.spacing = std::clamp(s.stroke.spacing, 0.02, 2.0);
    s.frontFacesOnly = p.value<bool>("general", "/sculpt/frontFacesOnly", s.frontFacesOnly);
    s.levels = std::clamp(p.value<int>("general", "/sculpt/startLevels", s.levels), 0, 6);
    s.symmetryLine = p.value<bool>("general", "/sculpt/symmetryLine", s.symmetryLine);
    s.shareRadius = p.value<bool>("general", "/sculpt/shareRadius", s.shareRadius);
    s.shareStrength = p.value<bool>("general", "/sculpt/shareStrength", s.shareStrength);
    s.shareStroke = p.value<bool>("general", "/sculpt/shareStroke", s.shareStroke);
    s.wireframe = p.value<bool>("general", "/sculpt/wireframe", s.wireframe);
    s.wireOpacity = std::clamp(p.value<double>("general", "/sculpt/wireOpacity", s.wireOpacity), 0.05, 1.0);
    s.seeThrough = p.value<bool>("general", "/sculpt/seeThrough", s.seeThrough);
    s.seeThroughOpacity = std::clamp(p.value<double>("general", "/sculpt/seeThroughOpacity", s.seeThroughOpacity), 0.0, 1.0);
    s.material = p.value<std::string>("general", "/sculpt/material", s.material);
    if (json per = p.value("general", "/sculpt/perBrush"); per.is_object())
        for (auto& [name, j] : per.items())
            s.perBrush[name] = brush::BrushSettings::fromJson(j, SculptSettings{}.stroke);
    return s;
}

std::string brushKey(sculpt::Brush b, const std::string& custom, const std::string& preset)
{
    if (!preset.empty())
        return "preset:" + preset;
    return b == sculpt::Brush::Custom ? custom : std::string(sculpt::brushName(b));
}

/** A brush's starting settings the first time it is picked. */
brush::BrushSettings defaultStroke(sculpt::Brush b)
{
    brush::BrushSettings s = SculptSettings{}.stroke;
    switch (b)
    {
    case sculpt::Brush::DamStandard:
        s.radius = 25.0;
        s.strength = 0.6;
        s.falloff = "sharp";
        break;
    case sculpt::Brush::Crease:
    case sculpt::Brush::Pinch: s.falloff = "sharp"; break;
    case sculpt::Brush::TrimDynamic: s.strength = 0.7; break;
    case sculpt::Brush::Fold:
        s.radius = 40.0;
        s.strength = 0.4;
        s.falloff = "root";
        break;
    case sculpt::Brush::Mask:
    case sculpt::Brush::Paint:
    case sculpt::Brush::Rotate:
    case sculpt::Brush::Pose:
    case sculpt::Brush::ElasticGrab:
    case sculpt::Brush::Boundary: s.strength = 1.0; break;
    case sculpt::Brush::SnakeHook:
        s.radius = 40.0;
        s.strength = 0.9;
        s.falloff = "root";
        break;
    case sculpt::Brush::Layer:
    case sculpt::Brush::Planar: s.strength = 0.8; break;
    case sculpt::Brush::Magnify: s.falloff = "sharp"; break;
    default: break;
    }
    return s;
}

/** A sculpt preview material; colours linear. */
struct SculptMaterial
{
    const char* id;
    const char* label;
    render::Material material;
};

const std::vector<SculptMaterial>& materialPresets()
{
    static const std::vector<SculptMaterial> presets = [] {
        auto lit = [](glm::vec3 color, float roughness, float specular = 0.5f, float metallic = 0.0f) {
            render::Material m;
            m.baseColor = color;
            m.roughness = roughness;
            m.specular = specular;
            m.metallic = metallic;
            return m;
        };
        auto skin = [&](glm::vec3 color, float roughness, glm::vec3 scatter, float amount) {
            render::Material m = lit(color, roughness);
            m.style.model = render::ShadingModel::Skin;
            m.style.subsurfaceColor = scatter;
            m.style.subsurfaceAmount = amount;
            return m;
        };
        auto coat = [&](glm::vec3 color, float roughness, float coatRoughness) {
            render::Material m = lit(color, roughness);
            m.style.model = render::ShadingModel::Clearcoat;
            m.style.clearcoat = 1.0f;
            m.style.clearcoatRoughness = coatRoughness;
            return m;
        };
        render::Material toon = lit(glm::vec3(0.55f), 0.9f, 0.2f);
        toon.style.model = render::ShadingModel::Toon;
        toon.style.toonShadowColor = glm::vec3(0.22f, 0.24f, 0.32f);
        toon.style.toonOuterColor = glm::vec3(0.05f);
        return std::vector<SculptMaterial>{
            {"clay", "Clay", lit(glm::vec3(0.42f), 0.6f)},
            {"terracotta", "Terracotta", lit(glm::vec3(0.42f, 0.17f, 0.09f), 0.85f, 0.3f)},
            {"redWax", "Red Wax", skin(glm::vec3(0.35f, 0.035f, 0.025f), 0.35f, glm::vec3(0.9f, 0.15f, 0.08f), 0.7f)},
            {"skin", "Skin", skin(glm::vec3(0.55f, 0.32f, 0.24f), 0.5f, glm::vec3(0.9f, 0.3f, 0.2f), 0.8f)},
            {"chalk", "Chalk", lit(glm::vec3(0.78f), 1.0f, 0.15f)},
            {"jade", "Jade", skin(glm::vec3(0.08f, 0.32f, 0.16f), 0.2f, glm::vec3(0.3f, 0.8f, 0.45f), 0.8f)},
            {"bronze", "Bronze", lit(glm::vec3(0.6f, 0.38f, 0.2f), 0.35f, 0.5f, 1.0f)},
            {"ceramic", "Ceramic", coat(glm::vec3(0.82f), 0.5f, 0.05f)},
            {"plastic", "Toy Plastic", coat(glm::vec3(0.06f, 0.15f, 0.55f), 0.4f, 0.1f)},
            {"toon", "Toon", toon},
        };
    }();
    return presets;
}

/** Lila's matcaps in resources/matcaps, by file stem and label. */
constexpr std::pair<const char*, const char*> kMatcaps[] = {{"clay", "Clay"},   {"redWax", "Red Wax"}, {"skin", "Skin"},
                                                            {"chalk", "Chalk"}, {"gloss", "Gloss"},    {"cavity", "Cavity"}};

std::string matcapPath(const std::string& id)
{
    if (id == kMatcapFileId)
        return PrefStore::get().value<std::string>("general", kMatcapFilePref, "");
    return paths::toUtf8(paths::resourcesDir() / "matcaps" / (id.substr(std::string_view(kMatcapPrefix).size()) + ".png"));
}

/** The preset to draw with, or null for the mesh's own materials. */
const render::Material* sculptMaterial(const std::string& id)
{
    for (const SculptMaterial& m : materialPresets())
        if (id == m.id)
            return &m.material;
    if (!id.starts_with(kMatcapPrefix))
        return nullptr;
    static std::map<std::string, render::Material> matcaps;
    render::Material& m = matcaps[id];
    m.style.model = render::ShadingModel::Matcap;
    m.baseColor = glm::vec3(1.0f);
    // Null while the image loads; the viewport redraws when it arrives.
    m.baseColorMap = textures::get(matcapPath(id), true);
    if (m.baseColorMap)
        m.flags |= render::MatBaseColorMap;
    m.sampling = 0x5; // clamp U and V, so the sphere's rim never wraps to the far side
    return &m;
}

glm::dvec3 xformPoint(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }
glm::dvec3 xformDir(const glm::dmat4& m, const glm::dvec3& d) { return glm::dvec3(m * glm::dvec4(d, 0.0)); }

class SculptTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "sculpt"; }
    // The brush ring and its centre dot are the cursor.
    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }
    std::string markingMenu(std::string_view base) const override { return base == "viewportShift" ? "sculptShift" : std::string(); }

    void activate() override
    {
        mControls.setStrengthTap([] {
            SculptSettings& s = sculptSettings();
            bool& wire = s.wireframe;
            wire = !wire;
            sculptSettingsChanged();
            viewportToast::showActive(wire ? "Wireframe on" : "Wireframe off", 1000);
        });
        mControls.activate();
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        mControls.deactivate();
        viewportTools::requestRedraw();
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        auto s = sculpt::active();
        if (!s || !scene::Graph::current().node(s->node))
            return true;
        Hit hit;
        const brush::StrokeMode mode = sculptSettings().stroke.mode;
        const bool line = (mode == brush::StrokeMode::Line || mode == brush::StrokeMode::Curve) && !sculpt::dragsCursor(sculptSettings().brush);
        // A press off the mesh never selects while sculpting: the stroke starts and takes effect once it reaches the
        // surface. Brushes that hold the point under the cursor have nothing to hold, so the press does nothing.
        if (!surfaceHit(*s, e->position(), view, hit) && !line && sculpt::dragsCursor(sculptSettings().brush))
            return true;
        beginStroke(*s, e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mStroking)
        {
            strokeTo(e, view);
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        switch (e->key())
        {
        case Qt::Key_Escape:
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        default: return false;
        }
    }

    /** Level keys while sculpting: D finer, Shift+D coarser, Ctrl+D adds a level. */
    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        if (e.key() == Qt::Key_X && e.type() == QEvent::KeyPress && sculpt::active() &&
            (e.modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier)) == Qt::AltModifier)
        {
            if (!e.isAutoRepeat())
                QTimer::singleShot(0, [] { actions::run(json("sculpt.toggleSeeThrough")); });
            return true;
        }
        auto s = sculpt::active();
        if (!s || !s->stack || e.key() != Qt::Key_D || e.type() != QEvent::KeyPress)
            return false;
        const Qt::KeyboardModifiers mods = e.modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
        const char* id = mods == Qt::NoModifier ? "sculpt.levelUp" : mods == Qt::ShiftModifier ? "sculpt.levelDown"
                       : mods == Qt::ControlModifier ? "sculpt.addLevel" : nullptr;
        if (!id)
            return false;
        if (!e.isAutoRepeat())
            QTimer::singleShot(0, [id] { actions::run(json(id)); });
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        const SculptSettings& st = sculptSettings();
        auto s = sculpt::active();
        scene::Node* n = s ? scene::Graph::current().node(s->node) : nullptr;
        if (!n)
        {
            drawToolStatus(p, view, {.tool = "Sculpt", .prompt = "select a mesh and Enter Sculpt"});
            return;
        }
        // Shift smooths and Ctrl inverts the next stroke, so the message names the brush that stroke would use.
        const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        sculpt::BrushSettings shown = mSettings;
        const bool blurs = blursWithShift(mStroking ? mSettings.brush : st.brush);
        if (!mStroking)
        {
            shown.brush = (mods & Qt::ShiftModifier) && !blurs ? sculpt::Brush::Smooth : st.brush;
            shown.keepVolume = smoothStrokeKeepsVolume(st, mods);
            shown.custom = st.custom;
            shown.blur = blurs && (mods & Qt::ShiftModifier);
        }
        const bool invert = mStroking ? mSettings.invert : (mods & Qt::ControlModifier) != 0;
        QString name = QString::fromStdString(sculpt::brushId(shown));
        if (shown.brush == sculpt::Brush::Custom && !sculpt::brushRegistry().contains(shown.custom))
            name += " (not loaded)";
        ToolStatus status{.tool = "Sculpt", .subject = QString::fromStdString(n->name()), .mode = name};
        if (s->stack)
            status.fields.push_back({"level", QString("%1/%2").arg(s->level).arg(s->stack->top())});
        status.fields.push_back({"radius", QString("%1 px").arg(int(std::lround(st.stroke.radius)))});
        status.fields.push_back({"strength", QString::number(st.stroke.strength, 'f', 2)});
        const bool smoothing = shown.brush == sculpt::Brush::Smooth;
        // Ctrl does nothing else to Smooth, so its flag reads as the flatten it gives.
        status.flags = {smoothing ? ToolFlag{"flatten", !shown.keepVolume} : ToolFlag{"invert", invert}};
        if (blurs)
            status.flags.push_back({"blur", shown.blur});
        status.symmetry = ToolSymmetry{s->sculpt.symmetric()};
        const QString flip = smoothStrokeKeepsVolume(st, {}) ? "flatten" : "keep volume";
        status.keys = {{"Shift", blurs ? "blur" : st.brush == sculpt::Brush::Smooth ? flip : "smooth"},
                       {"Ctrl", smoothing ? flip : shown.brush == sculpt::Brush::Grab ? "along normal" : "invert"},
                       {"B-drag", "radius"},
                       {"M-drag", "strength"}};
        drawToolStatus(p, view, status);

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        Hit hit;
        std::optional<brush::ui::CursorSurface> surface;
        if (surfaceHit(*s, at, view, hit))
            surface = brush::ui::CursorSurface{hit.world, hit.worldNormal};
        brush::ui::drawCursor(p, view, st.stroke, at, surface,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength,
                               .normalTick = true,
                               .stroke = &mStroke});
    }

private:
    struct Hit
    {
        glm::dvec3 local{0.0}, normal{0.0};
        glm::dvec3 world{0.0}, worldNormal{0.0};
        glm::dvec3 viewDir{0.0}; // local
    };

    static glm::dmat4 worldOf(const sculpt::Session& s)
    {
        return scene::Graph::current().node(s.node)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    }

    bool surfaceHit(const sculpt::Session& s, QPointF px, const ViewInfo& view, Hit& out) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dmat4 w = worldOf(s), inv = glm::inverse(w);
        geo::BvhHit hit;
        glm::dvec3 lo = xformPoint(inv, o), ld = xformDir(inv, d);
        if (!s.sculpt.raycast(lo, ld, hit))
            return false;
        out.local = hit.point;
        out.normal = s.sculpt.normalAt(hit);
        out.world = xformPoint(w, hit.point);
        glm::dvec3 wn = glm::transpose(glm::inverse(glm::dmat3(w))) * out.normal;
        out.worldNormal = glm::length(wn) > 0.0 ? glm::normalize(wn) : glm::dvec3(0, 1, 0);
        out.viewDir = glm::normalize(ld);
        return true;
    }

    /** Shift on these blurs what they lay down (colour, mask) instead of smoothing the shape. */
    static bool blursWithShift(sculpt::Brush b) { return b == sculpt::Brush::Paint || b == sculpt::Brush::Mask; }

    void beginStroke(sculpt::Session& s, const QMouseEvent* e, const ViewInfo& view)
    {
        const Qt::KeyboardModifiers mods = e->modifiers();
        const SculptSettings& st = sculptSettings();
        mSession = sculpt::active();
        sculpt::syncSymmetry(s);
        const bool blurring = (mods & Qt::ShiftModifier) && blursWithShift(st.brush);
        const bool smoothing = (mods & Qt::ShiftModifier) && st.brush != sculpt::Brush::Smooth && !blurring;
        mSettings.brush = smoothing ? sculpt::Brush::Smooth : st.brush;
        mSettings.blur = blurring;
        mSettings.custom = st.custom;
        mSettings.paint = st.paint;
        mSettings.cavityPaint = st.cavityPaint;
        mSettings.color = st.color;
        mSettings.falloff = st.stroke.resolveFalloff();
        mFalloffName = st.stroke.falloff;
        mSettings.strength = st.stroke.strength;
        // Shift smooths with the Smooth brush's own strength.
        if (auto it = st.perBrush.find("smooth"); smoothing && !st.shareStrength && it != st.perBrush.end())
            mSettings.strength = it->second.strength;
        mSettings.invert = (mods & Qt::ControlModifier) != 0;
        mSettings.frontFacesOnly = st.frontFacesOnly;
        mSettings.radial = st.stroke.radial;
        auto smoothStroke = st.perBrush.find("smooth");
        const brush::BrushSettings& own = smoothing && smoothStroke != st.perBrush.end() ? smoothStroke->second : st.stroke;
        mSettings.keepVolume = smoothStrokeKeepsVolume(st, mods);
        mSettings.pinchDepth = own.param("pinchDepth", 1.0);
        s.sculpt.beginStroke(mSettings);
        mPivotBefore = viewportTools::orbitPoint();
        mDabs = json::array();
        mStroking = true;
        brush::BrushSettings stroke = st.stroke;
        // Grab-style brushes drag one dab across the view plane rather than laying a trail of them.
        if (sculpt::dragsCursor(mSettings.brush))
            stroke.mode = brush::StrokeMode::Freehand;
        stroke.seed = uint32_t(e->timestamp());
        const ViewInfo v = view;
        mStroke.begin(stroke, mControls.input(e), [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(QPointF(px.x, px.y), v, out); },
                      *this);
    }

    /** The shared stroke's view of the surface under a pixel, in the proxy's object space. */
    bool surfaceQuery(QPointF px, const ViewInfo& view, brush::SurfaceHit& out) const
    {
        if (!mSession || !scene::Graph::current().node(mSession->node))
            return false;
        Hit hit;
        if (!surfaceHit(*mSession, px, view, hit))
            return false;
        const glm::dmat4 w = worldOf(*mSession);
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        out.center = hit.local;
        out.normal = hit.normal;
        out.viewDir = hit.viewDir;
        out.right = brush::ui::screenRight(view, w);
        out.pixelSize = view.unitsPerPixel(hit.world) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    void dab(const brush::Dab& d) override
    {
        if (!mSession)
            return;
        if (mDabs.empty())
        {
            mGrab = d;
            mGrabLocal = d.center;
        }
        mLastLocal = d.center;
        send(d);
    }

    void send(const brush::Dab& d)
    {
        mSession->sculpt.dab(d);
        const glm::dmat4 w = worldOf(*mSession);
        viewportTools::setOrbitPoint(xformPoint(w, d.center));
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        mDabWorldRadius = d.radius * scale;
        const glm::dmat3 nrm = glm::transpose(glm::inverse(glm::dmat3(w)));
        mDabs.push_back({{"center", vecJson(xformPoint(w, d.center))},
                         {"normal", vecJson(glm::normalize(nrm * d.normal))},
                         {"radius", d.radius * scale},
                         {"pressure", d.pressure},
                         {"opacity", d.opacity},
                         {"flow", d.flow},
                         {"view", vecJson(xformDir(w, d.viewDir))},
                         {"delta", vecJson(xformDir(w, d.delta))},
                         {"direction", vecJson(xformDir(w, d.direction))}});
        viewportTools::requestRedraw();
    }

    void strokeTo(const QMouseEvent* e, const ViewInfo& view)
    {
        LILA_ZONE();

        auto s = sculpt::active();
        if (!s || s != mSession || !scene::Graph::current().node(s->node))
            return;
        if (sculpt::dragsCursor(mSettings.brush))
        {
            if (mDabs.empty())
                return;
            // The grabbed point follows the cursor across the plane facing the view at its starting depth.
            glm::dvec3 o, dir;
            view.ray(e->position(), o, dir);
            glm::dmat4 w = worldOf(*s), inv = glm::inverse(w);
            glm::dvec3 world = xformPoint(w, mGrabLocal);
            glm::dvec3 axis = glm::normalize(world - view.eye);
            double denom = glm::dot(glm::normalize(dir), axis);
            if (std::abs(denom) < 1e-9)
                return;
            glm::dvec3 target = o + glm::normalize(dir) * (glm::dot(world - o, axis) / denom);
            glm::dvec3 local = xformPoint(inv, target);
            brush::Dab d = mGrab;
            d.center = local;
            d.normal = glm::dvec3(0.0, 1.0, 0.0);
            d.viewDir = glm::normalize(xformDir(inv, axis));
            d.delta = local - mGrabLocal;
            if (mSettings.brush == sculpt::Brush::Grab && mSettings.invert)
            {
                // Ctrl-grab moves along the normal the stroke began on: dragging up the screen pulls out, down pushes in.
                const glm::dvec3 up = glm::normalize(xformDir(inv, glm::dvec3(glm::inverse(view.view)[1])));
                d.delta = glm::normalize(mGrab.normal) * glm::dot(d.delta, up);
            }
            d.direction = local - mLastLocal;
            mLastLocal = local;
            send(d);
            mGrabLocal = local;
            return;
        }
        mStroke.move(mControls.input(e));
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        auto s = sculpt::active();
        if (!s || s != mSession)
            return;
        sculpt::StrokeRecord rec = s->sculpt.endStroke();
        mSession.reset();
        if (rec.empty())
            return;
        rec.pivotBefore = mPivotBefore;
        rec.pivotAfter = viewportTools::orbitPoint();
        // Undo leaves the tumble point alone when no earlier stroke set it, or when it moved less than a brush width.
        rec.pivotMoved = rec.pivotBefore && rec.pivotAfter && glm::distance(*rec.pivotBefore, *rec.pivotAfter) > 2.0 * mDabWorldRadius;
        uint64_t token = sculpt::nextLiveToken();
        s->liveToken = token;
        s->live = std::move(rec);
        json call = {{"brush", sculpt::brushId(mSettings)},
                     {"paint", mSettings.paint},
                     {"color", json::array({mSettings.color.r, mSettings.color.g, mSettings.color.b})},
                     {"falloff", mFalloffName},
                     {"strength", mSettings.strength},
                     {"invert", mSettings.invert},
                     {"blur", mSettings.blur},
                     {"keepVolume", mSettings.keepVolume},
                     {"pinchDepth", mSettings.pinchDepth},
                     {"frontFacesOnly", mSettings.frontFacesOnly},
                     {"radialCount", mSettings.radial.count},
                     {"radialAxis", std::string(1, "xyz"[std::clamp(mSettings.radial.axis, 0, 2)])},
                     {"dabs", std::move(mDabs)},
                     {"live", int64_t(token)}};
        mDabs = json::array();
        try
        {
            // Not echoed: a stroke is hundreds of dabs, and repeating it from the script editor is rarely wanted.
            ops::execute("sculptStroke", call, CallSource::Script);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }

    // The controls' changed callback saves the prefs on every step, as the panel needs them current.
    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return sculptSettings().stroke; }, [] { sculptSettingsChanged(); }, {}};

    std::shared_ptr<sculpt::Session> mSession;
    sculpt::BrushSettings mSettings;
    std::optional<glm::dvec3> mPivotBefore;
    double mDabWorldRadius = 0.0;
    std::string mFalloffName;
    brush::Stroke mStroke;
    bool mStroking = false;
    json mDabs = json::array();
    QPointF mHover;
    glm::dvec3 mLastLocal{0.0}, mGrabLocal{0.0};
    brush::Dab mGrab;
};

/** Viewport content for the open session: the sculpt in place of its mesh, with the mask shading. */
class SculptDrawer
{
public:
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
    {
        LILA_ZONE();

        auto session = sculpt::active();
        // The panel is open but nothing is being sculpted: say so where the user is trying to draw.
        if (!session && !sculptToolActive() && sculptPanelOpen())
            drawToolStatus(p, view, {.tool = "Sculpt", .prompt = meshSelected() ? "not sculpting: pick a brush or Enter" : "not sculpting: select a mesh"});
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = session ? g.node(session->node) : nullptr;
        if (!n || !scene::effectivelyVisible(g, session->node))
            return;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);

        // The mesh itself is hidden while its proxy stands in for it. Its draw is found by its cached render copy.
        int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        auto shown = renderMeshFor(smoothedFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), smooth));
        auto isShown = [&](const render::MeshDraw& d) { return shown && d.mesh && (d.mesh == shown || d.mesh->shareVertices == shown); };
        const render::MeshDraw* replaced = nullptr;
        for (const render::MeshDraw& m : s.meshes)
            if (isShown(m))
            {
                replaced = &m;
                break;
            }
        const bool wireframeView = replaced && !(replaced->flags & render::DrawShaded);
        const uint32_t wireFlags = render::DrawWire | render::DrawSelected | render::DrawSelectedOther | render::DrawPreselect | render::DrawUnderSelected;
        const uint32_t viewWire = replaced ? replaced->flags & wireFlags : 0u;
        const glm::vec4 viewWireColor = replaced ? replaced->wireColor : glm::vec4(0.0f);
        std::erase_if(s.meshes.edit(), isShown);

        const SculptSettings& st = sculptSettings();
        render::MeshDraw d;
        d.mesh = session->sculpt.renderMesh();
        d.world = glm::mat4(world);
        if (const render::Material* mat = sculptMaterial(st.material))
        {
            d.materials = {*mat};
            d.baseColor = mat->baseColor;
            d.roughness = mat->roughness;
        }
        else if (replaced)
        {
            // The mesh's own materials; the levels keep each face's material id.
            d.materials = replaced->materials;
            d.baseColor = replaced->baseColor;
            d.roughness = replaced->roughness;
        }
        // Polypaint tints the material: white paint shows the material as it is. The mask and freeze ride in the same colours.
        const bool painted = !session->sculpt.colors().empty();
        if (painted || marked(*session))
            d.vertexColors = render::VertexColorMode::Shaded;
        if (painted && st.material == "clay" && !d.materials.empty())
            d.materials[0].baseColor = d.baseColor = glm::vec3(0.8f);
        // The sculpt mesh has no line list (strokes would leave it stale), so a wireframe view draws it as a clear surface
        // with the wire in its shader.
        const float opacity = wireframeView ? 0.0f : st.seeThrough ? float(std::clamp(st.seeThroughOpacity, 0.0, 1.0)) : 1.0f;
        if (opacity < 1.0f)
            for (render::Material& m : d.materials)
            {
                m.flags |= render::MatTranslucent;
                m.opacity = opacity;
            }
        if (wireframeView)
        {
            d.flags |= viewWire | render::DrawWire;
            d.wireColor = viewWireColor;
        }
        else if (st.wireframe)
        {
            d.flags |= render::DrawWire;
            d.wireColor = glm::vec4(0.06f, 0.06f, 0.06f, float(std::clamp(st.wireOpacity, 0.05, 1.0)));
        }
        s.meshes.push_back(std::move(d));
    }

private:
    /** Whether any point is masked or frozen, so the sculpt draws its vertex colours, which carry both. */
    bool marked(const sculpt::Session& session)
    {
        const sculpt::SculptMesh& sm = session.sculpt;
        if (&session != mMaskFor || sm.maskVersion() != mMaskVersion)
        {
            mMaskFor = &session;
            mMaskVersion = sm.maskVersion();
            mMarked = std::ranges::any_of(sm.mask(), [](float m) { return m > 0.0f; }) ||
                      std::ranges::any_of(sm.frozen(), [](uint8_t f) { return f != 0; });
        }
        return mMarked;
    }

    const sculpt::Session* mMaskFor = nullptr;
    uint64_t mMaskVersion = 0;
    bool mMarked = false;
};
} // namespace

SculptSettings& sculptSettings()
{
    static SculptSettings s = loadSettings();
    return s;
}

void sculptSettingsChanged()
{
    const SculptSettings& s = sculptSettings();
    PrefStore& p = PrefStore::get();
    p.set("general", "/sculpt/brush", s.brush == sculpt::Brush::Custom ? s.custom : std::string(sculpt::brushName(s.brush)));
    p.set("general", "/sculpt/preset", s.preset);
    p.set("general", "/sculpt/paint", s.paint);
    p.set("general", "/sculpt/cavityPaint", s.cavityPaint);
    p.set("general", "/sculpt/color", json::array({s.color.r, s.color.g, s.color.b}));
    s.stroke.savePrefs("/sculpt");
    p.set("general", "/sculpt/frontFacesOnly", s.frontFacesOnly);
    p.set("general", "/sculpt/startLevels", s.levels);
    p.set("general", "/sculpt/symmetryLine", s.symmetryLine);
    p.set("general", "/sculpt/shareRadius", s.shareRadius);
    p.set("general", "/sculpt/shareStrength", s.shareStrength);
    p.set("general", "/sculpt/shareStroke", s.shareStroke);
    p.set("general", "/sculpt/wireframe", s.wireframe);
    p.set("general", "/sculpt/wireOpacity", s.wireOpacity);
    p.set("general", "/sculpt/seeThrough", s.seeThrough);
    p.set("general", "/sculpt/seeThroughOpacity", s.seeThroughOpacity);
    p.set("general", "/sculpt/material", s.material);
    json per = json::object();
    for (const auto& [name, b] : s.perBrush)
        per[name] = b.toJson();
    p.set("general", "/sculpt/perBrush", per);
    auto copy = settingsListeners();
    for (auto& [id, fn] : copy)
        fn();
    viewportTools::requestRedraw();
}

int addSculptSettingsListener(std::function<void()> fn)
{
    static int next = 1;
    settingsListeners()[next] = std::move(fn);
    return next++;
}

void removeSculptSettingsListener(int id) { settingsListeners().erase(id); }

namespace
{
/**
 * Stores the current brush's stroke settings and swaps in the next one's (or first, its defaults), keeping what is shared.
 * Only the radius is shared to or from a preset: its stroke and strength are what make it the preset.
 */
void swapStroke(SculptSettings& s, const std::string& to, const brush::BrushSettings& first)
{
    const std::string from = brushKey(s.brush, s.custom, s.preset);
    if (from != to)
    {
        s.perBrush[from] = s.stroke;
        auto it = s.perBrush.find(to);
        const brush::BrushSettings stored = it != s.perBrush.end() ? it->second : first;
        const bool preset = from.starts_with("preset:") || to.starts_with("preset:");
        brush::BrushSettings next = s.shareStroke && !preset ? s.stroke : stored;
        const brush::BrushSettings& radius = s.shareRadius ? s.stroke : stored;
        next.radiusUnit = radius.radiusUnit;
        next.radius = radius.radius;
        next.worldRadius = radius.worldRadius;
        next.strength = (s.shareStrength && !preset ? s.stroke : stored).strength;
        s.stroke = next;
    }
}
} // namespace

void selectSculptBrush(sculpt::Brush b, const std::string& custom)
{
    SculptSettings& s = sculptSettings();
    swapStroke(s, brushKey(b, custom, {}), defaultStroke(b));
    s.brush = b;
    s.custom = b == sculpt::Brush::Custom ? custom : std::string();
    s.preset.clear();
    sculptSettingsChanged();
}

bool selectSculptPreset(const std::string& id)
{
    std::optional<brush::presets::Preset> p = brush::presets::find(id);
    if (!p)
        return false;
    sculpt::BrushSettings resolved;
    if (!sculpt::setBrush(resolved, p->brush))
    {
        log::warn("Brush preset {}: no brush named {}", id, p->brush);
        return false;
    }
    SculptSettings& s = sculptSettings();
    swapStroke(s, brushKey(resolved.brush, resolved.custom, id), p->settings);
    s.brush = resolved.brush;
    s.custom = resolved.custom;
    s.preset = id;
    sculptSettingsChanged();
    return true;
}

bool smoothStrokeKeepsVolume(const SculptSettings& st, Qt::KeyboardModifiers mods)
{
    const bool smoothBrush = st.brush == sculpt::Brush::Smooth;
    const bool own = [&] {
        if (smoothBrush)
            return st.stroke.param("keepVolume", 1.0) != 0.0;
        auto it = st.perBrush.find("smooth");
        return it == st.perBrush.end() || it->second.param("keepVolume", 1.0) != 0.0;
    }();
    // Shift on the Smooth brush has nothing else to do, so it flips too.
    const bool flip = (mods & Qt::ControlModifier) || (smoothBrush && (mods & Qt::ShiftModifier));
    return own != flip;
}

std::vector<std::pair<std::string, std::string>> sculptMaterials()
{
    std::vector<std::pair<std::string, std::string>> out{{"mesh", "Mesh Material"}};
    for (const SculptMaterial& m : materialPresets())
        out.emplace_back(m.id, m.label);
    for (const auto& [stem, label] : kMatcaps)
        out.emplace_back(std::string(kMatcapPrefix) + stem, std::string("Matcap: ") + label);
    if (const std::string file = PrefStore::get().value<std::string>("general", kMatcapFilePref, ""); !file.empty())
        out.emplace_back(kMatcapFileId, "Matcap: " + paths::toUtf8(paths::fromUtf8(file).stem()));
    return out;
}

void startSculptTool()
{
    if (!sculptToolActive())
        viewportTools::set(std::make_unique<SculptTool>());
}

bool sculptToolActive()
{
    ViewportTool* t = viewportTools::active();
    return t && std::string_view(t->name()) == "sculpt";
}

void installSculptDrawer()
{
    // Level changes, undo and session switches redraw whether or not the sculpt tool is active.
    sculpt::addListener([] { viewportTools::requestRedraw(); });
    sculpt::setPivotHandler([](const std::optional<glm::dvec3>& p) {
        if (sculptToolActive() && PrefStore::get().value<bool>("general", viewportTools::kOrbitUndoPref, true))
            viewportTools::setOrbitPoint(p);
    });
    static SculptDrawer drawer;
    viewportHooks::addDrawer([](render::FrameState& s, OverlayPainter& p, const ViewInfo& v) { drawer.draw(s, p, v); });
}
} // namespace lila::ui
