#pragma once

#include "core/Export.h"

#include <QWidget>

#include <memory>

namespace lila::ui
{
/**
 * The media lane above the time slider's ruler: one bar per footage file (a video's image plane and the audio nodes
 * playing its sound, or a sound file alone) of the media playing now, drawn with video thumbnails and the waveform.
 * Drag a bar to slide it in time, its ends to trim the sound, right-click for takes, track and lining up with the
 * playhead. Hidden while the scene has no live media. Shares the ruler's frame mapping, so the two line up.
 */
class LILA_UI_API MediaLane : public QWidget
{
public:
    explicit MediaLane(QWidget* parent = nullptr);
    ~MediaLane() override;

    /** The lane's bars changed (media added, removed, moved, retimed, a take switched): rebuild and show or hide. */
    void invalidate();

    struct Impl;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    std::unique_ptr<Impl> mImpl;
};

namespace mediaLane
{
/** The rows the lane shows now, as (file, node names) for tests and scripts. */
LILA_UI_API std::vector<std::pair<std::string, std::vector<std::string>>> rows();
/** Registers anim.importFootage. Call once the QApplication exists. */
LILA_UI_API void install();
/** Asks for a video or sound file and runs importFootage with the active view's camera. */
LILA_UI_API void importFootage(QWidget* parent);
} // namespace mediaLane
} // namespace lila::ui
