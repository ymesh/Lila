#include "rig/AnimProxy.h"

#include "Puppeteer.h"

#include <lila/Events.h>
#include <lila/Prefs.h>
#include <lila/ProxySets.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <algorithm>
#include <string>
#include <utility>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
bool proxyPref(const char* key, bool fallback) { return PrefStore::get().value<bool>(kPrefs, key, fallback); }

bool isPuppeteerMode(const std::string& mode) { return mode == kRigMode || mode == kAnimateMode; }

/** Every set to d without an undo step: a mode switch is not an edit. */
void showAll(proxy::Display d)
{
    Graph& g = Graph::current();
    const std::vector<NodeId> sets = proxy::all(g);
    if (sets.empty())
        return;
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    for (NodeId s : sets)
        proxy::setDisplay(g, s, d);
    u.setEnabled(was);
    viewportTools::requestRedraw();
}

void modeChanged(const std::string& from, const std::string& to)
{
    if (!animProxyEnabled() || isPuppeteerMode(from) == isPuppeteerMode(to))
        return;
    if (isPuppeteerMode(to) && proxyPref(kProxyOnEnter, true))
        showAll(proxy::Display::Proxy);
    else if (!isPuppeteerMode(to) && proxyPref(kRealOnExit, true))
        showAll(proxy::Display::Real);
}

std::vector<NodeId> proxyJoints()
{
    Graph& g = Graph::current();
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
    {
        const Node* n = g.node(id);
        if (!n)
            continue;
        NodeId j = rig::isJoint(*n) ? id : kNoNode;
        if (j == kNoNode)
            if (const NodeId s = proxy::setOfPiece(g, id); s != kNoNode)
                for (const proxy::Piece& p : proxy::pieces(g, s))
                    if (p.mesh == id)
                        j = p.joint;
        if (j != kNoNode && std::ranges::find(out, j) == out.end())
            out.push_back(j);
    }
    return out;
}

void runProxyCommand(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
    }
    viewportTools::requestRedraw();
}
} // namespace

bool animProxyEnabled() { return proxyPref(kUseAnimProxy, true); }

std::vector<NodeId> selectedProxySets()
{
    Graph& g = Graph::current();
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
        for (NodeId s : proxy::setsFor(g, id))
            if (std::ranges::find(out, s) == out.end())
                out.push_back(s);
    return out;
}

bool selectionShowsProxy()
{
    const std::vector<NodeId> sets = selectedProxySets();
    return !sets.empty() && proxy::display(Graph::current(), sets.front()) == proxy::Display::Proxy;
}

void toggleSelectedProxy()
{
    json names = json::array();
    for (NodeId s : selectedProxySets())
        names.push_back(Graph::current().node(s)->path());
    runProxyCommand("setProxyDisplay", {{"args", names}});
}

void toggleSelectedRealPiece()
{
    Graph& g = Graph::current();
    for (NodeId j : proxyJoints())
    {
        bool real = false;
        for (NodeId s : proxy::setsFor(g, j))
            for (const proxy::Piece& p : proxy::pieces(g, s))
                real |= p.joint == j && p.display == proxy::PieceDisplay::Real;
        runProxyCommand("setProxyDisplay", {{"piece", g.node(j)->path()}, {"pieceDisplay", real ? "follow" : "real"}});
    }
}

void registerAnimProxy(ext::Context& ctx)
{
    prefPages::add(ctx, {.id = "puppeteer", .title = "Puppeteer", .file = kPrefs, .build = [](prefPages::PageBuilder& b) {
                             b.section("Anim Proxy");
                             b.toggle("Use anim proxy", kUseAnimProxy, true);
                             b.toggle("Show proxy when Puppeteer turns on", kProxyOnEnter, true);
                             b.toggle("Show real mesh when Puppeteer turns off", kRealOnExit, true);
                             b.section("Curve Overlay");
                             b.number("Scrim", "/curveOverlay/scrim", 0.45, 0.0, 0.9, 0.05);
                         }});

    const char* cat = "Puppeteer";
    actions::add(ctx, {.id = "puppeteer.animProxy.toggle", .label = "Toggle Anim Proxy", .category = cat, .run = toggleSelectedProxy,
                       .doc = "Shows the selected character's anim proxy in place of its real meshes, or the real meshes again."});
    actions::add(ctx, {.id = "puppeteer.animProxy.realPiece", .label = "Toggle Real Piece", .category = cat, .run = toggleSelectedRealPiece,
                       .doc = "Shows the true piece of the selected joints (the real head, for facial animation) while the rest stays proxy."});

    static std::string last;
    last = modes::current();
    const events::SubscriptionId sub = events::subscribe(
        events::Event::ModeChanged,
        [](const events::EventData& e) {
            const std::string from = std::exchange(last, e.name.empty() ? modes::current() : e.name);
            modeChanged(from, last);
        },
        "Puppeteer");
    ctx.track("puppeteer:animProxyModes", [sub] { events::unsubscribe(sub); });
}
} // namespace puppeteer
