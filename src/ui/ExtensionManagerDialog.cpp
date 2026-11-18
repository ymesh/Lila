#include "ui/ExtensionManagerDialog.h"

#include "core/Events.h"
#include "core/Log.h"
#include "core/Features.h"
#include "extensions/ExtensionManager.h"
#include "ui/FeatureStatus.h"
#include "ui/Icons.h"
#include "ui/LocateFlash.h"
#include "ui/FileDialog.h"
#include "ui/UiCommon.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using extensions::ExtensionInfo;
using extensions::ExtensionManager;

enum ExtensionColumn
{
    Name,
    Version,
    Vendor,
    Sdk,
    Loaded,
    AutoLoad,
    Status,
    ColumnCount,
};

/** A checkbox centred in its cell. */
QWidget* centredCheck(QTableWidget* table, bool on, bool enabled, const QString& tip, std::function<void(bool)> changed)
{
    auto* box = new QWidget(table);
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignCenter);
    auto* check = new QCheckBox(box);
    check->setChecked(on);
    check->setEnabled(enabled);
    check->setToolTip(tip);
    layout->addWidget(check);
    QObject::connect(check, &QCheckBox::toggled, box, std::move(changed));
    return box;
}
} // namespace

ExtensionManagerDialog::ExtensionManagerDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Extension Manager");
    setWindowIcon(icon("puzzle"));
    resize(820, 420);

    mTable = new QTableWidget(0, ColumnCount, this);
    mTable->setHorizontalHeaderLabels({"Extension", "Version", "Vendor", "SDK", "Loaded", "Auto Load", "Status"});
    mTable->verticalHeader()->hide();
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTable->setSelectionMode(QAbstractItemView::SingleSelection);
    mTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    mTable->horizontalHeader()->setSectionResizeMode(Status, QHeaderView::Stretch);
    connect(mTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (QTableWidgetItem* item = mTable->item(row, Name))
            showInfo(item->data(Qt::UserRole).toString().toStdString());
    });

    mPaths = new QLabel(this);
    mPaths->setWordWrap(true);
    mPaths->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mPaths->setForegroundRole(QPalette::PlaceholderText);

    auto* refreshButton = new QPushButton(icon("refresh"), "Refresh", this);
    auto* browseButton = new QPushButton(icon("folder-open"), "Browse...", this);
    browseButton->setToolTip("Load an extension package (a .lilaext folder) from anywhere.");
    auto* infoButton = new QPushButton(icon("info-circle"), "Info", this);
    infoButton->setToolTip("What the selected extension registered, and where it lives.");
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(refreshButton, QDialogButtonBox::ActionRole);
    buttons->addButton(browseButton, QDialogButtonBox::ActionRole);
    buttons->addButton(infoButton, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(refreshButton, &QPushButton::clicked, this, &ExtensionManagerDialog::refresh);
    connect(browseButton, &QPushButton::clicked, this, &ExtensionManagerDialog::browse);
    connect(infoButton, &QPushButton::clicked, this, [this] {
        int row = mTable->currentRow();
        if (QTableWidgetItem* item = row >= 0 ? mTable->item(row, Name) : nullptr)
            showInfo(item->data(Qt::UserRole).toString().toStdString());
    });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(mTable, 1);
    layout->addWidget(mPaths);
    layout->addWidget(buttons);

    // Loads and unloads from scripts or other windows show up here too; queued so a load in progress finishes first.
    auto later = [self = QPointer<ExtensionManagerDialog>(this)](const events::EventData&) {
        QTimer::singleShot(0, [self] {
            if (self)
                self->refresh();
        });
    };
    mLoadedSub = events::subscribe(events::Event::ExtensionLoaded, later);
    mUnloadedSub = events::subscribe(events::Event::ExtensionUnloaded, later);
    refresh();
}

ExtensionManagerDialog::~ExtensionManagerDialog()
{
    events::unsubscribe(mLoadedSub);
    events::unsubscribe(mUnloadedSub);
}

void ExtensionManagerDialog::open(QWidget* parent)
{
    static QPointer<ExtensionManagerDialog> dialog;
    if (!dialog)
    {
        dialog = new ExtensionManagerDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->refresh();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    flashLocate(dialog);
}

void ExtensionManagerDialog::refresh()
{
    mFilling = true;
    ExtensionManager& pm = ExtensionManager::get();
    const std::vector<ExtensionInfo> all = pm.available();
    mTable->setRowCount(int(all.size()));
    for (int row = 0; row < int(all.size()); ++row)
    {
        const ExtensionInfo& info = all[size_t(row)];
        const bool broken = info.manifest == std::nullopt && !info.error.empty();
        auto* name = new QTableWidgetItem(qs(info.manifest ? info.manifest->name : info.name));
        name->setData(Qt::UserRole, qs(info.name));
        QString tip = qs(info.name) + "\n" + QString::fromStdWString(info.path.wstring());
        if (info.manifest && !info.manifest->description.empty())
            tip += "\n\n" + qs(info.manifest->description);
        if (info.manifest && !info.manifest->dependencies.empty())
        {
            QStringList deps;
            for (const ext::Dependency& d : info.manifest->dependencies)
                deps << qs(d.id + (d.minVersion.empty() ? "" : " " + d.minVersion));
            tip += "\n\nNeeds " + deps.join(", ");
        }
        name->setToolTip(tip);
        name->setIcon(icon(info.kind == extensions::ExtensionKind::Native      ? "puzzle"
                           : info.kind == extensions::ExtensionKind::Resources ? "package"
                                                                              : "brand-python"));
        mTable->setItem(row, Name, name);
        mTable->setItem(row, Version, new QTableWidgetItem(info.manifest ? qs(info.manifest->version) : QString()));
        mTable->setItem(row, Vendor, new QTableWidgetItem(info.manifest ? qs(info.manifest->vendor) : QString()));
        mTable->setItem(row, Sdk, new QTableWidgetItem(info.manifest ? qs(info.manifest->sdk.str()) : QString()));

        const std::string id = info.name;
        mTable->setCellWidget(row, Loaded, centredCheck(mTable, info.loaded, !broken, "Load or unload it now.", [this, id](bool on) {
            if (!mFilling)
                setLoaded(id, on);
        }));
        mTable->setCellWidget(row, AutoLoad, centredCheck(mTable, info.autoload, !broken, "Load it every time Lila starts.", [this, id](bool on) {
            if (!mFilling)
                ExtensionManager::get().setAutoload(id, on);
        }));

        auto* status = new QTableWidgetItem;
        if (!info.error.empty())
        {
            const std::string firstLine = info.error.substr(0, info.error.find('\n'));
            status->setText(qs(firstLine));
            status->setToolTip(qs(info.error));
            status->setIcon(icon("alert-triangle"));
        }
        else if (const features::Feature* f = features::ofExtension(info.name); f && featureStatus::marked(f->status))
        {
            const bool notReady = f->status == features::Status::NotReady;
            status->setText(features::label(f->status));
            status->setToolTip(notReady ? "Not ready for use yet, so it stays off until you turn it on. Expect broken tools and lost work."
                                        : "Usable, with bugs to expect. Report them with Help > Report a Bug.");
            if (notReady)
                status->setIcon(icon("alert-triangle"));
        }
        else
            status->setText(info.loaded ? "Loaded" : "");
        mTable->setItem(row, Status, status);
    }

    QStringList dirs;
    for (const std::filesystem::path& p : pm.searchPaths())
        dirs << QString::fromStdWString(p.wstring());
    mPaths->setText("Searched: " + dirs.join("; "));
    mFilling = false;
}

void ExtensionManagerDialog::setLoaded(const std::string& name, bool on)
{
    try
    {
        if (on)
            ExtensionManager::get().load(name);
        else
            ExtensionManager::get().unload(name);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        QMessageBox::warning(this, on ? "Load Extension" : "Unload Extension", qs(e.what()));
    }
    refresh();
}

void ExtensionManagerDialog::browse()
{
    QString dir = ui::fileDialog::getExistingDirectory(this, "Load Extension Package");
    if (dir.isEmpty())
        return;
    try
    {
        ExtensionManager::get().load(dir.toStdString());
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        QMessageBox::warning(this, "Load Extension", qs(e.what()));
    }
    refresh();
}

void ExtensionManagerDialog::showInfo(const std::string& name)
{
    try
    {
        QMessageBox box(this);
        box.setWindowTitle("Extension Info");
        box.setText(qs(name));
        box.setDetailedText(qs(ExtensionManager::get().describe(name).dump(2)));
        box.setInformativeText("Show Details lists everything it registered.");
        box.exec();
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "Extension Info", qs(e.what()));
    }
}
} // namespace lila::ui
