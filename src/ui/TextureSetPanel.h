#pragma once

#include "core/Export.h"

#include <QStringList>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>

class QComboBox;

class QListWidget;
class QTreeWidget;

namespace lila::ui
{
/** Dock object name. */
inline constexpr const char* kTextureSetsDock = "textureSets";

/**
 * Registers the Texture Set panel (window.textureSets), its textureSets.* actions and their Texture menu entries. Call
 * after registerTextureGraphUi.
 */
LILA_UI_API void registerTextureSetPanel();

/** Opens the name box on the set picker of the visible Texture Set panel; false when none is visible. */
LILA_UI_API bool startSetRename();

namespace textureSets
{
/**
 * The paint layer painting tools write into on a set: the paint layer last selected in the Texture Set panel (it stays
 * active while a layer of another kind is selected). Empty when none was picked or it no longer exists.
 */
LILA_UI_API std::string activeLayer(const std::string& set);
/** Makes a paint layer the set's active one (the panel selects it); a tool that adds a paint layer calls this. */
LILA_UI_API void setActiveLayer(const std::string& set, const std::string& layer);
/** Hears active layer changes (main thread). Returns an id. */
LILA_UI_API int addActiveLayerListener(std::function<void(const std::string& set, const std::string& layer)> fn);
LILA_UI_API void removeActiveLayerListener(int id);
/** Shows the Brush page (opening the panel when none is up), or the Layer page. Texture Paint calls this as it starts and ends. */
LILA_UI_API void showBrushPage(bool on);
/** Switches the library shelf to a kind ("materials", "masks", "brushes", "alphas", "textures"), opening the panel when none is up. */
LILA_UI_API void showLibrary(const std::string& kind);
/** Rereads the library shelf's folders (after a preset or texture is saved). */
LILA_UI_API void refreshLibrary();
} // namespace textureSets

/**
 * The layer stack of one texture set, Painter style, as a view over texLayerStack. Every edit is a tex* command, so it
 * undoes and echoes. Top layer first; a folder's children and a layer's masks sit under it.
 */
class LILA_UI_API TextureSetPanel : public QWidget
{
public:
    /** Where a dragged layer lands relative to the row it is dropped on. */
    enum class Drop
    {
        Above,
        Below,
        Into,
    };

    explicit TextureSetPanel(QWidget* parent = nullptr);
    ~TextureSetPanel() override;

    void setTextureSet(const std::string& set);
    std::string textureSet() const;
    /** Opens a name box over the set picker; Enter renames the set shown. */
    void startRename();
    /** Rereads the set now instead of on the next event loop turn. */
    void refresh();

    /** The selected layer's id, or the layer of a selected mask; empty for none. */
    std::string selectedLayer() const;
    std::string selectedMask() const;
    void selectLayer(const std::string& id);
    void selectMask(const std::string& layer, const std::string& mask);

    /** Adds a layer of a kind (fill, paint, procedural, folder, instance, filter) above the selection, the way the menus do. */
    std::string addLayer(const std::string& kind, const std::string& filter = {});
    /** Adds a mask to the selected layer: fill, paint, levels, generator or filter (with the node type), anchor (with its name). */
    std::string addMask(const std::string& kind, const std::string& type = {});
    /** Moves a layer as a drop onto target's row does (a mask onto a mask of the same layer reorders the masks). */
    void moveLayer(const std::string& id, const std::string& target, Drop where);
    /** Adds a library material as a fill or procedural layer placed as a drop onto target's row; empty target puts it on top. */
    void dropMaterial(const std::string& material, const std::string& target, Drop where);
    void duplicateSelected();
    /** The selected layer to the clipboard as text (texCopyLayers), and back above the selection, in this set or another. */
    void copySelected();
    void paste();
    /** Bake the selected layer into one paint layer: alone (flatten) or with the layer below (merge down). */
    void flattenSelected(bool down);
    /** Write what flattening the selected layer would make as images. */
    void exportFlattened();
    void deleteSelected();
    /** Shows the Texture Graph editor on the set with the selected layer's node selected. */
    void openSelectedAsNodes();
    /** Applies a smart material on top of the selection, or a smart mask to the selected layer. */
    void applySmartMaterial(const std::string& name);
    /** The lower pane's Brush page (the Texture Paint settings), or its Layer page. */
    void showBrushPage(bool on);
    /** Switches the library shelf to a kind: "materials", "masks", "brushes", "alphas" or "textures". */
    void showLibrary(const std::string& kind);
    void refreshLibrary();
    /** Copies images into the user's textures folder and shows them; one image becomes the brush's texture. */
    void addTextures(const QStringList& files);

    QTreeWidget* tree() const;
    QComboBox* setPicker() const;
    QListWidget* smartShelf() const;
    /** The properties area's content (the selected layer or mask), rebuilt on selection. */
    QWidget* properties() const;
    /** The graph-mode banner, visible while the set's graph holds edits the stack can't show. */
    QWidget* banner() const;

    /** The panel the textureSets.* actions act on: the focused one, else the last focused, else any. */
    static TextureSetPanel* active();

    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace lila::ui
