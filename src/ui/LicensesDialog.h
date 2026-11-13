#pragma once

#include "core/Export.h"

#include <QString>

#include <string>
#include <vector>

class QWidget;

namespace lila::ui
{
namespace licenses
{
/** One component row of THIRD-PARTY-NOTICES.md, with its section's notes (relinking, source offers). */
struct Component
{
    QString name;
    QString license;
    QString copyright;
    QString source;
    QString section;
    QString notes;
};

/** The tables of a THIRD-PARTY-NOTICES.md. */
LILA_UI_API std::vector<Component> parse(const QString& markdown);
/** The notices file beside the executable, else the source tree's. */
LILA_UI_API QString noticesPath();
/** The full license text shipped for a component (licenses/<id>.txt beside the executable); empty when none is. */
LILA_UI_API QString licenseText(const Component& c);
/** The licenses/<id>.txt name a component's text ships under: its vcpkg port, or the vendored file's name. */
LILA_UI_API QString fileId(const QString& name);
} // namespace licenses

/** Help > Licenses: every third-party component with its license, searchable. One window, raised if already open. */
LILA_UI_API void showLicenses(QWidget* parent);

/** Lila's own LICENSE, read from beside the executable or the source tree, else opened on GitHub at the build's commit. */
LILA_UI_API void showLilaLicense(QWidget* parent);
} // namespace lila::ui
