#include "ui/UvSeamOverride.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/UvProjectTool.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr const char* kOverrideAttr = "seamViewOverride";

scene::Graph& currentGraph() { return scene::Graph::current(); }

const scene::Node* overrideNode()
{
    scene::Graph& g = currentGraph();
    for (scene::NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->type().isA("transform"))
            if (int i = n->type().attrIndex(kOverrideAttr); i >= 0)
                if (const bool* on = std::get_if<bool>(&n->value(i)); on && *on)
                    return n;
    return nullptr;
}

void runCommand(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
    }
}

void setOverride(const std::string& path) { runCommand("uvSeamViewOverride", path.empty() ? json::object() : json{{"args", {path}}}); }

void newViewAtCamera(bool plane)
{
    json call = seamViewAtCamera(plane);
    call["override"] = true;
    runCommand("uvSeamView", call);
}
} // namespace

SeamOverrideRow::SeamOverrideRow(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("seamOverride");
    setProperty("lilaRawColors", true);
    auto* h = new QHBoxLayout(this);
    h->setSpacing(6);
    mLabel = new QLabel(this);
    h->addWidget(mLabel);
    mPick = new QToolButton(this);
    mPick->setObjectName("seamOverridePick");
    mPick->setPopupMode(QToolButton::InstantPopup);
    mPick->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mPick->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mPick->setAccessibleName("Seam View Override");
    controls::setRole(mPick, "push");
    auto* menu = new QMenu(mPick);
    menu->setToolTipsVisible(true);
    connect(menu, &QMenu::aboutToShow, this, [this] { fillMenu(); });
    mPick->setMenu(menu);
    h->addWidget(mPick, 1);
    mClear = new QToolButton(this);
    mClear->setObjectName("seamOverrideClear");
    mClear->setIcon(icon("x"));
    mClear->setAccessibleName("Clear Seam View Override");
    mClear->setToolTip("Clear the override: Auto UV, Auto Seams and Hose UVs use their options' seam views again");
    controls::setRole(mClear, "push");
    connect(mClear, &QToolButton::clicked, this, [] { setOverride({}); });
    h->addWidget(mClear);

    mListener = currentGraph().addListener([this](scene::Change c, scene::NodeId) {
        if (c == scene::Change::Selection || mQueued)
            return;
        mQueued = true;
        QTimer::singleShot(0, this, [this] {
            mQueued = false;
            refresh();
        });
    });
    refresh();
}

SeamOverrideRow::~SeamOverrideRow() { currentGraph().removeListener(mListener); }

void SeamOverrideRow::refresh()
{
    const scene::Node* n = overrideNode();
    const bool on = n != nullptr;
    const QColor warn = themeColor("issueWarning");
    setStyleSheet(on ? QString("QFrame#seamOverride { background: rgba(%1, %2, %3, 40); border-left: 3px solid %4; }"
                               "QFrame#seamOverride > QLabel { color: %4; font-weight: 600; background: transparent; }")
                           .arg(warn.red())
                           .arg(warn.green())
                           .arg(warn.blue())
                           .arg(warn.name())
                     : QString());
    layout()->setContentsMargins(on ? 8 : 0, on ? 4 : 2, on ? 4 : 0, on ? 4 : 2);
    mLabel->setText(on ? "Override" : "Seam View");
    const QString name = on ? QString::fromStdString(n->name()) : QString();
    mPick->setText(on ? name : "None");
    mPick->setIcon(on ? controls::tintedIcon("alert-triangle", "issueWarning") : icon("eye"));
    mClear->setVisible(on);
    const QString tip = on ? QString("Seam view override: Auto UV, Auto Seams and Hose UVs hide seams from %1 alone, whatever their "
                                     "options say")
                                 .arg(name)
                           : QString("Seam View: a camera, locator or plane that Auto UV, Auto Seams and Hose UVs hide seams from, "
                                     "over their options");
    mLabel->setToolTip(tip);
    mPick->setToolTip(tip);
}

void SeamOverrideRow::fillMenu()
{
    QMenu* m = mPick->menu();
    m->clear();
    const scene::Node* current = overrideNode();
    QAction* none = m->addAction("None", [] { setOverride({}); });
    none->setCheckable(true);
    none->setChecked(!current);

    // Seam views and cameras, by name.
    scene::Graph& g = currentGraph();
    std::vector<const scene::Node*> views;
    for (scene::NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->type().isA("transform"))
            if (n->type().isA("camera") || n->type().attrIndex("seamView") >= 0)
                views.push_back(n);
    std::sort(views.begin(), views.end(), [](const scene::Node* a, const scene::Node* b) { return a->name() < b->name(); });
    if (!views.empty())
        m->addSeparator();
    for (const scene::Node* n : views)
    {
        const std::string path = n->path();
        QAction* a = m->addAction(icon(n->type().isA("camera") ? "camera" : n->type().attrIndex("seamViewNormal") >= 0 ? "border-horizontal" : "eye"),
                                  QString::fromStdString(n->name()), [path] { setOverride(path); });
        a->setCheckable(true);
        a->setChecked(n == current);
    }

    m->addSeparator();
    m->addAction(icon("eye"), "New Point at Camera", [] { newViewAtCamera(false); })
        ->setToolTip("A seam view where the viewport camera is now, made the override");
    m->addAction(icon("border-horizontal"), "New Plane Facing Camera", [] { newViewAtCamera(true); })
        ->setToolTip("A plane facing the viewport camera, made the override: seams hide from that whole side, like a fixed game view");
    m->addAction(icon("pointer"), "Selected", [] { actions::run("uv.seamViewOverride"); })
        ->setToolTip("The selected camera, locator or plane");
}
} // namespace lila::ui
