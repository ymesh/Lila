#pragma once

#include "core/Export.h"

#include <QImage>
#include <QWidget>

#include <memory>
#include <string>

namespace lila::scene
{
class Graph;
}

namespace lila::ui
{
/** Dock object names. */
inline constexpr const char* kTextureGraphDock = "textureGraph";
inline constexpr const char* kTexture2dDock = "texture2d";

/**
 * Registers the texture-graph backend, the Texture Graph and 2D View panels ("window.textureGraph",
 * "window.texture2d"), the texture.* actions and the Texture menu.
 */
LILA_UI_API void registerTextureGraphUi();

/** The texture set a material slot binds to: the one whose material setting names the slot, else the one named like it. */
LILA_UI_API std::string textureSetForSlot(scene::Graph& g, const std::string& slot);

/** Shows the Texture Graph panel with the set open in its editors. */
LILA_UI_API void openTextureSet(const std::string& set);

/** The set the texture actions work on: the Texture Graph editor's, else the 2D View's, else the first. */
LILA_UI_API std::string currentTextureSet();

namespace texture2d
{
/** What the 2D View shows: a node's output (node, socket; empty socket for its first) or a channel of the set. */
struct Source
{
    std::string set;
    std::string node;
    std::string socket;
    std::string channel;

    bool operator==(const Source&) const = default;
};

/** Shows the output in the 2D View and, while Show on Mesh is on, as base colour on the meshes bound to its set. */
LILA_UI_API void view(const Source& source);
LILA_UI_API Source current();
/** Points the 2D View at a set, keeping the channel it shows (base colour for a node output). Leaves the mesh preview as it is. */
LILA_UI_API void showSet(const std::string& set);
/** Stops showing the preview on meshes; the 2D View keeps it. */
LILA_UI_API void clearMeshPreview();
/** Whether some mesh draws the preview now. */
LILA_UI_API bool meshPreviewShown();
} // namespace texture2d

/**
 * The 2D View (window.texture2d): one texture-graph output or channel, zoomable and pannable, tiled 3x3 to check seams,
 * with channel solo, a histogram and a pixel inspector. Ctrl+Shift+click a node in the Texture Graph editor to show it.
 */
class LILA_UI_API Texture2DView : public QWidget
{
public:
    explicit Texture2DView(QWidget* parent = nullptr);
    ~Texture2DView() override;

    /** The image as displayed (solo applied), null before the first render. */
    QImage displayed() const;
    /** The status line: what is shown, its size, and whether a newer render is on its way. */
    QString status() const;
    /** Readout of texel (x, y) of the shown image: "x, y  r g b a" in the graph's floats. Empty outside it. */
    QString inspect(int x, int y) const;
    /** 0 shows RGB, 1 to 4 solo R, G, B or A as gray. */
    void setSolo(int channel);
    void setTiled(bool on);
    void setResolution(int size);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace lila::ui
