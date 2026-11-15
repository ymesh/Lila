#include "ui/PainterImportUi.h"

#include "core/Log.h"
#include "tex/PainterImport.h"
#include "ui/FileDialog.h"
#include "ui/UiCommands.h"
#include "ui/ViewportToast.h"

#include <QApplication>
#include <QStandardPaths>

#include <filesystem>

namespace lila::ui
{
namespace
{
/** Painter's own smart material folder when it exists, so the picker opens where the files are. */
QString painterSmartMaterialDir()
{
    const std::filesystem::path docs(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdWString());
    const std::filesystem::path dir = docs / "Adobe" / "Adobe Substance 3D Painter" / "assets" / "smart-materials";
    std::error_code ec;
    return std::filesystem::is_directory(dir, ec) ? QString::fromStdWString(dir.wstring()) : QString();
}

std::string joined(const json& list)
{
    std::string out;
    for (const json& s : list)
        out += (out.empty() ? "" : ", ") + s.get<std::string>();
    return out;
}
} // namespace

bool isPainterSmartMaterial(const QString& file)
{
    return tex::painter::isPainterFile(file.toStdWString());
}

PainterImported importPainterSmartMaterials(const QStringList& files)
{
    PainterImported made;
    if (files.isEmpty())
        return made;
    json args = json::array();
    for (const QString& f : files)
        args.push_back(f.toStdString());
    const json r = runUi("texImportPainterSmartMaterials", json{{"args", args}});
    if (!r.is_array())
        return made;
    int failed = 0, lossy = 0;
    for (const json& f : r)
    {
        const std::string file = std::filesystem::path(f.value("file", "")).filename().string();
        if (f.contains("error"))
        {
            ++failed;
            log::error("{}: {}", file, f["error"].get<std::string>());
            continue;
        }
        for (const json& m : f.value("materials", json::array()))
            ++(m.value("kind", "material") == "mask" ? made.masks : made.materials);
        made.brushes += int(f.value("brushes", json::array()).size());
        if (const json dropped = f.value("dropped", json::array()); !dropped.empty())
        {
            ++lossy;
            log::warn("{}: left out {}", file, joined(dropped));
        }
        if (const json approx = f.value("approximated", json::array()); !approx.empty())
            log::info("{}: approximated {}", file, joined(approx));
    }
    QStringList parts;
    auto count = [&](int n, const char* one, const char* many) {
        if (n)
            parts << QString("%1 %2").arg(n).arg(n == 1 ? one : many);
    };
    count(made.materials, "smart material", "smart materials");
    count(made.masks, "smart mask", "smart masks");
    count(made.brushes, "brush preset", "brush presets");
    QString text = "Imported " + (parts.isEmpty() ? QString("nothing") : parts.join(", "));
    if (lossy)
        text += QString(", %1 with parts left out (see log)").arg(lossy);
    if (failed)
        text += QString(", %1 failed").arg(failed);
    viewportToast::showActive(text, 4000, failed ? viewportToast::Kind::Error : lossy ? viewportToast::Kind::Warning : viewportToast::Kind::Info);
    return made;
}

void importPainterSmartMaterialsDialog()
{
    importPainterSmartMaterials(ui::fileDialog::getOpenFileNames(QApplication::activeWindow(), "Import from Painter", painterSmartMaterialDir(),
                                                                 "Painter files (*.spsm *.spmsk *.sppr)"));
}
} // namespace lila::ui
