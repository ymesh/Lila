#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <QWidget>

#include <functional>
#include <vector>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace lila::scene
{
class Node;
}

namespace lila::ui
{
/** Dock object name of the material editor. */
inline constexpr const char* kMaterialEditorDock = "materialEditor";

/**
 * Lists the scene's materials and edits the selected one: sliders, color swatches, texture slots with thumbnails, a
 * fake-shaded preview ball and an assign-to-selection button. Every edit goes through commands, so it is undoable;
 * a slider drag is one undo step.
 */
class LILA_UI_API MaterialEditor : public QWidget
{
    Q_OBJECT

public:
    explicit MaterialEditor(QWidget* parent = nullptr);
    ~MaterialEditor() override;

    void showMaterial(const std::string& name);
    /** Shows the first material of the last selected mesh. */
    void followSelection();
    /** Side-panel width, like the channel box, rather than whatever the scroll area's contents ask for. */
    QSize sizeHint() const override { return {340, 640}; }

private:
    void scheduleRefresh();
    /** Shows the row's material, or the read-only Default for the Default row. */
    void pickRow(QListWidgetItem* item);
    void refreshList();
    /**
     * Hides the materials no filter term matches (a glob over the whole name, else a part of it, ignoring case), and
     * those the filter menu's options leave out: type, referenced, unused, or not on the selected objects.
     */
    void applyFilter();
    void rebuild();
    void updateValues();
    /** Keeps the body wide enough for its rows, so it never scrolls sideways. */
    void fitWidth();
    void setAttr(int attr, const scene::json& value);
    scene::Node* current() const;
    /** The shown material's path, or the default slot while the Default row is current; empty with neither. */
    std::string currentName() const;

    QLineEdit* mFilter = nullptr;
    QListWidget* mList = nullptr;
    QLabel* mPreview = nullptr;
    QScrollArea* mScroll = nullptr;
    QWidget* mBody = nullptr;
    QVBoxLayout* mBodyLayout = nullptr;
    QPushButton* mAssign = nullptr;
    QPushButton* mDelete = nullptr;
    QPushButton* mSelectUsers = nullptr;
    QToolButton* mFilterMenu = nullptr;
    scene::NodeId mNode = scene::kNoNode;
    std::vector<std::function<void()>> mUpdaters;
    int mListener = 0;
    int mTextureListener = 0;
    bool mQueued = false;
    bool mDragging = false;
    bool mDefaultShown = false; // the read-only Default row is current
    int mShownModel = -1; // shading model whose inputs the panel shows
};

/**
 * Registers "window.materialEditor" (shows the dock named kMaterialEditorDock, or a floating editor when the main
 * window has none), "material.assignNew" (creates a material and assigns it to the selection) and
 * "viewport.textured" (shaded with materials in the viewport; hotkey 6). Also registers the material node type and
 * commands when the scene has not yet.
 */
LILA_UI_API void registerMaterialUi();
} // namespace lila::ui
