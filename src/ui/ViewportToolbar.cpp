#include "ui/ViewportToolbar.h"
#include "ui/ViewportHeader.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Components.h"
#include "scene/RenderSettings.h"
#include "ui/Actions.h"
#include "ui/DisplayAids.h"
#include "ui/DroneCamera.h"
#include "ui/Environments.h"
#include "ui/Icons.h"
#include "ui/ImagePlaneUi.h"
#include "ui/MaterialDraw.h"
#include "ui/PaintSelect.h"
#include "ui/RetopoDrawTool.h"
#include "ui/SpinScrub.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportWidget.h"
#include "ui/FileDialog.h"

#include <QActionGroup>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <utility>
#include <string>

namespace lila::ui
{
namespace
{
namespace vd = viewportDisplay;

constexpr const char* kViews[] = {"persp", "top", "front", "right", "left", "back", "bottom"};

QString labelOf(const std::string& id)
{
    const Action* a = actions::find(id);
    return QString::fromStdString(a ? a->label : id);
}

/** The select, transform and selection tools, and the component modes: which one is in use. */
std::optional<bool> toolActionState(const std::string& id)
{
    const bool viewportTool = viewportTools::active() != nullptr;
    for (Tool t : {Tool::Select, Tool::Move, Tool::Rotate, Tool::Scale})
        if (id == std::string("tool.") + tools::name(t))
            return !viewportTool && tools::current() == t;
    if (id == "select.marqueeTool" || id == "select.lassoTool")
        return !viewportTool && tools::current() == Tool::Select && generalPref(paintSelect::kLassoPref, false) == (id == "select.lassoTool");
    if (id == "select.paintTool")
        return viewportTools::activeId() == paintSelect::kToolId;
    using scene::ComponentMode;
    for (auto [mode, action] : {std::pair{ComponentMode::Object, "select.objectMode"}, {ComponentMode::Vertex, "select.vertexMode"},
                                {ComponentMode::Edge, "select.edgeMode"}, {ComponentMode::Face, "select.faceMode"}})
        if (id == action)
            return scene::components::mode() == mode;
    return std::nullopt;
}
} // namespace

std::optional<bool> viewportActionState(const std::string& id, const ViewportWidget* view)
{
    // Wireframe, Shaded and Textured are one choice, as in Maya.
    const bool wire = PrefStore::get().value<std::string>("general", "/viewport/displayMode", "shaded") == "wireframe";
    if (id == "viewport.wireframe")
        return wire;
    if (id == "viewport.shaded")
        return !wire && !materialsShown();
    if (id == "viewport.textured")
        return !wire && materialsShown();
    if (id == "rig.xrayJoints")
        return generalPref("/rig/xrayJoints", false);
    if (id == "retopo.toggleSnapSpace")
        return RetopoDrawTool::snapObjectSpace();
    if (std::optional<bool> tool = toolActionState(id))
        return tool;
    struct Toggle
    {
        const char* id;
        const char* key;
        bool fallback;
    };
    static const Toggle toggles[] = {
        {"viewport.wireOnShaded", "/viewport/wireOnShaded", false},
        {"viewport.toggleAO", "/viewport/ao/enabled", true},
        {"viewport.toggleAA", vd::kAaEnabled, true},
        {"viewport.toggleGrid", vd::kGrid, true},
        {"viewport.toggleXray", vd::kXray, false},
        {"viewport.toggleBackfaceCulling", vd::kBackfaceCulling, false},
        {"viewport.toggleKuwahara", vd::kKuwahara, false},
        {"viewport.toggleOutline", vd::kOutline, false},
        {"viewport.toggleShadows", vd::kShadows, true},
        {"viewport.defaultMaterial", vd::kDefaultMaterial, false},
        {"viewport.greyBaseColor", vd::kGreyBaseColor, false},
        {"viewport.smoothSubdivisions", vd::kSmoothSubdivisions, false},
        {"viewport.toggleEnvBackground", "/viewport/env/background", true},
        {"viewport.toggleToolbar", vd::kToolbar, true},
        {"viewport.grayscaleWeights", vd::kGrayscaleWeights, false},
        {"viewport.monotone", vd::kMonotone, false},
        {"viewport.toggleHighlightSelection", vd::kHighlightSelection, true},
        {"viewport.togglePreselectObjects", "/viewport/preselect/objects", false},
        {"viewport.togglePreselectComponents", "/viewport/preselect/components", true},
        {"window.toggleToolBox", "/ui/showToolBox", true},
        {"softSelect.toggle", "/softSelect/enabled", false},
        {"measure.toggleShow", "/measure/show", true},
        {"select.toggleCameraBased", "/select/cameraBased", false},
        {"select.toggleBackfaceCulling", "/select/backfaceCulling", false},
        {"select.toggleLasso", "/select/lasso", false},
        {"select.edgesByCenter", scene::componentPick::kEdgesByCenter, false},
        {"select.facesByCenter", scene::componentPick::kFacesByCenter, false},
    };
    for (const Toggle& t : toggles)
        if (id == t.id)
            return generalPref(t.key, t.fallback);
    if (id.starts_with("select.mask."))
        for (scene::selectionMask::Kind k : scene::selectionMask::kAll)
            if (id.substr(12) == scene::selectionMask::name(k))
                return scene::selectionMask::enabled(k);
    if (id.starts_with("viewport.show."))
        for (scene::selectionMask::Kind k : scene::selectionMask::kAll)
            if (id.substr(14) == scene::selectionMask::name(k))
                return scene::selectionMask::shown(k);
    if (id.starts_with("viewport.aaSamples"))
        return generalPref(vd::kAaEnabled, true) && !vd::temporalAa() &&
               PrefStore::get().value<int>("general", vd::kAaSamples, vd::kDefaultSamples) == std::stoi(id.substr(18));
    if (id == "viewport.aaTemporal")
        return generalPref(vd::kAaEnabled, true) && vd::temporalAa();
    if (id == "viewport.toggleVertexColors")
        return PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") != "off";
    if (id.starts_with("viewport.vertexColors."))
        return PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") == id.substr(22);
    if (id.starts_with("viewport.colorChannel."))
        return PrefStore::get().value<std::string>("general", vd::kColorChannel, "rgb") == id.substr(22);
    if (id.starts_with("viewport.lighting."))
        return PrefStore::get().value<std::string>("general", vd::kLighting, "environment") == id.substr(18);
    if (view && id.starts_with("viewport.aid."))
        return displayAids::actionState(id, view);
    if (view && id == "view.drone")
        return drone::active(view);
    if (const Action* a = actions::find(id); a && a->checked)
        return a->checked();
    if (!view)
        return std::nullopt;
    if (std::optional<bool> hudState = hud::actionState(id, view))
        return hudState;
    if (id == "viewport.isolate")
        return view->isolating();
    if (id == "viewport.toggleImagePlanes")
        return imagePlanes::shownIn(view->viewName());
    if (id.starts_with("viewport.view."))
        return view->viewName() == id.substr(14);
    if (id == "viewport.toggleResolutionGate" || id == "viewport.toggleFilmGate")
    {
        scene::CameraSettings c = scene::renderSettings().camera(view->viewName());
        return id == "viewport.toggleFilmGate" ? c.filmGate : c.resolutionGate;
    }
    return std::nullopt;
}

namespace
{
/** Intensity of the current HDRI as a slider (0..4) and a field (0..10), remembered per HDRI; Reset follows the global pref. */
QWidgetAction* environmentIntensityRow(QMenu* menu)
{
    // static: std::min takes it by reference, which needs a capture inside the lambdas unless it has static storage.
    static constexpr double kSliderMax = 4.0;
    constexpr int kSliderSteps = 400;
    auto* holder = new QWidget(menu);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);
    row->addWidget(new QLabel("Intensity", holder));
    auto* slider = new QSlider(Qt::Horizontal, holder);
    slider->setRange(0, kSliderSteps);
    slider->setMinimumWidth(120);
    auto* spin = new QDoubleSpinBox(holder);
    spin->setRange(0.0, 10.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    spin->setKeyboardTracking(false);
    spin->setFixedWidth(52);
    setSpinDefault(spin, 1.0);
    auto* reset = new QToolButton(holder);
    reset->setText("Reset");
    reset->setAutoRaise(true);
    reset->setToolTip("Use the default intensity from Preferences for this HDRI");
    row->addWidget(slider, 1);
    row->addWidget(spin);
    row->addWidget(reset);

    auto show = [slider, spin, reset] {
        double v = environments::intensity();
        QSignalBlocker a(slider), b(spin);
        slider->setValue(int(std::lround(std::min(v, kSliderMax) / kSliderMax * kSliderSteps)));
        spin->setValue(v);
        reset->setEnabled(environments::hasOwnIntensity());
    };
    show();
    QObject::connect(slider, &QSlider::valueChanged, holder, [show](int i) {
        environments::setIntensity(float(i * kSliderMax / kSliderSteps));
        show();
    });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, holder, [show](double v) {
        environments::setIntensity(float(v));
        show();
    });
    QObject::connect(reset, &QToolButton::clicked, holder, [show] {
        environments::setIntensity(std::nullopt);
        show();
    });
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(holder);
    return action;
}

/** How much of the HDRI's tint its light keeps on surfaces, 0..1; the backdrop keeps its own saturation. */
QWidgetAction* environmentLightColorRow(QMenu* menu)
{
    constexpr int kSliderSteps = 100;
    auto* holder = new QWidget(menu);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);
    auto* label = new QLabel("Light Color", holder);
    label->setToolTip("0 lights surfaces with the HDRI's brightness only (neutral gray), 1 with its full colour");
    row->addWidget(label);
    auto* slider = new QSlider(Qt::Horizontal, holder);
    slider->setRange(0, kSliderSteps);
    slider->setMinimumWidth(120);
    auto* spin = new QDoubleSpinBox(holder);
    spin->setRange(0.0, 1.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    spin->setKeyboardTracking(false);
    spin->setFixedWidth(52);
    setSpinDefault(spin, 1.0);
    row->addWidget(slider, 1);
    row->addWidget(spin);

    auto show = [slider, spin] {
        double v = std::clamp(double(PrefStore::get().value<float>("general", environments::kLightSaturation, 1.0f)), 0.0, 1.0);
        QSignalBlocker a(slider), b(spin);
        slider->setValue(int(std::lround(v * kSliderSteps)));
        spin->setValue(v);
    };
    show();
    QObject::connect(slider, &QSlider::valueChanged, holder, [show](int i) {
        PrefStore::get().set("general", environments::kLightSaturation, double(i) / kSliderSteps);
        show();
    });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, holder, [show](double v) {
        PrefStore::get().set("general", environments::kLightSaturation, v);
        show();
    });
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(holder);
    return action;
}
} // namespace

void fillCameraMenu(QMenu* menu, ViewportWidget* view, std::function<void()> activate)
{
    menu->clear();
    if (!view)
        return;
    QPointer<ViewportWidget> target(view);
    auto pick = [menu, activate](QAction* a, std::function<void()> fn) {
        QObject::connect(a, &QAction::triggered, menu, [activate, fn] {
            if (activate)
                activate();
            fn();
        });
    };
    for (const char* v : kViews)
    {
        std::string id = std::string("viewport.view.") + v;
        QAction* a = menu->addAction(labelOf(id));
        a->setCheckable(true);
        a->setChecked(view->viewName() == v && view->lookThroughCamera() == scene::kNoNode);
        pick(a, [id] { actions::run(id); });
    }
    scene::Graph& g = scene::Graph::current();
    bool first = true;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n->type().isA("camera"))
            continue;
        if (std::exchange(first, false))
            menu->addSeparator();
        QAction* a = menu->addAction(icon("camera"), QString::fromStdString(n->name()));
        a->setToolTip(QString::fromStdString(n->fullPath()));
        a->setCheckable(true);
        a->setChecked(view->lookThroughCamera() == id);
        pick(a, [target, id] {
            if (target)
                target->lookThrough(id);
        });
    }
    menu->addSeparator();
    for (const char* id : {"viewport.lookThroughSelected", "create.cameraFromView"})
    {
        QAction* a = menu->addAction(labelOf(id));
        a->setEnabled(actions::isEnabled(nlohmann::json(id)));
        pick(a, [id = std::string(id)] { actions::run(id); });
    }
}

void fillEnvironmentMenu(QMenu* menu)
{
    menu->clear();
    std::string resolved = environments::resolve(PrefStore::get().value<std::string>("general", "/viewport/env/path", ""));
    auto item = [&](const QString& label, const std::string& value, bool checked) {
        QAction* a = menu->addAction(label);
        a->setCheckable(true);
        a->setChecked(checked);
        QObject::connect(a, &QAction::triggered, menu, [value] { PrefStore::get().set("general", "/viewport/env/path", value); });
    };
    for (const std::filesystem::path& hdri : environments::builtIn())
    {
        std::u8string name = hdri.filename().u8string();
        std::string value(name.begin(), name.end());
        item(QString::fromStdString(environments::displayName(hdri)), value, !resolved.empty() && environments::resolve(value) == resolved);
    }
    item("Procedural Studio", environments::kProcedural, resolved.empty());
    menu->addSeparator();
    QObject::connect(menu->addAction("Browse..."), &QAction::triggered, menu, [menu] {
        QString f = ui::fileDialog::getOpenFileName(menu->parentWidget(), "Choose Environment", QString(), "Radiance HDR (*.hdr)");
        if (!f.isEmpty())
            PrefStore::get().set("general", "/viewport/env/path", f.toStdString());
    });
    menu->addSeparator();
    menu->addAction(environmentIntensityRow(menu));
    menu->addAction(environmentLightColorRow(menu));
}

ViewportPanel::ViewportPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    mView = new ViewportWidget(this);
    mHeader = new ViewportHeader(mView, this);
    v->addWidget(mHeader);
    v->addWidget(mView, 1);
    imagePlanes::acceptDrops(mView);
    auto showHeader = [this] { mHeader->setVisible(generalPref(vd::kToolbar, true)); };
    showHeader();
    mPrefListener = PrefStore::get().onChanged([showHeader](const std::string& file) {
        if (file == "general")
            showHeader();
    });
}

ViewportPanel::~ViewportPanel()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}
} // namespace lila::ui
