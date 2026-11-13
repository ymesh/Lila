#include "ui/SculptPanel.h"

#include "brush/Alphas.h"
#include "brush/BrushUi.h"
#include "brush/Presets.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"
#include "vcolor/Paint.h"
#include "ui/ColorPopup.h"
#include "ui/Controls.h"
#include "ui/ReferenceOverlay.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DockSidebar.h"
#include "ui/ModelingToolkit.h"
#include "ui/FileDialog.h"
#include "ui/FixedRowLayout.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/LayoutsUi.h"
#include "ui/Outliner.h"
#include "ui/ModelingToolkit.h"
#include "ui/SculptTool.h"
#include "ui/SpinScrub.h"
#include "ui/UiCommon.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportDisplay.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QPointer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileSystemWatcher>
#include <QImageReader>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>

namespace lila::ui
{
namespace
{
/** The Material list's entry that asks for a matcap image instead of picking a material. */
constexpr const char* kMatcapBrowse = "matcap.browse";
struct BrushInfo
{
    sculpt::Brush brush;
    const char* icon;
    const char* label;
    const char* doc;
};

/** In the panel's order: shape brushes, then the mask, freeze and paint layers. */
constexpr std::array<BrushInfo, std::size_t(sculpt::Brush::Count)> kBrushes = {{
    {sculpt::Brush::Draw, "brush", "Draw", "Push the surface out along its normal; Ctrl digs in."},
    {sculpt::Brush::ClayStrips, "layers-subtract", "Clay Strips", "Build up flat square strips of clay; Ctrl carves them."},
    {sculpt::Brush::Clay, "circle-dot", "Clay", "Build up rounded layers of clay; Ctrl carves them."},
    {sculpt::Brush::Inflate, "balloon", "Inflate", "Swell each point along its own normal; Ctrl deflates."},
    {sculpt::Brush::Blob, "circles", "Blob", "Swell a rounded ball; Ctrl sucks one in."},
    {sculpt::Brush::Layer, "stack", "Layer", "Raise a plateau to one height however often you pass; Ctrl sinks one."},
    {sculpt::Brush::Grab, "hand-grab", "Grab", "Drag a patch of surface with the cursor; Ctrl pulls it out along its normal as you drag up, pushes it in as you drag down."},
    {sculpt::Brush::ElasticGrab, "hand-move", "Elastic Grab", "Grab with a soft, wide reach that keeps the form's volume."},
    {sculpt::Brush::SnakeHook, "route", "Snake Hook", "Pull the surface out after the cursor, thinning as it goes: horns, spikes, tendrils."},
    {sculpt::Brush::Smooth, "droplet", "Smooth", "Smooth away bumps and noise, keeping the form's volume. Shift gives this brush for one stroke. Ctrl flattens as it smooths, "
                                                    "and so does Shift while Smooth is picked."},
    {sculpt::Brush::Relax, "wave-sine", "Relax", "Even out the point spacing, sliding along the surface so the form stays."},
    {sculpt::Brush::Flatten, "ironing", "Flatten", "Press toward the average plane under the brush; Ctrl raises contrast."},
    {sculpt::Brush::Planar, "box-align-bottom", "Planar", "Flatten onto the plane where the stroke began, for hard planes."},
    {sculpt::Brush::Fill, "bucket", "Fill", "Raise dips up to the average plane; Ctrl scrapes."},
    {sculpt::Brush::Scrape, "shovel", "Scrape", "Shave off what sticks out of the average plane; Ctrl fills dips."},
    {sculpt::Brush::Pinch, "arrows-minimize", "Pinch", "Pull points toward the brush centre and press them in to sharpen creases; Ctrl spreads and lifts them."},
    {sculpt::Brush::Magnify, "arrows-maximize", "Magnify", "Push points away from the brush centre; Ctrl pinches."},
    {sculpt::Brush::Crease, "chisel", "Crease", "Cut a sharp groove; Ctrl raises a sharp ridge."},
    {sculpt::Brush::DamStandard, "slash", "Dam Standard", "Cut a narrow V groove that pinches its edges in, for wrinkles and seams; Ctrl raises a sharp ridge."},
    {sculpt::Brush::TrimDynamic, "cut", "Trim Dynamic", "Cut flat to a plane just under the brush, for hard planes; Ctrl fills up to it."},
    {sculpt::Brush::Polish, "sparkles", "Polish", "Smooth along the surface's normal only and ease onto its plane: crisp, flat forms."},
    {sculpt::Brush::Fold, "ripple", "Fold", "Raise a cloth fold along the stroke, pinched from the sides; Ctrl presses a crease in."},
    {sculpt::Brush::Nudge, "hand-finger", "Nudge", "Push the surface along the stroke."},
    {sculpt::Brush::Thumb, "hand-click", "Thumb", "Push along the stroke and press flat, like a thumb through clay."},
    {sculpt::Brush::Rotate, "rotate-clockwise", "Rotate", "Twist the patch under the brush: drag out, then circle around where you clicked."},
    {sculpt::Brush::Pose, "walk", "Pose", "Click on a part with the brush covering it and drag: it bends at the ring where the brush meets the rest."},
    {sculpt::Brush::Boundary, "border-outer", "Boundary", "Drag the open borders under the brush and the points just inside them."},
    {sculpt::Brush::Mask, "mask", "Mask", "Paint the mask, which shields points from every brush; Ctrl erases it, Shift blurs it."},
    {sculpt::Brush::Freeze, "snowflake", "Freeze", "Pin points so no brush moves them; Ctrl unfreezes."},
    {sculpt::Brush::Paint, "paint", "Paint", "Paint polypaint in the Polypaint color without moving anything; Shift blurs the color. The mask shields points."},
}};

/** Brush button ids past the built-ins pick a registered brush, and past those a preset. */
constexpr int kCustomBrushId = 1000;
constexpr int kPresetId = 2000;

/** A preset's button icon: its own icon, else its alpha, else its brush's. */
QIcon presetIcon(const brush::presets::Preset& p)
{
    if (!p.icon.empty())
        return icon(QString::fromStdString(p.icon));
    if (const std::string file = brush::alphas::resolve(p.settings.alphaPath); !file.empty())
    {
        QImageReader reader(QString::fromStdString(file));
        reader.setScaledSize(QSize(48, 48));
        if (QImage img = reader.read(); !img.isNull())
            return QIcon(QPixmap::fromImage(img));
    }
    if (auto b = sculpt::brushFromName(p.brush))
        for (const BrushInfo& info : kBrushes)
            if (info.brush == *b)
                return icon(info.icon);
    if (const sculpt::BrushDef* def = sculpt::brushRegistry().find(p.brush); def && !def->icon.empty())
        return icon(QString::fromStdString(def->icon));
    return icon("puzzle");
}

std::vector<std::string> droppedPresets(const QMimeData* mime)
{
    const QString ext = QString::fromUtf8(brush::presets::kExtension.data(), qsizetype(brush::presets::kExtension.size()));
    std::vector<std::string> out;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && url.toLocalFile().endsWith(ext, Qt::CaseInsensitive))
            out.push_back(url.toLocalFile().toStdString());
    return out;
}

QColor swatchColor(const glm::vec3& linear)
{
    const glm::vec4 s = vcolor::linearToSrgb(glm::vec4(glm::clamp(linear, 0.0f, 1.0f), 1.0f));
    return QColor::fromRgbF(s.r, s.g, s.b);
}

bool runActionWithOptions(const char* name, const json& call) { return actions::run({{"command", name}, {"args", ops::options::callWith(name, call)}}); }

QDockWidget* sculptDock(bool create)
{
    QMainWindow* mw = mainWindow();
    if (!mw)
        return nullptr;
    // Asked every frame by the viewport: remembered, and looked up among the window's own children (docks are), not its whole tree.
    static QPointer<QDockWidget> found;
    if (found)
        return found;
    if (auto* d = mw->findChild<QDockWidget*>(kSculptDock, Qt::FindDirectChildrenOnly))
        return found = d;
    if (!create)
        return nullptr;
    auto* dock = new QDockWidget("Sculpt", mw);
    dock->setObjectName(kSculptDock);
    found = dock;
    auto* scroll = new QScrollArea(dock);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(new SculptPanel(scroll));
    dock->setWidget(scroll);
    setDockDocs(dock, "sculpting");
    // A tool panel: the left column, tabbed with the toolkits.
    mw->addDockWidget(Qt::LeftDockWidgetArea, dock);
    if (auto* tools = mw->findChild<QDockWidget*>(kModelingToolkitDock); tools && mw->dockWidgetArea(tools) == Qt::LeftDockWidgetArea)
        mw->tabifyDockWidget(tools, dock);
    // Wide enough for five brush columns with their labels and the session rows; its own column opens at this width.
    dock->setProperty(kDefaultTabExtent, std::max(dock->sizeHint().width(), dock->fontMetrics().averageCharWidth() * 88));
    return dock;
}

/** Entering opened the panel, so leaving sculpt mode closes it again. */
bool gClosesOnExit = false;

bool dockOpen()
{
    QDockWidget* d = sculptDock(false);
    return d && d->isVisible();
}

void showDock()
{
    if (QDockWidget* d = sculptDock(true))
        revealDock(d);
}

std::vector<std::string> selectedMeshes()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> out;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n->path());
    return out;
}

void enterSculpt()
{
    const SculptSettings& st = sculptSettings();
    auto meshes = selectedMeshes();
    if (meshes.empty())
    {
        log::warn("Enter Sculpt: select a mesh");
        return;
    }
    const bool wasOpen = dockOpen();
    if (runActionWithOptions("sculptEnter", {{"args", {meshes.back()}}, {"levels", st.levels}}))
    {
        gClosesOnExit = !wasOpen;
        showDock();
        startSculptTool();
    }
}

void exitSculpt()
{
    if (!sculpt::active())
        return;
    const json out = [] {
        try
        {
            return ops::execute("sculptExit", json::object(), CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
            return json();
        }
    }();
    if (!out.is_object())
        return;
    if (sculptToolActive())
        viewportTools::set(nullptr);
    if (const int levels = out.value("levels", 0); levels > 0)
        viewportToast::showActive(QString("%1: level %2 of %3, levels kept").arg(QString::fromStdString(out.value("mesh", std::string())))
                                      .arg(out.value("level", 0)).arg(levels), 3000);
}

/** Asks what to do when a command left meshes' sculpt levels not fitting their topology: undo it, or delete the levels. */
void askAboutBrokenLevels(const std::vector<scene::NodeId>& ids, std::string_view command)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> paths;
    QStringList names;
    for (scene::NodeId id : ids)
        if (scene::Node* n = g.node(id))
        {
            paths.push_back(n->path());
            names << QString::fromStdString(n->name());
        }
    if (paths.empty())
        return;
    QMessageBox box(mainWindow());
    box.setWindowTitle("Sculpt Levels");
    box.setIcon(QMessageBox::Warning);
    box.setText(QString("%1 changed the topology of %2, so its sculpt levels no longer fit.")
                    .arg(niceName(std::string(command)), names.join(", ")));
    box.setInformativeText("Undo the edit to keep the levels, or delete them to keep the edit.");
    QPushButton* undo = box.addButton("Undo", QMessageBox::AcceptRole);
    box.addButton("Delete Levels", QMessageBox::DestructiveRole);
    box.setDefaultButton(undo);
    box.exec();
    if (box.clickedButton() == undo)
        ops::execute("undo", json::object(), CallSource::UI);
    else
        ops::execute("sculptDeleteLevels", {{"args", paths}}, CallSource::UI);
}

QToolButton* sculptActionButton(const std::string& id, const QString& iconName, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setAutoRaise(true);
    if (actions::find(id))
        b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    QObject::connect(b, &QToolButton::clicked, b, [id] { actions::run(json(id)); });
    return b;
}

controls::SliderField* sculptSlider(QWidget* parent, double lo, double hi, int decimals, const QString& suffix = {})
{
    auto* s = new controls::SliderField(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    if (!suffix.isEmpty())
        s->setSuffix(suffix);
    return s;
}

QFormLayout* sculptForm(QWidget* w)
{
    auto* form = new QFormLayout(w);
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    return form;
}

/** A middle click (no drag) on any segment picks segment `id`, the way a combo's default comes back. */
void setSegmentedDefault(controls::Segmented* seg, int id, const QString& defaultText)
{
    for (auto* b : seg->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        onMiddleClickReset(
            b,
            [seg, id] {
                seg->setCurrent(id);
                Q_EMIT seg->picked(id);
            },
            defaultText, seg);
}
} // namespace

SculptPanel::SculptPanel(QWidget* parent) : QWidget(parent)
{
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(4);
    constexpr const char* kSections = "sculptPanel";

    const SculptSettings defaults;

    // Session: enter / exit, levels and duplicates, always on top.
    auto* session = new QWidget(this);
    auto* sessionCol = new QVBoxLayout(session);
    sessionCol->setContentsMargins(0, 0, 0, 0);
    sessionCol->setSpacing(4);
    mStatus = new QLabel(session);
    mStatus->setObjectName("sculptStatus");
    sessionCol->addWidget(mStatus);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    mEnter = new QPushButton(icon("door-enter"), "Enter", session);
    mExit = new QPushButton(icon("door-exit"), "Exit", session);
    mEnter->setToolTip("Enter: sculpt the selected mesh, at its stored levels if it has any.");
    mExit->setToolTip("Exit: leave sculpt mode. The mesh stays at this level and keeps its levels.");
    buttons->addWidget(mEnter, 1);
    buttons->addWidget(mExit, 1);
    buttons->addWidget(controls::overflow({"sculpt.cancel", "sculpt.remesh", "sculpt.deleteLevels"}, session));
    sessionCol->addLayout(buttons);

    mLevelsBox = new QWidget(session);
    mLevelsBox->setObjectName("sculptLevels");
    {
        auto* levels = new QVBoxLayout(mLevelsBox);
        levels->setContentsMargins(0, 4, 0, 0);
        levels->setSpacing(2);
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        mLevelDown = sculptActionButton("sculpt.levelDown", "chevron-down", mLevelsBox);
        mLevelUp = sculptActionButton("sculpt.levelUp", "chevron-up", mLevelsBox);
        mLevelLabel = new QLabel(mLevelsBox);
        mLevelLabel->setMinimumWidth(QFontMetrics(mLevelLabel->font()).horizontalAdvance("Level 0 / 0  "));
        mLevelLabel->setAlignment(Qt::AlignCenter);
        mLevelLabel->setObjectName("sculptLevel");
        mAddLevel = controls::push("sculpt.addLevel", mLevelsBox, "Add Level");
        row->addWidget(mLevelDown);
        row->addWidget(mLevelLabel);
        row->addWidget(mLevelUp);
        row->addSpacing(6);
        row->addWidget(mAddLevel, 1);
        levels->addLayout(row);
        mReconstruct = controls::push("sculpt.reconstruct", mLevelsBox, "Reconstruct");
        mDeleteLower = controls::push("sculpt.deleteLower", mLevelsBox, "Delete Lower");
        mDeleteHigher = controls::push("sculpt.deleteHigher", mLevelsBox, "Delete Higher");
        levels->addWidget(controls::grid({mReconstruct, mDeleteLower, mDeleteHigher}, 3, mLevelsBox));
        mDuplicate = controls::push("sculpt.duplicate", mLevelsBox, "Duplicate");
        mDuplicateHide = controls::push("sculpt.duplicateHide", mLevelsBox, "Duplicate and Hide");
        levels->addSpacing(4);
        levels->addWidget(controls::grid({mDuplicate, mDuplicateHide}, 2, mLevelsBox));
    }
    sessionCol->addWidget(mLevelsBox);
    col->addWidget(session);

    // Brushes
    // Brushes
    mBrushBox = new QWidget(this);
    mBrushFlow = new FixedRowLayout(5, 2);
    mBrushFlow->setContentsMargins(0, 0, 0, 0);
    mBrushBox->setLayout(mBrushFlow);
    mBrushes = new QButtonGroup(this);
    mBrushes->setExclusive(true);
    for (const BrushInfo& b : kBrushes)
        mBrushes->addButton(brushButton(icon(b.icon), b.label, b.doc), int(b.brush));
    rebuildCustomBrushes();
    col->addWidget(controls::section("Brushes", mBrushBox, this, kSections));

    // Brush settings
    auto* settingsBox = new QWidget(this);
    auto* bform = sculptForm(settingsBox);
    mRadius = sculptSlider(settingsBox, 2.0, 2000.0, 0, " px");
    mRadius->setToolTip("Radius: brush radius on screen. Hold F or B and drag in the viewport to resize.");
    setSpinDefault(mRadius, defaults.stroke.radius);
    bform->addRow("Radius", mRadius);
    mStrength = sculptSlider(settingsBox, 0.0, 1.0, 2);
    mStrength->setSingleStep(0.05);
    mStrength->setToolTip("Strength: hold M (or Shift+F) and drag in the viewport, or hold B and drag with the right button, to change.");
    setSpinDefault(mStrength, defaults.stroke.strength);
    bform->addRow("Strength", mStrength);
    mFalloff = new QComboBox(settingsBox);
    for (const char* f : {"Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant"})
        mFalloff->addItem(f);
    setComboDefault(mFalloff, int(sculpt::falloffFromName(defaults.stroke.falloff).value_or(sculpt::Falloff::Smooth)));
    bform->addRow("Falloff", mFalloff);
    mSpacing = sculptSlider(settingsBox, 0.02, 2.0, 2, " r");
    mSpacing->setSingleStep(0.05);
    mSpacing->setToolTip("Spacing: distance between dabs along a stroke, in brush radii.");
    setSpinDefault(mSpacing, defaults.stroke.spacing);
    bform->addRow("Spacing", mSpacing);
    // Settings only one brush has, shown while it is picked.
    mKeepVolume = new QCheckBox("Keep Volume", settingsBox);
    mKeepVolume->setToolTip("Keep Volume: Smooth removes bumps but keeps the form. Off averages points, which also flattens it. "
                          "Ctrl, or Shift with Smooth picked, gives the other one for a stroke.");
    setCheckDefault(mKeepVolume, true);
    bform->addRow("", mKeepVolume);
    mPinchDepth = sculptSlider(settingsBox, 0.0, 4.0, 2);
    mPinchDepth->setSingleStep(0.1);
    mPinchDepth->setToolTip("Depth: how far Pinch presses the surface in as it draws it together. 0 pinches without pressing in.");
    setSpinDefault(mPinchDepth, 1.0);
    bform->addRow("Depth", mPinchDepth);
    mBrushForm = bform;
    connect(mKeepVolume, &QCheckBox::toggled, this, [](bool on) {
        sculptSettings().stroke.params["keepVolume"] = on ? 1.0 : 0.0;
        sculptSettingsChanged();
    });
    connect(mPinchDepth, &QDoubleSpinBox::valueChanged, this, [](double v) {
        sculptSettings().stroke.params["pinchDepth"] = v;
        sculptSettingsChanged();
    });
    mSymmetry = new controls::Segmented(settingsBox);
    // Segment ids are the mesh's symmetry values: off, x, y, z, radial.
    mSymmetry->add("Off", {}, "No symmetry", 0);
    mSymmetry->add("X", {}, "Mirror strokes across X", 1);
    mSymmetry->add("Y", {}, "Mirror strokes across Y", 2);
    mSymmetry->add("Z", {}, "Mirror strokes across Z", 3);
    mSymmetry->add("Radial", {}, "Repeat strokes round the mesh's radial axis", 4);
    mSymmetry->setToolTip("Symmetry: the mesh's symmetry setting. Strokes mirror exactly across it, or repeat exactly round its radial axis.");
    setSegmentedDefault(mSymmetry, 0, "Off");
    bform->addRow("Symmetry", mSymmetry);
    mFrontFaces = new QCheckBox("Front Faces Only", settingsBox);
    mFrontFaces->setToolTip("Front Faces Only: leave surfaces facing away from the view, so strokes don't reach through thin parts.");
    setCheckDefault(mFrontFaces, defaults.frontFacesOnly);
    bform->addRow("", mFrontFaces);
    auto* shareRow = new QHBoxLayout;
    const SculptSettings shareDefaults;
    mShareRadius = new QCheckBox("Radius", settingsBox);
    mShareStrength = new QCheckBox("Strength", settingsBox);
    mShareStroke = new QCheckBox("Stroke", settingsBox);
    mShareRadius->setToolTip("Share Radius: every brush uses the same radius; off, each brush keeps its own");
    mShareStrength->setToolTip("Share Strength: every brush uses the same strength; off, each brush keeps its own");
    mShareStroke->setToolTip("Share Stroke: every brush uses the same falloff, spacing, alpha, stabiliser and the rest of the Stroke group; off, "
                             "each brush keeps its own");
    setCheckDefault(mShareRadius, shareDefaults.shareRadius);
    setCheckDefault(mShareStrength, shareDefaults.shareStrength);
    setCheckDefault(mShareStroke, shareDefaults.shareStroke);
    shareRow->addWidget(mShareRadius);
    shareRow->addWidget(mShareStrength);
    shareRow->addWidget(mShareStroke);
    shareRow->addStretch(1);
    bform->addRow("Shared", shareRow);
    col->addWidget(controls::section("Brush", settingsBox, this, kSections));
    mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return sculptSettings().stroke; },
                                                  [] { sculptSettingsChanged(); });
    col->addWidget(mStrokeOptions);

    // Mask and freeze
    std::vector<QWidget*> maskButtons = {controls::push("sculpt.maskInvert", this, "Invert"), controls::push("sculpt.maskBlur", this, "Blur"),
                                         controls::push("sculpt.maskFill", this, "Fill"), controls::push("sculpt.maskClear", this, "Clear"),
                                         controls::push("sculpt.freezeClear", this, "Unfreeze All")};
    col->addWidget(controls::section("Mask and Freeze", controls::grid(maskButtons, 3, this), this, kSections));

    // Presets
    mPresetBox = new QWidget(this);
    auto* presetCol = new QVBoxLayout(mPresetBox);
    presetCol->setContentsMargins(0, 0, 0, 0);
    presetCol->setSpacing(4);
    mPresetFlow = new FlowLayout(2);
    presetCol->addLayout(mPresetFlow);
    mPresetEmpty = new QLabel("No presets. Click Save Preset, or drop .lilabrush files here.", mPresetBox);
    mPresetEmpty->setWordWrap(true);
    mPresetEmpty->setEnabled(false);
    presetCol->addWidget(mPresetEmpty);
    auto* presetRow = new QHBoxLayout;
    presetRow->setSpacing(2);
    auto* savePresetButton = new QToolButton(mPresetBox);
    savePresetButton->setText("Save Preset");
    savePresetButton->setToolTip("Save Preset: save the current brush and its settings, alpha included, as a preset");
    auto* importButton = new QToolButton(mPresetBox);
    importButton->setText("Import");
    importButton->setToolTip("Import Presets: install .lilabrush files (or drop them on this panel)");
    for (QToolButton* b : {savePresetButton, importButton})
    {
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        controls::setRole(b, "push");
        presetRow->addWidget(b);
    }
    presetCol->addLayout(presetRow);
    col->addWidget(controls::section("Presets", mPresetBox, this, kSections));
    connect(savePresetButton, &QToolButton::clicked, this, [this] { savePreset(); });
    connect(importButton, &QToolButton::clicked, this, [this] { importPresets(); });
    setAcceptDrops(true);
    mPresetWatcher = new QFileSystemWatcher(this);
    connect(mPresetWatcher, &QFileSystemWatcher::directoryChanged, this, [] { brush::presets::notifyChanged(); });
    rebuildPresets();

    // Polypaint
    auto* paintBox = new QWidget(this);
    auto* pform = sculptForm(paintBox);
    mColor = new QToolButton(paintBox);
    mColor->setMinimumWidth(60);
    mColor->setProperty("lilaRawColors", true);
    mColor->setToolTip("Polypaint Color: the color the Paint brush, and shape brushes with Paint While Sculpting, lay down");
    auto* colorRow = new QHBoxLayout;
    colorRow->setSpacing(2);
    colorRow->addWidget(mColor);
    pform->addRow("Color", colorRow);
    mPaint = new QCheckBox("Paint While Sculpting", paintBox);
    mPaint->setToolTip("Paint While Sculpting: shape brushes also paint the color wherever they move the surface");
    setCheckDefault(mPaint, SculptSettings{}.paint);
    pform->addRow("", mPaint);
    mCavityPaint = new QCheckBox("Only Where It Digs", paintBox);
    mCavityPaint->setToolTip("Only Where It Digs: Paint While Sculpting colors only where a stroke cuts in, darker the deeper, so Dam "
                             "Standard and Crease lines come out painted along their grooves");
    setCheckDefault(mCavityPaint, SculptSettings{}.cavityPaint);
    pform->addRow("", mCavityPaint);
    pform->addRow("", controls::grid({controls::push("sculpt.colorFill", paintBox, "Fill"), controls::push("sculpt.colorClear", paintBox, "Clear")}, 2,
                                     paintBox));
    col->addWidget(controls::section("Polypaint", paintBox, this, kSections));

    // Display
    auto* displayBox = new QWidget(this);
    auto* dform = sculptForm(displayBox);
    mMaterial = new QComboBox(displayBox);
    fillMaterials();
    mMaterial->setToolTip("Material: what the sculpt is drawn with while sculpting. Mesh Material uses the mesh's own. "
                          "Matcaps light the sculpt from a lit-sphere image; Matcap from File uses your own.");
    setComboDefault(mMaterial, std::max(0, mMaterial->findData(QString::fromStdString(SculptSettings{}.material))));
    dform->addRow("Material", mMaterial);
    mSculptLighting = new QCheckBox("Sculpt Lighting", displayBox);
    mSculptLighting->setToolTip("Sculpt Lighting: one key light at an angle to your view, so surface form reads. "
                                "Off goes back to the lighting you had. Shift+Alt+right-drag in the viewport moves the light.");
    dform->addRow("", mSculptLighting);
    mLightAngle = sculptSlider(displayBox, 0, 90, 0, QString(QChar(0x00B0)));
    mLightAngle->setToolTip("Light Angle: how far the key light sits off your view. 0 lights from the camera, 90 grazes.");
    setSpinDefault(mLightAngle, viewportDisplay::kDefaultRigTilt);
    dform->addRow("Light Angle", mLightAngle);
    connect(mSculptLighting, &QCheckBox::clicked, this, [] { actions::run(json("viewport.toggleSculptLighting")); });
    connect(mLightAngle, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", viewportDisplay::kRigTilt, v); });
    auto syncLighting = [this] {
        const PrefStore& p = PrefStore::get();
        const bool on = p.value<std::string>("general", viewportDisplay::kLighting, "environment") == "sculpt";
        QSignalBlocker a(mSculptLighting), b(mLightAngle);
        mSculptLighting->setChecked(on);
        mLightAngle->setValue(p.value<double>("general", viewportDisplay::kRigTilt, viewportDisplay::kDefaultRigTilt));
        mLightAngle->setEnabled(on);
    };
    syncLighting();
    mPrefListener = PrefStore::get().onChanged([syncLighting](const std::string& file) {
        if (file == "general")
            syncLighting();
    });
    mWireframe = new QCheckBox("Wireframe", displayBox);
    mWireframe->setToolTip("Wireframe: the sculpt mesh's wireframe (Shift+F in the viewport)");
    setCheckDefault(mWireframe, SculptSettings{}.wireframe);
    dform->addRow("", mWireframe);
    mWireOpacity = sculptSlider(displayBox, 5, 100, 0, " %");
    mWireOpacity->setToolTip("Wire Opacity: how strongly the wireframe draws over the sculpt");
    setSpinDefault(mWireOpacity, SculptSettings{}.wireOpacity * 100.0);
    dform->addRow("Wire Opacity", mWireOpacity);
    mSeeThrough = new QCheckBox("See-Through", displayBox);
    mSeeThrough->setToolTip("See-Through: image planes behind the sculpt show through (Alt+X in the viewport)");
    setCheckDefault(mSeeThrough, SculptSettings{}.seeThrough);
    dform->addRow("", mSeeThrough);
    mOpacity = sculptSlider(displayBox, 0, 100, 0, " %");
    mOpacity->setToolTip("Opacity: the sculpt surface's opacity while see-through");
    setSpinDefault(mOpacity, SculptSettings{}.seeThroughOpacity * 100.0);
    dform->addRow("Opacity", mOpacity);
    mSymmetryLine = new QCheckBox("Symmetry Line", displayBox);
    mSymmetryLine->setToolTip("Symmetry Line: the red line along the mesh's mirror plane.");
    setCheckDefault(mSymmetryLine, defaults.symmetryLine);
    dform->addRow("", mSymmetryLine);
    col->addWidget(controls::section("Display", displayBox, this, kSections));

    col->addWidget(referenceOverlays::createSculptSection(this));
    col->addStretch(1);

    syncFromSettings();
    refreshSession();

    auto changed = [] { sculptSettingsChanged(); };
    connect(mBrushes, &QButtonGroup::idClicked, this, [this](int id) {
        if (id >= kPresetId && size_t(id - kPresetId) < mPresets.size())
        {
            if (!selectSculptPreset(mPresets[size_t(id - kPresetId)]))
                syncFromSettings();
        }
        else if (id >= kCustomBrushId && size_t(id - kCustomBrushId) < mCustomBrushes.size())
            selectSculptBrush(sculpt::Brush::Custom, mCustomBrushes[size_t(id - kCustomBrushId)]);
        else
            selectSculptBrush(sculpt::Brush(id));
        // Picking a brush means sculpting, so it starts the session too.
        if (sculpt::active())
            startSculptTool();
        else if (!selectedMeshes().empty())
            enterSculpt();
        else
            viewportToast::show("Select a mesh to sculpt");
    });
    connect(mRadius, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().stroke.radius = v; changed(); });
    connect(mStrength, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().stroke.strength = v; changed(); });
    connect(mSpacing, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().stroke.spacing = v; changed(); });
    connect(mFalloff, &QComboBox::currentIndexChanged, this, [changed](int i) {
        sculptSettings().stroke.falloff = std::string(sculpt::falloffName(sculpt::Falloff(i)));
        changed();
    });
    connect(mFrontFaces, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().frontFacesOnly = on; changed(); });
    connect(mPaint, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().paint = on; changed(); });
    connect(mCavityPaint, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().cavityPaint = on; changed(); });
    connect(mShareRadius, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().shareRadius = on; changed(); });
    connect(mShareStrength, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().shareStrength = on; changed(); });
    connect(mShareStroke, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().shareStroke = on; changed(); });
    connect(mWireframe, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().wireframe = on; changed(); });
    connect(mWireOpacity, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().wireOpacity = v / 100.0; changed(); });
    connect(mSeeThrough, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().seeThrough = on; changed(); });
    connect(mOpacity, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().seeThroughOpacity = v / 100.0; changed(); });
    connect(mMaterial, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        if (mMaterial->currentData().toString() == kMatcapBrowse)
        {
            const QString file = fileDialog::getOpenFileName(this, "Choose Matcap", QString(), "Images (*.png *.jpg *.jpeg *.tga *.exr)");
            if (!file.isEmpty())
            {
                PrefStore::get().set("general", kMatcapFilePref, file.toStdString());
                sculptSettings().material = kMatcapFileId;
                changed();
            }
            QTimer::singleShot(0, this, [this] {
                fillMaterials();
                syncFromSettings();
            });
            return;
        }
        sculptSettings().material = mMaterial->currentData().toString().toStdString();
        changed();
    });
    connect(mColor, &QToolButton::clicked, this, [this, changed] {
        auto apply = [changed](const QColor& c) {
            sculptSettings().color = glm::vec3(vcolor::srgbToLinear(glm::vec4(float(c.redF()), float(c.greenF()), float(c.blueF()), 1.0f)));
            changed();
        };
        if (QColor c = colorPopup::getColor(swatchColor(sculptSettings().color), mColor, "Polypaint Color", false, apply); c.isValid())
            apply(c);
    });
    colorRow->addWidget(colorPopup::eyedropperButton(paintBox, [changed](const glm::dvec3& c) {
        sculptSettings().color = glm::vec3(c);
        changed();
    }));
    colorRow->addStretch(1);
    connect(mSymmetryLine, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().symmetryLine = on; changed(); });
    connect(mSymmetry, &controls::Segmented::picked, this, [](int id) {
        static constexpr std::array<const char*, 5> kAxes = {"off", "x", "y", "z", "radial"};
        std::vector<std::string> targets;
        if (auto s = sculpt::active(); s && scene::Graph::current().node(s->node))
            targets.push_back(scene::Graph::current().node(s->node)->path());
        else
            targets = selectedMeshes();
        if (!targets.empty())
            runActionWithOptions("setSymmetry", {{"args", targets}, {"axis", kAxes[size_t(std::clamp(id, 0, 4))]}});
    });
    connect(mEnter, &QPushButton::clicked, this, [] { actions::run(json("sculpt.enter")); });
    connect(mExit, &QPushButton::clicked, this, [] { actions::run(json("sculpt.exit")); });

    mSettingsListener = addSculptSettingsListener([this] { syncFromSettings(); });
    mBrushRegistryListener = sculpt::brushRegistry().addListener([this](RegistryChange, const std::string&, const std::string&) {
        QTimer::singleShot(0, this, [this] {
            rebuildCustomBrushes();
            rebuildPresets();
            syncFromSettings();
        });
    });
    mPresetListener = brush::presets::addListener([this] {
        if (mPresetsQueued)
            return;
        mPresetsQueued = true;
        QTimer::singleShot(0, this, [this] {
            mPresetsQueued = false;
            rebuildPresets();
            syncFromSettings();
        });
    });
    auto queue = [this] {
        if (mRefreshQueued)
            return;
        mRefreshQueued = true;
        QTimer::singleShot(0, this, [this] {
            mRefreshQueued = false;
            refreshSession();
        });
    };
    mSessionListener = sculpt::addListener(queue);
    mGraphListener = scene::Graph::current().addListener([queue](scene::Change c, scene::NodeId) {
        if (c == scene::Change::Value || c == scene::Change::Selection || c == scene::Change::Deleted || c == scene::Change::Cleared ||
            c == scene::Change::Reset)
            queue();
    });
}

SculptPanel::~SculptPanel()
{
    PrefStore::get().removeOnChanged(mPrefListener);
    sculpt::brushRegistry().removeListener(mBrushRegistryListener);
    brush::presets::removeListener(mPresetListener);
    removeSculptSettingsListener(mSettingsListener);
    sculpt::removeListener(mSessionListener);
    scene::Graph::current().removeListener(mGraphListener);
}

QToolButton* SculptPanel::brushButton(const QIcon& ic, const QString& label, const QString& doc, QLayout* flow)
{
    flow = flow ? flow : mBrushFlow;
    auto* btn = new QToolButton(flow == mBrushFlow ? mBrushBox : mPresetBox);
    btn->setIcon(ic);
    btn->setText(label);
    btn->setCheckable(true);
    btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    btn->setIconSize(QSize(24, 24));
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setFocusPolicy(Qt::NoFocus);
    controls::setRole(btn, "tile");
    if (flow != mBrushFlow)
        btn->setMinimumWidth(66);
    btn->setToolTip(doc.isEmpty() ? label : QString("%1\n%2").arg(label, doc));
    flow->addWidget(btn);
    return btn;
}

void SculptPanel::rebuildCustomBrushes()
{
    for (size_t k = 0; k < mCustomBrushes.size(); ++k)
        if (QAbstractButton* b = mBrushes->button(kCustomBrushId + int(k)))
        {
            mBrushes->removeButton(b);
            delete b;
        }
    mCustomBrushes = sculpt::brushRegistry().idsInOrder();
    for (size_t k = 0; k < mCustomBrushes.size(); ++k)
    {
        const sculpt::BrushDef& def = *sculpt::brushRegistry().find(mCustomBrushes[k]);
        const QString label = QString::fromStdString(def.label.empty() ? mCustomBrushes[k] : def.label);
        mBrushes->addButton(brushButton(icon(QString::fromStdString(def.icon.empty() ? "puzzle" : def.icon)), label, QString::fromStdString(def.doc)),
                            kCustomBrushId + int(k));
    }
}

void SculptPanel::rebuildPresets()
{
    for (size_t k = 0; k < mPresets.size(); ++k)
        if (QAbstractButton* b = mBrushes->button(kPresetId + int(k)))
        {
            mBrushes->removeButton(b);
            delete b;
        }
    mPresets.clear();
    for (const brush::presets::Preset& p : brush::presets::library("sculpt"))
    {
        QString doc = QString::fromStdString(p.doc);
        const QString where = QString::fromStdString(p.sourceLabel + (p.category.empty() ? "" : " / " + p.category));
        doc += doc.isEmpty() ? where : "\n" + where;
        QToolButton* btn = brushButton(presetIcon(p), QString::fromStdString(p.label), doc, mPresetFlow);
        const size_t index = mPresets.size();
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QToolButton::customContextMenuRequested, this, [this, btn, index](const QPoint& at) { presetMenu(index, btn->mapToGlobal(at)); });
        mBrushes->addButton(btn, kPresetId + int(index));
        mPresets.push_back(p.id);
    }
    mPresetEmpty->setVisible(mPresets.empty());

    if (!mPresetWatcher->directories().isEmpty())
        mPresetWatcher->removePaths(mPresetWatcher->directories());
    std::error_code ec;
    for (const brush::presets::Folder& f : brush::presets::folders())
        if (std::filesystem::is_directory(f.dir, ec))
            mPresetWatcher->addPath(QString::fromStdWString(f.dir.wstring()));
}

void SculptPanel::savePreset()
{
    SculptSettings& s = sculptSettings();
    std::optional<brush::presets::Preset> current = s.preset.empty() ? std::nullopt : brush::presets::find(s.preset);
    const bool own = current && (current->source == "user" || current->source == "project");
    const std::string brushName = s.brush == sculpt::Brush::Custom ? s.custom : std::string(sculpt::brushName(s.brush));
    QString suggested = current ? QString::fromStdString(current->label)
                        : s.brush == sculpt::Brush::Custom ? QString::fromStdString(s.custom)
                                                           : QString(std::ranges::find(kBrushes, s.brush, &BrushInfo::brush)->label);
    bool ok = false;
    const QString label = QInputDialog::getText(this, "Save Brush Preset", "Name", QLineEdit::Normal, suggested, &ok).trimmed();
    if (!ok || label.isEmpty())
        return;
    // Saving under the open preset's own name updates it in place; any other name makes a new one.
    const bool replace = own && label.toStdString() == current->label;
    json call = {{"args", {label.toStdString()}},
                 {"brush", brushName},
                 {"settings", s.stroke.toJson()},
                 {"alpha", s.stroke.alphaPath},
                 {"scope", replace ? current->source : std::string("user")},
                 {"replace", replace}};
    json result;
    try
    {
        result = ops::execute("brushPresetSave", call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        return;
    }
    const std::string id = result.value("id", "");
    if (id.empty())
        return;
    if (replace)
        s.perBrush.erase("preset:" + id);
    rebuildPresets();
    selectSculptPreset(id);
}

void SculptPanel::importPresets()
{
    const fileDialog::ContextScope scope("brushPresets", "Brush Presets");
    const QStringList files = fileDialog::getOpenFileNames(this, "Import Brush Presets", {}, "Brush presets (*.lilabrush)");
    if (files.isEmpty())
        return;
    json args = json::array();
    for (const QString& f : files)
        args.push_back(f.toStdString());
    runActionWithOptions("brushPresetInstall", {{"args", args}});
}

void SculptPanel::presetMenu(size_t index, const QPoint& globalPos)
{
    if (index >= mPresets.size())
        return;
    std::optional<brush::presets::Preset> p = brush::presets::find(mPresets[index]);
    if (!p)
        return;
    QMenu menu(this);
    menu.addAction(icon("file-export"), "Export...", this, [this, p] {
        const fileDialog::ContextScope scope("brushPresets", "Brush Presets");
        const QString file = fileDialog::getSaveFileName(this, "Export Brush Preset", QString::fromStdString(p->label + ".lilabrush"),
                                                         "Brush presets (*.lilabrush)");
        if (!file.isEmpty())
            runActionWithOptions("brushPresetExport", {{"args", {p->id}}, {"file", file.toStdString()}});
    });
    menu.addAction(icon("folder-open"), "Show in Folder", this, [p] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(p->file.parent_path().wstring())));
    });
    if (p->source == "user" || p->source == "project")
    {
        menu.addSeparator();
        menu.addAction(icon("trash"), "Delete", this, [p] {
            if (!runActionWithOptions("brushPresetDelete", {{"args", {p->id}}}))
                return;
            SculptSettings& s = sculptSettings();
            s.perBrush.erase("preset:" + p->id);
            if (s.preset == p->id)
                selectSculptBrush(s.brush, s.custom);
        });
    }
    menu.exec(globalPos);
}

void SculptPanel::dragEnterEvent(QDragEnterEvent* e)
{
    if (droppedPresets(e->mimeData()).empty())
        return e->ignore();
    e->acceptProposedAction();
}

void SculptPanel::dropEvent(QDropEvent* e)
{
    std::vector<std::string> files = droppedPresets(e->mimeData());
    if (files.empty())
        return e->ignore();
    e->acceptProposedAction();
    // After the drop returns, so Explorer's drag loop isn't held.
    QTimer::singleShot(0, this, [files] { runActionWithOptions("brushPresetInstall", {{"args", files}}); });
}

void SculptPanel::syncFromSettings()
{
    const SculptSettings& s = sculptSettings();
    QSignalBlocker b1(mRadius), b2(mStrength), b3(mSpacing), b4(mFalloff), b5(mFrontFaces), b20(mSymmetryLine), b12(mPaint), b13(mShareRadius), b14(mShareStrength), b15(mShareStroke),
        b16(mWireframe), b17(mMaterial), b18(mWireOpacity), b22(mSeeThrough), b23(mOpacity), b24(mKeepVolume), b25(mPinchDepth);
    mKeepVolume->setChecked(s.stroke.param("keepVolume", 1.0) != 0.0);
    mPinchDepth->setValue(s.stroke.param("pinchDepth", 1.0));
    mBrushForm->setRowVisible(mKeepVolume, s.brush == sculpt::Brush::Smooth);
    mBrushForm->setRowVisible(mPinchDepth, s.brush == sculpt::Brush::Pinch);
    mShareRadius->setChecked(s.shareRadius);
    mShareStrength->setChecked(s.shareStrength);
    mShareStroke->setChecked(s.shareStroke);
    mWireframe->setChecked(s.wireframe);
    mWireOpacity->setValue(std::round(s.wireOpacity * 100.0));
    mWireOpacity->setEnabled(s.wireframe);
    mSeeThrough->setChecked(s.seeThrough);
    mOpacity->setValue(std::round(s.seeThroughOpacity * 100.0));
    mOpacity->setEnabled(s.seeThrough);
    mMaterial->setCurrentIndex(std::max(0, mMaterial->findData(QString::fromStdString(s.material))));
    int checked = int(s.brush);
    if (auto preset = std::ranges::find(mPresets, s.preset); !s.preset.empty() && preset != mPresets.end())
        checked = kPresetId + int(preset - mPresets.begin());
    else if (s.brush == sculpt::Brush::Custom)
    {
        auto it = std::ranges::find(mCustomBrushes, s.custom);
        checked = it == mCustomBrushes.end() ? -1 : kCustomBrushId + int(it - mCustomBrushes.begin());
    }
    if (QAbstractButton* b = mBrushes->button(checked))
        b->setChecked(true);
    else if (QAbstractButton* on = mBrushes->checkedButton())
    {
        mBrushes->setExclusive(false);
        on->setChecked(false);
        mBrushes->setExclusive(true);
    }
    mPaint->setChecked(s.paint);
    mCavityPaint->setChecked(s.cavityPaint);
    mCavityPaint->setEnabled(s.paint);
    mColor->setStyleSheet(QString("QToolButton { background: %1; border: none; border-radius: 3px; min-height: 18px; }").arg(swatchColor(s.color).name()));
    mRadius->setValue(s.stroke.radius);
    mStrength->setValue(s.stroke.strength);
    mSpacing->setValue(s.stroke.spacing);
    mFalloff->setCurrentIndex(int(sculpt::falloffFromName(s.stroke.falloff).value_or(sculpt::Falloff::Smooth)));
    if (mStrokeOptions)
        mStrokeOptions->sync();
    mFrontFaces->setChecked(s.frontFacesOnly);
    mSymmetryLine->setChecked(s.symmetryLine);
    refreshSession();
}

void SculptPanel::refreshSession()
{
    scene::Graph& g = scene::Graph::current();
    auto s = sculpt::active();
    scene::Node* n = s ? g.node(s->node) : nullptr;
    if (n)
        mStatus->setText(QString("%1  ·  %L2 faces").arg(QString::fromStdString(n->name())).arg(s->sculpt.mesh().numFaces()));
    else
    {
        auto meshes = selectedMeshes();
        scene::Node* m = meshes.empty() ? nullptr : g.find(meshes.back());
        const std::optional<sculpt::StoredLevels> stored = m ? sculpt::storedLevels(*m) : std::nullopt;
        if (!m)
            mStatus->setText("No mesh selected");
        else if (stored && stored->fits)
            mStatus->setText(QString("%1  ·  level %2 of %3  ·  not sculpting").arg(QString::fromStdString(m->name())).arg(stored->level).arg(stored->levels));
        else
            mStatus->setText(QString("%1  ·  not sculpting").arg(QString::fromStdString(m->name())));
    }
    mLevelsBox->setVisible(n != nullptr);
    if (n)
    {
        const int top = s->stack->top();
        mLevelLabel->setText(QString("Level %1 / %2").arg(s->level).arg(top));
        mLevelDown->setEnabled(s->level > 0);
        mLevelUp->setEnabled(s->level < top);
        mAddLevel->setEnabled(top < sculpt::kMaxLevels);
        mReconstruct->setEnabled(top < sculpt::kMaxLevels);
        mDeleteLower->setEnabled(s->level > 0);
        mDeleteHigher->setEnabled(s->level < top);
    }
    mEnter->setEnabled(!n && !selectedMeshes().empty());
    mExit->setEnabled(n != nullptr);

    scene::Node* sym = n;
    if (!sym)
        for (auto it = g.selection().rbegin(); it != g.selection().rend() && !sym; ++it)
            if (scene::Node* m = g.node(*it); m && m->type().isA("mesh"))
                sym = m;
    QSignalBlocker block(mSymmetry);
    mSymmetry->setEnabled(sym != nullptr);
    mSymmetry->setCurrent(sym ? int(std::clamp<int64_t>(sym->as<int64_t>(scene::MeshNode::kSymmetry), 0, scene::kSymmetryRadial)) : 0);
}

bool sculptPanelOpen() { return dockOpen(); }

void SculptPanel::fillMaterials()
{
    QSignalBlocker block(mMaterial);
    mMaterial->clear();
    for (const auto& [id, label] : sculptMaterials())
    {
        // Matcaps after the lit materials, set apart.
        if (id.starts_with(kMatcapPrefix) && mMaterial->findData(QString(kMatcapPrefix), Qt::UserRole, Qt::MatchStartsWith) < 0)
            mMaterial->insertSeparator(mMaterial->count());
        mMaterial->addItem(QString::fromStdString(label), QString::fromStdString(id));
    }
    mMaterial->addItem("Matcap from File...", QString(kMatcapBrowse));
}

bool meshSelected() { return !selectedMeshes().empty(); }

void registerSculptUi()
{
    viewportKeys::add("Sculpt", [] {
        std::vector<KeyHint> keys = {{"Shift+Alt+right-drag", "Move the light"}, {"Shift+Alt+right-click", "Reset the light"}};
        viewportKeys::addAction(keys, "sculpt.levelUp", "Finer level");
        viewportKeys::addAction(keys, "sculpt.levelDown", "Coarser level");
        viewportKeys::addAction(keys, "window.searchMenus", "Search");
        return keys;
    });
    installSculptDrawer();
    sculpt::setLevelsBrokenHandler([](const std::vector<scene::NodeId>& ids, std::string_view command) {
        QTimer::singleShot(0, QCoreApplication::instance(), [ids, command = std::string(command)] { askAboutBrokenLevels(ids, command); });
    });
    addLazyDock(kSculptDock, [] { return sculptDock(true); });
    // Leaving sculpt mode, however it is left, closes the panel if entering opened it.
    sculpt::addListener([] {
        QTimer::singleShot(0, QCoreApplication::instance(), [] {
            static bool wasActive = false;
            const bool active = sculpt::active() != nullptr;
            if (QDockWidget* d = sculptDock(false); wasActive && !active && gClosesOnExit && d && d->isVisible())
                d->close();
            wasActive = active;
        });
    });

    ops::registerCommand({
        .name = "sculptBrush",
        .doc = "Pick the sculpt tool's brush: a built-in or registered brush by name, or a brush preset by id (see brushPresets). "
               "Returns {brush, preset}.",
        .flags = {{.name = "brush", .type = ArgType::String, .doc = "A brush name: draw, clayStrips, damStandard, ... or a registered brush's."},
                  {.name = "preset", .type = ArgType::String, .doc = "A brush preset id, such as user/Skin Pores."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (const std::string preset = a.get<std::string>("preset"); !preset.empty())
            {
                if (!selectSculptPreset(preset))
                    throw CommandError("sculptBrush: no usable preset '" + preset + "'");
            }
            else if (a.has("brush"))
            {
                sculpt::BrushSettings resolved;
                if (!sculpt::setBrush(resolved, a.get<std::string>("brush")))
                    throw CommandError("sculptBrush: no brush '" + a.get<std::string>("brush") + "'");
                selectSculptBrush(resolved.brush, resolved.custom);
            }
            const SculptSettings& s = sculptSettings();
            return {{"brush", s.brush == sculpt::Brush::Custom ? s.custom : std::string(sculpt::brushName(s.brush))}, {"preset", s.preset}};
        },
    });

    actions::add({.id = "window.sculpt",
                  .label = "Sculpt Panel",
                  .category = "Windows",
                  .run = [] {
                      if (QDockWidget* d = sculptDock(false))
                          toggleDock(d);
                      else
                          revealDock(sculptDock(true));
                  },
                  .doc = "Sculpt mode, brushes, mask and reprojection tools.",
                  .python = "lila.ui.run_action(\"window.sculpt\")"});
    actions::add({.id = "sculpt.tool",
                  .label = "Sculpt",
                  .category = "Sculpt",
                  .run = [] {
                      if (sculpt::active() && sculptToolActive())
                          return exitSculpt();
                      const bool wasOpen = dockOpen();
                      showDock();
                      if (!sculpt::active())
                      {
                          if (selectedMeshes().empty())
                          {
                              log::warn("Sculpt: select a mesh");
                              return;
                          }
                          enterSculpt();
                          gClosesOnExit = !wasOpen;
                      }
                      startSculptTool();
                  },
                  .doc = "Sculpt the selected mesh: enters sculpt mode if needed and picks up the sculpt brush. Again while sculpting exits sculpt mode.",
                  .python = "lila.ui.run_action(\"sculpt.tool\")"});
    actions::add({.id = "sculpt.enter",
                  .label = "Enter Sculpt",
                  .category = "Sculpt",
                  .run = enterSculpt,
                  .doc = "Sculpt the selected mesh itself, at its stored levels if it has any.",
                  .python = ops::toPython("sculptEnter", json::object())});
    auto levelAction = [](std::string id, std::string label, std::string command, json args, std::string doc) {
        actions::add({.id = id,
                      .label = label,
                      .category = "Sculpt",
                      .run = [command, args] { runActionWithOptions(command.c_str(), args); },
                      .enabled = [] { return sculpt::active() != nullptr; },
                      .doc = doc,
                      .python = ops::toPython(command, args)});
    };
    levelAction("sculpt.levelUp", "Sculpt Level Up", "sculptLevel", {{"step", 1}}, "Sculpt the next finer subdivision level (D while sculpting).");
    levelAction("sculpt.levelDown", "Sculpt Level Down", "sculptLevel", {{"step", -1}},
                "Sculpt the next coarser level; finer detail follows what you change there (Shift+D while sculpting).");
    levelAction("sculpt.addLevel", "Add Sculpt Level", "sculptAddLevel", json::object(), "Subdivide the finest level once more (Ctrl+D while sculpting).");
    levelAction("sculpt.deleteHigher", "Delete Higher Levels", "sculptDeleteHigher", json::object(),
                "Delete the levels above this one and their detail.");
    levelAction("sculpt.deleteLower", "Delete Lower Levels", "sculptDeleteLower", json::object(),
                "Delete the levels below this one: it becomes level 0, and the mesh's topology on exit.");
    levelAction("sculpt.reconstruct", "Reconstruct Lower Level", "sculptReconstruct", json::object(),
                "Rebuild a level below level 0 from a mesh that was subdivided before sculpting, so you can sculpt broad shapes on it.");
    levelAction("sculpt.remesh", "Voxel Remesh", "sculptRemesh", json::object(),
                "Replace the sculpt with an even voxel remesh of its finest level, free of its topology. UVs and levels go; polypaint stays.");
    levelAction("sculpt.duplicate", "Duplicate Level", "sculptDuplicate", json::object(),
                "Copy the mesh at this level beside it in the outliner, select the copy and leave sculpt mode.");
    levelAction("sculpt.duplicateHide", "Duplicate Level and Hide", "sculptDuplicate", {{"hide", true}},
                "Copy the mesh at this level beside it in the outliner, hidden, and keep sculpting.");
    actions::add({.id = "sculpt.deleteLevels",
                  .label = "Delete Sculpt Levels",
                  .category = "Sculpt",
                  .run = [] {
                      if (auto meshes = selectedMeshes(); !meshes.empty())
                          runActionWithOptions("sculptDeleteLevels", {{"args", meshes}});
                  },
                  .enabled = [] {
                      scene::Graph& g = scene::Graph::current();
                      for (const std::string& m : selectedMeshes())
                          if (scene::Node* n = g.find(m); n && sculpt::storedLevels(*n))
                              return !sculpt::active() || sculpt::active()->node != n->id();
                      return false;
                  },
                  .doc = "Remove the sculpt levels stored on the selected meshes, leaving each at the level it is at.",
                  .python = ops::toPython("sculptDeleteLevels", {{"args", {"Body"}}})});
    actions::add({.id = "sculpt.toggleSeeThrough",
                  .label = "See-Through Sculpt",
                  .category = "Sculpt",
                  .run =
                      [] {
                          bool& on = sculptSettings().seeThrough;
                          on = !on;
                          sculptSettingsChanged();
                          viewportToast::showActive(on ? "See-through on" : "See-through off", 1000);
                      },
                  .doc = "Draw the sculpt at the Display opacity so image planes behind it show through (Alt+X while sculpting).",
                  .python = "lila.ui.run_action(\"sculpt.toggleSeeThrough\")",
                  .checked = [] { return sculptSettings().seeThrough; }});
    actions::add({.id = "sculpt.exit",
                  .label = "Exit Sculpt",
                  .category = "Sculpt",
                  .run = exitSculpt,
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Leave sculpt mode. The mesh stays at the level being sculpted and keeps every level for next time.",
                  .python = ops::toPython("sculptExit", json::object())});
    actions::add({.id = "sculpt.cancel",
                  .label = "Cancel Sculpt",
                  .category = "Sculpt",
                  .run = [] {
                      if (runActionWithOptions("sculptCancel", json::object()) && sculptToolActive())
                          viewportTools::set(nullptr);
                  },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Leave sculpt mode without changing the mesh, discarding this session's sculpting.",
                  .python = ops::toPython("sculptCancel", json::object())});
    for (const BrushInfo& b : kBrushes)
    {
        std::string name(sculpt::brushName(b.brush));
        sculpt::Brush brush = b.brush;
        actions::add({.id = "sculpt.brush." + name,
                      .label = std::string("Sculpt Brush: ") + b.label,
                      .category = "Sculpt",
                      .run = [brush] {
                          selectSculptBrush(brush);
                          if (sculpt::active())
                              startSculptTool();
                      },
                      .doc = b.doc,
                      .python = "lila.ui.run_action(\"sculpt.brush." + name + "\")"});
    }
    auto maskAction = [](std::string id, std::string label, std::string op, std::string layer, std::string doc) {
        json call = {{"op", op}, {"layer", layer}};
        actions::add({.id = id,
                      .label = label,
                      .category = "Sculpt",
                      .run = [call] { runActionWithOptions("sculptMask", call); },
                      .enabled = [] { return sculpt::active() != nullptr; },
                      .doc = doc,
                      .python = ops::toPython("sculptMask", call)});
    };
    maskAction("sculpt.maskInvert", "Invert Mask", "invert", "mask", "Masked points become free and free ones masked.");
    maskAction("sculpt.maskClear", "Clear Mask", "clear", "mask", "Unmask everything.");
    maskAction("sculpt.maskFill", "Mask All", "fill", "mask", "Mask everything (then paint with Ctrl to free areas).");
    maskAction("sculpt.maskBlur", "Blur Mask", "blur", "mask", "Soften the mask's edges.");
    maskAction("sculpt.freezeClear", "Unfreeze All", "clear", "freeze", "Free every frozen point.");
    actions::add({.id = "sculpt.colorFill",
                  .label = "Fill Polypaint",
                  .category = "Sculpt",
                  .run = [] {
                      const glm::vec3 c = sculptSettings().color;
                      runActionWithOptions("sculptColor", {{"op", "fill"}, {"color", json::array({c.r, c.g, c.b})}});
                  },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Fill the sculpt's polypaint with the Polypaint color; masked points keep theirs.",
                  .python = ops::toPython("sculptColor", {{"op", "fill"}, {"color", json::array({1.0, 0.5, 0.2})}})});
    actions::add({.id = "sculpt.colorClear",
                  .label = "Clear Polypaint",
                  .category = "Sculpt",
                  .run = [] { runActionWithOptions("sculptColor", {{"op", "clear"}}); },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Clear the sculpt's polypaint back to white.",
                  .python = ops::toPython("sculptColor", {{"op", "clear"}})});

    actions::add({.id = "mesh.relaxKeepForm",
                  .label = "Relax (Keep Form)",
                  .category = "Mesh",
                  .run = [] { runActionWithOptions("relaxMesh", {{"pinCreases", true}, {"pinSeams", true}}); },
                  .doc = "Even out the point spacing of the selected components (or whole meshes), sliding points along the "
                         "surface so the shape stays. Borders, creases and UV seams stay put.",
                  .python = ops::toPython("relaxMesh", {{"iterations", 10}, {"pinCreases", true}, {"pinSeams", true}}),
                  .command = "relaxMesh",
                  .call = {{"pinCreases", true}, {"pinSeams", true}}});
    actions::add({.id = "mesh.reproject",
                  .label = "Reproject onto Target",
                  .category = "Mesh",
                  .run = [] {
                      auto meshes = selectedMeshes();
                      if (meshes.size() < 2)
                      {
                          log::warn("Reproject onto Target: select the target surface, then the meshes to move");
                          return;
                      }
                      runActionWithOptions("shrinkwrap", {{"args", std::vector<std::string>(meshes.begin() + 1, meshes.end())}, {"target", meshes.front()}});
                  },
                  .doc = "Shrinkwrap meshes onto a surface: select the target first, then the meshes to move. The shrinkwrap "
                         "command also projects along normals or a direction, with an offset.",
                  .python = ops::toPython("shrinkwrap", {{"args", {"low"}}, {"target", "high"}, {"method", "closest"}, {"offset", 0.0}}),
                  .command = "shrinkwrap"});
    actions::add({.id = "mesh.transferShape",
                  .label = "Transfer Shape (High to Low)",
                  .category = "Mesh",
                  .run = [] {
                      auto meshes = selectedMeshes();
                      if (meshes.size() < 2)
                      {
                          log::warn("Transfer Shape: select the high poly first, then the low-poly cages");
                          return;
                      }
                      runActionWithOptions("transferShape", {{"args", meshes}});
                  },
                  .doc = "Fit low-poly cages to a high poly (an imported sculpt) so their subdivision matches it, keeping UVs. "
                         "Select the high poly first.",
                  .python = ops::toPython("transferShape", {{"args", {"high", "low"}}, {"method", "fit"}, {"levels", 2}}),
                  .command = "transferShape"});
}
} // namespace lila::ui
