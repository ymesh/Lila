#pragma once

#include "core/Export.h"

#include <QPixmap>
#include <QWidget>

#include <string>

class QLabel;
class QTimer;
class QToolButton;

namespace lila::ui
{
class NodeEditor;
namespace controls
{
class Segmented;
}

/**
 * The material preview pinned above a Texture Graph's side panel: the texture set the editor shows, whichever of its node
 * groups is open, rendered on the preview ball or on a mesh using it, and rendered again whenever the set changes. Pinned,
 * it keeps its set while the editor opens other sets.
 */
class LILA_UI_API TexGraphPreview final : public QWidget
{
public:
    enum class Shape : uint8_t
    {
        Ball,
        Mesh,
    };

    TexGraphPreview(NodeEditor& editor, QWidget* parent);
    ~TexGraphPreview() override;

    /** The set shown; empty when there is none. */
    const std::string& set() const { return mSet; }
    void setPinned(bool on);
    bool pinned() const;
    void setShape(Shape shape);
    Shape shape() const { return mShape; }
    /** The mesh the Mesh shape renders on: the first mesh with a material using the set; empty when none does. */
    std::string mesh() const;
    /** The last render; null before the first. */
    QPixmap image() const { return mImage; }
    /** What the preview is waiting on or why it has no image; empty while it shows a current render. */
    QString status() const;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void follow();
    void poll();
    void render(const std::string& key);
    void showImage();
    void setStatus(const QString& text);

    NodeEditor& mEditor;
    int mListener = 0;
    std::string mSet;
    Shape mShape = Shape::Ball;
    QLabel* mView = nullptr;
    QLabel* mStatus = nullptr;
    controls::Segmented* mShapes = nullptr;
    QToolButton* mPin = nullptr;
    QTimer* mPoll = nullptr;
    QPixmap mImage;
    /** The set, shape and stamp of the render shown (or failed), and of the one running. */
    std::string mShownKey;
    std::string mBusyKey;
};
} // namespace lila::ui
