// The Ground Tiles panel: presets, the source list, every setting grouped as the schema sections them, and the
// generate, bake plane, ID map and export buttons, each also an action for menus, hotkeys and shelves.

#include "GroundTiles.h"
#include "lila/FileDialog.h"

#include "PanelKit.h"

#include <lila/Command.h>
#include <lila/Events.h>
#include <lila/Graph.h>
#include <lila/Icons.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <map>
#include <random>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace groundtiles
{
namespace
{
constexpr const char* kPanelId = "groundTiles";

QString qs(const std::string& s) { return QString::fromStdString(s); }

json run(const char* name, const json& call = json::object(), bool echo = true, std::string* error = nullptr)
{
    try
    {
        return ops::execute(name, call, echo ? CallSource::UI : CallSource::Script);
    }
    catch (const std::exception& e)
    {
        if (error)
            *error = e.what();
        log::write(LogLevel::Error, e.what());
        return nullptr;
    }
}

/** A titled block whose contents fold away under its header. */
struct Section
{
    QWidget* root = nullptr;
    QWidget* body = nullptr;
    QVBoxLayout* layout = nullptr;
};

Section makeSection(QWidget* parent, const QString& title, bool collapsed)
{
    Section s;
    s.body = new QWidget;
    s.layout = new QVBoxLayout(s.body);
    s.layout->setContentsMargins(0, 0, 0, 0);
    s.layout->setSpacing(4);
    s.root = panelkit::section(title, s.body, parent, kPanelId, !collapsed);
    return s;
}

/** A labelled button on the raised fill, for a step that is not an action. */
QToolButton* tilePush(QWidget* parent, const QString& label, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(tip);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    panelkit::setRole(b, "push");
    return b;
}

/** A small dim line: readouts and empty states. */
QLabel* dimLine(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setEnabled(false);
    return l;
}

QString swatchStyle(const glm::dvec3& c)
{
    // The ID colour is data, so the swatch shows it as is.
    return QString("QToolButton { background-color: rgb(%1, %2, %3); border: none; border-radius: 3px; }")
        .arg(int(std::lround(std::clamp(c.r, 0.0, 1.0) * 255.0)))
        .arg(int(std::lround(std::clamp(c.g, 0.0, 1.0) * 255.0)))
        .arg(int(std::lround(std::clamp(c.b, 0.0, 1.0) * 255.0)));
}

/** One setting's editor: a slider field for numbers (integer ones show no decimals), a check box or a choice. */
struct Control
{
    const Param* param = nullptr;
    QWidget* row = nullptr;
    QDoubleSpinBox* real = nullptr;
    QDoubleSpinBox* integer = nullptr;
    QCheckBox* check = nullptr;
    QComboBox* combo = nullptr;
};

class Panel : public QWidget
{
public:
    explicit Panel(QWidget* parent) : QWidget(parent)
    {
        mPrefs = loadPrefs();
        mAuto["name"] = mPrefs["Export"].value("autoName", true);
        mAuto["material"] = mPrefs["Export"].value("autoMaterial", true);
        mPreset = mPrefs["_default"].get<std::string>();
        if (!hasPreset(mPreset))
            mPreset = defaultPreset();
        build();
        applyValues(presetSettings(mPreset, mPrefs["Presets"]));
        applyPresetNaming();
        mSelectionSub = events::subscribe(events::Event::SelectionChanged, [this](const events::EventData&) { onSelectionChanged(); }, "GroundTiles");
    }

    ~Panel() override { events::unsubscribe(mSelectionSub); }

    // ------------------------------------------------------------------------------------------------ actions

    void generate()
    {
        if (mSources.empty())
        {
            status("Add at least one source mesh before generating.");
            return;
        }
        report(run("groundTileGenerate", generateCall()));
    }

    void regenerate()
    {
        const json s = run("groundTileSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated ground tile group to regenerate.");
            return;
        }
        json call = generateCall();
        call["args"] = {s["group"]};
        report(run("groundTileRegenerate", call));
    }

    void deleteSelected()
    {
        const json name = run("groundTileDelete");
        status(name.is_string() ? "Deleted " + name.get<std::string>() + "." : "Select a generated ground tile group to delete.");
    }

    void createBakePlane()
    {
        const json s = run("groundTileSettings", json::object(), false);
        json call = {{"values", readValues()}, {"preset", mPreset}};
        if (s.is_object())
            call["args"] = {s["group"]};
        const json plane = run("groundTileCreateBakePlane", call);
        if (plane.is_string())
            status("Created " + plane.get<std::string>() + ".");
    }

    void bakeIdMap()
    {
        const json s = run("groundTileSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated ground tile group to bake.");
            return;
        }
        if (!s.contains("bakePlane"))
        {
            status(s["group"].get<std::string>() + " has no bake plane. Click Create Bake Plane first.");
            return;
        }
        const json exp = exportSettings();
        std::string target;
        if (!exp["path"].get<std::string>().empty() && !exp["name"].get<std::string>().empty())
            target = exp["path"].get<std::string>() + "/" + exp["name"].get<std::string>() + kIdMapSuffix + ".png";
        else
        {
            const QString chosen = ui::fileDialog::getSaveFileName(this, "Bake ID Map", QString(), "PNG (*.png);;TIFF (*.tif);;Targa (*.tga)");
            if (chosen.isEmpty())
                return;
            target = chosen.toStdString();
        }
        std::string error;
        const json path = run("groundTileBakeIdMap", {{"args", {s["group"]}}, {"path", target}, {"resolution", int(std::lround(mControls["bakeResolution"].integer->value()))}}, true, &error);
        status(path.is_string() ? "Baked ID map to " + path.get<std::string>() + "." : error);
    }

    void exportBakes()
    {
        const json s = run("groundTileSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated ground tile group to export.");
            return;
        }
        const json exp = exportSettings();
        std::string error;
        const json r = run("groundTileExport", {{"args", {s["group"]}}, {"path", exp["path"]}, {"name", exp["name"]}, {"material", exp["material"]}}, true, &error);
        if (!r.is_object())
        {
            status(error);
            return;
        }
        status("Exported " + r["high"].get<std::string>() + (r["low"].is_string() ? " and " + r["low"].get<std::string>() : " (no bake plane)") + ".");
    }

    void measureHeight()
    {
        std::string error;
        const json r = run("groundTileMeasureHeight", json::object(), true, &error);
        if (!r.is_object())
        {
            status(error);
            return;
        }
        const json& b = r["bake"];
        char buf[256];
        std::snprintf(buf, sizeof buf, "Cage %.2f / %.2f, height outer %.2f inner %.2f. Full list in the script editor.", b["cageMax"].get<double>(),
                      b["cageMin"].get<double>(), b["heightOuter"].get<double>(), b["heightInner"].get<double>());
        status(buf);
    }

    void splitMasks()
    {
        const json exp = exportSettings();
        const json found = run("groundTileFindVertexMap", {{"args", {exp["path"]}}, {"name", exp["name"]}}, false);
        if (!found.is_string())
        {
            status("No vertex colour map in " + exp["path"].get<std::string>() + ". Bake one first.");
            return;
        }
        const json written = run("groundTileSplitMasks", {{"args", {found}}});
        status("Split " + found.get<std::string>() + " into " + std::to_string(written.is_array() ? written.size() : 0) + " mask(s).");
    }

    void addSelectedSources()
    {
        Graph& g = Graph::current();
        if (g.selection().empty())
        {
            status("Nothing is selected. Select the meshes you want to scatter.");
            return;
        }
        int added = 0;
        for (NodeId id : g.selection())
        {
            Node* n = g.node(id);
            if (!n)
                continue;
            const std::string name = n->path();
            if (std::any_of(mSources.begin(), mSources.end(), [&](const json& s) { return s["name"] == name; }))
                continue;
            if (!n->type().isA("mesh"))
            {
                log::warn("Ground Tiles: '{}' is not a polygon mesh, skipping.", name);
                continue;
            }
            mSources.push_back({{"name", name}, {"weight", 1.0}, {"objectId", 0}, {"heightOffset", 0.0}, {"enabled", true}, {"lengthAxis", "Auto"}, {"baseEnd", "Auto"}});
            ++added;
        }
        if (added)
        {
            rebuildSources();
            status("Added " + std::to_string(added) + " source mesh(es).");
        }
    }

    void randomizeSeed()
    {
        static std::mt19937 rng{std::random_device{}()};
        writeValue("seed", int64_t(std::uniform_int_distribution<int>(0, 999999)(rng)));
    }

    void loadFromSelection()
    {
        const json s = run("groundTileSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated ground tile group first.");
            return;
        }
        loadGroup(s);
    }

private:
    // ------------------------------------------------------------------------------------------------ building

    void build()
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        auto* bar = new QHBoxLayout;
        bar->setSpacing(2);
        mPresetMenu = new QComboBox(this);
        mPresetMenu->setToolTip("Preset: the settings to start from. A * marks a built-in preset you changed.");
        bar->addWidget(mPresetMenu, 1);
        auto tool = [&](const char* iconName, const char* tip, std::function<void()> fn) {
            auto* b = new QToolButton(this);
            b->setIcon(icon(iconName));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, std::move(fn));
            bar->addWidget(b);
            return b;
        };
        tool("device-floppy", "Save Preset: save the current settings as a preset", [this] { savePreset(); });
        tool("arrow-back-up", "Reset to Preset: put every setting back to the preset's", [this] {
            applyValues(presetSettings(mPreset, mPrefs["Presets"]));
            status("Reset to '" + mPreset + "'.");
        });
        tool("download", "Load Settings: load the selected generated group's settings", [this] { loadFromSelection(); });
        QToolButton* moreButton = tool("dots", "More preset commands", [] {});
        auto* moreMenu = new QMenu(moreButton);
        moreMenu->addAction(icon("star"), "Set Default Preset", this, [this] {
            run("groundTileSetDefaultPreset", {{"args", {mPreset}}}, false);
            mPrefs = loadPrefs();
            status("Default preset set to " + mPreset + ".");
        });
        moreMenu->addAction(icon("trash"), "Delete Preset", this, [this] { deletePreset(); });
        moreMenu->addSeparator();
        QAction* autoLoad = moreMenu->addAction("Load Settings on Selection");
        autoLoad->setCheckable(true);
        autoLoad->setChecked(mPrefs["Window"].value("autoLoadSelection", true));
        autoLoad->setToolTip("Selecting a generated group loads its settings into the panel");
        connect(autoLoad, &QAction::toggled, this, [this](bool on) {
            mPrefs["Window"]["autoLoadSelection"] = on;
            savePrefs(mPrefs);
        });
        moreMenu->addSeparator();
        moreMenu->addAction(icon("info-circle"), "About Ground Tiles", this, [this] {
            QMessageBox::about(this, "Ground Tiles",
                               QString("Ground Tiles v%1\nScatters seamlessly tiling ground for baking to tiling textures.\nCreated by: Jared Taylor").arg(kVersion));
        });
        moreMenu->setToolTipsVisible(true);
        moreButton->setMenu(moreMenu);
        moreButton->setPopupMode(QToolButton::InstantPopup);
        v->addLayout(bar);
        connect(mPresetMenu, &QComboBox::activated, this, [this](int i) { changePreset(mPresetMenu->itemData(i).toString().toStdString()); });

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget(scroll);
        mBody = new QVBoxLayout(body);
        mBody->setContentsMargins(0, 0, 0, 0);
        mBody->setSpacing(2);
        scroll->setWidget(body);
        v->addWidget(scroll, 1);

        buildSources(body);
        for (const auto& [id, label] : sections())
            buildSection(body, id, label);
        buildExport(body);
        mBody->addStretch(1);

        mStatus = dimLine({}, this);
        v->addWidget(mStatus);
        v->addWidget(panelkit::grid({panelkit::push("groundTiles.generate", this, "Generate"), panelkit::push("groundTiles.regenerate", this, "Regenerate"),
                                     panelkit::push("groundTiles.bakePlane", this, "Bake Plane"), panelkit::push("groundTiles.bakeIdMap", this, "Bake ID Map")},
                                    2, this));
        v->addWidget(panelkit::overflow({"groundTiles.delete"}, this));
        updatePresetMenu();
    }

    void buildSources(QWidget* body)
    {
        Section s = makeSection(body, "Source Meshes", false);
        QToolButton* add = panelkit::push("groundTiles.addSources", s.body, "Add Selected");
        add->setToolTip("Add Selected: add every selected polygon mesh as a scatter source. Pivot at the base; the longest axis is the length, "
                        "the thinnest the bend direction. Right-click a source to override what was detected.");
        QToolButton* clear = tilePush(s.body, "Remove All", "Remove All: clear the source list.");
        connect(clear, &QToolButton::clicked, this, [this] {
            mSources.clear();
            rebuildSources();
        });
        s.layout->addWidget(panelkit::grid({add, clear}, 2, s.body));
        mSourceHolder = new QWidget(s.body);
        mSourceRows = new QVBoxLayout(mSourceHolder);
        mSourceRows->setContentsMargins(0, 0, 0, 0);
        mSourceRows->setSpacing(1);
        s.layout->addWidget(mSourceHolder);
        mBody->addWidget(s.root);
        rebuildSources();
    }

    void rebuildSources()
    {
        while (QLayoutItem* item = mSourceRows->takeAt(0))
        {
            if (QWidget* w = item->widget())
                w->deleteLater();
            delete item;
        }
        if (mSources.empty())
        {
            mSourceRows->addWidget(dimLine("No sources. Select meshes and click Add Selected.", mSourceHolder));
            return;
        }
        {
            // Column names for the fields in each source row.
            auto* head = new QWidget(mSourceHolder);
            auto* h = new QHBoxLayout(head);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            auto column = [&](const char* text, int width, int stretch) {
                QLabel* l = dimLine(text, head);
                l->setWordWrap(false);
                l->setAlignment((width ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
                if (width)
                    l->setFixedWidth(width);
                h->addWidget(l, stretch);
            };
            column("Source", 0, 1);
            column("Weight", 70, 0);
            column("ID", 44, 0);
            column("Offset", 60, 0);
            h->addSpacing(22 + 3 + 22 + 3);
            mSourceRows->addWidget(head);
        }
        for (size_t i = 0; i < mSources.size(); ++i)
        {
            json& src = mSources[i];
            auto* w = new QWidget(mSourceHolder);
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            auto* on = new QCheckBox(w);
            on->setChecked(src.value("enabled", true));
            on->setToolTip("Include this source in the scatter.");
            connect(on, &QCheckBox::toggled, this, [this, i](bool v) { mSources[i]["enabled"] = v; });
            h->addWidget(on);

            const json info = run("groundTileSourceInfo", {{"args", {src["name"]}}, {"lengthAxis", src.value("lengthAxis", "Auto")}, {"baseEnd", src.value("baseEnd", "Auto")}}, false);
            const bool valid = info.is_object() && info.value("valid", false);
            const std::string name = src.value("name", "");
            auto* label = new QLabel(qs(valid ? name + "   [" + info["describe"].get<std::string>() + "]" : name + "  (missing)"), w);
            label->setToolTip(valid ? qs(name + "\nLength " + info["length"].get<std::string>() + ", width " + info["width"].get<std::string>() +
                                         ", thickness " + info["thickness"].get<std::string>() + ".\nBend, twist and sine pivot from the " +
                                         (info["baseSign"].get<double>() > 0 ? "low" : "high") + " end.\nRight click to override.")
                                    : qs(name + " is not in this scene."));
            label->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(label, &QLabel::customContextMenuRequested, this, [this, i, label](const QPoint& at) { sourceMenu(i, label->mapToGlobal(at)); });
            h->addWidget(label, 1);

            QDoubleSpinBox* weight = panelkit::valueField(0.0, 1e6, 4, {}, w);
            weight->setSingleStep(0.01);
            weight->setValue(src.value("weight", 1.0));
            weight->setToolTip("Weight: relative chance this source is picked, against the other weights.");
            weight->setFixedWidth(70);
            connect(weight, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { mSources[i]["weight"] = std::max(0.0, v); });
            h->addWidget(weight);

            QDoubleSpinBox* id = panelkit::valueField(0, 9999, 0, {}, w);
            id->setValue(double(src.value("objectId", 0)));
            id->setToolTip("ID: the Object ID. Sources sharing an ID are one material and one colour in the baked ID map, so related meshes can be masked together.");
            id->setFixedWidth(44);
            connect(id, &QDoubleSpinBox::editingFinished, this, [this, i, id] {
                mSources[i]["objectId"] = std::max(0, int(std::lround(id->value())));
                QTimer::singleShot(0, this, [this] { rebuildSources(); });
            });
            h->addWidget(id);

            QDoubleSpinBox* offset = panelkit::valueField(-1e5, 1e5, 2, {}, w);
            offset->setSingleStep(0.1);
            offset->setValue(src.value("heightOffset", 0.0));
            offset->setToolTip("Offset: offset along the up axis for this source only, on top of its height layer. Lift a source clear of the others so nothing scatters over it.");
            offset->setFixedWidth(60);
            connect(offset, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { mSources[i]["heightOffset"] = v; });
            h->addWidget(offset);

            auto* swatch = new QToolButton(w);
            swatch->setFixedSize(22, 18);
            swatch->setStyleSheet(swatchStyle(idColour(src.value("objectId", 0), mPrefs["Palette"])));
            swatch->setProperty("lilaRawColors", true);
            swatch->setToolTip("ID Colour: the colour this Object ID bakes as. Click to change it.");
            connect(swatch, &QToolButton::clicked, this, [this, i] { editIdColour(mSources[i].value("objectId", 0)); });
            h->addWidget(swatch);

            auto* remove = new QToolButton(w);
            remove->setIcon(icon("x"));
            remove->setAutoRaise(true);
            remove->setToolTip("Remove: take this source off the list.");
            connect(remove, &QToolButton::clicked, this, [this, i] {
                mSources.erase(mSources.begin() + std::ptrdiff_t(i));
                QTimer::singleShot(0, this, [this] { rebuildSources(); });
            });
            h->addWidget(remove);
            mSourceRows->addWidget(w);
        }
    }

    void sourceMenu(size_t i, const QPoint& at)
    {
        QMenu menu(this);
        auto choices = [&](const char* title, const char* key, std::initializer_list<const char*> options) {
            QMenu* sub = menu.addMenu(title);
            for (const char* c : options)
            {
                QAction* a = sub->addAction(c);
                a->setCheckable(true);
                a->setChecked(mSources[i].value(key, "Auto") == c);
                connect(a, &QAction::triggered, this, [this, i, key, c] {
                    mSources[i][key] = c;
                    QTimer::singleShot(0, this, [this] { rebuildSources(); });
                });
            }
        };
        choices("Length Axis", "lengthAxis", {"Auto", "X", "Y", "Z"});
        choices("Base End", "baseEnd", {"Auto", "Min", "Max"});
        menu.addSeparator();
        QAction* select = menu.addAction("Select In Scene");
        connect(select, &QAction::triggered, this, [this, i] { run("select", {{"args", {mSources[i]["name"]}}}); });
        menu.exec(at);
    }

    void buildSection(QWidget* body, const std::string& id, const std::string& label)
    {
        Section s = makeSection(body, qs(label), id == "output");
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        int row = 0;
        for (const Param& p : parameters())
        {
            if (p.section != id)
                continue;
            mControls[p.name] = buildControl(s.body, grid, row++, p);
        }
        s.layout->addLayout(grid);
        if (id == "output")
        {
            auto* w = new QWidget(s.body);
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(new QLabel("Ground Colour", w));
            mGroundSwatch = new QToolButton(w);
            mGroundSwatch->setFixedSize(22, 18);
            mGroundSwatch->setProperty("lilaRawColors", true);
            mGroundSwatch->setToolTip("Ground Colour: the colour the ground plane's Object ID bakes as. Click to change it.");
            connect(mGroundSwatch, &QToolButton::clicked, this, [this] { editIdColour(std::lround(mControls["groundId"].integer->value())); });
            h->addWidget(mGroundSwatch);
            h->addStretch(1);
            mGroundRow = w;
            s.layout->addWidget(w);
        }
        if (id == "distribution")
        {
            mClusterReadout = dimLine({}, s.body);
            mClusterReadout->setToolTip("Cluster discs that add up to more than the tile area cover it evenly, which reads as uniform scatter rather than clumps.");
            s.layout->addWidget(mClusterReadout);
        }
        if (id == "tile")
        {
            s.layout->addWidget(panelkit::push("groundTiles.randomizeSeed", s.body, "Randomize Seed"));
        }
        mBody->addWidget(s.root);
    }

    Control buildControl(QWidget* parent, QGridLayout* grid, int row, const Param& p)
    {
        Control c;
        c.param = &p;
        const QString tip = qs(p.help);
        auto* label = new QLabel(qs(p.label), parent);
        label->setToolTip(tip);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, row, 0);
        // Row widgets show and hide together, so they share a holder that stands in for the row.
        c.row = label;
        const std::string name = p.name;
        if (p.type == "float" || p.type == "int")
        {
            auto* field = panelkit::valueField(0, 1, 0, {}, parent);
            const bool real = p.type == "float";
            field->setDecimals(real ? 3 : 0);
            field->setRange(p.min, p.max);
            field->setSingleStep(real ? std::max((p.max - p.min) / 200.0, 0.001) : 1.0);
            field->setToolTip(tip);
            grid->addWidget(field, row, 1);
            (real ? c.real : c.integer) = field;
            connect(field, &QDoubleSpinBox::valueChanged, this, [this, name](double) { afterNumber(name); });
        }
        else if (p.type == "bool")
        {
            c.check = new QCheckBox(parent);
            c.check->setToolTip(tip);
            grid->addWidget(c.check, row, 1);
            connect(c.check, &QCheckBox::toggled, this, [this](bool) { onDriverChanged(); });
        }
        else
        {
            c.combo = new QComboBox(parent);
            c.combo->setToolTip(tip);
            for (const std::string& o : p.options)
                c.combo->addItem(qs(o));
            grid->addWidget(c.combo, row, 1);
            connect(c.combo, &QComboBox::currentIndexChanged, this, [this](int) { onDriverChanged(); });
        }
        return c;
    }

    void buildExport(QWidget* body)
    {
        Section s = makeSection(body, "Export", true);
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        auto field = [&](int row, const char* label, const std::string& value, const char* tip) {
            auto* l = new QLabel(label, s.body);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
            auto* e = new QLineEdit(qs(value), s.body);
            e->setToolTip(tip);
            grid->addWidget(e, row, 1);
            return e;
        };
        const json& exp = mPrefs["Export"];
        mExportPath = field(0, "Export Path", exp.value("path", ""), "Folder the FBX pair is written to.");
        auto* browse = new QToolButton(s.body);
        browse->setIcon(icon("folder-open"));
        browse->setAutoRaise(true);
        browse->setToolTip("Browse: pick the export folder.");
        grid->addWidget(browse, 0, 2);
        mExportName = field(1, "File Name", exp.value("name", ""), "Written as <name>_Hi.fbx and <name>_Lo.fbx.");
        mExportMaterial = field(2, "Bake Plane Material", exp.value("material", ""),
                                "The bake plane's material is renamed to this for the export, since that is what the baker names its outputs after. Renamed back afterwards.");
        s.layout->addLayout(grid);
        connect(browse, &QToolButton::clicked, this, [this] {
            const QString chosen = ui::fileDialog::getExistingDirectory(this, "Export Folder", mExportPath->text());
            if (chosen.isEmpty())
                return;
            mExportPath->setText(chosen);
            setExportPref("path", chosen.toStdString());
        });
        connect(mExportPath, &QLineEdit::editingFinished, this, [this] { setExportPref("path", mExportPath->text().trimmed().toStdString()); });
        connect(mExportName, &QLineEdit::editingFinished, this, [this] { setExportPref("name", mExportName->text().trimmed().toStdString()); });
        connect(mExportMaterial, &QLineEdit::editingFinished, this, [this] { setExportPref("material", mExportMaterial->text().trimmed().toStdString()); });
        mExportReadout = dimLine({}, s.body);
        s.layout->addWidget(mExportReadout);
        s.layout->addWidget(panelkit::push("groundTiles.export", s.body, "Export FBX Pair"));
        s.layout->addWidget(panelkit::overflow({"groundTiles.measureHeight", "groundTiles.splitMasks"}, s.body));
        mBody->addWidget(s.root);
    }

    // ------------------------------------------------------------------------------------------------ values

    json readValue(const Control& c) const
    {
        if (c.real)
            return c.real->value();
        if (c.integer)
            return int64_t(std::llround(c.integer->value()));
        if (c.check)
            return c.check->isChecked();
        return c.combo->currentText().toStdString();
    }

    json readValues() const
    {
        json v = json::object();
        for (const auto& [name, c] : mControls)
            v[name] = readValue(c);
        return sanitize(v);
    }

    void writeValue(const std::string& name, const json& value)
    {
        auto it = mControls.find(name);
        if (it == mControls.end())
            return;
        Control& c = it->second;
        if (c.real)
            c.real->setValue(value.is_number() ? value.get<double>() : 0.0);
        else if (c.integer)
            c.integer->setValue(value.is_number() ? double(int(value.get<double>())) : 0.0);
        else if (c.check)
            c.check->setChecked(value.is_boolean() && value.get<bool>());
        else if (c.combo)
            c.combo->setCurrentText(qs(value.is_string() ? value.get<std::string>() : ""));
    }

    void afterNumber(const std::string& name)
    {
        if (name == "clusterCount" || name == "clusterRadius" || name == "clusterStrength" || name == "tileSize")
            updateClusterReadout();
        if (name == "groundId")
            refreshGroundSwatch();
    }

    void applyValues(const json& values)
    {
        mLoading = true;
        // Range-for keeps only items()'s view alive, not the json it reads.
        const json clean = sanitize(values);
        for (const auto& [name, value] : clean.items())
            writeValue(name, value);
        mLoading = false;
        refreshVisibility();
        updateClusterReadout();
    }

    void onDriverChanged()
    {
        if (mLoading)
            return;
        refreshVisibility();
        updateClusterReadout();
    }

    void refreshVisibility()
    {
        const json values = readValues();
        for (auto& [name, c] : mControls)
        {
            const bool show = groundtiles::isVisible(*c.param, values);
            for (QWidget* w : {static_cast<QWidget*>(c.row), static_cast<QWidget*>(c.real), static_cast<QWidget*>(c.integer),
                               static_cast<QWidget*>(c.check), static_cast<QWidget*>(c.combo)})
                if (w)
                    w->setVisible(show);
        }
        if (mGroundRow)
            mGroundRow->setVisible(flag(values, "groundPlane"));
        refreshGroundSwatch();
    }

    void updateClusterReadout()
    {
        if (!mClusterReadout)
            return;
        const json values = readValues();
        const bool clustered = text(values, "distribution") == "Clustered";
        mClusterReadout->setVisible(clustered);
        if (clustered)
            mClusterReadout->setText(qs(describeClustering(values)));
    }

    void refreshGroundSwatch()
    {
        if (mGroundSwatch && mControls.count("groundId"))
            mGroundSwatch->setStyleSheet(swatchStyle(idColour(std::lround(mControls["groundId"].integer->value()), mPrefs["Palette"])));
    }

    void editIdColour(int64_t id)
    {
        const glm::dvec3 c = idColour(id, mPrefs["Palette"]);
        const QColor chosen = ui::colorPopup::getColor(QColor::fromRgbF(float(c.r), float(c.g), float(c.b)), this, "Object ID " + QString::number(id));
        if (!chosen.isValid())
            return;
        run("groundTileSetIdColour", {{"args", {id}}, {"colour", {chosen.redF(), chosen.greenF(), chosen.blueF()}}}, false);
        mPrefs = loadPrefs();
        refreshGroundSwatch();
        rebuildSources();
    }

    void status(const std::string& message)
    {
        if (mStatus)
            mStatus->setText(qs(message));
    }

    void report(const json& r)
    {
        if (!r.is_object() || !r["group"].is_string())
        {
            const json& w = r.is_object() ? r["warnings"] : json();
            status(w.is_array() && !w.empty() ? w.back().get<std::string>() : "Nothing was generated.");
            return;
        }
        const json s = run("groundTileSettings", {{"args", {r["group"]}}}, false);
        const json m = s.is_object() ? s["metrics"] : json::object();
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s: %d instances, height %.3f to %.3f, %.3f above the plane.", r["group"].get<std::string>().c_str(), r["instances"].get<int>(),
                      m.value(kAttrMinHeight, 0.0), m.value(kAttrMaxHeight, 0.0), m.value(kAttrAbovePlane, 0.0));
        status(buf);
    }

    json generateCall() const
    {
        return {{"values", readValues()}, {"sources", mSources}, {"preset", mPreset}, {"palette", mPrefs["Palette"]}, {"export", exportSettings()}};
    }

    // ------------------------------------------------------------------------------------------------ presets

    std::vector<std::string> presetNames() const
    {
        std::vector<std::string> names = builtInPresets();
        std::vector<std::string> user;
        for (auto it = mPrefs["Presets"].begin(); it != mPrefs["Presets"].end(); ++it)
            if (std::find(names.begin(), names.end(), it.key()) == names.end())
                user.push_back(it.key());
        std::sort(user.begin(), user.end());
        names.insert(names.end(), user.begin(), user.end());
        return names;
    }

    bool hasPreset(const std::string& name) const
    {
        const std::vector<std::string> names = presetNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    void updatePresetMenu()
    {
        QSignalBlocker block(mPresetMenu);
        mPresetMenu->clear();
        for (const std::string& n : presetNames())
            mPresetMenu->addItem(qs(builtInOverrides(n) ? n : n + " *"), qs(n));
        mPresetMenu->setCurrentIndex(std::max(0, mPresetMenu->findData(qs(mPreset))));
    }

    void changePreset(const std::string& name)
    {
        mPreset = name;
        applyValues(presetSettings(name, mPrefs["Presets"]));
        applyPresetNaming();
        status("Loaded preset '" + name + "'.");
    }

    void savePreset()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save Preset", "Preset Name:", QLineEdit::Normal, qs(mPreset), &ok).trimmed();
        if (!ok)
            return;
        if (name.isEmpty())
        {
            status("A preset needs a name.");
            return;
        }
        run("groundTileSavePreset", {{"args", {name.toStdString()}}, {"values", readValues()}}, false);
        mPrefs = loadPrefs();
        mPreset = name.toStdString();
        updatePresetMenu();
        status("Saved preset '" + mPreset + "'.");
    }

    void deletePreset()
    {
        if (builtInOverrides(mPreset))
        {
            status("Built-in presets cannot be deleted.");
            return;
        }
        if (QMessageBox::question(this, "Delete Preset", qs("Delete the preset '" + mPreset + "'?")) != QMessageBox::Yes)
            return;
        run("groundTileDeletePreset", {{"args", {mPreset}}}, false);
        mPrefs = loadPrefs();
        mPreset = defaultPreset();
        updatePresetMenu();
        applyValues(presetSettings(mPreset, mPrefs["Presets"]));
    }

    // ------------------------------------------------------------------------------------------------ export fields

    json exportSettings() const
    {
        return {{"path", mExportPath->text().trimmed().toStdString()},
                {"name", mExportName->text().trimmed().toStdString()},
                {"material", mExportMaterial->text().trimmed().toStdString()}};
    }

    void setExportPref(const std::string& key, const std::string& value)
    {
        mPrefs["Export"][key] = value;
        if (mAuto.count(key))
        {
            // An empty field goes back to following the preset.
            mAuto[key] = value.empty();
            mPrefs["Export"][key == "name" ? "autoName" : "autoMaterial"] = mAuto[key];
            if (mAuto[key])
                applyPresetNaming();
        }
        savePrefs(mPrefs);
        updateExportReadout();
    }

    /** File name and material follow the preset until they are typed over. */
    void applyPresetNaming()
    {
        const std::string stem = presetStem(mPreset);
        if (mAuto["name"])
        {
            mExportName->setText(qs(stem));
            mPrefs["Export"]["name"] = stem;
        }
        if (mAuto["material"])
        {
            mExportMaterial->setText(qs(stem));
            mPrefs["Export"]["material"] = stem;
        }
        updateExportReadout();
    }

    void updateExportReadout()
    {
        const json e = exportSettings();
        const std::string name = e["name"].get<std::string>();
        mExportReadout->setText(!e["path"].get<std::string>().empty() && !name.empty() ? qs(name + kHighSuffix + ".fbx and " + name + kLowSuffix + ".fbx")
                                                                                       : "Set a path and a name.");
    }

    // ------------------------------------------------------------------------------------------------ groups

    void loadGroup(const json& stored)
    {
        applyValues(stored["values"]);
        json palette = mPrefs["Palette"];
        palette.update(stored.value("palette", json::object()));
        mPrefs["Palette"] = palette;
        const json& exp = stored["export"];
        if (!exp.value("path", "").empty())
        {
            mExportPath->setText(qs(exp.value("path", "")));
            mPrefs["Export"]["path"] = exp.value("path", "");
        }
        const std::string storedStem = presetStem(stored.value("preset", ""));
        for (const auto& [key, field] : {std::pair{std::string("name"), mExportName}, std::pair{std::string("material"), mExportMaterial}})
            if (!exp.value(key, "").empty())
            {
                field->setText(qs(exp.value(key, "")));
                mPrefs["Export"][key] = exp.value(key, "");
                // A stored name that matches its preset was never typed over.
                mAuto[key] = exp.value(key, "") == storedStem;
            }
        mSources.clear();
        for (const json& s : stored["sources"])
            if (s.is_object() && !s.value("name", "").empty())
                mSources.push_back({{"name", s.value("name", "")},
                                    {"weight", s.value("weight", 1.0)},
                                    {"objectId", s.value("objectId", 0)},
                                    {"heightOffset", s.value("heightOffset", 0.0)},
                                    {"enabled", s.value("enabled", true)},
                                    {"lengthAxis", s.value("lengthAxis", "Auto")},
                                    {"baseEnd", s.value("baseEnd", "Auto")}});
        rebuildSources();
        const std::string preset = stored.value("preset", "");
        if (!preset.empty() && hasPreset(preset))
        {
            mPreset = preset;
            updatePresetMenu();
        }
        applyPresetNaming();
        status("Loaded settings from " + stored.value("group", std::string()) + ".");
    }

    void onSelectionChanged()
    {
        if (!mPrefs["Window"].value("autoLoadSelection", true) || !isVisible())
            return;
        const json s = run("groundTileSettings", json::object(), false);
        if (s.is_object())
            loadGroup(s);
    }

    json mPrefs;
    std::map<std::string, bool> mAuto;
    std::string mPreset;
    std::vector<json> mSources;
    std::map<std::string, Control> mControls;
    QComboBox* mPresetMenu = nullptr;
    QVBoxLayout* mBody = nullptr;
    QWidget* mSourceHolder = nullptr;
    QVBoxLayout* mSourceRows = nullptr;
    QLabel* mClusterReadout = nullptr;
    QToolButton* mGroundSwatch = nullptr;
    QWidget* mGroundRow = nullptr;
    QLineEdit* mExportPath = nullptr;
    QLineEdit* mExportName = nullptr;
    QLineEdit* mExportMaterial = nullptr;
    QLabel* mExportReadout = nullptr;
    QLabel* mStatus = nullptr;
    bool mLoading = false;
    events::SubscriptionId mSelectionSub = 0;
};

QPointer<Panel> gPanel;

/** Runs fn on the panel, showing it first: its fields are what the buttons act on. */
void onPanel(void (Panel::*fn)())
{
    panels::show(kPanelId);
    if (gPanel)
        (gPanel.data()->*fn)();
    else
        log::warn("Ground Tiles: open the panel (Windows > Ground Tiles) first");
}
} // namespace

void registerUi(ext::Context& ctx)
{
    panels::add(ctx, {.id = kPanelId,
                      .title = "Ground Tiles",
                      .create = [](QWidget* parent) -> QWidget* {
                          auto* p = new Panel(parent);
                          gPanel = p;
                          return p;
                      },
                      .icon = "mountain",
                      .open = false});

    struct Entry
    {
        const char* id;
        const char* label;
        const char* doc;
        void (Panel::*fn)();
    };
    const Entry entries[] = {
        {"groundTiles.generate", "Generate Tile", "Scatter a new tiling ground tile group with the Ground Tiles panel's settings.", &Panel::generate},
        {"groundTiles.regenerate", "Regenerate Tile", "Rebuild the selected generated ground tile group with the panel's settings.", &Panel::regenerate},
        {"groundTiles.delete", "Delete Tile", "Delete the selected generated ground tile group.", &Panel::deleteSelected},
        {"groundTiles.addSources", "Add Selected Tile Sources", "Add the selected meshes to the Ground Tiles's sources.", &Panel::addSelectedSources},
        {"groundTiles.bakePlane", "Create Tile Bake Plane", "A tile-sized plane with 0-1 UVs to bake onto, inside the selected generated group.", &Panel::createBakePlane},
        {"groundTiles.bakeIdMap", "Bake Tile ID Map", "Bake the Object ID colours of the selected generated group onto its bake plane.", &Panel::bakeIdMap},
        {"groundTiles.export", "Export Tile FBX Pair", "Write the selected generated group and its bake plane as <name>_Hi.fbx and <name>_Lo.fbx with the ID sidecars.", &Panel::exportBakes},
        {"groundTiles.measureHeight", "Measure Tile Height Range", "Sample the selected group's top surface and stamp its p50, p99 and over-tile heights.", &Panel::measureHeight},
        {"groundTiles.splitMasks", "Split Vertex Colour Masks", "Split the baked vertex colour map in the export path into R, G and B greyscale masks.", &Panel::splitMasks},
        {"groundTiles.randomizeSeed", "Randomize Tile Seed", "Pick a new random seed in the Ground Tiles panel.", &Panel::randomizeSeed},
        {"groundTiles.loadSelected", "Load Tile Settings", "Load the selected generated group's settings into the Ground Tiles panel.", &Panel::loadFromSelection},
    };
    for (const Entry& e : entries)
        actions::add(ctx, {.id = e.id, .label = e.label, .category = "Ground Tiles", .run = [fn = e.fn] { onPanel(fn); }, .doc = e.doc});

    menus::addItem(ctx, {.id = "groundTiles.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Ground Tiles"});
    const char* const items[] = {"window.groundTiles", "", "groundTiles.addSources", "groundTiles.generate", "groundTiles.regenerate",
                                 "groundTiles.delete", "", "groundTiles.bakePlane", "groundTiles.bakeIdMap", "groundTiles.measureHeight",
                                 "groundTiles.export", "groundTiles.splitMasks"};
    int separators = 0;
    for (const char* a : items)
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("groundTiles.menu.") + a, .menu = "Level/Ground Tiles", .action = a});
        else
            menus::addItem(ctx, {.id = "groundTiles.menu.sep" + std::to_string(separators++), .menu = "Level/Ground Tiles", .kind = menus::Kind::Separator});
    }

    levelToolkit::add(ctx, {.id = "groundTiles",
                            .title = "Ground Tiles",
                            .items = {{"window.groundTiles", "mountain", "Panel"}, {"groundTiles.addSources", "plus", "Sources"},
                                      {"groundTiles.generate", "grid-pattern", "Generate"}, {"groundTiles.regenerate", "refresh", "Regenerate"}, {},
                                      {"groundTiles.bakePlane", "square", "Bake Plane"}, {"groundTiles.bakeIdMap", "palette", "ID Map"},
                                      {"groundTiles.export", "file-export", "Export"}},
                            .order = 50});
}
} // namespace groundtiles
