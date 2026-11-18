#include "ui/SceneSettingsPanel.h"

#include "core/Paths.h"
#include "io/ExportOnSave.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "scene/Georef.h"
#include "scene/SceneData.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
namespace fs = std::filesystem;

constexpr const char* kDock = "sceneSettings";

/** Scene-wide settings that travel with the scene file: its place on the Earth and the file every save exports. */
class SceneSettingsPanel : public QWidget
{
public:
    explicit SceneSettingsPanel(QWidget* parent) : QWidget(parent)
    {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(8, 8, 8, 8);
        outer->setSpacing(8);

        auto* body = new QWidget(this);
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        int row = 0;
        auto label = [&](const QString& text) {
            auto* l = new QLabel(text, body);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
        };

        label("CRS");
        mCrs = new QLineEdit(body);
        mCrs->setPlaceholderText("EPSG code or WKT");
        mCrs->setToolTip("The scene's coordinate reference system: a projected one such as UTM or a national grid. Type a code, or part of "
                         "a name (\"UTM zone 33N\") to search.");
        auto* more = controls::overflow({"scene.clearGeoreference"}, body);
        auto* crsRow = new QHBoxLayout();
        crsRow->setSpacing(4);
        crsRow->addWidget(mCrs, 1);
        crsRow->addWidget(more);
        grid->addLayout(crsRow, row++, 1);
        mName = new QLabel(body);
        controls::setRole(mName, "dim");
        mName->setWordWrap(true);
        grid->addWidget(mName, row++, 1);

        auto field = [&](const QString& text, double lo, double hi, int decimals, const QString& unit, const QString& tip) {
            label(text);
            QDoubleSpinBox* f = controls::valueField(lo, hi, decimals, unit, body);
            f->setToolTip(tip);
            grid->addWidget(f, row++, 1);
            return f;
        };
        mEast = field("Easting", -1e10, 1e10, 3, " m", "The scene origin's easting in the CRS.");
        mNorth = field("Northing", -1e10, 1e10, 3, " m", "The scene origin's northing in the CRS.");
        mHeight = field("Height", -1e6, 1e6, 3, " m", "The scene origin's height in the CRS's vertical datum.");
        mLat = field("Latitude", -90.0, 90.0, 7, QString::fromUtf8("°"), "The scene origin's latitude; setting it projects into the CRS.");
        mLon = field("Longitude", -180.0, 180.0, 7, QString::fromUtf8("°"), "The scene origin's longitude; setting it projects into the CRS.");
        grid->setColumnStretch(1, 1);

        outer->addWidget(controls::section("Georeference", body, this, "/sceneSettings/sections"));
        outer->addWidget(controls::section("Export on Save", buildExportOnSave(), this, "/sceneSettings/sections"));
        outer->addStretch(1);

        auto* completer = new QCompleter(this);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        completer->setMaxVisibleItems(16);
        mCrs->setCompleter(completer);
        // PROJ's list takes a moment to read, so it loads on first focus rather than with the panel.
        connect(mCrs, &QLineEdit::textEdited, this, [this, completer] {
            if (completer->model())
                return;
            QStringList items;
            for (const auto& [code, name] : scene::georef::search("", 100000))
                items << QString::fromStdString(code + "  " + name);
            completer->setModel(new QStringListModel(items, completer));
            completer->complete();
        });
        connect(mCrs, &QLineEdit::editingFinished, this, [this] {
            // A completer entry is "EPSG:32633  WGS 84 / UTM zone 33N": the code is what precedes the gap.
            QString text = mCrs->text().trimmed();
            if (const int gap = text.indexOf("  "); gap > 0)
                text = text.left(gap);
            if (!mCrs->isModified() || text.isEmpty())
                return refresh(true);
            mCrs->setModified(false);
            runUi("setGeoreference", {{"crs", text.toStdString()}});
            refresh(true);
        });
        auto commitProjected = [this] {
            if (mLoading)
                return;
            runUi("setGeoreference", {{"origin", {mEast->value(), mNorth->value(), mHeight->value()}}});
            refresh(true);
        };
        auto commitGeographic = [this] {
            if (mLoading)
                return;
            runUi("setGeoreference", {{"latLonHeight", {mLat->value(), mLon->value(), mHeight->value()}}});
            refresh(true);
        };
        for (QDoubleSpinBox* f : {mEast, mNorth, mHeight})
            connect(f, &QDoubleSpinBox::editingFinished, this, commitProjected);
        for (QDoubleSpinBox* f : {mLat, mLon})
            connect(f, &QDoubleSpinBox::editingFinished, this, commitGeographic);

        // Undo, scripts and file opens change the georeference too; the panel follows while it is shown.
        auto* poll = new QTimer(this);
        poll->setInterval(500);
        connect(poll, &QTimer::timeout, this, [this] {
            if (isVisible())
            {
                refresh(false);
                refreshExport(false);
            }
        });
        poll->start();
        refresh(true);
        refreshExport(true);
    }

private:
    QWidget* buildExportOnSave()
    {
        auto* body = new QWidget(this);
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        int row = 0;
        auto label = [&](const QString& text) {
            auto* l = new QLabel(text, body);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
        };

        label("Enabled");
        mExportOn = new QCheckBox(body);
        mExportOn->setToolTip("Every save also exports the whole scene to File, so an engine can watch a plain FBX or glTF.");
        grid->addWidget(mExportOn, row++, 1);

        label("File");
        mExportPath = new QLineEdit(body);
        mExportPath->setPlaceholderText("Exports/Scene.fbx");
        mExportPath->setToolTip("Relative to the scene file, or absolute. The extension picks the format: .fbx, .glb, .gltf, .obj, ...");
        auto* browse = new QToolButton(body);
        browse->setIcon(icon("folder-open"));
        browse->setAutoRaise(true);
        browse->setToolTip("Choose the export file");
        auto* pathRow = new QHBoxLayout();
        pathRow->setSpacing(4);
        pathRow->addWidget(mExportPath, 1);
        pathRow->addWidget(browse);
        grid->addLayout(pathRow, row++, 1);

        label("Axes");
        mExportAxes = new QComboBox(body);
        mExportAxes->addItem("Unreal 5 (Z up)", "ue5");
        mExportAxes->addItem("Unity, Godot, Unreal 6 (Y up)", "unity");
        mExportAxes->addItem("Scene axes", "generic");
        mExportAxes->setToolTip("FBX axis profile. glTF is always Y up.");
        grid->addWidget(mExportAxes, row++, 1);

        mExportStatus = new QLabel(body);
        controls::setRole(mExportStatus, "dim");
        mExportStatus->setWordWrap(true);
        grid->addWidget(mExportStatus, row++, 1);
        grid->setColumnStretch(1, 1);

        connect(mExportOn, &QCheckBox::toggled, this, [this](bool on) {
            if (mLoading)
                return;
            runUi("exportOnSave", {{"enable", on}});
            refreshExport(true);
        });
        connect(mExportPath, &QLineEdit::editingFinished, this, [this] {
            if (!mExportPath->isModified())
                return;
            mExportPath->setModified(false);
            const std::string text = mExportPath->text().trimmed().toStdString();
            if (text.empty())
                runUi("exportOnSave", {{"enable", false}});
            else
                runUi("exportOnSave", {{"path", text}});
            refreshExport(true);
        });
        connect(browse, &QToolButton::clicked, this, [this] { pickExportFile(); });
        connect(mExportAxes, &QComboBox::activated, this, [this] {
            json options = io::exportOnSave::setting().value("options", json::object());
            options["profile"] = mExportAxes->currentData().toString().toStdString();
            runUi("exportOnSave", {{"options", options}});
            refreshExport(true);
        });
        return body;
    }

    void pickExportFile()
    {
        const fs::path scene = io::currentPath();
        const QString dir = scene.empty() ? QString() : QString::fromStdWString(scene.parent_path().wstring());
        const QString filter = QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Export, {}));
        const ui::fileDialog::ContextScope scope("export", "Exports");
        const QString file = ui::fileDialog::getSaveFileName(this, "Export on Save", dir, filter);
        if (file.isEmpty())
            return;
        fs::path picked = fs::path(file.toStdWString());
        // Kept relative when it sits under the scene's folder, so the setting travels with the project.
        if (!scene.empty())
            if (fs::path rel = picked.lexically_relative(scene.parent_path()); !rel.empty() && *rel.begin() != "..")
                picked = rel;
        runUi("exportOnSave", {{"path", paths::toUtf8(fs::path(picked.generic_wstring()))}});
        refreshExport(true);
    }

    void refreshExport(bool force)
    {
        const json now = io::exportOnSave::setting();
        const fs::path scene = io::currentPath();
        if (!force && now == mExportShown && scene == mExportScene)
            return;
        mExportShown = now;
        mExportScene = scene;
        mLoading = true;
        const std::string path = now.value("path", std::string());
        const bool on = now.value("enabled", false);
        mExportOn->setChecked(on);
        mExportOn->setEnabled(!path.empty());
        if (!mExportPath->hasFocus())
        {
            mExportPath->setText(QString::fromStdString(path));
            mExportPath->setModified(false);
        }
        const json options = now.value("options", json::object());
        const std::string profile = options.is_object() ? options.value("profile", std::string("ue5")) : std::string("ue5");
        const int axes = mExportAxes->findData(QString::fromStdString(profile == "ue6" ? std::string("unity") : profile));
        mExportAxes->setCurrentIndex(axes < 0 ? 0 : axes);
        const io::translators::FileTranslator* t =
            path.empty() ? nullptr : io::translators::forPath(io::translators::Op::Export, paths::fromUtf8(path));
        mExportAxes->setEnabled(on && t && t->id == "fbx");
        if (path.empty())
            mExportStatus->setText("No export file");
        else if (!t)
            mExportStatus->setText("No exporter for this extension");
        else if (!on)
            mExportStatus->setText("Off");
        else if (scene.empty())
            mExportStatus->setText(QString::fromStdString(t->label + ", relative to the scene once saved"));
        else
            mExportStatus->setText(QString::fromStdString(t->label + " to " + paths::toUtf8(io::exportOnSave::target(scene))));
        mLoading = false;
    }

    void refresh(bool force)
    {
        const json now = scene::data::get("georeference");
        if (!force && now == mShown)
            return;
        mShown = now;
        mLoading = true;
        const std::optional<scene::georef::Georeference> g = scene::georef::current();
        const bool set = g.has_value();
        // The EPSG code when PROJ knows one, so a WKT CRS still reads (and edits) on one line.
        if (!mCrs->hasFocus())
        {
            const std::string code = set ? scene::georef::crsInfo(g->crs).authority : std::string();
            mCrs->setText(QString::fromStdString(set ? (code.empty() ? g->crs : code) : std::string()));
            mCrs->setModified(false);
        }
        for (QWidget* w : std::initializer_list<QWidget*>{mEast, mNorth, mHeight, mLat, mLon})
            w->setEnabled(set);
        if (!set)
        {
            mName->setText("No georeference");
            for (QDoubleSpinBox* f : {mEast, mNorth, mHeight, mLat, mLon})
                f->setValue(0.0);
            mLoading = false;
            return;
        }
        const scene::georef::CrsInfo info = scene::georef::crsInfo(g->crs);
        mName->setText(QString::fromStdString(info.name));
        const QString unit = info.unitName.empty() || info.unitName == "metre" ? " m" : " " + QString::fromStdString(info.unitName);
        for (QDoubleSpinBox* f : {mEast, mNorth, mHeight})
            f->setSuffix(unit);
        mEast->setValue(g->origin.x);
        mNorth->setValue(g->origin.y);
        mHeight->setValue(g->origin.z);
        if (const std::optional<glm::dvec3> ll = scene::georef::toGeographic(g->crs, g->origin))
        {
            mLat->setValue(ll->x);
            mLon->setValue(ll->y);
        }
        mLoading = false;
    }

    QLineEdit* mCrs;
    QLabel* mName;
    QDoubleSpinBox *mEast, *mNorth, *mHeight, *mLat, *mLon;
    json mShown;
    QCheckBox* mExportOn = nullptr;
    QLineEdit* mExportPath = nullptr;
    QComboBox* mExportAxes = nullptr;
    QLabel* mExportStatus = nullptr;
    json mExportShown;
    fs::path mExportScene;
    bool mLoading = false;
};
} // namespace

void registerSceneSettingsPanel()
{
    actions::add({.id = "scene.clearGeoreference",
                  .label = "Clear Georeference",
                  .category = "Scene",
                  .run = [] { runUi("setGeoreference", {{"clear", true}}); },
                  .doc = "Remove the scene's georeference. Nothing in the scene moves.",
                  .keywords = {"crs", "epsg", "geolocation", "location"}});
    actions::add({.id = "file.exportOnSave",
                  .label = "Export on Save...",
                  .category = "File",
                  .run = [] { panels::show(kDock); },
                  .doc = "Set a file every save of this scene also exports to, for an engine to watch.",
                  .keywords = {"auto export", "autoexport", "export on save", "watch", "unity", "godot", "fbx", "gltf", "glb"}});
    panels::add({.id = kDock,
                 .title = "Scene Settings",
                 .create = [](QWidget* p) -> QWidget* { return new SceneSettingsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "world",
                 .open = false,
                 .toggleAction = "window.sceneSettings",
                 .keywords = {"export on save", "auto export", "georeference", "georeferencing", "crs", "epsg", "utm", "latitude", "longitude", "geolocation", "cesium", "location"},
                 .docs = "large-worlds"});
}
} // namespace lila::ui
