#include "ui/ViewportHeader.h"
#include "ui/ViewportHeaderConfig.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/ComponentPick.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/SoftSelect.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/DisplayAids.h"
#include "ui/GizmoMath.h"
#include "ui/Icons.h"
#include "ui/ImagePlaneUi.h"
#include "ui/LodDisplay.h"
#include "ui/OptionBox.h"
#include "ui/RetopoDrawTool.h"
#include "ui/SoftSelectPanel.h"
#include "ui/SpinScrub.h"
#include "ui/TearOff.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"
#include "ui/ViewportContext.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportRenderers.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolbar.h"
#include "ui/ViewportWidget.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace lila::ui
{
namespace
{
namespace vd = viewportDisplay;

constexpr int kHeaderHeight = 32;
constexpr int kHeaderIcon = 16;
constexpr int kHeaderGap = 4;
constexpr int kHeaderPad = 6;
constexpr int kRowLabelWidth = 72;
constexpr const char* kStateProp = "lilaHeaderState";
/** Which layout entry an item came from (left from 0, right from 1000), for the header's right-click Remove. */
constexpr const char* kEntryIndexProp = "lilaHeaderEntry";
constexpr const char* kMaskActions[] = {"select.mask.mesh", "select.mask.curve", "select.mask.joint", "select.mask.light", "select.mask.locator", "select.mask.other"};

QString hdrLabel(const std::string& id)
{
    const Action* a = actions::find(id);
    return QString::fromStdString(a ? a->label : id);
}

QString hdrTip(const std::string& id)
{
    return QString::fromStdString(actions::tooltip(id));
}

std::string hdrSnapPref(Tool t, const char* leaf)
{
    return std::string("/manipulator/snap/") + tools::snapKey(t) + "/" + leaf;
}

QString hdrStepText(double v)
{
    return QString::number(v, 'g', 6);
}

QString hdrCameraName(const ViewportWidget* v)
{
    if (const scene::Node* n = scene::Graph::current().node(v->lookThroughCamera()))
        return QString::fromStdString(n->name());
    const std::string name = v->viewName();
    if (name == "persp")
        return "Perspective";
    QString s = QString::fromStdString(name);
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

scene::Node* hdrLeadMesh()
{
    scene::Graph& g = scene::Graph::current();
    for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().isA("mesh"))
            return n;
    return nullptr;
}

/** True when the scene can use something needing one of `needs` (viewportContext), or the toggle `id` is on. */
bool hdrRelevant(uint32_t needs, const std::string& id, const ViewportWidget* view)
{
    const uint32_t present = generalPref(vd::kHideUnusedItems, true) ? viewportContext::present() : ~0u;
    return (present & needs) || viewportActionState(id, view).value_or(false);
}

/** The thin vertical rule between groups. */
class HeaderRule final : public QWidget
{
public:
    explicit HeaderRule(QWidget* parent) : QWidget(parent) { setFixedSize(9, 16); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(QRect(width() / 2, 0, 1, height()), themeColor("rule"));
    }
};

/**
 * A popover menu whose checkable rows stay open when clicked, so several toggles can be set in one visit. Rows tagged
 * with kStateProp re-read their state from the action after every click.
 */
class HeaderMenu final : public QMenu
{
public:
    HeaderMenu(ViewportWidget* view, QWidget* parent, bool stayOpen = true) : QMenu(parent), mView(view), mStayOpen(stayOpen)
    {
        setToolTipsVisible(true);
    }

    void sync()
    {
        for (QAction* a : actions())
            if (QString id = a->property(kStateProp).toString(); !id.isEmpty())
                if (std::optional<bool> on = viewportActionState(id.toStdString(), mView))
                {
                    QSignalBlocker block(a);
                    a->setChecked(*on);
                }
    }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        QAction* a = actionAt(e->position().toPoint());
        if (mStayOpen && e->button() == Qt::LeftButton && a && a->isCheckable() && a->isEnabled() && !a->menu() && !qobject_cast<QWidgetAction*>(a))
        {
            a->trigger();
            sync();
            return;
        }
        QMenu::mouseReleaseEvent(e);
    }

private:
    QPointer<ViewportWidget> mView;
    bool mStayOpen;
};

/** A step value: right-aligned, unit inside, no trailing zeros. */
class HeaderStepField final : public QDoubleSpinBox
{
public:
    HeaderStepField(double lo, double hi, const QString& unit, QWidget* parent) : QDoubleSpinBox(parent)
    {
        setRange(lo, hi);
        setDecimals(4);
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setKeyboardTracking(false);
        if (!unit.isEmpty())
            setSuffix(unit.startsWith(QChar(0x00B0)) ? unit : " " + unit);
        setFixedWidth(84);
    }

    QString textFromValue(double v) const override { return hdrStepText(v); }
};

/** A labelled row inside a popover: the label right-aligned in a fixed column, then the controls the caller adds. */
QHBoxLayout* hdrRow(QMenu* menu, const QString& label, QWidget** holderOut = nullptr)
{
    auto* holder = new QWidget(menu);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(8, 2, 8, 2);
    row->setSpacing(4);
    if (!label.isNull())
    {
        auto* l = new QLabel(label, holder);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->setFixedWidth(kRowLabelWidth);
        row->addWidget(l);
    }
    auto* a = new QWidgetAction(menu);
    a->setDefaultWidget(holder);
    menu->addAction(a);
    if (holderOut)
        *holderOut = holder;
    return row;
}

/**
 * The rotate increments as two columns in one menu entry: common angles, and divisions of a turn (360/n). Checkable
 * flat buttons stand in for menu items, and a click closes the menu.
 */
QWidgetAction* hdrRotateColumns(QMenu* menu, double current, std::function<void(double)> pick)
{
    auto* holder = new QWidget(menu);
    auto* grid = new QGridLayout(holder);
    grid->setContentsMargins(4, 2, 4, 2);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(0);
    auto header = [&](const QString& text, int col) {
        auto* l = new QLabel(text, holder);
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, themeColor("dimText"));
        l->setPalette(pal);
        l->setContentsMargins(6, 2, 6, 2);
        grid->addWidget(l, 0, col);
    };
    header("Angle", 0);
    header("Divisions of 360", 1);
    auto add = [&](double v, const QString& text, int row, int col) {
        auto* b = new QToolButton(holder);
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(std::abs(v - current) < 1e-9);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(b, &QToolButton::clicked, menu, [menu, pick, v] {
            pick(v);
            menu->close();
        });
        grid->addWidget(b, row, col);
    };
    const QString deg(QChar(0x00B0));
    int row = 1;
    for (double v : gizmo::kRotateSteps)
        add(v, hdrStepText(v) + deg, row++, 0);
    row = 1;
    for (int n : gizmo::kTurnDivisions)
    {
        double v = gizmo::turnDivision(n);
        add(v, hdrStepText(v) + deg + "  (360/" + QString::number(n) + ")", row++, 1);
    }
    grid->setRowStretch(row, 1);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(holder);
    return action;
}

/** A chevron beside a step field listing preset steps; picking one sets the field. */
QToolButton* hdrPresetButton(QWidget* parent, QDoubleSpinBox* field, std::vector<double> steps, bool rotate, const QString& unit)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon("chevron-down"));
    b->setIconSize(QSize(14, 14));
    b->setToolTip("Presets");
    b->setPopupMode(QToolButton::InstantPopup);
    b->setFocusPolicy(Qt::NoFocus);
    auto* menu = new QMenu(b);
    b->setMenu(menu);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, field, steps, rotate, unit] {
        menu->clear();
        const double current = field->value();
        auto pick = [field](double v) { field->setValue(v); };
        if (rotate)
            menu->addAction(hdrRotateColumns(menu, current, pick));
        for (double v : steps)
        {
            QAction* a = menu->addAction(hdrStepText(v) + unit);
            a->setCheckable(true);
            a->setChecked(std::abs(v - current) < 1e-9);
            QObject::connect(a, &QAction::triggered, menu, [pick, v] { pick(v); });
        }
    });
    return b;
}

void hdrSetRendererSetting(int panel, const std::string& key, json value)
{
    try
    {
        ops::execute("viewportRendererSetting", {{"args", {key, std::move(value)}}, {"panel", panel}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** The settings of the renderer a viewport draws with, applied as they are edited. */
void hdrShowRendererSettings(QWidget* parent, int panel)
{
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromStdString(viewportRenderers::selected(panel)) + " Settings");
    auto* form = new QFormLayout(dialog);
    for (const viewportRenderers::Setting& s : viewportRenderers::settings(panel))
    {
        const QString label = QString::fromStdString(s.name.empty() ? s.key : s.name);
        const std::string key = s.key;
        QWidget* field = nullptr;
        if (s.value.is_boolean())
        {
            auto* box = new QCheckBox(dialog);
            box->setChecked(s.value.get<bool>());
            QObject::connect(box, &QCheckBox::toggled, dialog, [panel, key](bool on) { hdrSetRendererSetting(panel, key, on); });
            field = box;
        }
        else if (s.value.is_number_integer())
        {
            auto* spin = new QSpinBox(dialog);
            spin->setRange(std::numeric_limits<int>::lowest(), std::numeric_limits<int>::max());
            spin->setValue(s.value.get<int>());
            QObject::connect(spin, &QSpinBox::editingFinished, dialog, [panel, key, spin] { hdrSetRendererSetting(panel, key, spin->value()); });
            field = spin;
        }
        else if (s.value.is_number())
        {
            auto* spin = new QDoubleSpinBox(dialog);
            spin->setRange(-1e9, 1e9);
            spin->setDecimals(4);
            spin->setValue(s.value.get<double>());
            QObject::connect(spin, &QDoubleSpinBox::editingFinished, dialog, [panel, key, spin] { hdrSetRendererSetting(panel, key, spin->value()); });
            field = spin;
        }
        else if (s.value.is_string())
        {
            auto* edit = new QLineEdit(QString::fromStdString(s.value.get<std::string>()), dialog);
            QObject::connect(edit, &QLineEdit::editingFinished, dialog, [panel, key, edit] { hdrSetRendererSetting(panel, key, edit->text().toStdString()); });
            field = edit;
        }
        if (field)
        {
            field->setToolTip(QString::fromStdString(key));
            form->addRow(label, field);
        }
    }
    dialog->show();
}

/** A submenu filled each time it opens. */
QMenu* hdrSubmenu(QMenu* m, const QString& title, std::function<void(QMenu*)> fill)
{
    QMenu* sub = m->addMenu(title);
    sub->setToolTipsVisible(true);
    QObject::connect(sub, &QMenu::aboutToShow, sub, [sub, fill] {
        sub->clear();
        fill(sub);
    });
    return sub;
}

/** An exclusive set of actions as radio rows. */
void hdrRadioRows(QMenu* m, const std::vector<std::string>& ids, const ViewportWidget* view, std::function<void()> before)
{
    auto* group = new QActionGroup(m);
    for (const std::string& id : ids)
    {
        QAction* a = m->addAction(hdrLabel(id));
        a->setCheckable(true);
        a->setChecked(viewportActionState(id, view).value_or(false));
        a->setToolTip(hdrTip(id));
        a->setProperty(kStateProp, QString::fromStdString(id));
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, m, [id, before] {
            if (before)
                before();
            actions::run(id);
        });
    }
}

/** What Shift+click on a header action button opens instead of running it, for the buttons that have one. */
QString hdrShiftHint(const std::string& action)
{
    if (action == "viewport.toggleImagePlanes")
        return "Reference Images menu";
    if (action == "view.presenter")
        return "edit the Presenter layout";
    return {};
}

bool hdrShiftClick(QToolButton* b, const std::string& action)
{
    if (action == "viewport.toggleImagePlanes")
    {
        QMenu* menu = tearoff::findMenu(mainWindow(), {"Create", "Reference Images"});
        if (!menu)
            return false;
        menu->popup(b->mapToGlobal(QPoint(0, b->height())));
        return true;
    }
    if (action == "view.presenter" && actions::find("view.presenterLayout"))
    {
        actions::run("view.presenterLayout");
        return true;
    }
    return false;
}

const char* const kModeActions[] = {"select.objectMode", "select.vertexMode", "select.edgeMode", "select.faceMode"};
const char* const kShadingActions[] = {"viewport.wireframe", "viewport.shaded", "viewport.textured"};
} // namespace

ViewportHeader::ViewportHeader(ViewportWidget* view, QWidget* parent) : QWidget(parent), mView(view)
{
    setObjectName("viewportHeader");
    setFixedHeight(kHeaderHeight);
    // Painted with the app palette's window colour, so it follows theme changes without a style sheet.
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);

    buildItems();
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) { contextMenu(at); });

    mChevron = new QToolButton(this);
    mChevron->setIcon(icon("chevrons-right"));
    mChevron->setIconSize(QSize(kHeaderIcon, kHeaderIcon));
    mChevron->setToolTip("More viewport options");
    mChevron->setPopupMode(QToolButton::InstantPopup);
    mChevron->setFocusPolicy(Qt::NoFocus);
    auto* overflow = new HeaderMenu(mView, mChevron);
    mChevron->setMenu(overflow);
    optionBox::decorateMenu(overflow);
    connect(overflow, &QMenu::aboutToShow, this, [this, overflow] {
        activateView();
        overflow->clear();
        for (const Item& item : mItems)
            if (item.overflow && item.widget->isHidden())
            {
                if (!overflow->isEmpty())
                    overflow->addSeparator();
                item.overflow(overflow);
            }
    });

    mSymmetryRefresh = new QTimer(this);
    mSymmetryRefresh->setSingleShot(true);
    // Value changes arrive per drag step, so coalesce them; the label only needs the latest state.
    mSymmetryRefresh->setInterval(30);
    connect(mSymmetryRefresh, &QTimer::timeout, this, &ViewportHeader::refreshSymmetry);
    mGraphListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Renamed && c != scene::Change::Reparented)
            mSymmetryRefresh->start();
        else
            refresh();
    });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file != "general")
            return;
        if (viewportHeader::config() != mConfig)
            QMetaObject::invokeMethod(this, [this] { rebuildItems(); }, Qt::QueuedConnection);
        else
            refresh();
    });
    mToolsListener = tools::addListener([this] { refresh(); });
    mComponentListener = scene::components::addListener([this] { refresh(); });
    mRendererListener = viewportRenderers::addListener([this](int panel) {
        if (panel == mView->hudPanel())
            refresh();
    });
    connect(mView, &ViewportWidget::viewStateChanged, this, &ViewportHeader::refresh);
    refresh();
    refreshTooltips();
}

ViewportHeader::~ViewportHeader()
{
    PrefStore::get().removeOnChanged(mPrefListener);
    tools::removeListener(mToolsListener);
    scene::components::removeListener(mComponentListener);
    scene::Graph::current().removeListener(mGraphListener);
    viewportRenderers::removeListener(mRendererListener);
}

void ViewportHeader::add(QWidget* w, bool, int keep, std::function<void(QMenu*)> overflow, std::vector<QToolButton*> labelled)
{
    // The side comes from the layout being built, whatever a builder asks for.
    w->setProperty(kEntryIndexProp, mEntryIndex);
    mItems.push_back({w, mRight, keep, std::move(overflow), std::move(labelled)});
}

void ViewportHeader::addSeparator(bool)
{
    auto* rule = new HeaderRule(this);
    rule->setProperty(kEntryIndexProp, mEntryIndex);
    mItems.push_back({rule, mRight, 0, nullptr, {}});
}

void ViewportHeader::buildItems()
{
    mConfig = viewportHeader::config();
    for (const char* side : {"left", "right"})
    {
        mRight = std::string(side) == "right";
        int index = 0;
        for (const json& e : mConfig.value(side, json::array()))
        {
            mEntryIndex = (mRight ? 1000 : 0) + index++;
            buildEntry(e);
        }
    }
    mRight = false;
    mEntryIndex = -1;
}

void ViewportHeader::rebuildItems()
{
    for (Item& item : mItems)
        delete item.widget;
    mItems.clear();
    mActionButtons.clear();
    mModes = nullptr;
    mShading = nullptr;
    mSymmetry = nullptr;
    mSoftSelect = nullptr;
    mCamera = nullptr;
    mQuad = nullptr;
    buildItems();
    for (Item& item : mItems)
        item.widget->show();
    refresh();
    refreshTooltips();
    layoutItems();
    updateGeometry();
}

void ViewportHeader::buildEntry(const json& e)
{
    if (e.is_string())
    {
        const std::string id = e.get<std::string>();
        if (id == viewportHeader::kSeparator)
            addSeparator(mRight);
        else if (id == "componentMode")
            buildModes();
        else if (id == "shading")
            buildShading();
        else if (id == "symmetry")
            buildSymmetry();
        else if (id == "softSelect")
            buildSoftSelect();
        else if (id == "selectionMasks")
            buildSelectionMasks();
        else if (id == "camera")
            buildCamera();
        else if (id == "snap")
        {
            QToolButton* snap = popover("magnet", "Snap", [this](QMenu* m) { fillSnapMenu(m); });
            add(snap, true, 4, [this](QMenu* m) { hdrSubmenu(m, "Snap", [this](QMenu* s) { fillSnapMenu(s); }); }, {snap});
        }
        else if (id == "overlays")
        {
            QToolButton* overlays = popover("eye", "Overlays", [this](QMenu* m) { fillOverlaysMenu(m); });
            optionBox::decorateMenu(overlays->menu());
            add(overlays, true, 7, [this](QMenu* m) { hdrSubmenu(m, "Overlays", [this](QMenu* s) { fillOverlaysMenu(s); }); }, {overlays});
        }
        else if (id == "lighting")
        {
            QToolButton* lighting = popover("sun", "Lighting", [this](QMenu* m) { fillLightingMenu(m); });
            add(lighting, true, 6, [this](QMenu* m) { hdrSubmenu(m, "Lighting", [this](QMenu* s) { fillLightingMenu(s); }); }, {lighting});
        }
        else if (id == "jointSize")
        {
            QToolButton* joints = popover("bone", "Joint Size", [this](QMenu* m) { addJointSizeRow(m); }, false);
            add(joints, true, 2, [this](QMenu* m) { addJointSizeRow(m); }, {joints});
        }
        else if (id == "renderer")
        {
            QToolButton* renderer = popover("aperture", "Renderer", [this](QMenu* m) { fillRendererMenu(m); }, false);
            add(renderer, true, 3, [this](QMenu* m) { hdrSubmenu(m, "Renderer", [this](QMenu* s) { fillRendererMenu(s); }); }, {renderer});
        }
        return;
    }
    if (!e.is_object())
        return;
    if (e.contains("action"))
    {
        // Built even when the action is not registered yet: headers are made before the window registers its actions.
        const std::string id = e.value("action", std::string());
        if (id.empty())
            return;
        QToolButton* b = actionButton(id.c_str(), nullptr, 2);
        if (id == "viewport.toggleQuad")
            mQuad = b;
        return;
    }
    if (e.contains("menu"))
    {
        const QString title = QString::fromStdString(e.value("menu", std::string("Menu")));
        const std::string iconName = e.value("icon", std::string("menu-2"));
        const json items = e.value("items", json::array());
        QToolButton* b = popover(iconName.c_str(), title, [this, items](QMenu* m) { fillCommandMenu(m, items); });
        add(b, mRight, 3, [this, title, items](QMenu* m) { hdrSubmenu(m, title, [this, items](QMenu* s) { fillCommandMenu(s, items); }); }, {b});
    }
}

void ViewportHeader::buildSelectionMasks()
{
    // Always expanded: what a click can pick is state you need to see at a glance, not look up in a menu.
    auto* group = new QWidget(this);
    auto* row = new QHBoxLayout(group);
    row->setContentsMargins(0, 0, 0, 0);
    // Independent toggles, so spaced like separate buttons rather than joined like a segmented choice.
    row->setSpacing(kHeaderGap);
    std::vector<QToolButton*> buttons;
    for (const char* id : kMaskActions)
    {
        QToolButton* b = button(actions::iconFor(id).empty() ? "filter" : actions::iconFor(id).c_str(), {});
        b->setCheckable(true);
        const std::string action = id;
        connect(b, &QToolButton::clicked, this, [this, action] {
            activateView();
            actions::run(action);
            refresh();
        });
        mActionButtons.emplace_back(b, action);
        row->addWidget(b);
        buttons.push_back(b);
    }
    add(group, mRight, 6, [this](QMenu* m) {
        m->addSection("Selectable");
        for (const char* id : kMaskActions)
        {
            QAction* a = m->addAction(hdrLabel(id));
            a->setCheckable(true);
            a->setChecked(viewportActionState(id, mView).value_or(false));
            const std::string action = id;
            connect(a, &QAction::triggered, this, [this, action] {
                activateView();
                actions::run(action);
            });
        }
    });
}

void ViewportHeader::fillCommandMenu(QMenu* m, const json& items)
{
    m->setToolTipsVisible(true);
    for (const json& item : items)
    {
        if (!item.is_string())
            continue;
        const std::string id = item.get<std::string>();
        if (id == viewportHeader::kSeparator)
        {
            m->addSeparator();
            continue;
        }
        if (id == "jointSize")
        {
            addJointSizeRow(m);
            continue;
        }
        if (!actions::find(id))
            continue;
        std::optional<bool> on = viewportActionState(id, mView);
        if (!on)
            if (const Action* act = actions::find(id); act && act->checked)
                on = act->checked();
        // A menu row draws an icon in place of its check mark, so toggles go without one and read alike.
        QAction* a = on ? m->addAction(hdrLabel(id)) : m->addAction(icon(QString::fromStdString(actions::iconFor(id))), hdrLabel(id));
        a->setCheckable(on.has_value());
        a->setChecked(on.value_or(false));
        a->setToolTip(hdrTip(id));
        a->setData(QString::fromStdString(id));
        a->setProperty(kStateProp, QString::fromStdString(id));
        connect(a, &QAction::triggered, this, [this, id] {
            activateView();
            actions::run(id);
        });
    }
    if (m->isEmpty())
        m->addAction("Empty: add commands in Customize Viewport Header")->setEnabled(false);
    optionBox::decorateMenu(m);
}

void ViewportHeader::addJointSizeRow(QMenu* m)
{
    static constexpr const char* kPref = "/rig/jointDisplayScale";
    QHBoxLayout* row = hdrRow(m, "Joint Size");
    QDoubleSpinBox* size = controls::valueField(0.01, 100.0, 2, QString(QChar(0x00D7)), row->parentWidget());
    size->setSingleStep(0.1);
    size->setToolTip("How large joints draw in every view");
    setSpinDefault(size, 1.0);
    size->setValue(PrefStore::get().value<double>("general", kPref, 1.0));
    connect(size, &QDoubleSpinBox::valueChanged, this, [](double v) {
        PrefStore::get().set("general", kPref, v);
        viewportTools::requestRedraw();
    });
    row->addWidget(size, 1);
}

void ViewportHeader::contextMenu(const QPoint& at)
{
    QMenu menu;
    int entry = -1;
    for (QWidget* w = childAt(at); w && w != this; w = w->parentWidget())
        if (QVariant v = w->property(kEntryIndexProp); v.isValid())
        {
            entry = v.toInt();
            break;
        }
    if (entry >= 0)
        menu.addAction(icon("x"), "Remove from Header", this, [entry] {
            json c = viewportHeader::config();
            const char* side = entry >= 1000 ? "right" : "left";
            const size_t index = size_t(entry % 1000);
            if (c[side].is_array() && index < c[side].size())
            {
                c[side].erase(index);
                viewportHeader::setConfig(c);
            }
        });
    menu.addAction(icon("layout-navbar"), "Customize Header...", this, [this] { viewportHeader::openEditor(this); });
    menu.addAction(icon("restore"), "Reset Header", this, [] { actions::run(nlohmann::json("viewport.resetHeader")); });
    menu.exec(mapToGlobal(at));
}

QToolButton* ViewportHeader::button(const char* iconName, const QString& text)
{
    auto* b = new QToolButton(this);
    b->setIcon(icon(iconName));
    b->setIconSize(QSize(kHeaderIcon, kHeaderIcon));
    b->setText(text);
    b->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QToolButton* ViewportHeader::popover(const char* iconName, const QString& text, std::function<void(QMenu*)> fill, bool stayOpen)
{
    QToolButton* b = button(iconName, text);
    b->setToolTip(text);
    b->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new HeaderMenu(mView, b, stayOpen);
    b->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu, fill] {
        activateView();
        menu->clear();
        fill(menu);
    });
    return b;
}

QToolButton* ViewportHeader::actionButton(const char* id, const char* iconName, int keep)
{
    const std::string fallbackIcon = actions::iconFor(id);
    if (!iconName)
        iconName = fallbackIcon.empty() ? "point" : fallbackIcon.c_str();
    QToolButton* b = button(iconName, {});
    const std::string action = id;
    connect(b, &QToolButton::clicked, this, [this, b, action] {
        activateView();
        if ((QApplication::keyboardModifiers() & Qt::ShiftModifier) && hdrShiftClick(b, action))
            return;
        actions::run(action);
        refresh();
    });
    mActionButtons.emplace_back(b, action);
    const QString overflowIcon = QString::fromUtf8(iconName);
    add(b, true, keep, [this, action, overflowIcon](QMenu* m) {
        QAction* a = m->addAction(icon(overflowIcon), hdrLabel(action));
        a->setToolTip(hdrTip(action));
        connect(a, &QAction::triggered, this, [this, action] {
            activateView();
            actions::run(action);
        });
    });
    return b;
}

void ViewportHeader::buildModes()
{
    using scene::ComponentMode;
    mModes = new controls::Segmented(this);
    const std::pair<ComponentMode, const char*> modes[] = {
        {ComponentMode::Object, "Object"}, {ComponentMode::Vertex, "Vertex"}, {ComponentMode::Edge, "Edge"}, {ComponentMode::Face, "Face"}};
    for (int i = 0; i < 4; ++i)
    {
        QToolButton* b = mModes->add(modes[i].second, QString::fromStdString(actions::iconFor(kModeActions[i])), hdrTip(kModeActions[i]),
                                     int(modes[i].first));
        b->setIconSize(QSize(kHeaderIcon, kHeaderIcon));
        b->setFocusPolicy(Qt::NoFocus);
        b->setProperty("lilaAction", kModeActions[i]);
    }
    connect(mModes, &controls::Segmented::picked, this, [this] {
        activateView();
        for (QToolButton* b : mModes->findChildren<QToolButton*>())
            if (b->isChecked())
                actions::run(b->property("lilaAction").toString().toStdString());
        refresh();
    });
    add(
        mModes, false, 10,
        [this](QMenu* m) {
            m->addSection("Mode");
            hdrRadioRows(m, {std::begin(kModeActions), std::end(kModeActions)}, mView, [this] { activateView(); });
        },
        [this] {
            const QList<QToolButton*> segments = mModes->findChildren<QToolButton*>();
            return std::vector<QToolButton*>(segments.begin(), segments.end());
        }());
}

void ViewportHeader::buildShading()
{
    mShading = new controls::Segmented(this);
    for (int i = 0; i < 3; ++i)
    {
        QToolButton* b = mShading->add({}, QString::fromStdString(actions::iconFor(kShadingActions[i])), hdrTip(kShadingActions[i]), i);
        b->setIconSize(QSize(kHeaderIcon, kHeaderIcon));
        b->setFocusPolicy(Qt::NoFocus);
    }
    connect(mShading, &controls::Segmented::picked, this, [this](int i) {
        activateView();
        actions::run(kShadingActions[i]);
        refresh();
    });
    add(mShading, false, 8, [this](QMenu* m) {
        m->addSection("Shading");
        hdrRadioRows(m, {std::begin(kShadingActions), std::end(kShadingActions)}, mView, [this] { activateView(); });
    });
}

void ViewportHeader::buildSymmetry()
{
    mSymmetry = button("flip-vertical", "Symmetry");
    mSymmetry->setCheckable(true);
    mSymmetry->setPopupMode(QToolButton::MenuButtonPopup);
    auto* menu = new HeaderMenu(mView, mSymmetry);
    mSymmetry->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        activateView();
        menu->clear();
        fillSymmetryMenu(menu);
    });
    connect(mSymmetry, &QToolButton::clicked, this, [this] {
        activateView();
        actions::run("mesh.toggleSymmetry");
        refreshSymmetry();
    });
    add(mSymmetry, false, 6, [this](QMenu* m) { hdrSubmenu(m, "Symmetry", [this](QMenu* s) { fillSymmetryMenu(s); }); }, {mSymmetry});
}

void ViewportHeader::buildSoftSelect()
{
    mSoftSelect = button("ripple", "Soft Select");
    mSoftSelect->setCheckable(true);
    mSoftSelect->setPopupMode(QToolButton::MenuButtonPopup);
    auto* menu = new HeaderMenu(mView, mSoftSelect);
    mSoftSelect->setMenu(menu);
    // Built once on first open: the falloff editor keeps itself in step with the prefs.
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        if (menu->isEmpty())
            fillSoftSelectMenu(menu, false);
    });
    connect(mSoftSelect, &QToolButton::clicked, this, [this] {
        activateView();
        actions::run("softSelect.toggle");
        refresh();
    });
    add(mSoftSelect, false, 5, [this](QMenu* m) { hdrSubmenu(m, "Soft Select", [this](QMenu* s) { fillSoftSelectMenu(s, true); }); },
        {mSoftSelect});
}

void ViewportHeader::buildCamera()
{
    mCamera = popover("camera", "Camera", [this](QMenu* m) { fillCameraMenu(m); }, false);
    // Right-click selects the camera node this view looks through, as Maya's panel camera button does.
    mCamera->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mCamera, &QWidget::customContextMenuRequested, this, [this] {
        const scene::Node* n = scene::Graph::current().node(mView->lookThroughCamera());
        if (!n)
        {
            log::info("{} is a built-in view; there is no camera node to select.", mView->viewName());
            return;
        }
        try
        {
            UndoChunk chunk("Select Camera");
            ops::execute("selectMode", {{"args", {"object"}}}, CallSource::UI);
            ops::execute("select", {{"args", {n->path()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    });
    add(mCamera, true, 9, [this](QMenu* m) { hdrSubmenu(m, "Camera", [this](QMenu* s) { fillCameraMenu(s); }); }, {mCamera});
}

void ViewportHeader::fillSymmetryMenu(QMenu* menu)
{
    scene::Graph& g = scene::Graph::current();
    scene::SymmetryState st = scene::symmetryState(g, g.selection());
    auto* group = new QActionGroup(menu);
    auto run = [this](const std::string& id) {
        activateView();
        actions::run(id);
        refreshSymmetry();
    };
    QAction* off = menu->addAction(hdrLabel("mesh.symmetryOff"));
    off->setCheckable(true);
    off->setChecked(st.meshes > 0 && st.on == 0);
    off->setEnabled(st.meshes > 0);
    group->addAction(off);
    connect(off, &QAction::triggered, this, [run] { run("mesh.symmetryOff"); });
    for (int i = 0; i < 3; ++i)
    {
        std::string id = std::string("mesh.symmetry") + char('X' + i);
        QAction* item = menu->addAction(hdrLabel(id));
        item->setCheckable(true);
        item->setChecked(st.on > 0 && st.axis == i);
        item->setToolTip(hdrTip(id));
        group->addAction(item);
        connect(item, &QAction::triggered, this, [run, i, id] {
            PrefStore::get().set("general", "/symmetry/axis", std::string(1, char('x' + i)));
            run(id);
        });
    }
    // Radial: the setting, then its count and mirror; picking either turns radial symmetry on with it.
    QAction* radial = menu->addAction(hdrLabel("mesh.symmetryRadial"));
    radial->setCheckable(true);
    radial->setChecked(st.on > 0 && st.axis == scene::SymmetryState::kRadialState);
    radial->setToolTip(hdrTip("mesh.symmetryRadial"));
    group->addAction(radial);
    connect(radial, &QAction::triggered, this, [run] {
        PrefStore::get().set("general", "/symmetry/axis", std::string("radial"));
        run("mesh.symmetryRadial");
    });
    auto setRadial = [this](json flags) {
        scene::Graph& graph = scene::Graph::current();
        json names = json::array();
        for (scene::NodeId id : graph.selection())
            if (const scene::Node* n = graph.node(id); n && n->type().isA("mesh"))
                names.push_back(n->path());
        if (names.empty())
            return;
        flags["args"] = names;
        flags["axis"] = "radial";
        PrefStore::get().set("general", "/symmetry/axis", std::string("radial"));
        activateView();
        try
        {
            ops::execute("setSymmetry", flags, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
        refreshSymmetry();
    };
    QMenu* counts = menu->addMenu("Radial Count");
    for (int count : {2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32})
    {
        QAction* item = counts->addAction(QString::number(count));
        item->setCheckable(true);
        item->setChecked(st.axis == scene::SymmetryState::kRadialState && st.radialCount == count);
        connect(item, &QAction::triggered, this, [setRadial, count] { setRadial({{"count", count}}); });
    }
    QAction* mirrored = menu->addAction("Radial Mirror");
    mirrored->setCheckable(true);
    mirrored->setChecked(st.axis == scene::SymmetryState::kRadialState && st.radialCount > 0 && st.radialMirror);
    mirrored->setToolTip("Also mirror each wedge across a plane through the axis (dihedral symmetry: rims, flowers)");
    connect(mirrored, &QAction::triggered, this, [setRadial](bool on) { setRadial({{"mirror", on}}); });

    menu->addSection("Plane");
    bool custom = false;
    for (scene::NodeId id : g.selection())
        if (const scene::Node* n = g.node(id); n && n->type().isA("mesh") && n->as<int64_t>(scene::MeshNode::kSymmetryPlane) == 1)
            custom = true;
    QAction* fromSel = menu->addAction(hdrLabel("mesh.symmetryPlaneFromSelection"));
    fromSel->setToolTip(hdrTip("mesh.symmetryPlaneFromSelection"));
    fromSel->setEnabled(scene::components::mode() == scene::ComponentMode::Vertex || scene::components::mode() == scene::ComponentMode::Edge);
    connect(fromSel, &QAction::triggered, this, [run] { run("mesh.symmetryPlaneFromSelection"); });
    QAction* atPivot = menu->addAction(hdrLabel("mesh.symmetryPlaneFromPivot"));
    atPivot->setToolTip(hdrTip("mesh.symmetryPlaneFromPivot"));
    atPivot->setCheckable(true);
    atPivot->setChecked(!custom);
    atPivot->setEnabled(custom);
    connect(atPivot, &QAction::triggered, this, [run] { run("mesh.symmetryPlaneFromPivot"); });

    menu->addSection("Selection");
    QAction* symSelect = menu->addAction(hdrLabel("select.toggleSymmetric"));
    symSelect->setToolTip(hdrTip("select.toggleSymmetric"));
    symSelect->setCheckable(true);
    symSelect->setChecked(PrefStore::get().value<bool>("general", "/select/symmetric", true));
    connect(symSelect, &QAction::triggered, this, [] { actions::run("select.toggleSymmetric"); });
    QAction* symLine = menu->addAction(hdrLabel("viewport.toggleSymmetryLine"));
    symLine->setToolTip(hdrTip("viewport.toggleSymmetryLine"));
    symLine->setCheckable(true);
    symLine->setChecked(PrefStore::get().value<bool>("general", vd::kSymmetryEdges, true));
    connect(symLine, &QAction::triggered, this, [] { actions::run("viewport.toggleSymmetryLine"); });
    QAction* symmetrize = menu->addAction(hdrLabel("select.symmetrize"));
    symmetrize->setToolTip(hdrTip("select.symmetrize"));
    symmetrize->setEnabled(scene::components::mode() != scene::ComponentMode::Object);
    connect(symmetrize, &QAction::triggered, this, [this] {
        activateView();
        actions::run("select.symmetrize");
    });
}

void ViewportHeader::fillSoftSelectMenu(QMenu* m, bool withToggle)
{
    namespace ss = scene::softSelect;
    if (withToggle)
    {
        QAction* on = m->addAction(hdrLabel("softSelect.toggle"));
        on->setCheckable(true);
        on->setChecked(generalPref(ss::kEnabled, false));
        on->setToolTip(hdrTip("softSelect.toggle"));
        connect(on, &QAction::triggered, this, [] { actions::run("softSelect.toggle"); });
    }
    QHBoxLayout* row = hdrRow(m, "Radius");
    QDoubleSpinBox* radius = controls::valueField(0.01, 100000.0, 2, "cm", row->parentWidget());
    radius->setToolTip("Soft selection falloff radius, world space (hold B and drag in the viewport to set it)");
    setSpinDefault(radius, 25.0);
    radius->setValue(PrefStore::get().value<double>("general", ss::kRadius, 25.0));
    connect(radius, &QDoubleSpinBox::valueChanged, this, [](double r) { PrefStore::get().set("general", ss::kRadius, r); });
    row->addWidget(radius, 1);
    connect(m, &QMenu::aboutToShow, radius, [radius] {
        if (radius->hasFocus())
            return;
        QSignalBlocker block(radius);
        radius->setValue(PrefStore::get().value<double>("general", ss::kRadius, 25.0));
    });

    m->addSection("Falloff");
    auto* panel = new SoftSelectPanel(m);
    panel->setFixedWidth(300);
    auto* a = new QWidgetAction(m);
    a->setDefaultWidget(panel);
    m->addAction(a);
}

void ViewportHeader::fillSnapMenu(QMenu* m)
{
    m->addSection("Increments");
    struct Inc
    {
        Tool tool;
        const char* label;
        QString unit;
        std::vector<double> steps;
    };
    const Inc incs[] = {{Tool::Move, "Move", "cm", {0.001, 0.01, 0.05, 0.1, 0.5, 1, 5, 10, 50, 100, 500, 1000}},
                        {Tool::Rotate, "Rotate", QString(QChar(0x00B0)), {}},
                        {Tool::Scale, "Scale", QString(QChar(0x00D7)), {0.0625, 0.1, 0.125, 0.25, 0.5, 1}}};
    for (const Inc& inc : incs)
    {
        const Tool t = inc.tool;
        const std::string action = std::string("snap.") + tools::snapKey(t) + "Increment";
        QWidget* holder = nullptr;
        QHBoxLayout* row = hdrRow(m, QString(), &holder);
        auto* box = new QCheckBox(inc.label, holder);
        box->setFixedWidth(kRowLabelWidth);
        box->setChecked(PrefStore::get().value<bool>("general", hdrSnapPref(t, "enabled"), false));
        box->setToolTip(hdrTip(action));
        setCheckDefault(box, false);
        auto* field = new HeaderStepField(1e-4, 1e6, inc.unit, holder);
        field->setValue(tools::incrementSnap(t).step);
        setSpinDefault(field, t == Tool::Rotate ? gizmo::kDefaultRotateStep : t == Tool::Scale ? gizmo::kDefaultScaleStep : gizmo::kDefaultMoveStep);
        field->setToolTip(QString(inc.label) + " increment");
        const std::string stepKey = hdrSnapPref(t, "step");
        connect(field, &QDoubleSpinBox::valueChanged, this, [stepKey](double v) {
            if (v > 0.0)
                PrefStore::get().set("general", stepKey, v);
        });
        QToolButton* presets = hdrPresetButton(holder, field, inc.steps, t == Tool::Rotate, inc.unit == "cm" ? QString(" cm") : inc.unit);
        auto enable = [field, presets](bool on) {
            field->setEnabled(on);
            presets->setEnabled(on);
        };
        enable(box->isChecked());
        connect(box, &QCheckBox::toggled, this, [this, action, t, enable](bool on) {
            if (PrefStore::get().value<bool>("general", hdrSnapPref(t, "enabled"), false) != on)
                actions::run(action);
            enable(on);
        });
        row->addWidget(box);
        row->addWidget(field, 1);
        row->addWidget(presets);
    }
    const std::string absKey = hdrSnapPref(Tool::Move, "absolute");
    QAction* abs = m->addAction("Snap to World Grid");
    abs->setCheckable(true);
    abs->setChecked(PrefStore::get().value<bool>("general", absKey, false));
    abs->setToolTip("Move increments land on multiples of the increment instead of stepping from where the drag started");
    connect(abs, &QAction::triggered, this, [absKey](bool on) { PrefStore::get().set("general", absKey, on); });

    m->addSection("Snap To");
    struct To
    {
        SnapFlags flag;
        const char* label;
        const char* hold;
    };
    for (const To& to : {To{SnapGrid, "Grid", "snap.gridOn"}, To{SnapEdge, "Edge", "snap.edgeOn"}, To{SnapVertex, "Vertex", "snap.vertexOn"},
                         To{SnapSurface, "Surface", "snap.surfaceOn"}})
    {
        QAction* a = m->addAction(to.label);
        a->setCheckable(true);
        a->setChecked(tools::snaps() & to.flag);
        a->setToolTip(QString("Snap to %1, until turned off here\n").arg(QString(to.label).toLower()) + hdrTip(to.hold));
        const SnapFlags flag = to.flag;
        connect(a, &QAction::triggered, this, [flag](bool on) { tools::setSnap(flag, on); });
    }

    if (RetopoDrawTool* tool = activeRetopoDraw())
    {
        m->addSection("Retopo Draw");
        QAction* space = m->addAction(hdrLabel("retopo.toggleSnapSpace"));
        space->setCheckable(true);
        space->setChecked(RetopoDrawTool::snapObjectSpace());
        space->setToolTip(hdrTip("retopo.toggleSnapSpace"));
        space->setProperty(kStateProp, "retopo.toggleSnapSpace");
        connect(space, &QAction::triggered, this, [] { actions::run("retopo.toggleSnapSpace"); });

        QWidget* holder = nullptr;
        QHBoxLayout* row = hdrRow(m, "Line Angle", &holder);
        auto* angle = new HeaderStepField(0.1, 180.0, QString(QChar(0x00B0)), holder);
        angle->setValue(RetopoDrawTool::snapAngle());
        setSpinDefault(angle, 15.0);
        angle->setToolTip("Retopo Draw snap angle: Shift places the next dot on a line from the last one at a multiple of this");
        connect(angle, &QDoubleSpinBox::valueChanged, this, [](double v) { RetopoDrawTool::setSnapAngle(v); });
        row->addWidget(angle, 1);
        row->addWidget(hdrPresetButton(holder, angle, {}, true, QString(QChar(0x00B0))));

        const scene::Node* ref = scene::Graph::current().node(tool->reference());
        QAction* surface = m->addAction(icon("target"), "Reference: " + (ref ? QString::fromStdString(ref->name()) : QString("None")));
        surface->setToolTip(hdrTip("retopo.setReference"));
        connect(surface, &QAction::triggered, this, [this] {
            activateView();
            actions::run("retopo.setReference");
        });
    }
}

void ViewportHeader::fillOverlaysMenu(QMenu* m)
{
    namespace vc = viewportContext;
    m->setToolTipsVisible(true);
    const bool lilaDraws = viewportRenderers::selected(mView->hudPanel()) == viewportRenderers::kLila;
    auto row = [this, m](const std::string& id, bool enabled = true) { return actionRow(m, id, enabled); };

    m->addSection("Shading");
    row("viewport.wireOnShaded");
    for (const char* id : {"viewport.defaultMaterial", "viewport.greyBaseColor"})
        if (hdrRelevant(vc::Materials, id, mView))
            row(id);
    hdrSubmenu(m, "Vertex Colors", [this](QMenu* s) {
        hdrRadioRows(s, {"viewport.vertexColors.off", "viewport.vertexColors.shaded", "viewport.vertexColors.unlit"}, mView, {});
        s->addSeparator();
        hdrRadioRows(s, {"viewport.colorChannel.rgb", "viewport.colorChannel.r", "viewport.colorChannel.g", "viewport.colorChannel.b",
                         "viewport.colorChannel.a"},
                     mView, {});
        s->addSeparator();
        QAction* gray = s->addAction(hdrLabel("viewport.grayscaleWeights"));
        gray->setCheckable(true);
        gray->setChecked(viewportActionState("viewport.grayscaleWeights", mView).value_or(false));
        connect(gray, &QAction::triggered, this, [] { actions::run("viewport.grayscaleWeights"); });
    });
    row("viewport.monotone");
    row("viewport.toggleBackfaceCulling");
    row("viewport.toggleOutline");
    row("viewport.toggleKuwahara");

    m->addSection("Renderer");
    hdrSubmenu(m, "Renderer", [this](QMenu* s) { fillRendererMenu(s); });
    row("viewport.toggleAA", lilaDraws);
    QMenu* samples = hdrSubmenu(m, "Antialiasing Samples", [this](QMenu* s) {
        hdrRadioRows(s, {"viewport.aaSamples2", "viewport.aaSamples4", "viewport.aaSamples8", "viewport.aaTemporal"}, mView, {});
    });
    samples->setEnabled(lilaDraws && generalPref(vd::kAaEnabled, true));

    m->addSection("Show");
    for (const char* id : {"viewport.show.mesh", "viewport.show.curve", "viewport.show.joint", "viewport.show.light", "viewport.toggleGrid",
                           "viewport.toggleFilmGate", "viewport.toggleResolutionGate", "viewport.toggleImagePlanes"})
        row(id);
    hdrSubmenu(m, "Image Planes", [this](QMenu* s) { imagePlanes::fillCameraMenu(s, mView->viewName()); });
    row("viewport.isolate");
    hdrSubmenu(m, "Level of Detail", [this](QMenu* s) { lodDisplay::fillMenu(s, mView); });

    m->addSection("Selection Display");
    row("viewport.toggleXray");
    if (hdrRelevant(vc::Joints, "rig.xrayJoints", mView))
        row("rig.xrayJoints");

    m->addSection("Aids");
    hdrSubmenu(m, "Mesh Display", [this](QMenu* s) { displayAids::fillMenu(s, mView); });

    m->addSection("Selectable");
    namespace sm = scene::selectionMask;
    for (sm::Kind k : {sm::Kind::Mesh, sm::Kind::Curve, sm::Kind::Joint, sm::Kind::Light, sm::Kind::Locator, sm::Kind::Other})
        row(std::string("select.mask.") + sm::name(k));

    m->addSection("HUD");
    row("viewport.toggleKeys");
    row("viewport.hud.originAxis");
    QMenu* hudMenu = hdrSubmenu(m, "Heads Up Display", [this](QMenu* s) { hud::fillMenu(s, mView); });
    // Decorated after the fill above is connected, so the option box marks go onto the rows it just made.
    optionBox::decorateMenu(hudMenu);
}

QAction* ViewportHeader::actionRow(QMenu* m, const std::string& id, bool enabled)
{
    QAction* a = m->addAction(hdrLabel(id));
    std::optional<bool> on = viewportActionState(id, mView);
    a->setCheckable(on.has_value());
    a->setChecked(on.value_or(false));
    a->setEnabled(enabled);
    a->setToolTip(hdrTip(id));
    a->setData(QString::fromStdString(id));
    a->setProperty(kStateProp, QString::fromStdString(id));
    connect(a, &QAction::triggered, this, [this, id] {
        activateView();
        actions::run(id);
    });
    return a;
}

void ViewportHeader::fillLightingMenu(QMenu* m)
{
    m->setToolTipsVisible(true);
    const bool lilaDraws = viewportRenderers::selected(mView->hudPanel()) == viewportRenderers::kLila;
    static constexpr const char* kModes[] = {"environment", "studio", "sculpt", "flat"};
    static constexpr const char* kModeLabels[] = {"Environment", "Studio", "Sculpt", "Flat"};
    const std::string mode = PrefStore::get().value<std::string>("general", vd::kLighting, "environment");

    QWidget* holder = nullptr;
    QHBoxLayout* modeRow = hdrRow(m, "Mode", &holder);
    auto* modes = new controls::Segmented(holder);
    for (int i = 0; i < 4; ++i)
    {
        const std::string id = std::string("viewport.lighting.") + kModes[i];
        modes->add(kModeLabels[i], {}, hdrTip(id), i);
        if (mode == kModes[i])
            modes->setCurrent(i);
    }
    modes->setEnabled(lilaDraws);
    modeRow->addWidget(modes, 1);

    // One rotation field for whichever light the mode uses: the HDRI turns around the up axis, the rigs around the view.
    QHBoxLayout* rotRow = hdrRow(m, "Rotation", &holder);
    QDoubleSpinBox* rotation = controls::valueField(-360.0, 360.0, 0, QString(QChar(0x00B0)), holder);
    rotation->setSingleStep(5.0);
    setSpinDefault(rotation, 0.0);
    rotation->setToolTip("Turns the light. Shift+Alt+right-drag in the viewport does the same.");
    rotRow->addWidget(rotation, 1);
    QHBoxLayout* tiltRow = hdrRow(m, "Angle", &holder);
    QDoubleSpinBox* tilt = controls::valueField(0.0, 90.0, 0, QString(QChar(0x00B0)), holder);
    tilt->setSingleStep(5.0);
    setSpinDefault(tilt, vd::kDefaultRigTilt);
    tilt->setToolTip("Sculpt lighting: how far the key light sits off your view. 0 lights from the camera, 90 grazes. "
                     "Shift+Alt+drag up or down in the viewport does the same.");
    tiltRow->addWidget(tilt, 1);
    connect(tilt, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", vd::kRigTilt, v); });
    auto showRotation = [rotation, tilt](const std::string& mode) {
        QSignalBlocker b(rotation), t(tilt);
        const char* pref = mode == "environment" ? vd::kEnvRotation : vd::kRigRotation;
        rotation->setValue(PrefStore::get().value<double>("general", pref, 0.0));
        rotation->setEnabled(mode != "flat");
        tilt->setValue(PrefStore::get().value<double>("general", vd::kRigTilt, vd::kDefaultRigTilt));
        tilt->setEnabled(mode == "sculpt");
    };
    showRotation(mode);
    connect(rotation, &QDoubleSpinBox::valueChanged, this, [](double v) {
        const bool env = PrefStore::get().value<std::string>("general", vd::kLighting, "environment") == "environment";
        PrefStore::get().set("general", env ? vd::kEnvRotation : vd::kRigRotation, v);
    });
    connect(modes, &controls::Segmented::picked, this, [this, showRotation](int i) {
        activateView();
        actions::run(std::string("viewport.lighting.") + kModes[i]);
        showRotation(kModes[i]);
    });

    actionRow(m, "viewport.resetLight");

    m->addSection("Environment");
    hdrSubmenu(m, "HDRI", [](QMenu* s) { fillEnvironmentMenu(s); });
    actionRow(m, "viewport.toggleEnvBackground");

    m->addSection("Shadows");
    actionRow(m, "viewport.toggleShadows", lilaDraws);
    actionRow(m, "viewport.toggleAO", lilaDraws);

    m->addSection("Exposure");
    QHBoxLayout* r = hdrRow(m, "Exposure", &holder);
    QDoubleSpinBox* exposure = controls::valueField(-10.0, 10.0, 2, "EV", holder);
    exposure->setSingleStep(0.1);
    setSpinDefault(exposure, 0.0);
    exposure->setValue(PrefStore::get().value<double>("general", vd::kExposure, 0.0));
    exposure->setToolTip("Exposure (stops)");
    connect(exposure, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", vd::kExposure, v); });
    r->addWidget(exposure, 1);
    QHBoxLayout* g = hdrRow(m, "Gamma", &holder);
    QDoubleSpinBox* gamma = controls::valueField(0.1, 4.0, 2, {}, holder);
    gamma->setSingleStep(0.05);
    setSpinDefault(gamma, 1.0);
    gamma->setValue(PrefStore::get().value<double>("general", vd::kGamma, 1.0));
    gamma->setToolTip("Gamma");
    connect(gamma, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", vd::kGamma, v); });
    g->addWidget(gamma, 1);
    actionRow(m, "viewport.resetExposure");

    m->addSeparator();
    actionRow(m, "window.preferences");
}

void ViewportHeader::fillRendererMenu(QMenu* s)
{
    {
        auto* group = new QActionGroup(s);
        const std::string current = viewportRenderers::selected(mView->hudPanel());
        for (const auto& [id, name] : viewportRenderers::choices())
        {
            QAction* a = s->addAction(QString::fromStdString(name));
            a->setCheckable(true);
            a->setChecked(id == current);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, id] {
                activateView();
                try
                {
                    ops::execute("viewportRenderer", {{"args", {id}}, {"panel", mView->hudPanel()}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
        }
        if (!viewportRenderers::settings(mView->hudPanel()).empty())
        {
            s->addSeparator();
            connect(s->addAction("Renderer Settings..."), &QAction::triggered, this, [this] { hdrShowRendererSettings(this, mView->hudPanel()); });
        }
    }
}

void ViewportHeader::fillCameraMenu(QMenu* m)
{
    ui::fillCameraMenu(m, mView, [this] { activateView(); });
    m->addSeparator();
    for (const char* id : {"viewport.cameraSettings", "view.presenter", "view.presenterLayout"})
    {
        QAction* a = m->addAction(icon(QString::fromStdString(actions::iconFor(id))), hdrLabel(id));
        a->setToolTip(hdrTip(id));
        a->setEnabled(actions::isEnabled(nlohmann::json(id)));
        if (std::optional<bool> on = viewportActionState(id, mView))
        {
            a->setCheckable(true);
            a->setChecked(*on);
        }
        connect(a, &QAction::triggered, this, [this, id = std::string(id)] {
            activateView();
            actions::run(id);
        });
    }
}

void ViewportHeader::activateView()
{
    mView->setFocus(Qt::OtherFocusReason);
}

void ViewportHeader::refresh()
{
    if (mModes)
        mModes->setCurrent(int(scene::components::mode()));
    for (int i = 0; i < 3 && mShading; ++i)
        if (viewportActionState(kShadingActions[i], mView).value_or(false))
            mShading->setCurrent(i);
    if (mSoftSelect)
    {
        QSignalBlocker block(mSoftSelect);
        mSoftSelect->setChecked(generalPref(scene::softSelect::kEnabled, false));
    }
    for (auto& [b, id] : mActionButtons)
    {
        std::optional<bool> on = viewportActionState(id, mView);
        if (!on)
            if (const Action* a = actions::find(id); a && a->checked)
                on = a->checked();
        if (on)
        {
            QSignalBlocker block(b);
            b->setCheckable(true);
            b->setChecked(*on);
        }
    }
    const QString camera = hdrCameraName(mView);
    if (mCamera && mCamera->property("lilaLabel").toString() != camera)
    {
        mCamera->setProperty("lilaLabel", camera);
        mCamera->setText(camera);
        mCamera->setToolTip("Camera: " + camera + "\nRight-click to select it");
        layoutItems();
    }
    if (auto* layout = qobject_cast<ViewportLayout*>(parentWidget() ? parentWidget()->parentWidget() : nullptr); layout && mQuad)
    {
        QSignalBlocker block(mQuad);
        mQuad->setChecked(layout->quad());
    }
    refreshSymmetry();
}

void ViewportHeader::refreshSymmetry()
{
    scene::Graph& g = scene::Graph::current();
    const scene::SymmetryState st = scene::symmetryState(g, g.selection());
    scene::Node* lead = hdrLeadMesh();
    const int64_t value = lead ? std::clamp<int64_t>(lead->as<int64_t>(scene::MeshNode::kSymmetry), 0, scene::kSymmetryRadial) : 0;
    static const char* kPlanes[] = {"", "X", "Y", "Z", "Radial"};
    if (!mSymmetry)
        return;
    const QString text = value > 0 ? QString("Symmetry %1").arg(kPlanes[value]) : QString("Symmetry");
    mSymmetry->setEnabled(lead != nullptr);
    {
        QSignalBlocker block(mSymmetry);
        mSymmetry->setChecked(value > 0);
    }
    QString state = !lead           ? QString("No mesh selected")
                    : st.on == 0     ? QString("Off")
                    : st.on < st.meshes ? QString("On for %1 of %2 selected meshes").arg(st.on).arg(st.meshes)
                                     : "On (" + QString(kPlanes[value]) + ")";
    mSymmetry->setToolTip(hdrTip("mesh.toggleSymmetry") + "\nSymmetry: " + state);
    if (mSymmetry->property("lilaLabel").toString() != text)
    {
        mSymmetry->setProperty("lilaLabel", text);
        mSymmetry->setText(text);
        layoutItems();
    }
}

void ViewportHeader::refreshTooltips()
{
    for (auto& [b, id] : mActionButtons)
    {
        const QString shift = hdrShiftHint(id);
        b->setToolTip(shift.isEmpty() ? hdrTip(id) : hdrTip(id) + "\nShift+click: " + shift);
    }
    for (int i = 0; i < 4 && mModes; ++i)
        if (auto* b = mModes->findChildren<QToolButton*>().value(i))
            b->setToolTip(hdrTip(kModeActions[i]));
    for (int i = 0; i < 3 && mShading; ++i)
        if (auto* b = mShading->findChildren<QToolButton*>().value(i))
            b->setToolTip(hdrTip(kShadingActions[i]));
    if (mSoftSelect)
        mSoftSelect->setToolTip(hdrTip("softSelect.toggle") + "\nArrow: radius and falloff");
    refreshSymmetry();
}

QSize ViewportHeader::sizeHint() const
{
    int w = 2 * kHeaderPad;
    for (const Item& item : mItems)
        w += item.widget->sizeHint().width() + kHeaderGap;
    return {w, kHeaderHeight};
}

QSize ViewportHeader::minimumSizeHint() const
{
    return {0, kHeaderHeight};
}

void ViewportHeader::layoutItems()
{
    const size_t n = mItems.size();
    // The width each item's labels add beside their icons, as QToolButton sizes TextBesideIcon (it pads the text by two spaces).
    auto labelWidth = [](const Item& item) {
        int w = 0;
        for (QToolButton* b : item.labelled)
            w += 4 + b->fontMetrics().size(Qt::TextShowMnemonic, b->text()).width() + 2 * b->fontMetrics().horizontalAdvance(QLatin1Char(' '));
        return w;
    };
    auto isCompact = [](const Item& item) { return !item.labelled.empty() && item.labelled.front()->toolButtonStyle() == Qt::ToolButtonIconOnly; };
    auto wantCompact = [this](const Item& item, int level) { return item.widget == mModes ? level >= 1 : level >= 2; };
    std::vector<int> full(n), compact(n);
    for (size_t i = 0; i < n; ++i)
    {
        Item& item = mItems[i];
        const int now = item.widget->sizeHint().width();
        const int extra = labelWidth(item);
        if (item.labelText != extra)
            item.measured[0] = item.measured[1] = 0;
        item.labelText = extra;
        // Estimates are only a first guess: a wrong one flips the row between labelled and icon-only every pass.
        item.measured[isCompact(item) ? 1 : 0] = now;
        full[i] = item.measured[0] ? item.measured[0] : now + extra;
        compact[i] = item.labelled.empty() ? now : item.measured[1] ? item.measured[1] : std::max(0, now - extra);
    }

    std::vector<bool> kept(n, true);
    auto isRule = [this](size_t i) { return !mItems[i].overflow; };
    // A rule shows only with a kept item on each side of it, on its own side of the row.
    auto shown = [&](size_t i) {
        if (!isRule(i))
            return bool(kept[i]);
        auto keptNeighbour = [&](int step) {
            for (int j = int(i) + step; j >= 0 && j < int(n) && mItems[j].right == mItems[i].right; j += step)
                if (!isRule(j) && kept[j])
                    return true;
            return false;
        };
        return keptNeighbour(-1) && keptNeighbour(1);
    };
    int level = 0;
    auto widthOf = [&](size_t i) { return wantCompact(mItems[i], level) ? compact[i] : full[i]; };
    auto total = [&] {
        int w = 0;
        for (size_t i = 0; i < n; ++i)
            if (shown(i))
                w += widthOf(i) + kHeaderGap;
        return w;
    };
    const int room = width() - 2 * kHeaderPad;
    // Labels go before items do: first the mode labels, then every label.
    while (level < 2 && total() > room)
        ++level;
    bool overflowed = false;
    if (total() > room)
    {
        overflowed = true;
        std::vector<size_t> order;
        for (size_t i = 0; i < n; ++i)
            if (!isRule(i))
                order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) { return mItems[a].keep < mItems[b].keep; });
        const int chevron = mChevron->sizeHint().width() + kHeaderGap;
        for (size_t i : order)
        {
            if (total() + chevron <= room)
                break;
            kept[i] = false;
        }
    }

    for (Item& item : mItems)
    {
        const Qt::ToolButtonStyle style = wantCompact(item, level) ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon;
        for (QToolButton* b : item.labelled)
            if (b->toolButtonStyle() != style)
                b->setToolButtonStyle(style);
        if (item.widget->layout())
            item.widget->layout()->invalidate();
    }
    auto place = [this](QWidget* w, int x) {
        const QSize s = w->sizeHint();
        const QRect r(x, (kHeaderHeight - s.height()) / 2, s.width(), s.height());
        if (w->geometry() != r)
            w->setGeometry(r);
        return s.width();
    };
    int left = kHeaderPad;
    for (size_t i = 0; i < n; ++i)
        if (!mItems[i].right && shown(i))
            left += place(mItems[i].widget, left) + kHeaderGap;
    int right = width() - kHeaderPad;
    if (overflowed)
        right -= place(mChevron, right - mChevron->sizeHint().width()) + kHeaderGap;
    for (size_t i = n; i-- > 0;)
        if (mItems[i].right && shown(i))
            right -= place(mItems[i].widget, right - mItems[i].widget->sizeHint().width()) + kHeaderGap;
    // Visibility changes only where needed: each one asks for another layout pass.
    for (size_t i = 0; i < n; ++i)
        if (mItems[i].widget->isVisibleTo(this) != shown(i))
            mItems[i].widget->setVisible(shown(i));
    if (mChevron->isVisibleTo(this) != overflowed)
        mChevron->setVisible(overflowed);
}

void ViewportHeader::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutItems();
    // The panel resizes when the layout switches between one and four views.
    if (auto* layout = qobject_cast<ViewportLayout*>(parentWidget() ? parentWidget()->parentWidget() : nullptr); layout && mQuad)
    {
        QSignalBlocker block(mQuad);
        mQuad->setChecked(layout->quad());
    }
}

bool ViewportHeader::event(QEvent* e)
{
    // Hotkeys are live-editable, so tooltips pick up the current binding whenever the pointer comes over the row.
    if (e->type() == QEvent::Enter)
        refreshTooltips();
    else if (e->type() == QEvent::Show || e->type() == QEvent::LayoutRequest)
        layoutItems();
    return QWidget::event(e);
}
} // namespace lila::ui
