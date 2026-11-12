#pragma once

#include "core/PrefStore.h"
#include "ui/Controls.h"
#include "ui/FixedRowLayout.h"

#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

namespace lila::ui
{
/** Buttons per row in the toolkit panels' button grids. */
inline constexpr int kToolkitColumns = 2;

/** A widget holding a FixedRowLayout of buttons, returned in `layout`. */
inline QWidget* rowBox(QWidget* parent, FixedRowLayout** layout, int columns = 0)
{
    auto* w = new QWidget(parent);
    *layout = new FixedRowLayout(columns, 3);
    (*layout)->setContentsMargins(2, 0, 2, 2);
    w->setLayout(*layout);
    return w;
}

/**
 * `content` under a header that collapses it (controls::section). Whether it is collapsed is remembered in the general
 * prefs under `collapsedPrefs` + "/" + title. `headerOut` gets the header, for callers that open or close it themselves.
 */
inline QWidget* collapsibleSection(const QString& title, QWidget* content, QWidget* parent, const char* collapsedPrefs,
                                   bool openByDefault = true, QToolButton** headerOut = nullptr)
{
    return controls::section(title, content, parent, collapsedPrefs, openByDefault, headerOut);
}
} // namespace lila::ui
