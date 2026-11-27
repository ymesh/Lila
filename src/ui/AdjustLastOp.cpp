#include "ui/AdjustLastOp.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "ui/Actions.h"
#include "ui/DocsUi.h"
#include "ui/FlagFields.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/ValueReset.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace lila::ui
{
namespace lo = scene::lastOp;

LastOpEditor::LastOpEditor(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);
    mTitle = new QLabel(this);
    QFont tf = mTitle->font();
    tf.setWeight(QFont::DemiBold);
    mTitle->setFont(tf);
    mTitleRow = new QHBoxLayout();
    mTitleRow->setContentsMargins(0, 0, 0, 0);
    mTitleRow->addWidget(mTitle, 1);
    root->addLayout(mTitleRow);
    mFields = new QWidget(this);
    // A drag re-runs the op as one replaced undo step, so the fields' scrubs must leave the undo stack free.
    setScrubsWithoutUndoStep(mFields);
    mFieldsLayout = new QVBoxLayout(mFields);
    mFieldsLayout->setContentsMargins(0, 0, 0, 0);
    mFieldsLayout->setSpacing(2);
    root->addWidget(mFields);
    mNonDestructive = new QToolButton(this);
    mNonDestructive->setText("Make Non-Destructive");
    mNonDestructive->setIcon(icon("stack-2"));
    mNonDestructive->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mNonDestructive->setToolTip("Undo this edit and add the matching modifier to the stack instead, with the same settings");
    root->addWidget(mNonDestructive);
    mStatus = new QLabel(this);
    mStatus->setWordWrap(true);
    mStatus->setStyleSheet("color: #9a9a9a;");
    root->addWidget(mStatus);

    mApply = new QTimer(this);
    mApply->setSingleShot(true);
    mApply->setInterval(0);
    connect(mApply, &QTimer::timeout, this, &LastOpEditor::applyPending);
    // One echo when the edits settle, not one per scrub step.
    mEcho = new QTimer(this);
    mEcho->setSingleShot(true);
    mEcho->setInterval(700);
    connect(mEcho, &QTimer::timeout, this, [] {
        if (const lo::Op* op = lo::current())
            log::write(LogLevel::Echo, ops::toPython(op->command, op->call) + "\n");
    });
    mLiveCheck = new QTimer(this);
    mLiveCheck->setSingleShot(true);
    mLiveCheck->setInterval(0);
    connect(mLiveCheck, &QTimer::timeout, this, [this] {
        bool live = lo::reachable();
        mFields->setEnabled(live);
        mNonDestructive->setEnabled(live);
    });
    connect(mNonDestructive, &QToolButton::clicked, this, [this] {
        try
        {
            lo::makeNonDestructive();
        }
        catch (const CommandError& e)
        {
            mStatus->setText(e.what());
        }
    });

    mLastOpListener = lo::addListener([this](lo::Change c) {
        // A rerun from elsewhere (the extrude handles) changes the values the fields show.
        if (c == lo::Change::Recorded || c == lo::Change::Cleared || c == lo::Change::Restored || (c == lo::Change::Rerun && !mSelfRerun))
            rebuild();
        else
            refresh();
    });
    // Restarting a timer costs more than the check; a world build makes hundreds of thousands of changes.
    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) {
        if (!mLiveCheck->isActive())
            mLiveCheck->start();
    });
    mComponentListener = scene::components::addListener([this] { mLiveCheck->start(); });
    qApp->installEventFilter(this);
    rebuild();
}

bool LastOpEditor::eventFilter(QObject*, QEvent* e)
{
    // A release or a confirmed entry ends the edit, so the next one is an undo step of its own.
    bool settles = e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::FocusOut;
    if (e->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent*>(e)->key();
        settles = key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab;
    }
    if (settles)
    {
        if (mApply->isActive())
        {
            mApply->stop();
            applyPending();
        }
        lo::endDrag();
        mDragFlags.clear();
    }
    return false;
}

LastOpEditor::~LastOpEditor()
{
    lo::removeListener(mLastOpListener);
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
}

void LastOpEditor::rebuild()
{
    // The fields live in a grid inside the layout; the grid is its own layout item, so deleting it once frees both.
    while (QLayoutItem* item = mFieldsLayout->takeAt(0))
    {
        if (QLayout* l = item->layout())
            while (QLayoutItem* inner = l->takeAt(0))
            {
                if (QWidget* w = inner->widget())
                    w->deleteLater();
                delete inner;
            }
        else if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    mPending = json::object();
    mShow.clear();
    delete mDocs;
    mDocs = nullptr;
    const lo::Op* op = lo::current();
    mCommand = op ? op->command : std::string();
    if (!op)
    {
        mTitle->setText("Tool Options");
        mStatus->setText("Run a modeling operation (bevel, extrude, inset...) to adjust its settings here.");
        mNonDestructive->hide();
        mFields->hide();
        return;
    }
    mTitle->setText(QString::fromStdString(lo::displayName(op->command)));
    if (const ops::options::Override* o = ops::options::findOverride(op->command); o && !o->docs.empty())
    {
        mDocs = docs::helpButton(o->docs, this);
        mTitleRow->addWidget(mDocs);
    }
    mStatus->clear();
    mFields->show();
    std::vector<const FlagDef*> flags = lo::editableFlags(op->command);
    if (flags.empty())
        mStatus->setText("No settings to adjust.");
    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(2);
    grid->setColumnStretch(1, 1);
    int row = 0;
    for (const FlagDef* f : flags)
    {
        std::string name = f->name;
        FlagFieldWidgets w = makeFlagField(ops::options::fieldFor(*f), lo::flagValue(*op, *f), [this, name](const json& v) { setFlag(name, v); },
                                           mFields);
        mShow[name] = w.set;
        if (w.label)
        {
            grid->addWidget(w.label, row, 0);
            grid->addWidget(w.field, row, 1);
        }
        else
            grid->addWidget(w.field, row, 0, 1, 2);
        ++row;
    }
    mFieldsLayout->addLayout(grid);
    mNonDestructive->setVisible(!lo::modifierEquivalent(op->command).empty());
    mLiveCheck->start();
}

void LastOpEditor::refresh()
{
    const lo::Op* op = lo::current();
    if (!op || op->command != mCommand)
    {
        rebuild();
        return;
    }
    mLiveCheck->start();
}

void LastOpEditor::setFlag(const std::string& name, const json& value)
{
    if (mUpdating)
        return;
    mPending[name] = value;
    mApply->start();
}

void LastOpEditor::applyPending()
{
    if (mPending.empty())
        return;
    json flags = std::move(mPending);
    mPending = json::object();
    std::vector<std::string> names;
    for (auto& [k, v] : flags.items())
        names.push_back(k);
    if (names != mDragFlags)
        lo::endDrag();
    mDragFlags = std::move(names);
    mSelfRerun = true;
    try
    {
        lo::rerun(flags, CallSource::Script, true);
        mSelfRerun = false;
        // A value the command had to limit shows as the limit, so the field stops where the mesh does.
        if (const lo::Op* op = lo::current())
            for (auto& [k, v] : flags.items())
                if (auto used = op->call.find(k); used != op->call.end() && *used != v && mShow.count(k) && mShow[k])
                    mShow[k](*used);
        mStatus->clear();
        mEcho->start();
    }
    catch (const CommandError& e)
    {
        mSelfRerun = false;
        mStatus->setText(e.what());
    }
}

namespace
{
/** Translucent in-view panel over the active viewport, centre-right by default; drag its background to move it. */
class AdjustLastOpPopup : public QWidget
{
public:
    explicit AdjustLastOpPopup(QWidget* window)
        : QWidget(window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setProperty("hotkeyContext", "viewport");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 16, 10, 8);
        setCursor(Qt::SizeAllCursor);
        auto* head = new QHBoxLayout();
        head->addStretch(1);
        auto* close = new QToolButton(this);
        close->setIcon(icon("x"));
        close->setAutoRaise(true);
        QString key = Hotkeys::get().keyFor("adjustLastOp.show");
        close->setToolTip("Close (Esc)." + (key.isEmpty() ? QString() : " " + key + " reopens it for the last operation."));
        connect(close, &QToolButton::clicked, this, &QWidget::hide);
        head->addWidget(close);
        mEditor = new LastOpEditor(this);
        mEditor->setCursor(Qt::ArrowCursor);
        auto* body = new QHBoxLayout();
        body->addWidget(mEditor, 1);
        body->addLayout(head);
        body->setAlignment(head, Qt::AlignTop);
        root->addLayout(body);
        setMinimumWidth(220);
        qApp->installEventFilter(this);
    }

    void popUp()
    {
        mEditor->refresh();
        adjustSize();
        place();
        show();
        raise();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(themeShade(255, 36));
        p.setBrush(themeShade(34, 215));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(themeShade(255, 60));
        p.drawRoundedRect(QRectF(width() / 2.0 - 16, 6, 32, 3), 1.5, 1.5);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
            mDragFrom = e->globalPosition().toPoint() - pos();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (mDragFrom)
            move(e->globalPosition().toPoint() - *mDragFrom);
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!mDragFrom)
            return;
        mDragFrom.reset();
        if (QWidget* view = activeView(); view && view->width() > 0 && view->height() > 0)
        {
            QPointF centre = QPointF(view->mapFromGlobal(geometry().center()));
            PrefStore::get().set("general", "/modeling/adjustLastOpPos", json::array({centre.x() / view->width(), centre.y() / view->height()}));
        }
        place();
    }

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (!isVisible())
            return false;
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape)
            hide();
        else if ((e->type() == QEvent::Move || e->type() == QEvent::Resize) && obj == parentWidget())
            QTimer::singleShot(0, this, [this] { place(); });
        return false;
    }

private:
    QWidget* activeView() const
    {
        ViewportLayout* layout = parentWidget() ? parentWidget()->findChild<ViewportLayout*>() : nullptr;
        return layout && layout->active() ? static_cast<QWidget*>(layout->active()) : layout;
    }

    /** The remembered spot is the panel centre as a fraction of the view, so it survives view resizes. */
    void place()
    {
        QWidget* view = activeView();
        if (!view)
            return;
        json at = PrefStore::get().value("general", "/modeling/adjustLastOpPos", json::array({0.82, 0.5}));
        double fx = at.is_array() && at.size() == 2 ? at[0].get<double>() : 0.82;
        double fy = at.is_array() && at.size() == 2 ? at[1].get<double>() : 0.5;
        QPoint centre(int(fx * view->width()), int(fy * view->height()));
        QPoint topLeft = centre - QPoint(width() / 2, height() / 2);
        topLeft.setX(std::clamp(topLeft.x(), 0, std::max(0, view->width() - width())));
        topLeft.setY(std::clamp(topLeft.y(), 0, std::max(0, view->height() - height())));
        move(view->mapToGlobal(topLeft));
    }

    LastOpEditor* mEditor = nullptr;
    std::optional<QPoint> mDragFrom;
};

QPointer<AdjustLastOpPopup>& popup()
{
    static QPointer<AdjustLastOpPopup> p;
    return p;
}

QWidget* viewportWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->findChild<ViewportLayout*>())
            return w;
    return nullptr;
}

void showPopup()
{
    if (!popup())
    {
        QWidget* win = viewportWindow();
        if (!win)
            return;
        popup() = new AdjustLastOpPopup(win);
    }
    popup()->popUp();
}

void hidePopup()
{
    if (popup())
        popup()->hide();
}
} // namespace

namespace adjustLastOp
{
void install()
{
    lo::install();
    lo::addListener([](lo::Change c) {
        if (c == lo::Change::Recorded || c == lo::Change::Restored)
        {
            const std::string& command = lo::current()->command;
            bool worthShowing = !lo::editableFlags(command).empty() || !lo::modifierEquivalent(command).empty();
            if (PrefStore::get().value<bool>("general", kEnabledPref, true) && worthShowing)
                showPopup();
            else
                hidePopup();
        }
        else if (c == lo::Change::Other || c == lo::Change::Cleared)
            hidePopup();
    });
    // Edits outside the undo stack end the op's adjustability too; selection changes never do.
    auto checkLive = [] {
        if (popup() && popup()->isVisible() && !lo::reachable())
            QTimer::singleShot(0, [] {
                if (!lo::reachable())
                    hidePopup();
            });
    };
    scene::Graph::current().addListener([checkLive](scene::Change, scene::NodeId) { checkLive(); });
    scene::components::addListener(checkLive);
    // Picking a transform tool (Q, W, E, R) finishes with the op, as in Maya.
    actions::addRunListener([](const actions::RunEvent& e) {
        static const std::array<std::string_view, 4> kTools = {"tool.select", "tool.move", "tool.rotate", "tool.scale"};
        if (e.done && e.target.is_string() && std::ranges::find(kTools, e.target.get_ref<const std::string&>()) != kTools.end())
            hidePopup();
    });

    actions::add({.id = "adjustLastOp.show", .label = "Adjust Last Operation", .category = "Edit", .run = [] {
                      if (lo::reachable())
                          showPopup();
                      else
                          log::warn("Adjust Last Operation: nothing to adjust (another edit came after the last modeling operation)");
                  },
                  .doc = "Reopens the in-view settings of the last modeling operation, to change them live. Selection changes made since "
                         "are undone when a setting changes.",
                  .python = "lila.ui.run_action(\"adjustLastOp.show\")"});
    actions::add({.id = "adjustLastOp.makeNonDestructive", .label = "Make Last Operation Non-Destructive", .category = "Mesh", .run = [] {
                      try
                      {
                          lo::makeNonDestructive();
                      }
                      catch (const CommandError& e)
                      {
                          log::error("{}", e.what());
                      }
                  },
                  .doc = "Swaps the last bevel, mirror, subdivide or triangulate for the matching modifier in the stack."});
}
} // namespace adjustLastOp
} // namespace lila::ui
