#include "ui/LevelToolkit.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/DocsUi.h"
#include "ui/AdjustLastOp.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Shelf.h"
#include "ui/ToolkitLayout.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr const char* kLevelCollapsedPref = "/levelToolkit/collapsed";

/** The grid of a group's buttons, skipping null ones, with its overflow menu in the last cell. */
QWidget* levelGroup(const std::vector<QToolButton*>& buttons, const std::vector<std::string>& more, int columns, QWidget* parent)
{
    std::vector<QWidget*> items;
    for (QToolButton* b : buttons)
        if (b)
            items.push_back(b);
    if (!more.empty())
        items.push_back(controls::overflow(more, parent));
    return controls::grid(items, columns, parent);
}

std::vector<const levelToolkit::Section*> sectionsInOrder()
{
    std::vector<const levelToolkit::Section*> out;
    for (const levelToolkit::Section* s : levelToolkit::registry().allInOrder())
        out.push_back(s);
    std::stable_sort(out.begin(), out.end(), [](const auto* a, const auto* b) { return a->order < b->order; });
    return out;
}
} // namespace

Registry<levelToolkit::Section>& levelToolkit::registry()
{
    static Registry<Section> r;
    return r;
}

void levelToolkit::add(Section s, std::string owner)
{
    std::string id = s.id;
    registry().put(std::move(id), std::move(s), std::move(owner));
}

uint64_t levelToolkit::add(ext::Context& ctx, Section s)
{
    if (const std::string* owner = registry().ownerOf(s.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "level toolkit section '" + s.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = s.id;
    add(std::move(s), ctx.owner());
    return ctx.track("levelToolkit:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool levelToolkit::remove(const std::string& id)
{
    return registry().remove(id);
}

void levelToolkit::registerShelf()
{
    auto sync = [] {
        json items = json::array();
        for (const Section* s : sectionsInOrder())
            for (const Item& item : s->items)
                if (!item.shelf.empty() && !item.action.empty())
                    items.push_back({{"label", item.shelf}, {"icon", item.icon}, {"tooltip", ""}, {"target", item.action}});
        if (items.empty())
        {
            shelves::registry().remove("Level");
            return;
        }
        items.insert(items.begin(), json{{"label", "Toolkit"}, {"icon", "map"}, {"tooltip", "Show or hide the Level Toolkit"}, {"target", "window.levelToolkit"}});
        shelves::add({"Level", std::move(items)});
    };
    static const int listener = registry().addListener([sync](RegistryChange, const std::string&, const std::string&) { sync(); });
    (void)listener;
    sync();
}

LevelToolkit::LevelToolkit(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    mScroll = new QScrollArea(this);
    mScroll->setWidgetResizable(true);
    mScroll->setFrameShape(QFrame::NoFrame);
    mScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(mScroll);
    mBody = new QWidget(mScroll);
    new QVBoxLayout(mBody);
    mScroll->setWidget(mBody);
    controls::fitScrollWidth(mScroll);

    // Extensions register sections and actions in bursts while they load and unload, so the rebuild waits for the burst.
    mRebuild = new QTimer(this);
    mRebuild->setSingleShot(true);
    mRebuild->setInterval(0);
    connect(mRebuild, &QTimer::timeout, this, [this] { rebuild(); });
    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(30);
    connect(mRefresh, &QTimer::timeout, this, [this] { refreshButtons(); });
    mSectionListener = levelToolkit::registry().addListener([this](RegistryChange, const std::string&, const std::string&) { mRebuild->start(); });
    mActionListener = actions::registry().addListener([this](RegistryChange, const std::string&, const std::string&) { mRebuild->start(); });
    rebuild();
}

LevelToolkit::~LevelToolkit()
{
    levelToolkit::registry().removeListener(mSectionListener);
    actions::registry().removeListener(mActionListener);
}

void LevelToolkit::rebuild()
{
    mButtons.clear();
    QLayout* root = mBody->layout();
    while (QLayoutItem* item = root->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    auto* v = static_cast<QVBoxLayout*>(root);
    v->setContentsMargins(10, 4, 10, 8);
    v->setSpacing(4);

    int shown = 0;
    for (const levelToolkit::Section* s : sectionsInOrder())
    {
        auto* box = new QWidget(mBody);
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 4);
        boxLayout->setSpacing(4);
        if (s->create && s->createFirst)
            if (QWidget* extra = s->create(box))
                boxLayout->addWidget(extra);
        std::vector<QToolButton*> tiles;
        std::vector<std::pair<std::string, std::vector<QToolButton*>>> groups;
        std::vector<QToolButton*> pushes;
        std::vector<std::string> more;
        for (const levelToolkit::Item& item : s->items)
        {
            if (item.action.empty() || !actions::find(item.action))
                continue;
            const QString label = QString::fromStdString(item.text);
            if (item.rare)
                more.push_back(item.action);
            else if (!item.group.empty())
            {
                auto g = std::ranges::find(groups, item.group, &std::pair<std::string, std::vector<QToolButton*>>::first);
                if (g == groups.end())
                    g = groups.insert(groups.end(), {item.group, {}});
                g->second.push_back(tile(item.action, item.icon, label, box));
            }
            else if (!item.shelf.empty())
                tiles.push_back(tile(item.action, item.icon, label, box));
            else if (pushes.size() < 6)
                pushes.push_back(tile(item.action, item.icon, label, box));
            else
                more.push_back(item.action);
        }
        if (!tiles.empty())
            boxLayout->addWidget(levelGroup(tiles, {}, kToolkitColumns, box));
        for (const auto& [title, buttons] : groups)
        {
            auto* heading = new QLabel(QString::fromStdString(title), box);
            heading->setContentsMargins(2, 4, 0, 0);
            controls::setRole(heading, "dim");
            boxLayout->addWidget(heading);
            boxLayout->addWidget(levelGroup(buttons, {}, kToolkitColumns, box));
        }
        if (!pushes.empty() || !more.empty())
            boxLayout->addWidget(levelGroup(pushes, more, 2, box));
        if (s->create && !s->createFirst)
            if (QWidget* extra = s->create(box))
                boxLayout->addWidget(extra);
        if (boxLayout->count() == 0)
        {
            delete box;
            continue;
        }
        QToolButton* header = nullptr;
        v->addWidget(collapsibleSection(QString::fromStdString(s->title), box, mBody, kLevelCollapsedPref, true, &header));
        // The docs button takes its own clicks, so it doesn't fold the section.
        if (!s->docs.empty() && header)
        {
            auto* row = new QHBoxLayout(header);
            row->setContentsMargins(0, 0, 2, 0);
            row->addStretch(1);
            row->addWidget(docs::helpButton(s->docs, header));
        }
        ++shown;
    }

    if (!shown)
    {
        auto* empty = new QLabel("No level tools loaded. Enable Terrain, Settlement, Masonry or Stagehand in the Extension Manager.", mBody);
        empty->setWordWrap(true);
        empty->setContentsMargins(2, 4, 2, 4);
        controls::setRole(empty, "hint");
        v->addWidget(empty);
        if (actions::find("window.extensionManager"))
            v->addWidget(levelGroup({push("window.extensionManager", "Extension Manager", mBody)}, {}, 2, mBody));
    }
    else
    {
        // The in-view popup is the main place for these; the section serves those who turn it off.
        auto* optionsBox = new QWidget(mBody);
        auto* optionsLayout = new QVBoxLayout(optionsBox);
        optionsLayout->setContentsMargins(0, 0, 0, 4);
        optionsLayout->addWidget(new LastOpEditor(optionsBox));
        v->addWidget(collapsibleSection("Tool Options", optionsBox, mBody, kLevelCollapsedPref, false));
    }
    v->addStretch(1);
    refreshButtons();
}

QToolButton* LevelToolkit::tile(const std::string& id, const std::string& iconName, const QString& label, QWidget* parent)
{
    if (!actions::find(id))
        return nullptr;
    QToolButton* b = controls::row(id, iconName.empty() ? nullptr : iconName.c_str(), parent, label);
    b->setProperty("lilaAction", QString::fromStdString(id));
    connect(b, &QToolButton::clicked, this, [this] { mRefresh->start(); });
    mButtons.push_back(b);
    return b;
}

QToolButton* LevelToolkit::push(const std::string& id, const QString& label, QWidget* parent)
{
    if (!actions::find(id))
        return nullptr;
    QToolButton* b = controls::row(id, nullptr, parent, label);
    b->setProperty("lilaAction", QString::fromStdString(id));
    connect(b, &QToolButton::clicked, this, [this] { mRefresh->start(); });
    mButtons.push_back(b);
    return b;
}

void LevelToolkit::refreshButtons()
{
    for (QToolButton* b : mButtons)
    {
        const std::string id = b->property("lilaAction").toString().toStdString();
        const Action* a = actions::find(id);
        if (!a)
            continue;
        // Hotkeys are live-editable, so tooltips pick up the current keys.
        QString tip = QString::fromStdString(actions::tooltip(id));
        if (optionBox::available(id))
            tip += "\nClick corner, right-click or Shift+click for options";
        b->setToolTip(tip);
        if (a->checked && b->isCheckable())
            b->setChecked(a->checked());
    }
}

void LevelToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshButtons();
}
} // namespace lila::ui
