#include "ui/BakePaintTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "bake/Arrange.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "brush/BrushUi.h"
#include "brush/Surface.h"
#include "brush/Symmetry.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/BakeCageView.h"
#include "ui/MeshCache.h"
#include "ui/UiCommon.h"
#include "ui/UvTools.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"
#include "ui/Tools.h"

#include <QApplication>
#include <QCheckBox>
#include <QPainter>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
using scene::kNoNode;
using scene::NodeId;

constexpr const char* kBakePaintPrefix = "/bakePaint";

struct BakePaintSettings
{
    std::string attr = bake::kOffsetAttr;
    bake::PaintOp op = bake::PaintOp::Add;
    double value = 0.25;
    int symmetry = -1;
    /** Radius, strength, falloff (bell, linear or constant: the paint falloffs) and the shared brush. */
    brush::BrushSettings stroke = [] {
        brush::BrushSettings b;
        b.radius = 40.0;
        b.strength = 0.5;
        b.spacing = 0.25;
        b.falloff = "bell";
        return b;
    }();
};

BakePaintSettings& bakePaintSettings()
{
    static BakePaintSettings s = [] {
        BakePaintSettings out;
        out.stroke.loadPrefs(kBakePaintPrefix);
        return out;
    }();
    return s;
}

void saveBakePaintSettings() { bakePaintSettings().stroke.savePrefs(kBakePaintPrefix); }

constexpr const char* kFalloffNames[] = {"bell", "linear", "constant"};

/** The paint falloff of the brush's falloff name, for dabs in UV space. */
bake::PaintFalloff paintFalloff(const std::string& name)
{
    for (int i = 0; i < 3; ++i)
        if (name == kFalloffNames[i])
            return bake::PaintFalloff(i);
    return bake::PaintFalloff::Smooth;
}

const char* kOps[] = {"Add", "Subtract", "Smooth", "Set"};

/** The bake group holding mesh as its low, or null. */
scene::Node* groupOfLow(scene::Graph& g, NodeId mesh)
{
    const std::string path = g.node(mesh)->path();
    for (NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && bake::isGroup(*n))
        {
            const bake::GroupSettings s = bake::groupSettings(*n);
            if (std::ranges::find(s.lows, path) != s.lows.end())
                return n;
        }
    return nullptr;
}

/** Offsets read 0 to 2 around the unpainted 1 (more when painted higher), skew -1 to 1; through the weight ramp. */
glm::vec4 valueColor(const bake::PaintAttr& attr, float v, float top)
{
    const float t = attr.unpainted == 1.0f ? v / std::max(2.0f, top) : v * 0.5f + 0.5f;
    return viewportDisplay::weightColor(std::clamp(t, 0.0f, 1.0f), viewportDisplay::grayscaleWeights());
}

class BakePaintPanel final : public QWidget
{
public:
    std::function<void()> onChanged, onFlood, onClosed;
    QCheckBox* live = nullptr;

    explicit BakePaintPanel(QWidget* parent) : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Paint Bake Cage");
        auto* col = new QVBoxLayout(this);
        mTarget = new QLabel(this);
        col->addWidget(mTarget);
        auto* form = new QFormLayout;
        mAttr = new QComboBox(this);
        mAttr->addItem("Offset", bake::kOffsetAttr);
        mAttr->addItem("Skew", bake::kSkewAttr);
        mAttr->setToolTip("Offset scales the auto cage's push per point (1 unpainted). Skew tilts rays toward the face normal (added to the group's skew).");
        form->addRow("Attribute", mAttr);
        mOp = new QComboBox(this);
        for (const char* o : kOps)
            mOp->addItem(o);
        form->addRow("Mode", mOp);
        mValue = new QDoubleSpinBox(this);
        mValue->setRange(-1.0, 10.0);
        mValue->setDecimals(3);
        mValue->setSingleStep(0.05);
        form->addRow("Value", mValue);
        mRadius = new QSpinBox(this);
        mRadius->setRange(2, 2000);
        mRadius->setSuffix(" px");
        form->addRow("Radius", mRadius);
        mStrength = new QDoubleSpinBox(this);
        mStrength->setRange(0.0, 1.0);
        mStrength->setDecimals(3);
        mStrength->setSingleStep(0.05);
        form->addRow("Strength", mStrength);
        mFalloff = new QComboBox(this);
        mFalloff->addItems({"Smooth", "Linear", "Constant"});
        form->addRow("Falloff", mFalloff);
        mSymmetry = new QComboBox(this);
        mSymmetry->addItems({"Off", "X", "Y", "Z"});
        form->addRow("Symmetry", mSymmetry);
        col->addLayout(form);
        mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return bakePaintSettings().stroke; }, [this] { changed(); });
        col->addWidget(mStrokeOptions);

        auto* buttons = new QHBoxLayout;
        auto* flood = new QPushButton("Flood", this);
        flood->setToolTip("Apply the mode and value to every point (or the selected components)");
        auto* intoCage = new QPushButton("Bake Into Cage", this);
        intoCage->setToolTip("Create an editable cage from the painted auto cage, or move the group's cage to it");
        buttons->addWidget(flood);
        buttons->addWidget(intoCage);
        col->addLayout(buttons);
        auto* more = new QHBoxLayout;
        auto* autoPaint = new QPushButton("Auto Paint", this);
        autoPaint->setToolTip("Auto Paint: paint the offsets every ray needs to start outside the high and meet it, and set the group's Frontal and Rear, in one undo step");
        more->addWidget(autoPaint);
        live = new QCheckBox("Live preview", this);
        live->setToolTip("Rebake a low resolution preview of the group as you paint; missed rays glow red. Nothing is written.");
        live->setChecked(bakeCage::livePreview());
        more->addWidget(live);
        col->addLayout(more);
        auto* hint = new QLabel("Paints in the viewport and in the UV editor.", this);
        hint->setEnabled(false);
        col->addWidget(hint);
        connect(autoPaint, &QPushButton::clicked, this, [] { actions::run(json("bake.autoPaintCage")); });
        connect(live, &QCheckBox::toggled, this, [](bool on) { bakeCage::setLivePreview(on); });

        syncFromSettings();
        connect(mAttr, &QComboBox::currentIndexChanged, this, [this](int i) {
            bakePaintSettings().attr = mAttr->itemData(i).toString().toStdString();
            changed();
        });
        connect(mOp, &QComboBox::currentIndexChanged, this, [this](int i) {
            bakePaintSettings().op = bake::PaintOp(i);
            changed();
        });
        connect(mValue, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            bakePaintSettings().value = v;
            changed();
        });
        connect(mRadius, &QSpinBox::valueChanged, this, [this](int v) {
            bakePaintSettings().stroke.radius = v;
            changed();
        });
        connect(mStrength, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            bakePaintSettings().stroke.strength = v;
            changed();
        });
        connect(mSymmetry, &QComboBox::currentIndexChanged, this, [this](int i) {
            bakePaintSettings().symmetry = i - 1;
            changed();
        });
        connect(mFalloff, &QComboBox::currentIndexChanged, this, [this](int i) {
            bakePaintSettings().stroke.falloff = kFalloffNames[std::clamp(i, 0, 2)];
            changed();
        });
        connect(flood, &QPushButton::clicked, this, [this] {
            if (onFlood)
                onFlood();
        });
        connect(intoCage, &QPushButton::clicked, this, [] { actions::run(json("bake.cageFromPaint")); });
    }

    void setTarget(const QString& text) { mTarget->setText(text); }

    void syncFromSettings()
    {
        const BakePaintSettings& s = bakePaintSettings();
        const QSignalBlocker b0(mAttr), b1(mOp), b2(mValue), b3(mRadius), b4(mStrength), b5(mSymmetry), b6(mFalloff);
        mFalloff->setCurrentIndex(int(paintFalloff(s.stroke.falloff)));
        mAttr->setCurrentIndex(s.attr == bake::kSkewAttr ? 1 : 0);
        mOp->setCurrentIndex(int(s.op));
        mValue->setValue(s.value);
        mRadius->setValue(int(s.stroke.radius));
        mStrength->setValue(s.stroke.strength);
        mSymmetry->setCurrentIndex(s.symmetry + 1);
        if (mStrokeOptions)
            mStrokeOptions->sync();
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        QWidget::closeEvent(e);
        if (onClosed)
            onClosed();
    }

private:
    void changed()
    {
        saveBakePaintSettings();
        if (onChanged)
            onChanged();
    }

    QLabel* mTarget = nullptr;
    QComboBox* mAttr = nullptr;
    QComboBox* mOp = nullptr;
    QDoubleSpinBox* mValue = nullptr;
    QSpinBox* mRadius = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QComboBox* mSymmetry = nullptr;
    QComboBox* mFalloff = nullptr;
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
};

class BakePaintTool final : public ViewportTool, public uvTools::UvTool, public brush::DabSink
{
public:
    const char* name() const override { return "bakePaint"; }
    // The brush ring and its centre dot are the cursor.
    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }

    void activate() override
    {
        mAlive = std::make_shared<bool>(true);
        mControls.activate();
        mPanel = new BakePaintPanel(mainWindow());
        mPanel->onChanged = [this] {
            mPaint.reset();
            viewportTools::requestRedraw();
        };
        mPanel->onFlood = [this] { flood(); };
        mPanel->onClosed = [] {
            QTimer::singleShot(0, [] {
                if (viewportTools::active() && std::string(viewportTools::active()->name()) == "bakePaint")
                    viewportTools::set(nullptr);
            });
        };
        mPanel->show();
        uvTools::set(this);
        std::weak_ptr<bool> alive = mAlive;
        viewportHooks::setHideHook("lila.bakePaintLow", [this, alive](NodeId id) {
            if (alive.expired() || id != mMesh)
                return false;
            const scene::Node* n = scene::Graph::current().node(id);
            // The value ramp draws on the base mesh, which a smooth preview would cover.
            return n && n->as<int64_t>(scene::MeshNode::kSmoothPreview) > 0;
        });
        mListener = scene::Graph::current().addListener([this, alive](scene::Change, NodeId) {
            if (mStroking || mRefreshQueued)
                return;
            mRefreshQueued = true;
            QTimer::singleShot(0, [this, alive] {
                if (alive.expired())
                    return;
                mRefreshQueued = false;
                retarget();
            });
        });
        retarget();
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        mControls.deactivate();
        uvTools::set(nullptr);
        viewportHooks::removeHideHook("lila.bakePaintLow");
        bakeCage::setPainting(kNoNode);
        if (mMesh != kNoNode)
            bakeCage::clearWorkingOffsets(mMesh);
        scene::Graph::current().removeListener(mListener);
        mAlive.reset();
        if (mPanel)
        {
            mPanel->onClosed = nullptr;
            mPanel->deleteLater();
        }
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        pickUnder(e->position(), view);
        if (!ensurePaint())
            return true;
        mStroking = true;
        mMods = e->modifiers();
        const ViewInfo v = view;
        mStroke.begin(bakePaintSettings().stroke, mControls.input(e),
                      [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(QPointF(px.x, px.y), v, out); }, *this, false);
        return true;
    }

    /** The nearest bake low under the cursor, of any group. */
    NodeId lowUnder(QPointF px, const ViewInfo& view) const
    {
        scene::Graph& g = scene::Graph::current();
        glm::dvec3 o, d;
        view.ray(px, o, d);
        NodeId best = kNoNode;
        double nearest = 1e300;
        for (NodeId id : g.nodes())
        {
            scene::Node* grp = g.node(id);
            if (!grp || !bake::isGroup(*grp))
                continue;
            for (const std::string& name : bake::groupSettings(*grp).lows)
            {
                scene::Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                    continue;
                auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
                const glm::dmat4 w = bake::arrange::displayWorld(g, n->id(), n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
                if (auto hit = rm ? ui::raycast(*rm, w, o, d) : std::nullopt; hit && hit->distance < nearest)
                {
                    nearest = hit->distance;
                    best = n->id();
                }
            }
        }
        return best;
    }

    void pickUnder(QPointF px, const ViewInfo& view)
    {
        const NodeId low = lowUnder(px, view);
        if (low == kNoNode || low == mMesh)
            return;
        const auto& sel = scene::Graph::current().selection();
        mPicked = low;
        mPickedSel.assign(sel.begin(), sel.end());
        retarget();
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (!mStroking && mControls.adjusting() == brush::ui::BrushControls::Adjust::None && e->buttons() == Qt::NoButton)
            pickUnder(mHover, view);
        if (mControls.mouseMove(e))
            return true;
        if (mStroking)
        {
            mMods = e->modifiers();
            mStroke.move(mControls.input(e));
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
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

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        if (mMesh == kNoNode || !g.node(mMesh) || !ensurePaint())
        {
            drawToolStatus(p, view, {.tool = "Paint Bake", .prompt = "select a low mesh"});
            return;
        }
        addDisplay(s);
        const BakePaintSettings& st = bakePaintSettings();
        geo::BvhHit hit;
        const bool over = raycast(mHover, view, hit);
        QString under = "-";
        if (over)
        {
            const auto& tris = mPaint->paint->bvh().triangles();
            const geo::Mesh& m = mPaint->paint->mesh();
            float v = 0.0f;
            for (int k = 0; k < 3; ++k)
                v += float(hit.bary[k]) * mPaint->paint->values()[m.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + size_t(k)])];
            under = QString::number(v, 'f', 2);
        }
        ToolStatus status{.tool = "Paint Bake",
                          .subject = QString::fromStdString(g.node(mMesh)->name()),
                          .mode = kOps[int(opFor(mStroking ? mMods : QApplication::keyboardModifiers()))],
                          .fields = {{"map", st.attr == bake::kSkewAttr ? "skew" : "offset"},
                                     {"value", QString::number(st.value)},
                                     {"under cursor", under}},
                          .keys = {{"Shift", "smooth"}}};
        if (const bake::PaintOp op = bakePaintSettings().op; op == bake::PaintOp::Add || op == bake::PaintOp::Subtract)
            status.keys.push_back({"Ctrl", op == bake::PaintOp::Add ? "subtract" : "add"});
        // An editable cage replaces the auto cage the paint shapes, so painting alone changes nothing the bake uses.
        if (scene::Node* grp = groupOfLow(g, mMesh); grp && st.attr != bake::kSkewAttr)
            if (const std::string c = bake::groupSettings(*grp).cage; !c.empty())
            {
                status.fields.push_back({"cage", QString::fromStdString(c)});
                status.prompt = "Bake Into Cage to apply";
            }
        drawToolStatus(p, view, status);

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        std::optional<brush::ui::CursorSurface> surface;
        geo::BvhHit ringHit;
        if (raycast(at, view, ringHit))
        {
            const glm::dmat4 w = world();
            const glm::dvec3 c(w * glm::dvec4(ringHit.point, 1.0));
            const glm::dvec3 n = view.eye - c;
            surface = brush::ui::CursorSurface{c, glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0, 1, 0)};
        }
        brush::ui::drawCursor(p, view, st.stroke, at, surface,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .stroke = &mStroke});
    }

private:
    glm::dmat4 world() const
    {
        scene::Graph& g = scene::Graph::current();
        return bake::arrange::displayWorld(g, mMesh, g.node(mMesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    }

    scene::MeshPtr base() const
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        return std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, mMesh)));
    }

    void retarget()
    {
        scene::Graph& g = scene::Graph::current();
        const auto& sel = g.selection();
        if (mPicked != kNoNode && (!g.node(mPicked) || std::vector<NodeId>(sel.begin(), sel.end()) != mPickedSel))
            mPicked = kNoNode;
        NodeId mesh = mPicked;
        for (auto it = sel.rbegin(); it != sel.rend() && mesh == kNoNode; ++it)
        {
            scene::Node* n = g.node(*it);
            if (n && n->type().isA("mesh"))
                mesh = n->id();
            else if (n && bake::isGroup(*n))
            {
                const bake::GroupSettings s = bake::groupSettings(*n);
                if (!s.lows.empty())
                    if (scene::Node* low = g.find(s.lows.front()))
                        mesh = low->id();
            }
        }
        if (mesh == kNoNode && mMesh != kNoNode && g.node(mMesh))
            mesh = mMesh;
        if (mesh != mMesh)
        {
            mPaint.reset();
            if (mMesh != kNoNode)
                bakeCage::clearWorkingOffsets(mMesh);
        }
        mMesh = mesh;
        // A stale working copy (undo, another edit) is rebuilt from the scene on the next draw.
        if (mPaint && base() != mPaint->meshPtr)
            mPaint.reset();
        if (mPanel)
        {
            scene::Node* grp = mMesh != kNoNode ? groupOfLow(g, mMesh) : nullptr;
            mPanel->setTarget(mMesh == kNoNode ? QString("Select a low mesh")
                                               : QString::fromStdString(g.node(mMesh)->path() + (grp ? "  (" + grp->name() + ")" : "  (in no bake group)")));
            const QSignalBlocker block(mPanel->live);
            mPanel->live->setChecked(bakeCage::livePreview());
        }
        scene::Node* painted = mMesh != kNoNode ? groupOfLow(g, mMesh) : nullptr;
        bakeCage::setPainting(painted ? painted->id() : kNoNode);
        uvTools::requestRedraw();
        viewportTools::requestRedraw();
    }

    struct Working
    {
        scene::MeshPtr meshPtr;
        std::unique_ptr<bake::PointPaint> paint;
    };

    bool ensurePaint()
    {
        if (mMesh == kNoNode || !scene::Graph::current().node(mMesh))
            return false;
        if (mPaint)
        {
            mPaint->paint->setSymmetry(bakePaintSettings().symmetry);
            mPaint->paint->setFalloff(paintFalloff(bakePaintSettings().stroke.falloff));
            return true;
        }
        scene::MeshPtr m = base();
        const auto attr = bake::paintAttr(bakePaintSettings().attr);
        if (!m || !attr || !m->numFaces())
            return false;
        mPaint = std::make_unique<Working>();
        mPaint->meshPtr = m;
        mPaint->paint = std::make_unique<bake::PointPaint>(m, *attr);
        mPaint->paint->setSymmetry(bakePaintSettings().symmetry);
        mPaint->paint->setFalloff(paintFalloff(bakePaintSettings().stroke.falloff));
        mShellFor = nullptr;
        return true;
    }

    bool raycast(QPointF px, const ViewInfo& view, geo::BvhHit& hit)
    {
        if (!mPaint)
            return false;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const glm::dmat4 inv = glm::inverse(world());
        return mPaint->paint->raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit);
    }

    /** The shared stroke's view of the low under a pixel, in its object space. */
    bool surfaceQuery(QPointF px, const ViewInfo& view, brush::SurfaceHit& out)
    {
        geo::BvhHit hit;
        if (!raycast(px, view, hit))
            return false;
        const glm::dmat4 w = world();
        const glm::dvec3 c(w * glm::dvec4(hit.point, 1.0));
        const double scale = std::max(1e-12, glm::length(glm::dvec3(w[0])));
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const auto& tris = mPaint->paint->bvh().triangles();
        const geo::Mesh& m = mPaint->paint->mesh();
        glm::dvec3 a = m.position(m.cornerPoint(tris.corners[size_t(hit.triangle) * 3]));
        glm::dvec3 b = m.position(m.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + 1]));
        glm::dvec3 e = m.position(m.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + 2]));
        const glm::dvec3 n = glm::cross(b - a, e - a);
        out.center = hit.point;
        out.normal = glm::length(n) > 1e-18 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
        out.viewDir = glm::normalize(glm::dvec3(glm::inverse(w) * glm::dvec4(d, 0.0)));
        out.right = brush::ui::screenRight(view, w);
        out.pixelSize = view.unitsPerPixel(c) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    void dab(const brush::Dab& d) override
    {
        LILA_ZONE();

        if (!mPaint)
            return;
        const brush::Falloff falloff = bakePaintSettings().stroke.resolveFalloff();
        std::vector<std::pair<geo::Index, float>> pts;
        for (brush::Dab copy : brush::radialDabs(d, bakePaintSettings().stroke.radial))
        {
            geo::Index tri = geo::kInvalid;
            if (brush::SurfaceWalk::snap(mPaint->paint->bvh(), copy, tri))
            {
                auto more = mWalk.gather(mPaint->paint->mesh(), mPaint->paint->bvh(), tri, copy, falloff);
                pts.insert(pts.end(), more.begin(), more.end());
            }
        }
        if (bakePaintSettings().stroke.radial.any())
            brush::mergeHits(pts);
        applyDab(pts, mMods, float(d.opacity * d.flow));
    }

    static bake::PaintOp opFor(Qt::KeyboardModifiers mods)
    {
        bake::PaintOp op = bakePaintSettings().op;
        if (mods & Qt::ShiftModifier)
            op = bake::PaintOp::Smooth;
        else if ((mods & Qt::ControlModifier) && op == bake::PaintOp::Add)
            op = bake::PaintOp::Subtract;
        else if ((mods & Qt::ControlModifier) && op == bake::PaintOp::Subtract)
            op = bake::PaintOp::Add;
        return op;
    }

    void applyDab(const std::vector<std::pair<geo::Index, float>>& pts, Qt::KeyboardModifiers mods, float amount = 1.0f)
    {
        mPaint->paint->apply(pts, opFor(mods), float(bakePaintSettings().value), float(bakePaintSettings().stroke.strength) * amount);
        mShellFor = nullptr;
        if (mPaint->paint->attr().name == std::string(bake::kOffsetAttr))
            bakeCage::setWorkingOffsets(mMesh, mPaint->paint->values(), true);
        viewportTools::requestRedraw();
        uvTools::requestRedraw();
    }

    // UV editor painting: the same brush, its footprint taken in uv0 under the cursor.

    double uvRadius(const uvTools::Canvas& c) const { return bakePaintSettings().stroke.radius / std::max(1e-9, c.pixelsPerUv); }

    /** UV dabs for the sampler's new samples, sized and weighted by their pressure like the viewport's. */
    void uvDabs(const uvTools::Canvas& c, Qt::KeyboardModifiers mods)
    {
        const brush::BrushSettings& s = bakePaintSettings().stroke;
        for (const brush::StrokeSample& sample : mUvSampler.take())
        {
            const double size = std::max(0.05, s.pressureSize.map(sample.pressure)) * sample.size;
            const float amount = float(s.pressureOpacity.map(sample.pressure) * sample.opacity * s.flow * s.pressureFlow.map(sample.pressure));
            applyDab(mPaint->paint->uvPoints(c.toUv(QPointF(sample.position.x, sample.position.y)), uvRadius(c) * size), mods, amount);
        }
    }

    bool mousePress(QMouseEvent* e, const uvTools::Canvas& c) override
    {
        if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier) || !ensurePaint() || mPaint->paint->uvLayout().empty())
            return false;
        mUvHover = e->position();
        mStroking = true;
        mUvSampler.begin(bakePaintSettings().stroke, mControls.input(e), bakePaintSettings().stroke.radius);
        uvDabs(c, e->modifiers());
        return true;
    }

    bool mouseMove(QMouseEvent* e, const uvTools::Canvas& c) override
    {
        mUvHover = e->position();
        if (!mStroking)
            return true;
        mUvSampler.move(mControls.input(e));
        uvDabs(c, e->modifiers());
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const uvTools::Canvas& c) override
    {
        if (mUvSampler.active())
        {
            mUvSampler.end();
            uvDabs(c, e->modifiers());
        }
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const uvTools::Canvas&) override { return mControls.keyPress(e); }

    void leave() override { mUvHover.reset(); }

    /** The painted values over the UV layout, through the same ramp, and the brush. */
    void draw(QPainter& p, const uvTools::Canvas& c) override
    {
        LILA_ZONE();

        if (!ensurePaint())
            return;
        const bake::UvLayout& uv = mPaint->paint->uvLayout();
        const geo::Mesh& m = mPaint->paint->mesh();
        std::span<const float> values = mPaint->paint->values();
        float top = 0.0f;
        for (float v : values)
            top = std::max(top, v);
        const auto& tris = uv.triangles();
        p.save();
        p.setPen(Qt::NoPen);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (geo::Index t = 0; t < tris.triangleCount(); ++t)
        {
            QPointF poly[3];
            float v = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const geo::Index corner = tris.corners[size_t(t) * 3 + size_t(k)];
                poly[k] = c.toScreen(uv.uvs()[corner]);
                v += values[m.cornerPoint(corner)] / 3.0f;
            }
            // The ramp is linear; QColor wants display values.
            const glm::vec4 col = valueColor(mPaint->paint->attr(), v, top);
            auto enc = [](float x) { return int(std::clamp(std::pow(std::clamp(x, 0.0f, 1.0f), 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f); };
            p.setBrush(QColor(enc(col.r), enc(col.g), enc(col.b), 150));
            p.drawPolygon(poly, 3);
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        if (mUvHover)
        {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(240, 240, 240), 1.5));
            p.drawEllipse(*mUvHover, bakePaintSettings().stroke.radius, bakePaintSettings().stroke.radius);
        }
        p.restore();
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        commit("Bake Paint Stroke");
        uvTools::requestRedraw();
    }

    void flood()
    {
        if (!ensurePaint())
            return;
        mPaint->paint->flood(bakePaintSettings().op, float(bakePaintSettings().value), float(bakePaintSettings().stroke.strength));
        commit("Bake Paint Flood");
    }

    void commit(const std::string& label)
    {
        scene::Graph& g = scene::Graph::current();
        if (!mPaint || !g.node(mMesh))
            return;
        const std::vector<float> values(mPaint->paint->values().begin(), mPaint->paint->values().end());
        bakeCage::clearWorkingOffsets(mMesh);
        if (!bake::setPointValues(g, mMesh, mPaint->paint->attr(), values, label))
            log::warn("Paint bake cage: couldn't write {} on {}", mPaint->paint->attr().name, g.node(mMesh)->path());
        // The scene now holds the painted mesh: rebase on it so the next stroke continues from there.
        mPaint.reset();
        ensurePaint();
        viewportTools::requestRedraw();
    }

    /** The low through the value ramp by the painted value (the cage shell is the cage view's). */
    void addDisplay(render::FrameState& s)
    {
        LILA_ZONE();

        const geo::Mesh& m = mPaint->paint->mesh();
        const glm::dmat4 w = world();
        if (mShellFor != mPaint.get() || w != mDrawnWorld)
        {
            mShellFor = mPaint.get();
            mDrawnWorld = w;
            const bake::PaintAttr& attr = mPaint->paint->attr();
            std::span<const float> values = mPaint->paint->values();
            const auto& tris = mPaint->paint->bvh().triangles();
            mTris.resize(tris.corners.size());
            float top = 0.0f;
            for (float v : values)
                top = std::max(top, v);
            auto shade = [&](float v) {
                glm::vec4 c = valueColor(attr, v, top);
                c.a = 0.85f;
                return c;
            };
            tbb::parallel_for(size_t(0), tris.corners.size() / 3, [&](size_t t) {
                for (size_t k = 0; k < 3; ++k)
                {
                    const geo::Index p = m.cornerPoint(tris.corners[t * 3 + k]);
                    mTris[t * 3 + k] = {glm::vec3(glm::dvec3(w * glm::dvec4(m.position(p), 1.0))), shade(values[p])};
                }
            });
        }
        s.tris3d.insert(s.tris3d.end(), mTris.begin(), mTris.end());
    }

    std::shared_ptr<bool> mAlive;
    QPointer<BakePaintPanel> mPanel;
    int mListener = 0;
    bool mRefreshQueued = false;

    NodeId mMesh = kNoNode;
    /** A low picked under the brush, kept over the selection's until the selection changes. */
    NodeId mPicked = kNoNode;
    std::vector<NodeId> mPickedSel;
    std::unique_ptr<Working> mPaint;
    bool mStroking = false;
    QPointF mHover;
    Qt::KeyboardModifiers mMods;
    brush::Stroke mStroke;
    brush::StrokeSampler mUvSampler;
    brush::SurfaceWalk mWalk;
    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return bakePaintSettings().stroke; },
                                       [this] {
                                           if (mPanel)
                                               mPanel->syncFromSettings();
                                           viewportTools::requestRedraw();
                                           uvTools::requestRedraw();
                                       },
                                       [] { saveBakePaintSettings(); }};

    const Working* mShellFor = nullptr;
    glm::dmat4 mDrawnWorld{0.0};
    std::vector<render::LineVertex> mTris;
    std::optional<QPointF> mUvHover;
};
} // namespace

void startBakePaint() { viewportTools::set(std::make_unique<BakePaintTool>()); }

void registerBakePaintUi()
{
    bakeCage::registerBakeCageView();
    actions::add({.id = "bake.paintCage",
                  .label = "Paint Bake Cage",
                  .category = "Bake",
                  .run = [] { startBakePaint(); },
                  .doc = "Paint a low's bake cage: bakeOffset scales how far the auto cage pushes out per point, bakeSkew tilts rays "
                         "toward the face normal. Add, subtract, smooth, set and flood, with symmetry; each stroke is one undo step.",
                  .python = "lila.ui.run_action(\"bake.paintCage\")"});
    actions::add({.id = "bake.createCage",
                  .label = "Create Editable Cage",
                  .category = "Bake",
                  .doc = "Duplicates the selected group's low pushed out by its auto cage, as a cage mesh the group then uses.",
                  .command = "bakeCage",
                  .call = {{"create", true}}});
    actions::add({.id = "bake.cageFromPaint",
                  .label = "Bake Paint Into Cage",
                  .category = "Bake",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          // A group without an editable cage gets one; one that has it moves to the painted auto cage.
                          bool hasCage = false;
                          for (NodeId id : g.selection())
                              if (scene::Node* n = g.node(id))
                              {
                                  scene::Node* grp = bake::isGroup(*n) ? n : n->type().isA("mesh") ? groupOfLow(g, id) : nullptr;
                                  hasCage = hasCage || (grp && !bake::groupSettings(*grp).cage.empty());
                              }
                          actions::run({{"command", "bakeCage"}, {"args", hasCage ? json{{"fromPaint", true}} : json{{"create", true}}}});
                      },
                  .doc = "Writes the painted auto cage into the selected group's editable cage, creating the cage when it has none.",
                  .python = "bakeCage(fromPaint=True)"});
    actions::add({.id = "bake.resetCage",
                  .label = "Reset Cage to Auto",
                  .category = "Bake",
                  .doc = "The selected group goes back to the auto cage; its cage mesh stays in the scene.",
                  .command = "bakeCage",
                  .call = {{"reset", true}}});
    actions::add({.id = "bake.arrange",
                  .label = "Arrange Bake Groups",
                  .category = "Bake",
                  .doc = "Toggles drawing the project's groups side by side; scene meshes never move and bakes run where they are.",
                  .command = "bakeArrange",
                  .call = json::object()});
}
} // namespace lila::ui
