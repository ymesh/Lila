#include "ui/UvEditorInternal.h"

#include "ops/CommandOptions.h"
#include "ui/Menus.h"
#include "ui/OptionBox.h"
#include "ui/PanelDefaults.h"
#include "ui/FlowLayout.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/ToolkitLayout.h"
#include "ui/UvSeamOverride.h"

#include <QApplication>
#include <QGridLayout>
#include <QScrollArea>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>

namespace lila::ui
{
namespace
{
std::function<void()>& opener()
{
    static std::function<void()> fn;
    return fn;
}

/** "vertex", "edge" or "face" switch the component mode; "island" is face mode with island select. */
void setPickMode(const std::string& m)
{
    const bool island = m == "island";
    if (!island)
        uvLive::setIslandSelect(false);
    UvView::safeRun("selectMode", {{"args", {island ? std::string("face") : m}}}, CallSource::UI);
    if (island)
        uvLive::setIslandSelect(true);
}
} // namespace

namespace
{
constexpr const char* kUvCollapsed = "/uvEditor/collapsed";
constexpr const char* kToolkitPref = "/uvEditor/toolkitShown";
constexpr const char* kToolkitWidthPref = "/uvEditor/toolkitWidth";
constexpr const char* kToolkitHeightPref = "/uvEditor/toolkitHeight";
constexpr const char* kDefaultsPref = "/uvEditor/defaults";
/** Sections in the left column when the toolkit sits below the view; the rest, from Pack on, go right. */
constexpr size_t kLeftSections = 3;
constexpr int kPanelColumnGap = 8;

/** An action row for a toolkit action the editor runs itself (it needs the editor's size, padding or state). */
QToolButton* uvKitAction(const QString& label, const char* iconName, const QString& tip, QWidget* parent, const std::string& optionsId = {})
{
    QToolButton* b = controls::plainRow(label, iconName, tip, parent);
    if (!optionsId.empty())
        optionBox::withOptionBox(b, optionsId, parent);
    return b;
}

QToolButton* uvKitPush(const QString& label, const QString& tip, QWidget* parent, bool checkable = false, bool on = false)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(tip);
    b->setAccessibleName(label);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setCheckable(checkable);
    b->setChecked(on);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controls::setRole(b, "push");
    return b;
}

constexpr int kPlanarCell = 26;

/** Keeps a strip against a row's right edge, clear of its option-box corner, as the row resizes. */
class RightStripFit : public QObject
{
public:
    RightStripFit(QToolButton* row, QWidget* strip) : QObject(row), mRow(row), mStrip(strip) {}

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::Resize || e->type() == QEvent::Show)
        {
            const int w = 4 * kPlanarCell + 2;
            mStrip->setGeometry(mRow->width() - w - 12, 3, w, mRow->height() - 6);
        }
        return false;
    }

private:
    QToolButton* mRow;
    QWidget* mStrip;
};

/**
 * The Planar row: best-fit planar on the row itself, and camera, X, Y and Z in one click from a strip at its right end. The
 * row keeps its option box (the axis there is what the strip overrides).
 */
QToolButton* planarRow(QWidget* parent)
{
    QToolButton* t = controls::row("uv.projectPlanar", "square", parent, "Planar");
    auto* strip = new QWidget(t);
    controls::setRole(strip, "segmented");
    strip->setAttribute(Qt::WA_StyledBackground);
    auto* cells = new QGridLayout(strip);
    cells->setContentsMargins(1, 1, 1, 1);
    cells->setSpacing(1);
    const std::tuple<const char*, const char*, const char*> parts[] = {
        {nullptr, "uv.projectCamera", nullptr}, {"X", "uv.projectPlanarX", "axisX"}, {"Y", "uv.projectPlanarY", "axisY"}, {"Z", "uv.projectPlanarZ", "axisZ"}};
    for (int i = 0; auto [text, id, axisToken] : parts)
    {
        auto* b = new QToolButton(strip);
        if (text)
            b->setText(text);
        else
        {
            b->setIcon(icon("camera"));
            b->setIconSize(QSize(14, 14));
        }
        b->setToolTip(QString::fromStdString(actions::tooltip(id)));
        b->setAccessibleName(QString::fromStdString(actions::displayLabel(id)));
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        b->setFocusPolicy(Qt::NoFocus);
        controls::setRole(b, "segment");
        // The letter names the axis with colour off; the hue only speeds reading it.
        const QString colour = axisToken ? QString(" color: %1; font-weight: 600;").arg(themeColor(axisToken).name()) : QString();
        b->setStyleSheet("QToolButton { padding: 0;" + colour + " }");
        if (axisToken)
            b->setProperty("lilaRawColors", true);
        QObject::connect(b, &QToolButton::clicked, b, [id] { actions::run(nlohmann::json(id)); });
        cells->addWidget(b, 0, i);
        ++i;
    }
    t->installEventFilter(new RightStripFit(t, strip));
    return t;
}

/** Label and field rows, the labels right-aligned in one column (the Channel Box layout). */
QGridLayout* uvKitForm(QWidget* box)
{
    auto* g = new QGridLayout(box);
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(4);
    g->setColumnStretch(1, 1);
    return g;
}

void uvKitRow(QGridLayout* g, const QString& label, QWidget* field)
{
    const int row = g->rowCount();
    auto* l = new QLabel(label, field->parentWidget());
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->setBuddy(field);
    g->addWidget(l, row, 0);
    g->addWidget(field, row, 1);
}

/** "smoothBoundary" shows as "Smooth Boundary". */
QString uvBrushDisplayName(std::string_view n)
{
    QString out;
    for (size_t i = 0; i < n.size(); ++i)
    {
        const QChar c = QChar(n[i]);
        if (i == 0)
            out += c.toUpper();
        else
        {
            if (c.isUpper())
                out += ' ';
            out += c;
        }
    }
    return out;
}

/** Parts stacked in one section. */
QWidget* stacked(QWidget* parent, std::initializer_list<QWidget*> parts)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    for (QWidget* p : parts)
        v->addWidget(p);
    return w;
}

/** Unchecks every segment, for a state none of them shows (object mode). */
void clearSegments(const std::vector<QToolButton*>& segments)
{
    for (QToolButton* b : segments)
        if (b->isChecked())
        {
            QButtonGroup* g = b->group();
            g->setExclusive(false);
            b->setChecked(false);
            g->setExclusive(true);
        }
}
} // namespace

UvEditor::UvEditor(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // Header: the tool and component mode as two strips, then what the 2D view shows. It wraps rather than widen the editor.
    auto* header = new QWidget(this);
    header->setObjectName("uvHeader");
    auto* flow = new FlowLayout(6);
    flow->setContentsMargins(0, 0, 0, 0);
    header->setLayout(flow);
    auto* tools = new controls::Segmented(this);
    const std::tuple<const char*, const char*, UvView::Tool> toolDefs[] = {{"Select (Q)", "pointer", UvView::Tool::Select},
                                                                          {"Move (W)", "arrows-move", UvView::Tool::Move},
                                                                          {"Rotate (E)", "rotate", UvView::Tool::Rotate},
                                                                          {"Scale (R)", "resize", UvView::Tool::Scale},
                                                                          {"Brush", "brush", UvView::Tool::Brush}};
    for (auto [tip, ic, t] : toolDefs)
        tools->add({}, ic, tip, int(t));
    tools->setCurrent(int(UvView::Tool::Select));
    tools->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    flow->addWidget(tools);
    connect(tools, &controls::Segmented::picked, this, [this](int t) { mView->setTool(UvView::Tool(t)); });
    auto* modes = new controls::Segmented(this);
    const std::tuple<const char*, const char*, const char*> modeDefs[] = {{"Vertex", "vertex", "Vertex: select uv points (F9 in the view)"},
                                                                         {"Edge", "edge", "Edge: select uv edges (F10 in the view)"},
                                                                         {"Face", "face", "Face: select uv faces (F11 in the view)"},
                                                                         {"Island", "island", "Island: select whole uv islands (F12 in the view)"}};
    std::vector<QToolButton*> modeButtons;
    for (int i = 0; auto [label, mode, tip] : modeDefs)
        modeButtons.push_back(modes->add(label, {}, tip, i++));
    modes->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    flow->addWidget(modes);
    connect(modes, &controls::Segmented::picked, this, [](int i) {
        static const char* const kModes[] = {"vertex", "edge", "face", "island"};
        setPickMode(kModes[std::clamp(i, 0, 3)]);
    });
    auto showMode = [modes, modeButtons] {
        ComponentMode m = scene::components::mode();
        if (uvLive::islandSelect() && m != ComponentMode::Face)
            uvLive::setIslandSelect(false);
        int i = uvLive::islandSelect() ? 3 : m == ComponentMode::Vertex ? 0 : m == ComponentMode::Edge ? 1 : m == ComponentMode::Face ? 2 : -1;
        if (i < 0)
            clearSegments(modeButtons);
        else
            modes->setCurrent(i);
    };
    showMode();
    QToolButton* frameBtn = uvKitPush("Frame", "Frame: the selection (F), or everything (A)", this);
    frameBtn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    flow->addWidget(frameBtn);
    const bool toolkitShown = PrefStore::get().value<bool>("general", kToolkitPref, true);
    mToolkitToggle = uvKitPush("Toolkit", "Toolkit: show the UV Toolkit beside the view", this, true, toolkitShown);
    mToolkitToggle->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    auto* displayBox = new QWidget(header);
    auto* displayRow = new QHBoxLayout(displayBox);
    displayRow->setContentsMargins(0, 0, 0, 0);
    displayRow->setSpacing(4);
    displayRow->addWidget(new QLabel("Display", displayBox));
    mDisplay = new QComboBox(displayBox);
    mDisplay->addItems({"Plain", "Checker", "Distortion", "Stretch", "Islands"});
    mDisplay->setCurrentIndex(1);
    setComboDefault(mDisplay, 1);
    mDisplay->setObjectName("display");
    mDisplay->setToolTip("Display: Distortion shades distorted faces brighter; Stretch shades stretched faces light and compressed ones dark; "
                         "Islands shades each island apart");
    displayRow->addWidget(mDisplay);
    flow->addWidget(displayBox);
    QToolButton* twoColour = uvKitPush("Two-Tone", "Two-Tone: the Stretch display in two colours instead of grey", this, true, uvLive::twoColourHeatmap());
    twoColour->setEnabled(false);
    twoColour->setObjectName("twoTone");
    panelDefaults::setToggleDefault(twoColour, false);
    flow->addWidget(twoColour);
    QToolButton* udim = uvKitPush("UDIM", "UDIM: show the UDIM tiles", this, true, false);
    udim->setObjectName("udim");
    panelDefaults::setToggleDefault(udim, false);
    flow->addWidget(udim);
    QToolButton* selectedFaces = uvKitPush("Selected Faces", "Selected Faces: in face mode, show only the selected faces' UVs", this, true,
                                           PrefStore::get().value<bool>("general", kSelectedFacesOnlyPref, false));
    selectedFaces->setObjectName("selectedFaces");
    panelDefaults::setToggleDefault(selectedFaces, false);
    flow->addWidget(selectedFaces);
    QToolButton* packPreview = uvKitPush("Pack Preview",
                                         "Pack Preview: show the shown objects' islands as if packed together, with the Pack options' "
                                         "padding and rotation. View and select only; the UVs stay as they are",
                                         this, true, false);
    flow->addWidget(packPreview);
    // A texture set channel under the UVs, and the texture paint brush in UV space.
    QToolButton* texture = uvKitPush("Texture", "Texture: show a texture set channel under the UVs (drawn on the GPU at the view's resolution)", this);
    texture->setPopupMode(QToolButton::InstantPopup);
    auto* textureMenu = new QMenu(texture);
    texture->setMenu(textureMenu);
    QToolButton* paint = uvKitPush("Paint", "Paint: paint the shown texture set in UV space with the Texture Paint tool's brush and channels", this, true, false);
    paint->setEnabled(false);
    connect(textureMenu, &QMenu::aboutToShow, this, [this, textureMenu, texture, paint] {
        textureMenu->clear();
        textureMenu->addAction("None", this, [this, texture, paint] {
            mView->texturePaint().setChannel({}, {});
            texture->setText("Texture");
            paint->setChecked(false);
            paint->setEnabled(false);
        });
        const auto choices = UvTexturePaint::choices();
        if (choices.empty())
            textureMenu->addAction("No texture sets")->setEnabled(false);
        for (const auto& [set, channel] : choices)
        {
            const QString label = QString::fromStdString(set + " / " + channel);
            textureMenu->addAction(label, this, [this, texture, paint, set, channel, label] {
                mView->texturePaint().setChannel(set, channel);
                texture->setText(texture->fontMetrics().elidedText(label, Qt::ElideMiddle, 160));
                paint->setEnabled(true);
            });
        }
    });
    connect(paint, &QToolButton::toggled, this, [this](bool on) {
        mView->texturePaint().setPainting(on);
        mView->update();
    });
    flow->addWidget(texture);
    flow->addWidget(paint);
    flow->addWidget(mToolkitToggle);
    root->addWidget(header);

    auto* split = mSplit = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    root->addWidget(split, 1);
    auto* viewSide = new QWidget(split);
    auto* viewLayout = new QVBoxLayout(viewSide);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    viewLayout->setSpacing(4);
    mView = new UvView(viewSide);
    viewLayout->addWidget(mView, 1);
    mStatus = new QLabel(viewSide);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Command results can be one long line; without this the editor could not be narrower than the last one.
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    viewLayout->addWidget(mStatus);

    // The UV Toolkit: daily tiles lead each section, weekly buttons below them, rare ones in each section's overflow.
    mToolkitScroll = new QScrollArea(split);
    mToolkitScroll->setWidgetResizable(true);
    mToolkitScroll->setFrameShape(QFrame::NoFrame);
    mToolkitScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(mToolkitScroll);
    auto* kit = new QVBoxLayout(body);
    kit->setContentsMargins(8, 0, 4, 0);
    kit->setSpacing(4);
    mToolkitScroll->setWidget(body);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    mToolkitScroll->setVisible(toolkitShown);
    connect(split, &QSplitter::splitterMoved, this, [this] {
        if (mToolkitBelow)
            PrefStore::get().set("general", kToolkitHeightPref, mToolkitScroll->height());
        else
            PrefStore::get().set("general", kToolkitWidthPref, mToolkitScroll->width());
    });
    auto tile = [body](const char* actionId, const char* ic, const QString& label = {}) -> QWidget* {
        return controls::row(actionId, ic, body, label);
    };
    auto push = [body](const char* actionId, const QString& label = {}) -> QWidget* { return controls::row(actionId, nullptr, body, label); };

    // The uv set every tool works on.
    auto* setBox = new QWidget(body);
    auto* setRow = new QHBoxLayout(setBox);
    setRow->setContentsMargins(0, 4, 0, 4);
    setRow->setSpacing(6);
    setRow->addWidget(new QLabel("UV Set", setBox));
    mSet = new QComboBox(setBox);
    mSet->setMinimumContentsLength(4);
    mSet->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mSet->setToolTip("UV Set: the set the editor and tools work on (uvSetCurrent)");
    setRow->addWidget(mSet, 1);
    mAddSet = new QToolButton(setBox);
    mAddSet->setIcon(icon("plus"));
    mAddSet->setToolTip("New UV Set: a copy of the current set on the selected meshes (polyUVSet)");
    mAddSet->setAccessibleName("New UV Set");
    controls::setRole(mAddSet, "push");
    setRow->addWidget(mAddSet);
    mDeleteSet = new QToolButton(setBox);
    mDeleteSet->setIcon(icon("trash"));
    mDeleteSet->setToolTip("Delete UV Set: removes the set from the selected meshes; later sets renumber (polyUVSet)");
    mDeleteSet->setAccessibleName("Delete UV Set");
    controls::setRole(mDeleteSet, "push");
    setRow->addWidget(mDeleteSet);
    QToolButton* children = uvKitPush("Children", QString::fromStdString(actions::tooltip("uv.includeChildren")), setBox, true,
                                      uv::includeChildren());
    children->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    children->setObjectName("children");
    panelDefaults::setToggleDefault(children, true);
    setRow->addWidget(children);
    setRow->addWidget(panelDefaults::resetButton(this, kDefaultsPref, setBox));
    kit->addWidget(setBox);
    // Beside the UV set, outside any section, so an override can't hide in a collapsed one.
    kit->addWidget(new SeamOverrideRow(body));
    mSectionRow = new QHBoxLayout;
    mSectionRow->setContentsMargins(0, 0, 0, 0);
    mSectionRow->setSpacing(0);
    for (QBoxLayout*& column : mSectionColumn)
    {
        column = new QVBoxLayout;
        column->setSpacing(0);
        column->addStretch(1);
        mSectionRow->addLayout(column);
    }
    kit->addLayout(mSectionRow);
    auto addSection = [this](QWidget* s) { mSections.push_back(s); };

    auto packCall = [this] { return json{{"resolution", mResolution->value()}, {"padding", mPadding->value()}}; };
    QToolButton* autoBtn = uvKitAction("Auto", "wand", QString::fromStdString(actions::tooltip("uv.auto")), body, "uv.auto");
    connect(autoBtn, &QToolButton::clicked, this, [this, packCall] { runCommand("uvAuto", packCall()); });
    QWidget* unwrapGrid = controls::grid({autoBtn, tile("uv.unwrap", "cube-unfolded"), tile("uv.cut", "scissors", "Cut"),
                                          tile("uv.sew", "needle-thread", "Sew"),
                                          controls::splitRow("uv.autoSeams", {"uv.autoSeams2"}, "line-dashed", body, "Seams"), push("uv.weld", "Weld"),
                                          controls::splitRow("uv.bff", {"uv.bffDisk", "uv.bffRectangle", "uv.bffCones"}, "grid-dots", body, "Conformal"),
                                          controls::splitRow("uv.unwrapSymmetric", {"uv.unwrapSymmetricStack"}, "flip-vertical", body, "Symmetric"),
                                          push("uv.lightmap", "Lightmap"), push("mesh.transferAttributes", "Transfer"),
                                          controls::overflow({"uv.seamView", "uv.seamViewPlane", "uv.delete"}, body)},
                                         2, body);
    addSection(collapsibleSection("Unwrap", unwrapGrid, body, kUvCollapsed));

    QToolButton* hoseBtn = uvKitAction("Hose", "pipeline", QString::fromStdString(actions::tooltip("uv.hose")), body, "uv.hose");
    connect(hoseBtn, &QToolButton::clicked, this, [this, packCall] { runCommand("uvHose", packCall()); });
    QWidget* projectGrid = controls::grid({tile("uv.projectBox", "box", "Box"), tile("uv.projectSpherical", "sphere", "Spherical"), hoseBtn,
                                           controls::overflow({"uv.projectCylindrical"}, body)},
                                          2, body);
    addSection(collapsibleSection("Project", controls::grid({planarRow(body), projectGrid}, 1, body), body, kUvCollapsed));

    // Relaxing: live solving, one-shot solves, pins, the stretch/compress balance and the brushes.
    QToolButton* live = uvKitAction("Live", "bolt", QString::fromStdString(actions::tooltip("uv.liveMode")), body);
    live->setCheckable(true);
    live->setChecked(uvLive::liveMode());
    live->setObjectName("live");
    panelDefaults::setToggleDefault(live, false);
    QWidget* relaxActions = controls::grid(
        {tile("uv.liveRelax", "ripple", "Relax"), tile("uv.liveFlatten", "arrows-diagonal-minimize-2", "Flatten"), tile("uv.liveSettle", "circle-check", "Settle"),
         live,
         controls::splitRow("uv.optimizeConformal", {"uv.optimizeAuthalic", "uv.optimizeBlend", "uv.optimizeIsometric", "uv.mosaic"}, "adjustments", body,
                            "Optimize"),
         push("uv.brush3d", "Brush on Mesh"), push("uv.pin", "Pin"), push("uv.unpin", "Unpin")},
        2, body);
    auto* relaxForm = new QWidget(body);
    QGridLayout* relaxGrid = uvKitForm(relaxForm);
    auto* balance = new controls::SliderField(relaxForm);
    balance->setRange(-1.0, 1.0);
    balance->setDecimals(2);
    balance->setSingleStep(0.05);
    balance->setValue(uvLive::balance());
    setSpinDefault(balance, 0.0);
    balance->setObjectName("balance");
    balance->setToolTip("Balance: stretch against compression. Positive tolerates stretch and fixes compression, negative the reverse, "
                        "zero treats both alike");
    uvKitRow(relaxGrid, "Balance", balance);
    auto* brushCombo = new QComboBox(relaxForm);
    for (int i = 0; i <= int(uv::LiveBrush::Soften); ++i)
        brushCombo->addItem(uvBrushDisplayName(uv::liveBrushName(uv::LiveBrush(i))));
    brushCombo->setCurrentIndex(int(uvLive::brush().brush));
    setComboDefault(brushCombo, int(uvLive::BrushSettings{}.brush));
    brushCombo->setObjectName("brush");
    brushCombo->setToolTip("Brush: for the Brush tool here and Brush on Mesh in the viewport");
    uvKitRow(relaxGrid, "Brush", brushCombo);
    addSection(collapsibleSection("Relax", stacked(body, {relaxActions, relaxForm}), body, kUvCollapsed));

    // Packing and texel density share the texture size; the pack options fold beneath.
    auto* packForm = new QWidget(body);
    QGridLayout* packGrid = uvKitForm(packForm);
    mResolution = new QSpinBox(packForm);
    mResolution->setRange(16, 16384);
    mResolution->setValue(1024);
    setSpinDefault(mResolution, 1024);
    mResolution->setSuffix(" px");
    mResolution->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mResolution->setObjectName("textureSize");
    controls::setPresets(mResolution, kTextureSizePresets);
    mResolution->setToolTip("Texture Size: for packing and texel density");
    uvKitRow(packGrid, "Texture Size", mResolution);
    mPadding = new controls::SliderField(packForm);
    mPadding->setRange(0.0, 64.0);
    mPadding->setDecimals(1);
    mPadding->setValue(4.0);
    setSpinDefault(mPadding, 4.0);
    mPadding->setObjectName("padding");
    mPadding->setSuffix(" px");
    mPadding->setToolTip("Padding: texels between islands");
    uvKitRow(packGrid, "Padding", mPadding);
    mDensity = controls::valueField(0.0, 100000.0, 3, "px/cm", packForm);
    mDensity->setValue(10.24);
    setSpinDefault(mDensity, 10.24);
    mDensity->setObjectName("texelDensity");
    // 10.24 px/cm is 1024 px over a metre; the rest halve and double it.
    controls::setPresets(mDensity, {2.56, 5.12, 10.24, 20.48, 40.96});
    mDensity->setToolTip("Texel Density: at the texture size");
    uvKitRow(packGrid, "Texel Density", mDensity);
    QToolButton* packBtn = uvKitAction("Pack", "packages", QString::fromStdString(actions::tooltip("uv.pack")), body, "uv.pack");
    connect(packBtn, &QToolButton::clicked, this, [this] { runPack(); });
    QToolButton* getTd = uvKitAction("Get Density", "color-picker", "Get Density: measure the texel density of the selection", body);
    QToolButton* setTd = uvKitAction("Set Density", "ruler-measure", "Set Density: scale each selected island to the texel density", body);
    QToolButton* infoBtn = mInfo = uvKitAction("Info", "info-circle", "Info: islands, overlaps, distortion and density under the view while on (uvLayout)", body);
    mInfo->setCheckable(true);
    mInfoRefresh = new QTimer(this);
    mInfoRefresh->setSingleShot(true);
    mInfoRefresh->setInterval(300);
    connect(mInfoRefresh, &QTimer::timeout, this, [this] {
        if (mInfo->isChecked())
            showLayout();
    });
    connect(mInfo, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            showLayout();
        else
            mStatus->clear();
    });
    connect(mResolution, &QSpinBox::valueChanged, mInfoRefresh, qOverload<>(&QTimer::start));
    QWidget* packMore = controls::overflow({"uv.packAroundSelected", "uv.packSelectionOnly", "uv.packStacked", "uv.packPerObject",
                                            "uv.packPerMaterial", "uv.packUdimByMaterial", "uv.selectStacks"},
                                           body);
    QWidget* packActions = controls::grid({packBtn, getTd, setTd, infoBtn, packMore}, 2, body);
    mPackPanel = new UvPackPanel(body);
    mPackPanel->onShowStacks = [this](bool on) { mView->setShowStacks(on); };
    QWidget* packOptions = collapsibleSection("Pack Options", mPackPanel, body, kUvCollapsed, false, &mPackOptions);
    addSection(collapsibleSection("Pack", stacked(body, {packActions, packForm, packOptions}), body, kUvCollapsed, true, &mPackSection));

    auto* viewportForm = new QWidget(body);
    auto* viewportRow = new QHBoxLayout(viewportForm);
    viewportRow->setContentsMargins(0, 0, 0, 0);
    viewportRow->setSpacing(2);
    QToolButton* checker = uvKitPush("Checker", QString::fromStdString(actions::tooltip("uv.checker3d")), viewportForm, true, uvLive::checker3d());
    checker->setObjectName("checker");
    panelDefaults::setToggleDefault(checker, false);
    viewportRow->addWidget(checker);
    auto* tiling = new QSpinBox(viewportForm);
    tiling->setRange(1, 256);
    tiling->setValue(uvLive::checkerTiling());
    setSpinDefault(tiling, 32);
    tiling->setObjectName("checkerTiling");
    tiling->setKeyboardTracking(false);
    tiling->setButtonSymbols(QAbstractSpinBox::NoButtons);
    tiling->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tiling->setSuffix(" tiles");
    tiling->setToolTip("Checker Tiling: checks across one 0-1 tile, in the viewport and the editor's checker display");
    tiling->setEnabled(uvLive::checker3d());
    viewportRow->addWidget(tiling);
    QToolButton* seams = uvKitPush("Seams", QString::fromStdString(actions::tooltip("uv.seams3d")), viewportForm, true, uvLive::seams3d());
    seams->setObjectName("seams");
    panelDefaults::setToggleDefault(seams, true);
    viewportRow->addWidget(seams);
    addSection(collapsibleSection("Viewport", viewportForm, body, kUvCollapsed));

    addSection(collapsibleSection("Arrange",
                                  stacked(body, {controls::grid({push("uv.straighten", "Straighten"), push("uv.orient", "Orient"),
                                                                 push("uv.rotateLeft", "Left 90"), push("uv.rotateRight", "Right 90"),
                                                                 push("uv.flipX", "Flip U"), push("uv.flipY", "Flip V"),
                                                                 controls::overflow({"uv.straightenEdges", "uv.alignU", "uv.alignV", "uv.orientByEdge"}, body)},
                                                                2, body)}),
                                  body, kUvCollapsed, false));

    // Numeric transforms: each row's button applies its fields.
    auto* xformBox = new QWidget(body);
    auto* xform = new QGridLayout(xformBox);
    xform->setContentsMargins(0, 0, 0, 0);
    xform->setHorizontalSpacing(2);
    xform->setVerticalSpacing(4);
    mMoveU = controls::valueField(-100.0, 100.0, 4, {}, xformBox);
    mMoveV = controls::valueField(-100.0, 100.0, 4, {}, xformBox);
    mRotate = controls::valueField(-360.0, 360.0, 2, QString(QChar(0x00B0)), xformBox);
    mScaleU = controls::valueField(-100.0, 100.0, 4, {}, xformBox);
    mScaleV = controls::valueField(-100.0, 100.0, 4, {}, xformBox);
    mRotate->setValue(90.0);
    mMoveU->setObjectName("moveU");
    mMoveV->setObjectName("moveV");
    mRotate->setObjectName("rotate");
    mScaleU->setObjectName("scaleU");
    mScaleV->setObjectName("scaleV");
    mScaleU->setValue(1.0);
    mScaleV->setValue(1.0);
    for (QDoubleSpinBox* s : {mMoveU, mMoveV})
        setSpinDefault(s, 0.0);
    setSpinDefault(mRotate, 90.0);
    for (QDoubleSpinBox* s : {mScaleU, mScaleV})
        setSpinDefault(s, 1.0);
    for (QDoubleSpinBox* s : {mMoveU, mScaleU})
        s->setPrefix("U  ");
    for (QDoubleSpinBox* s : {mMoveV, mScaleV})
        s->setPrefix("V  ");
    QToolButton* moveBtn = uvKitPush("Move", "Move: the selected uvs by U, V", xformBox);
    QToolButton* rotBtn = uvKitPush("Rotate", "Rotate: the selected uvs, degrees counter-clockwise", xformBox);
    QToolButton* scaleBtn = uvKitPush("Scale", "Scale: the selected uvs about their centre", xformBox);
    const std::tuple<QToolButton*, QDoubleSpinBox*, QDoubleSpinBox*> xformRows[] = {
        {moveBtn, mMoveU, mMoveV}, {rotBtn, mRotate, nullptr}, {scaleBtn, mScaleU, mScaleV}};
    for (int row = 0; auto [apply, a, b] : xformRows)
    {
        xform->addWidget(apply, row, 0);
        xform->addWidget(a, row, 1, 1, b ? 1 : 2);
        if (b)
            xform->addWidget(b, row, 2);
        ++row;
    }
    xform->setColumnStretch(1, 1);
    xform->setColumnStretch(2, 1);
    addSection(collapsibleSection("Transform", xformBox, body, kUvCollapsed, false));
    for (size_t i = 0; i < mSections.size(); ++i)
        mSectionColumn[0]->insertWidget(int(i), mSections[i]);
    mSectionRow->setStretch(0, 1);
    mSectionColumns = 1;
    kit->addStretch(1);
    // The column stops at its narrowest row, folded Pack options included, instead of clipping it.
    const int widest = std::max(body->minimumSizeHint().width(), mPackPanel->minimumSizeHint().width() + kit->contentsMargins().left());
    mToolkitScroll->setMinimumWidth(widest + mToolkitScroll->verticalScrollBar()->sizeHint().width());
    connect(mToolkitToggle, &QToolButton::toggled, this, [this](bool on) {
        mToolkitScroll->setVisible(on);
        PrefStore::get().set("general", kToolkitPref, on);
    });

    connect(live, &QToolButton::toggled, this, [](bool on) { uvLive::setLiveMode(on); });
    connect(balance, &QDoubleSpinBox::valueChanged, this, [](double v) { uvLive::setBalance(v); });
    connect(brushCombo, &QComboBox::currentIndexChanged, this, [this, tools](int i) {
        uvLive::brush().brush = uv::LiveBrush(std::clamp(i, 0, int(uv::LiveBrush::Soften)));
        uvLive::settingsChanged();
        tools->setCurrent(int(UvView::Tool::Brush));
        mView->setTool(UvView::Tool::Brush);
    });
    connect(checker, &QToolButton::toggled, this, [tiling](bool on) {
        tiling->setEnabled(on);
        uvLive::setChecker3d(on);
    });
    connect(tiling, &QSpinBox::valueChanged, this, [this](int v) {
        uvLive::setCheckerTiling(v);
        mView->update();
    });
    connect(seams, &QToolButton::toggled, this, [](bool on) { uvLive::setSeams3d(on); });
    connect(children, &QToolButton::toggled, this, [](bool on) {
        uv::setIncludeChildren(on);
        uvLive::settingsChanged();
        viewportTools::requestRedraw();
    });
    connect(twoColour, &QToolButton::toggled, this, [this](bool on) {
        uvLive::setTwoColourHeatmap(on);
        mView->update();
    });
    mLiveListener = uvLive::addListener([this, live, brushCombo, checker, tiling, seams, children, twoColour, showMode] {
        QSignalBlocker b1(live), b2(brushCombo), b3(checker), b4(seams), b5(tiling), b6(children), b7(twoColour);
        children->setChecked(uv::includeChildren());
        live->setChecked(uvLive::liveMode());
        brushCombo->setCurrentIndex(int(uvLive::brush().brush));
        checker->setChecked(uvLive::checker3d());
        tiling->setValue(uvLive::checkerTiling());
        tiling->setEnabled(uvLive::checker3d());
        twoColour->setChecked(uvLive::twoColourHeatmap());
        showMode();
        seams->setChecked(uvLive::seams3d());
        mView->update();
    });

    mView->onToolChanged = [tools](UvView::Tool t) { tools->setCurrent(int(t)); };
    connect(frameBtn, &QToolButton::clicked, this, [this] { mView->frameSelection(); });
    connect(mDisplay, &QComboBox::currentIndexChanged, this, [this, twoColour](int i) {
        twoColour->setEnabled(UvView::Display(i) == UvView::Display::Stretch);
        mView->setDisplay(UvView::Display(i));
    });
    connect(udim, &QToolButton::toggled, this, [this](bool on) { mView->setShowUdim(on); });
    connect(selectedFaces, &QToolButton::toggled, this, [this](bool on) { mView->setSelectedFacesOnly(on); });
    connect(packPreview, &QToolButton::toggled, this, [this, tools, udim, paint](bool on) {
        if (on)
            paint->setChecked(false);
        tools->setEnabled(!on);
        udim->setEnabled(!on);
        mView->setPackPreview(on);
    });
    connect(mSet, &QComboBox::activated, this, [this](int i) {
        if (QString s = mSet->itemData(i).toString(); !s.isEmpty())
            UvView::safeRun("uvSetCurrent", {{"args", {s.toStdString()}}}, CallSource::UI);
        mView->invalidate();
        refreshSets();
    });
    connect(mAddSet, &QToolButton::clicked, this, [this] {
        json r = runCommand("polyUVSet", {{"copy", true}, {"uvSet", uv::currentUvSet()}});
        if (!r.is_object() || !r.contains("created") || r["created"].empty())
            return;
        std::map<std::string, std::vector<std::string>> byName;
        for (const auto& [mesh, set] : r["created"].items())
            byName[set.get<std::string>()].push_back(mesh);
        // Different set counts number the copies differently; the current set follows the most meshes.
        auto most = std::ranges::max_element(byName, {}, [](const auto& e) { return e.second.size(); });
        UvView::safeRun("uvSetCurrent", {{"args", {most->first}}}, CallSource::UI);
        if (byName.size() > 1)
        {
            std::string where;
            for (const auto& [set, meshes] : byName)
                for (const std::string& m : meshes)
                    where += (where.empty() ? "" : ", ") + m + " " + set;
            log::warn("New UV Set: the meshes had different set counts, so the copy got different numbers: {}", where);
        }
        mView->invalidate();
        refreshSets();
    });
    connect(mDeleteSet, &QToolButton::clicked, this, [this] {
        runCommand("polyUVSet", {{"delete", true}, {"uvSet", uv::currentUvSet()}});
        refreshSets();
    });
    connect(getTd, &QToolButton::clicked, this, [this] {
        json r = runCommand("uvTexelDensity", {{"resolution", mResolution->value()}});
        if (r.is_object() && !r.empty())
            r = r.begin().value();
        if (r.is_number())
            mDensity->setValue(r.get<double>());
    });
    connect(setTd, &QToolButton::clicked, this,
            [this] { runCommand("uvTexelDensity", {{"resolution", mResolution->value()}, {"density", mDensity->value()}}); });
    connect(moveBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"translate", {mMoveU->value(), mMoveV->value()}}}); });
    connect(rotBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"rotate", mRotate->value()}}); });
    connect(scaleBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"scale", {mScaleU->value(), mScaleV->value()}}}); });

    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) {
        mView->invalidate();
        if (mInfo->isChecked())
            mInfoRefresh->start();
        refreshSets();
    });
    mComponentListener = scene::components::addListener([this, showMode] {
        showMode();
        mView->update();
    });
    refreshSets();
    panelDefaults::load(this, kDefaultsPref);
}

UvEditor::~UvEditor()
{
    uvLive::setEditorShown(this, false);
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
    uvLive::removeListener(mLiveListener);
}

void UvEditor::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    uvLive::setEditorShown(this, true);
    if (mToolkitSized)
        return;
    placeToolkit();
    mToolkitSized = true;
}

void UvEditor::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (mToolkitSized)
        placeToolkit();
}

void UvEditor::placeToolkit()
{
    // A margin either side of square keeps a drag near it from flipping the toolkit back and forth.
    const QSize area = mSplit->size();
    const bool below = mToolkitBelow ? area.height() * 10 > area.width() * 9 : area.height() * 9 > area.width() * 10;
    // Pack's folded options count toward its column even while closed, so opening them never overflows.
    auto columnWidth = [this](size_t from, size_t to) {
        int w = 0;
        for (size_t i = from; i < to; ++i)
            w = std::max(w, mSections[i]->minimumSizeHint().width());
        return to > kLeftSections ? std::max(w, mPackPanel->minimumSizeHint().width()) : w;
    };
    const int twoWide = columnWidth(0, kLeftSections) + columnWidth(kLeftSections, mSections.size()) + kPanelColumnGap +
                        mToolkitScroll->widget()->layout()->contentsMargins().left() + mToolkitScroll->verticalScrollBar()->sizeHint().width();
    const int columns = below && area.width() >= twoWide ? 2 : 1;
    if (columns != mSectionColumns)
    {
        mSectionColumns = columns;
        const size_t split = columns == 2 ? kLeftSections : mSections.size();
        for (size_t i = 0; i < mSections.size(); ++i)
        {
            QBoxLayout* column = mSectionColumn[i < split ? 0 : 1];
            mSectionColumn[0]->removeWidget(mSections[i]);
            mSectionColumn[1]->removeWidget(mSections[i]);
            column->insertWidget(column->count() - 1, mSections[i]);
        }
        mSectionRow->setSpacing(columns == 2 ? kPanelColumnGap : 0);
        mSectionRow->setStretch(1, columns == 2 ? 1 : 0);
    }
    if (mToolkitSized && below == mToolkitBelow)
        return;
    mToolkitBelow = below;
    mSplit->setOrientation(below ? Qt::Vertical : Qt::Horizontal);
    // The toolkit opens at its last dragged size, or wide enough for every label (tall enough for every section below).
    const QSize hint = mToolkitScroll->widget()->sizeHint();
    const int bar = mToolkitScroll->verticalScrollBar()->sizeHint().width();
    const int total = below ? area.height() : area.width();
    const int size = below ? std::min(PrefStore::get().value<int>("general", kToolkitHeightPref, hint.height()), total / 2)
                           : PrefStore::get().value<int>("general", kToolkitWidthPref, hint.width() + bar);
    mSplit->setSizes({std::max(0, total - size), size});
}

void UvEditor::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    uvLive::setEditorShown(this, false);
}

json UvEditor::runCommand(const char* command, const json& call)
{
    if (optionBox::shiftOpens(command, call))
        return nullptr;
    json r;
    try
    {
        r = ops::execute(command, ops::options::callWith(command, call), CallSource::UI);
        actions::remember({{"command", command}, {"args", call}});
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    if (!r.is_null())
    {
        std::string text = r.dump();
        if (text.size() > 200)
            text = text.substr(0, 197) + "...";
        const QString result = QString(command) + ": " + QString::fromStdString(text);
        if (mInfo->isChecked())
            showLayout(result);
        else
            mStatus->setText(result);
    }
    mView->invalidate();
    return r;
}

namespace
{
/** The uv sets on the meshes the editor shows, each with how many of those meshes have it. */
struct ShownSets
{
    std::map<std::string, int> sets;
    int meshes = 0;
};

ShownSets shownUvSets()
{
    ShownSets out;
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : UvView::shownMeshes())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            if (scene::MeshPtr m = scene::modifiers::cage(g, id))
            {
                ++out.meshes;
                for (const std::string& name : m->cornerAttrs().names())
                    if (const geo::Attribute* a = m->cornerAttrs().find(name); a && a->type() == geo::AttrType::Float2)
                        ++out.sets[name];
            }
    return out;
}
} // namespace

QStringList uvSetNames()
{
    std::set<std::string> names{uv::currentUvSet(), "uv0", "uv1"};
    for (const auto& [name, count] : shownUvSets().sets)
        names.insert(name);
    QStringList items;
    for (const std::string& n : names)
        items << QString::fromStdString(n);
    return items;
}

void UvEditor::refreshSets()
{
    const ShownSets shown = shownUvSets();
    // Switch off a set none of the shown meshes have, so the box only ever names a real set.
    if (!shown.sets.empty() && !shown.sets.contains(uv::currentUvSet()))
    {
        uv::setCurrentUvSet(shown.sets.begin()->first);
        mView->invalidate();
    }
    const std::string cur = uv::currentUvSet();
    const bool exists = shown.sets.contains(cur);
    std::vector<std::pair<QString, QString>> items; // display text, set name
    for (const auto& [name, count] : shown.sets)
        items.emplace_back(count < shown.meshes ? QString("%1 (%2/%3)").arg(QString::fromStdString(name)).arg(count).arg(shown.meshes)
                                                : QString::fromStdString(name),
                           QString::fromStdString(name));
    if (!exists)
        items.emplace_back(QString::fromStdString(cur), QString::fromStdString(cur));
    bool same = int(items.size()) == mSet->count();
    for (int i = 0; same && i < mSet->count(); ++i)
        same = mSet->itemText(i) == items[size_t(i)].first && mSet->itemData(i).toString() == items[size_t(i)].second;
    QSignalBlocker block(mSet);
    if (!same)
    {
        mSet->clear();
        for (const auto& [text, name] : items)
            mSet->addItem(text, name);
    }
    mSet->setCurrentIndex(mSet->findData(QString::fromStdString(cur)));
    mSet->setEnabled(exists);
    mAddSet->setEnabled(exists);
    mDeleteSet->setEnabled(exists);
}

void UvEditor::showToolkit()
{
    mToolkitToggle->setChecked(true);
}

void UvEditor::showPackOptions()
{
    mToolkitToggle->setChecked(true);
    mPackSection->setChecked(true);
    mPackOptions->setChecked(true);
    QTimer::singleShot(0, this, [this] { mToolkitScroll->ensureWidgetVisible(mPackPanel); });
}

void UvEditor::runPack()
{
    json call = mPackPanel->options();
    call["resolution"] = mResolution->value();
    call["padding"] = mPadding->value();
    setUvPackOptions(call);
    json r = runCommand("uvPack", call);
    mPackPanel->showResult(r);
}

void UvEditor::showLayout(const QString& lead)
{
    json r = UvView::safeRun("uvLayout", {{"resolution", mResolution->value()}}, CallSource::Script);
    if (r.is_null())
        return;
    auto line = [](const std::string& name, const json& j) {
        if (!j.value("exists", true))
            return QString::fromStdString(name + ": no " + j.value("uvSet", std::string("uv")) + " set");
        return QString::fromStdString(name) + QString(": %1 islands, %2 overlapping, %3 folded, stretch %4 / %5, %6 px/cm")
                                                  .arg(j.value("islands", 0))
                                                  .arg(j.value("overlappingFaces", 0))
                                                  .arg(j.value("foldedFaces", 0))
                                                  .arg(j["areaDistortion"].value("mean", 0.0), 0, 'f', 3)
                                                  .arg(j["angleDistortion"].value("mean", 0.0), 0, 'f', 3)
                                                  .arg(j.value("texelDensity", 0.0), 0, 'f', 2);
    };
    QStringList lines;
    if (r.contains("islands"))
        lines << line("selection", r);
    else
        for (auto& [name, j] : r.items())
            lines << line(name, j);
    mStatus->setText((lead.isEmpty() ? QString() : lead + "\n") + lines.join("\n"));
    log::info("{}", lines.join("; ").toStdString());
}

void setUvEditorOpener(std::function<void()> open)
{
    opener() = std::move(open);
}

void showUvPackOptions()
{
    panels::show(kUvEditorDock);
    // The dock may build its editor on first show, so look for it once events have run.
    QTimer::singleShot(0, [] {
        for (QWidget* w : QApplication::allWidgets())
            if (auto* editor = qobject_cast<UvEditor*>(w))
                editor->showPackOptions();
    });
}

void showUvToolkit()
{
    panels::show(kUvEditorDock);
    QTimer::singleShot(0, [] {
        for (QWidget* w : QApplication::allWidgets())
            if (auto* editor = qobject_cast<UvEditor*>(w))
                editor->showToolkit();
    });
}

void registerUvActions()
{
    registerUvPackActions();
    installUvProjectTool();
    actions::add({.id = "window.uvEditor", .label = "UV Editor", .category = "Windows", .run = [] {
                      if (opener())
                          opener()();
                  },
                  .doc = "2D uv view with selection, manipulators and the uv tools.",
                  .python = "lila.ui.run_action(\"window.uvEditor\")"});
    actions::add({.id = "window.uvToolkit", .label = "UV Toolkit", .category = "Windows", .run = showUvToolkit,
                  .doc = "The UV editor with its toolkit column: unwrap, cut and sew, relax, arrange, transform, pack and texel density.",
                  .python = "lila.ui.run_action(\"window.uvToolkit\")"});
    auto cmd = [](std::string id, std::string label, const char* command, json call, std::string doc) {
        std::string py = ops::toPython(command, call);
        actions::add({.id = std::move(id), .label = std::move(label), .category = "UV", .doc = std::move(doc), .python = py, .command = command,
                      .call = call});
    };
    cmd("uv.auto", "Auto UV", "uvAuto", json::object(), "Seams, unwrap and pack in one step.");
    cmd("uv.autoSeams", "Auto Seams", "uvAutoSeams", json::object(), "Place seams for game assets.");
    cmd("uv.unwrap", "Unwrap", "uvUnwrap", json::object(), "Unwrap along the seams with low stretch.");
    cmd("uv.unwrapExported", "Unwrap Exported Mesh", "uvUnwrapExported", json::object(),
        "Apply the modifier stack and unwrap the result: no collapsed UVs. The modifiers are gone afterwards.");
    menus::addItem({.menu = "UV", .action = "uv.unwrapExported", .after = "UV/uv.unwrap"});
    // uv.cut, uv.sew and uv.weld come from uvLive::install, which re-solves the touched islands in live mode.
    uvLive::install();
    for (auto [mode, label, doc] : {std::tuple{"vertex", "UV Vertex Select", "Select uv points in the UV editor (vertex mode)."},
                                    {"edge", "UV Edge Select", "Select uv edges in the UV editor (edge mode)."},
                                    {"face", "UV Face Select", "Select uv faces in the UV editor (face mode)."},
                                    {"island", "UV Island Select", "Face mode where clicks and marquees, in the UV editor and the viewports, select whole uv islands."}})
    {
        std::string id = std::string("uv.") + mode + "Mode";
        actions::add({.id = id, .label = label, .category = "UV", .run = [m = std::string(mode)] { setPickMode(m); }, .doc = doc,
                      .python = "lila.ui.run_action(\"" + id + "\")"});
    }
    cmd("uv.straighten", "Straighten UVs", "uvStraighten", json::object(), "Rectify islands into grids.");
    cmd("uv.orient", "Orient UVs", "uvOrient", json::object(), "Turn islands upright.");
    cmd("uv.rotateLeft", "Rotate Islands Left", "uvTransform", json{{"rotate", 90.0}, {"perIsland", true}},
        "Rotate each selected island 90 degrees counter-clockwise about its centre.");
    cmd("uv.rotateRight", "Rotate Islands Right", "uvTransform", json{{"rotate", -90.0}, {"perIsland", true}},
        "Rotate each selected island 90 degrees clockwise about its centre.");
    cmd("uv.flipX", "Flip Islands X", "uvTransform", json{{"scale", {-1.0, 1.0}}, {"perIsland", true}},
        "Mirror each selected island left to right about its centre.");
    cmd("uv.flipY", "Flip Islands Y", "uvTransform", json{{"scale", {1.0, -1.0}}, {"perIsland", true}},
        "Mirror each selected island top to bottom about its centre.");
    cmd("uv.delete", "Delete UVs", "uvDelete", json::object(), "Delete the selected faces' uvs, keeping the faces; whole meshes lose the uv set.");
    cmd("uv.lightmap", "Lightmap UVs", "uvLightmap", json::object(), "Generate Unreal lightmap uvs in uv1.");
    cmd("uv.bff", "BFF Unwrap", "uvBff", json::object(), "Boundary First Flattening: conformal, boundary lengths kept, least area stretch.");
    cmd("uv.bffDisk", "BFF Unwrap to Disk", "uvBff", json{{"boundary", "disk"}}, "Flatten islands conformally onto circles.");
    cmd("uv.bffRectangle", "BFF Unwrap to Rectangle", "uvBff", json{{"boundary", "rectangle"}},
        "Flatten islands conformally onto rectangles; the selected vertices (or the sharpest four) are the corners.");
    cmd("uv.bffCones", "BFF Unwrap with Cones", "uvBff", json{{"cones", 6}}, "BFF with six automatic cone points to cut area stretch.");
    cmd("uv.optimizeConformal", "Optimize Angles", "uvOptimize", json{{"mode", "conformal"}}, "Relax islands in place to keep angles.");
    cmd("uv.optimizeAuthalic", "Optimize Areas", "uvOptimize", json{{"mode", "authalic"}}, "Relax islands in place to keep areas.");
    cmd("uv.optimizeBlend", "Optimize Blend", "uvOptimize", json{{"mode", "blend"}}, "Relax islands in place, half angles, half areas.");
    cmd("uv.optimizeIsometric", "Optimize Isometric", "uvOptimize", json{{"mode", "isometric"}}, "Relax islands in place with SLIM.");
    cmd("uv.mosaic", "Mosaic", "uvMosaic", json::object(), "Split islands that cannot flatten without stretch, then re-solve the pieces.");
    cmd("uv.autoSeams2", "Auto Seams 2", "uvAutoSeams", json{{"method", "optcuts"}},
        "Seams and stretch optimised together (OptCuts): short, hidden seams under a stretch bound.");
    cmd("uv.unwrapSymmetric", "Unwrap Symmetric", "uvUnwrapSymmetric", json::object(),
        "Unwrap one side and mirror it onto the other; islands across the plane come out symmetric.");
    cmd("uv.unwrapSymmetricStack", "Unwrap Symmetric Stacked", "uvUnwrapSymmetric", json{{"mode", "stack"}},
        "Unwrap one side; mirrored islands share its uvs.");
    cmd("uv.straightenEdges", "Straighten UV Edges", "uvStraightenEdges", json::object(),
        "Lay the selected edge chains straight in uv and re-solve around them.");
    cmd("uv.alignU", "Align Side to U", "uvAlignToEdge", json{{"axis", "u"}}, "Turn islands so the selected edges run along U.");
    cmd("uv.alignV", "Align Side to V", "uvAlignToEdge", json{{"axis", "v"}}, "Turn islands so the selected edges run along V.");
    cmd("uv.orientByEdge", "Orient by Edge", "uvAlignToEdge", json::object(), "Turn islands so the selected edges run along the nearest axis.");
    cmd("uv.projectPlanar", "Planar Projection", "uvProject", json{{"type", "planar"}}, "Project uvs along the best-fit plane.");
    cmd("uv.projectPlanarBest", "Planar Projection Best Fit", "uvProject", json{{"type", "planar"}, {"axis", "best"}},
        "Project uvs along the plane fitted to the faces.");
    for (const char* axis : {"X", "Y", "Z"})
    {
        const std::string lower(1, char(std::tolower(axis[0])));
        cmd(std::string("uv.projectPlanar") + axis, std::string("Planar Projection ") + axis, "uvProject",
            json{{"type", "planar"}, {"axis", lower}}, std::string("Project uvs along the ") + axis + " axis.");
    }
    cmd("uv.projectBox", "Box Projection", "uvProject", json{{"type", "box"}}, "Project uvs from six sides.");
    cmd("uv.projectCylindrical", "Cylindrical Projection", "uvProject", json{{"type", "cylindrical"}}, "Project uvs around an axis.");
    cmd("uv.projectSpherical", "Spherical Projection", "uvProject", json{{"type", "spherical"}}, "Project uvs from the centre.");
    cmd("uv.projectCamera", "Camera Projection", "uvProject", json{{"type", "camera"}},
        "Project uvs through the active view, filling 0-1 with what it shows. Pick a camera in the options to use its resolution gate.");
    cmd("uv.hose", "Hose UVs", "uvHose", json::object(),
        "Pipes, hoses, cables and rings: one straight strip per tube with its seam on the least seen side, long runs cut where least "
        "seen, round ends as circles. Seam views, cameras or planes in the options say where it is seen from.");
}
} // namespace lila::ui
