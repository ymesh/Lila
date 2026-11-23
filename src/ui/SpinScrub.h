#pragma once

#include "core/Export.h"

#include <QRect>
#include <QString>

#include <nlohmann/json_fwd.hpp>

#include <functional>

class QAbstractSpinBox;
class QCheckBox;
class QComboBox;
class QMenu;
class QObject;
class QWidget;

namespace lila::ui
{
/**
 * Gives every spin box the shared number-field drag (ScrubGesture): left or middle drag changes it, its form label
 * drags it too, a left click starts typing, a middle click restores kDefaultProperty when it has one. Enter, Esc or a
 * click elsewhere ends typing, for spin boxes and for fields carrying kScrubFieldProperty. Installed once on the application.
 */
LILA_UI_API void installSpinScrub(QObject* parent);

/**
 * Marks w and everything inside it as fields whose edits re-run an operation that already makes one undo step of a
 * drag (Adjust Last Op). Their scrubs then open no undo chunk of their own, which would block the re-run.
 */
LILA_UI_API void setScrubsWithoutUndoStep(QWidget* w);
LILA_UI_API bool scrubsWithoutUndoStep(const QWidget* w);

/** Dynamic property holding a spin box's default value (a double), restored by a middle click on the box. */
inline constexpr const char* kDefaultProperty = "lilaDefault";

/** Dynamic property marking a line edit as a number field: a click elsewhere ends typing in it, as for spin boxes. */
inline constexpr const char* kScrubFieldProperty = "lilaScrubField";

/** Gives a spin box a default to restore on middle click (on it, or on its buddy label); its tooltip then says so. */
LILA_UI_API void setSpinDefault(QAbstractSpinBox* box, double value);

/** The same for a check box, or a combo box (by item index). The form label naming the field as buddy resets it too. */
LILA_UI_API void setCheckDefault(QCheckBox* box, bool on);
LILA_UI_API void setComboDefault(QComboBox* box, int index);

/** "Middle-click to reset to default (X)", the line added to the tooltip of every field that resets. */
LILA_UI_API QString resetHint(const QString& defaultText);

/** Rich tooltip text: own as written, then hints (plain, one per line) dim and italic under it. */
LILA_UI_API QString hintedTip(const QString& own, const QString& hints);

/** A how-to line (like how a label scrubs) shown dim and italic under w's tooltip, above the reset hint. */
LILA_UI_API void setUsageHint(QWidget* w, const QString& hint);

/** Briefly lights up a field (or a rect of it, in its own coordinates) to acknowledge a reset. */
LILA_UI_API void flashReset(QWidget* w, QRect rect = {});

/**
 * Makes a middle click (not a drag) on target call reset and flash `flash` (target when null). Its tooltip gains
 * resetHint(defaultText). Middle drags and every other event pass through untouched.
 */
LILA_UI_API void onMiddleClickReset(QWidget* target, std::function<void()> reset, const QString& defaultText, QWidget* flash = nullptr);

/** Adds Copy Name, Copy Value and Copy Name and Value ("name = value") to menu. value is read when one is picked. */
LILA_UI_API void addCopyActions(QMenu* menu, const QString& name, std::function<nlohmann::json()> value);

/** A right-click menu on target holding only addCopyActions. */
LILA_UI_API void onRightClickCopy(QWidget* target, const QString& name, std::function<nlohmann::json()> value);
} // namespace lila::ui
