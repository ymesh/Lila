#pragma once

#include <QStringList>

namespace lila::ui
{
/** What an import made. */
struct PainterImported
{
    int materials = 0;
    int masks = 0;
    int brushes = 0;
    int total() const { return materials + masks + brushes; }
};

/** Converts Painter smart materials, smart masks and brush presets, reporting in a toast and the log. */
PainterImported importPainterSmartMaterials(const QStringList& files);

/** Picks Painter files (opening in Painter's smart material folder when it exists) and imports them. */
void importPainterSmartMaterialsDialog();

bool isPainterSmartMaterial(const QString& file);
} // namespace lila::ui
