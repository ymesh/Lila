#pragma once

#include "core/Export.h"

#include <QString>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class QWidget;

/**
 * Lila's one texture picker: a popup under a field with a search box that filters as you type, thumbnails, None,
 * Browse..., the caller's library (HDRIs, brush alphas...) by group and the project's textures. Arrows move, Enter
 * picks, Esc closes. ImagePicker is the field that opens it.
 */
namespace lila::ui::texturePicker
{
/** A library row. */
struct Item
{
    /** What picking it returns. */
    std::string value;
    QString name;
    std::filesystem::path file;
    /** The heading it is listed under. */
    QString group;
};

struct Options
{
    /** The stored value; its row is highlighted. A path is compared as a file, anything else against Item::value. */
    std::string current;
    /** "HDRI", "Alpha": the search placeholder and the Browse dialog's caption. */
    QString title = "Texture";
    /** Read each time the popup opens. */
    std::function<std::vector<Item>()> items;
    /** List the project's textures (projectTextures, filtered by textureFolders) after the library. */
    bool projectTextures = true;
    /** Lowercase extensions (".hdr") the project's textures are limited to; empty lists every image. */
    std::vector<std::string> extensions;
    /** The Browse dialog's filter; empty is every image format. */
    QString browseFilter;
    /** Footer menu entries: add to the library, open its folders... */
    std::vector<std::pair<QString, std::function<void()>>> actions;
    /** The favourites list rows star into; listed first under Favourites. Empty: no stars. */
    std::string favourites;
    /** The None and Browse... rows; off for a pick that must be one of the items. */
    bool none = true;
    bool browse = true;
};

/**
 * Opens the picker under anchor. pick gets the chosen Item::value, an absolute path for a project texture or a
 * browsed file, or "" for None; it is not called when the popup closes without a choice.
 */
LILA_UI_API void open(QWidget* anchor, Options options, std::function<void(const std::string&)> pick);

/** The picker with only the project's textures, for material slots: current is the slot's stored path. */
LILA_UI_API void open(QWidget* anchor, const std::string& current, std::function<void(const std::string&)> pick);

/** Edits the workspace's include and exclude globs and the open scene's overrides, through textureFolders. */
LILA_UI_API void showFoldersDialog(QWidget* parent);
} // namespace lila::ui::texturePicker
