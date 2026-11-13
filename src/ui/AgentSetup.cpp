#include "ui/AgentSetup.h"

#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "mcp/AgentIntegration.h"
#include "mcp/McpServer.h"
#include "ui/Actions.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui::agentSetup
{
namespace
{
QString native(const std::filesystem::path& p)
{
    std::u8string s = p.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(s.data()), qsizetype(s.size()));
}

/** The status bar's MCP toggle mirrors the server; it has no listener of its own. */
void syncMcpButton()
{
    for (QWidget* top : QApplication::topLevelWidgets())
        if (qobject_cast<QMainWindow*>(top))
            for (QToolButton* b : top->findChildren<QToolButton*>())
                if (b->text() == "MCP" && b->isCheckable())
                {
                    QSignalBlocker block(b);
                    b->setChecked(mcp::Server::get().running());
                }
}

bool dialogShowing = false;

/** Runs the dialog; true when the user chose Set Up. */
bool run(QWidget* parent, bool firstTime)
{
    Workspace& ws = Workspace::get();
    if (!ws.isOpen())
    {
        QMessageBox::information(parent, "Agent Integration", "Open a workspace folder first (File > Open Folder).");
        return false;
    }
    if (dialogShowing)
        return false;
    dialogShowing = true;
    struct Reset
    {
        ~Reset() { dialogShowing = false; }
    } reset;

    QDialog dlg(parent);
    dlg.setWindowTitle("Agent Integration");
    auto* layout = new QVBoxLayout(&dlg);
    const std::filesystem::path root = mcp::agents::setupRoot(ws.root());
    auto* intro = new QLabel(QString("Coding agents such as Claude Code can drive Lila through its MCP server: model with Lila's "
                                     "commands, build node setups, look at the viewport and check their work.<br><br>"
                                     "Set this up for <b>%1</b>?")
                                 .arg(native(root).toHtmlEscaped()));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    bool mcpOff = !mcp::Server::get().running();
    auto* mcpOn = new QCheckBox("Turn on Lila's MCP server (localhost only)");
    mcpOn->setChecked(true);
    mcpOn->setVisible(mcpOff);
    auto* mcpJson = new QCheckBox("Register Lila in .mcp.json (the token is read from Lila's settings when the agent connects, never "
                                  "written)");
    auto* instructions = new QCheckBox("Add a Lila section to CLAUDE.md and AGENTS.md (a marked block; your text is left alone)");
    auto* skill = new QCheckBox("Install Lila's skills in .claude/skills");
    for (QCheckBox* c : {mcpOn, mcpJson, instructions, skill})
    {
        if (c != mcpOn)
            c->setChecked(true);
        layout->addWidget(c);
    }
    auto* note = new QLabel("Files are written to the project root (the folder with .git or the .uproject) and not added to source "
                            "control. Agents started anywhere in the project find Lila. Run this again any time from Help > Set Up "
                            "Agent Integration.");
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    auto* buttons = new QDialogButtonBox;
    QPushButton* ok = buttons->addButton("Set Up", QDialogButtonBox::AcceptRole);
    buttons->addButton(firstTime ? "Not for This Workspace" : "Cancel", QDialogButtonBox::RejectRole);
    ok->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.resize(560, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted)
        return false;

    QStringList report;
    if (mcpOff && mcpOn->isChecked())
    {
        PrefStore::get().set("general", "/mcp/enabled", true);
        if (mcp::Server::get().start(mcp::portFromPrefs(), mcp::tokenFromPrefs()))
            report << "MCP server on at " + QString::fromStdString(mcp::Server::get().url());
        else
            report << "MCP server could not start (port in use?)";
        syncMcpButton();
    }
    mcp::agents::SetupOptions o;
    o.mcpJson = mcpJson->isChecked();
    o.instructions = instructions->isChecked();
    o.skill = skill->isChecked();
    o.port = mcp::Server::get().running() ? mcp::Server::get().port() : mcp::portFromPrefs();
    mcp::agents::SetupResult r = mcp::agents::setUp(root, o);
    for (const std::string& f : r.written)
        report << "Wrote " + QString::fromUtf8(f.c_str());
    for (const std::string& e : r.errors)
        report << "Failed: " + QString::fromUtf8(e.c_str());
    const bool failed = !r.errors.empty();
    report << "" << "Start the agent in the project (run claude again if it is already open) and approve the 'lila' server "
                    "when asked. Lila must be running with its MCP server on.";
    if (!failed)
        QMessageBox::information(parent, "Agent Integration", report.join("\n"));
    else
        QMessageBox::warning(parent, "Agent Integration", report.join("\n"));
    return true;
}
} // namespace

void open(QWidget* parent)
{
    if (run(parent, false))
        mcp::agents::setWorkspaceState(Workspace::get().root(), "done");
}

void maybePrompt(QWidget* parent)
{
    Workspace& ws = Workspace::get();
    if (!ws.isOpen() || !PrefStore::get().value<bool>("general", "/agents/promptOnTerminal", true))
        return;
    if (!mcp::agents::workspaceState(ws.root()).empty())
        return;
    // After the terminal is up, so the dialog never sits in front of a half-built panel.
    QTimer::singleShot(400, parent, [parent] {
        Workspace& w = Workspace::get();
        if (!w.isOpen() || !mcp::agents::workspaceState(w.root()).empty())
            return;
        bool done = run(parent, true);
        mcp::agents::setWorkspaceState(w.root(), done ? "done" : "declined");
    });
}

void registerActions()
{
    actions::add({"help.agentSetup", "Set Up Agent Integration...", "Help", [] { open(QApplication::activeWindow()); }, nullptr,
                  "Let coding agents (Claude Code and others) drive Lila: MCP server, .mcp.json, CLAUDE.md/AGENTS.md "
                  "instructions and Lila's skills."});
}
} // namespace lila::ui::agentSetup
