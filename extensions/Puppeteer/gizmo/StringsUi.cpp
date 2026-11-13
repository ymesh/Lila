// The Strings rules on a character's Attribute Editor: its contacts and limbs, where they come from, and buttons to set
// them from the selection.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/AttributeEditor.h>
#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/UI.h>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
QString joinedNames(const json& names, const char* separator)
{
    QStringList out;
    for (const json& n : names)
        out << QString::fromStdString(n.get<std::string>());
    return out.join(separator);
}

QLabel* dimLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

QWidget* rulesSection(NodeId character)
{
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    auto* contacts = new QLabel(w);
    contacts->setWordWrap(true);
    auto* contactsFrom = dimLabel({}, w);
    // The count leads; the list (a hand alone has five limbs) opens on demand.
    auto* limbsCount = new QToolButton(w);
    limbsCount->setCheckable(true);
    limbsCount->setAutoRaise(true);
    limbsCount->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    limbsCount->setArrowType(Qt::RightArrow);
    auto* limbs = new QLabel(w);
    limbs->setWordWrap(true);
    limbs->setVisible(false);
    QObject::connect(limbsCount, &QToolButton::toggled, limbs, [limbs, limbsCount](bool on) {
        limbs->setVisible(on);
        limbsCount->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    auto* limbsFrom = dimLabel({}, w);
    auto* contactsBox = new QVBoxLayout;
    contactsBox->setSpacing(0);
    contactsBox->addWidget(contacts);
    contactsBox->addWidget(contactsFrom);
    auto* limbsBox = new QVBoxLayout;
    limbsBox->setSpacing(0);
    limbsBox->addWidget(limbsCount, 0, Qt::AlignLeft);
    limbsBox->addWidget(limbs);
    limbsBox->addWidget(limbsFrom);
    auto* isolated = new QLabel(w);
    isolated->setWordWrap(true);
    auto* modes = new QLabel(w);
    modes->setWordWrap(true);
    form->addRow("Contacts", contactsBox);
    form->addRow("Limbs", limbsBox);
    form->addRow("Isolated", isolated);
    form->addRow("Modes", modes);
    v->addLayout(form);
    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    row->addWidget(controls::push("puppeteer.strings.contactsFromSelection", w, "Contacts from Selection"));
    row->addWidget(controls::push("puppeteer.strings.limbFromSelection", w, "Limb from Selection"));
    row->addWidget(controls::push("puppeteer.strings.resetRules", w, "Reset to Template"));
    row->addWidget(controls::push("window.puppeteerStringsRules", w, "Edit Rules"));
    row->addStretch(1);
    v->addLayout(row);

    auto fill = [=] {
        Graph& g = Graph::current();
        Node* n = g.node(character);
        if (!n)
            return;
        json r;
        try
        {
            r = ops::execute("stringsRules", {{"args", {n->path()}}});
        }
        catch (const std::exception&)
        {
            return;
        }
        const auto from = [](const json& source) { return "From " + QString::fromStdString(source.get<std::string>()); };
        contacts->setText(r["contacts"].empty() ? QString("None") : joinedNames(r["contacts"], ", "));
        contactsFrom->setText(from(r["contactsFrom"]));
        QStringList rows;
        for (const json& c : r["chains"])
            if (!c.empty())
                rows << QString("%1 to %2").arg(QString::fromStdString(c.front().get<std::string>()), QString::fromStdString(c.back().get<std::string>()));
        limbs->setText(rows.join("\n"));
        limbsCount->setText(rows.isEmpty() ? QString("None: limbs run to the next branch") : QString::number(rows.size()));
        limbsCount->setEnabled(!rows.isEmpty());
        limbsFrom->setText(from(r["chainsFrom"]));
        isolated->setText(r["isolate"].empty() ? QString("None") : joinedNames(r["isolate"], ", "));
        QStringList picks;
        for (const auto& [joint, mode] : r["modes"].items())
            if (const auto m = strings::modeFromName(mode.get<std::string>()))
                picks << QString("%1 %2").arg(QString::fromStdString(joint), QString::fromUtf8(stringsModeLabel(*m)));
        modes->setText(picks.isEmpty() ? QString("None: moves IK, turns Forward") : picks.join(", "));
    };
    fill();
    const events::SubscriptionId sub = events::subscribe(events::Event::UndoStackChanged, [fill](const events::EventData&) { fill(); }, "Puppeteer");
    QObject::connect(w, &QObject::destroyed, [sub] { events::unsubscribe(sub); });
    return w;
}

/** The selection's joints, and the character of the first. */
std::pair<std::vector<std::string>, std::string> selectedJoints()
{
    Graph& g = Graph::current();
    std::vector<std::string> joints;
    std::string character;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && rig::isJoint(*n))
        {
            joints.push_back(n->path());
            if (character.empty())
                if (const NodeId c = characterFor(g, id))
                    character = g.node(c)->path();
        }
        else if (n && n->type().isA(kCharacterType) && character.empty())
            character = n->path();
    return {joints, character};
}
} // namespace

void registerStringsUi(ext::Context& ctx)
{
    const char* cat = "Puppeteer Strings";
    actions::add(ctx, {.id = "puppeteer.strings.contactsFromSelection",
                       .label = "Contacts from Selection",
                       .category = cat,
                       .run =
                           [] {
                               const auto [joints, character] = selectedJoints();
                               if (character.empty())
                               {
                                   log::write(LogLevel::Warning, "Contacts from Selection: select joints of a character");
                                   return;
                               }
                               runCommand("stringsRules", {{"args", {character}}, {"contacts", joints}});
                           },
                       .doc = "The selected joints become the character's contacts: they hold still while you pose the rest.",
                       .icon = "shoe"});
    actions::add(ctx, {.id = "puppeteer.strings.limbFromSelection",
                       .label = "Limb from Selection",
                       .category = cat,
                       .run =
                           [] {
                               const auto [joints, character] = selectedJoints();
                               if (joints.size() != 2)
                               {
                                   log::write(LogLevel::Warning, "Limb from Selection: select the limb's top joint, then its end");
                                   return;
                               }
                               runCommand("stringsRules", {{"args", {character}}, {"addChain", joints}});
                           },
                       .doc = "Select a limb's top joint, then its end: those joints bend together when IK or a hold reaches.",
                       .icon = "vector"});
    actions::add(ctx, {.id = "puppeteer.strings.resetRules",
                       .label = "Reset Strings Rules",
                       .category = cat,
                       .run =
                           [] {
                               const auto [joints, character] = selectedJoints();
                               if (!character.empty())
                                   runCommand("stringsRules", {{"args", {character}}, {"reset", true}});
                           },
                       .doc = "The character's contacts and limbs go back to what its rig template or rig elements say.",
                       .icon = "restore"});
    actions::add(ctx, {.id = "puppeteer.strings.zeroRelative",
                       .label = "Zero Relative to Last",
                       .category = "Puppeteer Strings",
                       .run =
                           [] {
                               const auto [joints, character] = selectedJoints();
                               if (joints.size() < 2)
                                   return log::write(LogLevel::Warning, "Zero Relative to Last: select the joints to zero, then the one to zero relative to");
                               const std::vector<std::string> zeroed(joints.begin(), joints.end() - 1);
                               runCommand("stringsZero", {{"args", zeroed}, {"relativeTo", {joints.back()}}});
                           },
                       .doc = "Zero the selected joints' rotations while the last selected keeps its place in the world.",
                       .icon = "circle-dashed",
                       .keywords = {"reset", "relative", "zero"}});
    attributeEditor::addSection(ctx, kCharacterType, "Strings", [](NodeId id) { return rulesSection(id); });
}
} // namespace puppeteer
