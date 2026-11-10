// The Tailor toolkit: the garment workflow's actions as tiles, docked with the other toolkits.

#include "Tailor.h"

#include <lila/Actions.h>
#include <lila/Controls.h>
#include <lila/Extension.h>
#include <lila/Panels.h>

#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <vector>

using namespace lila;
using namespace lila::ui;

namespace tailor
{
namespace
{
constexpr const char* kCollapsedPref = "/tailorToolkit/collapsed";
constexpr int kColumns = 4;

struct Tile
{
    const char* action;
    const char* icon;
    const char* label;
};

QWidget* toolkitSection(const char* title, const std::vector<Tile>& tiles, const std::vector<std::string>& more, QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 4);
    std::vector<QWidget*> items;
    for (const Tile& t : tiles)
        if (actions::find(t.action))
            items.push_back(controls::tile(t.action, t.icon, box, t.label));
    std::vector<std::string> rare;
    for (const std::string& id : more)
        if (actions::find(id))
            rare.push_back(id);
    if (!rare.empty())
        items.push_back(controls::overflow(rare, box));
    layout->addWidget(controls::grid(items, kColumns, box));
    return controls::section(title, box, parent, kCollapsedPref, true);
}

QWidget* makeToolkit(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(10, 4, 10, 8);
    v->setSpacing(4);
    v->addWidget(toolkitSection("Pattern",
                                {{"tailor.mannequin", "man", "Body"},
                                 {"tailor.createBlock", "shirt", "Block"},
                                 {"tailor.drawPanel", "pencil", "Draw"},
                                 {"window.tailorPatternEditor", "layout-board-split", "Pattern"}},
                                {"tailor.importGarmentCode"}, body));
    v->addWidget(toolkitSection("Sew and Drape",
                                {{"tailor.simulate", "player-play", "Drape"},
                                 {"tailor.setFabric", "texture", "Fabric"},
                                 {"tailor.sew", "arrows-join-2", "Sew"},
                                 {"tailor.autoSew", "needle-thread", "Auto-Sew"},
                                 {"tailor.check", "checklist", "Check"}},
                                {}, body));
    v->addWidget(toolkitSection("Live",
                                {{"tailor.live", "bolt", "Live"},
                                 {"tailor.grab", "hand-grab", "Grab"},
                                 {"tailor.pin", "pinned", "Pin"},
                                 {"tailor.fold", "fold", "Fold"},
                                 {"tailor.shrink", "arrows-minimize", "Shrink"},
                                 {"tailor.steam", "ironing-steam", "Steam"}},
                                {"tailor.stopLive", "tailor.clearBrush"}, body));
    v->addWidget(toolkitSection("Finish",
                                {{"tailor.addStitching", "needle", "Stitching"},
                                 {"tailor.freeze", "snowflake", "Freeze"},
                                 {"tailor.bake", "package-export", "Bake"}},
                                {"tailor.unfreeze"}, body));
    v->addStretch(1);
    scroll->setWidget(body);
    controls::fitScrollWidth(scroll);
    return scroll;
}
} // namespace

void registerToolkit(ext::Context& ctx)
{
    panels::add(ctx, {.id = "tailorToolkit",
                      .title = "Tailor",
                      .create = makeToolkit,
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "shirt",
                      .open = false,
                      .docs = "tailor"});
}
} // namespace tailor
