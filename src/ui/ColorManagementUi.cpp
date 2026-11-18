#include "ui/ColorManagementUi.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "render/Ocio.h"
#include "scene/ColorManagement.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Controls.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
namespace cm = scene::colorManagement;

/** A combo of a config's names, the first entry "Default" (or "None") standing for the empty string. */
QComboBox* nameCombo(const std::vector<std::string>& names, const std::string& current, const QString& empty, const QString& defaultName)
{
    auto* box = new QComboBox;
    box->addItem(defaultName.isEmpty() ? empty : empty + " (" + defaultName + ")", QString());
    for (const std::string& n : names)
        box->addItem(QString::fromStdString(n), QString::fromStdString(n));
    const int at = box->findData(QString::fromStdString(current));
    box->setCurrentIndex(at >= 0 ? at : 0);
    return box;
}

/** The OCIO display, view and look as the config offers them, and why the config doesn't load when it doesn't. */
QWidget* ocioPanel(scene::NodeId id)
{
    const scene::Graph& g = scene::Graph::current();
    const scene::Node* n = g.node(id);
    if (!n)
        return nullptr;
    const cm::Settings s = cm::settings(g, PrefStore::get().value<std::string>("general", cm::kConfigPref, ""));
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    QWidget* banner = controls::warningBanner(w);
    v->addWidget(banner);
    const render::ocio::ConfigInfo info = render::ocio::info(s.config, s.display);
    if (!info.ok)
    {
        controls::setWarning(banner, QString("OCIO config failed: %1. Lila's tonemapper draws instead.").arg(QString::fromStdString(info.error)));
        return w;
    }
    if (s.forced)
        controls::setWarning(banner, QString("The studio sets the config: %1").arg(QString::fromStdString(s.config)));
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* config = new QLabel(QString::fromStdString(info.name));
    config->setToolTip(QString("Lila's scene-linear Rec.709 reads as %1 in this config.").arg(QString::fromStdString(info.workingSpace)));
    form->addRow("Config", config);
    const std::string name = n->name();
    auto bind = [name](QComboBox* box, const char* attr) {
        QObject::connect(box, &QComboBox::activated, box, [box, name, attr](int) {
            try
            {
                ops::execute("setAttr", {{"args", {name + "." + attr, box->currentData().toString().toStdString()}}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        });
    };
    QComboBox* display = nameCombo(info.displays, s.display, "Default", QString::fromStdString(info.defaultDisplay));
    display->setToolTip("The display the picture is for.");
    bind(display, "display");
    form->addRow("Display", display);
    QComboBox* view = nameCombo(info.views, s.view, "Default", QString::fromStdString(info.defaultView));
    view->setToolTip("The view transform: how scene light maps to the display.");
    bind(view, "view");
    form->addRow("View", view);
    QComboBox* look = nameCombo(info.looks, s.look, "None", {});
    look->setToolTip("A creative look applied before the view.");
    look->setEnabled(!info.looks.empty());
    bind(look, "look");
    form->addRow("Look", look);
    v->addLayout(form);
    w->setEnabled(s.ocio);
    if (!s.ocio)
        w->setToolTip("Set Mode to ocio to use these.");
    return w;
}
} // namespace

void registerColorManagementUi()
{
    actions::add({.id = "render.colorManagement",
                  .label = "Colour Management",
                  .category = "Render",
                  .run =
                      [] {
                          try
                          {
                              const std::string name = ops::execute("colorManagement", json::object(), CallSource::UI).get<std::string>();
                              ops::execute("select", {{"args", {name}}}, CallSource::UI);
                          }
                          catch (const CommandError& e)
                          {
                              log::error("{}", e.what());
                          }
                      },
                  .doc = "Selects the scene's colour management (creating it if it has none): Lila's tonemapper or an OpenColorIO config's "
                         "display, view and look, for the viewport and renders.",
                  .python = "lila.ops.colorManagement()",
                  .icon = "palette",
                  .keywords = {"ocio", "opencolorio", "aces", "color management", "colour", "view transform", "display", "filmic", "agx",
                               "resolve", "nuke", "blender", "unreal"}});
    attributeEditor::addSection("colorManagement", "OCIO", [](scene::NodeId id) { return ocioPanel(id); });
}
} // namespace lila::ui
