#pragma once

#include "core/Export.h"

#include "media/Media.h"

#include <QColor>
#include <QRectF>

#include <vector>

class QMenu;
class QPainter;
class QWidget;

/**
 * Sound in the animation UI: playback through media::AudioEngine with the audio as the clock, scrub sound, sound cues
 * fired during playback, the waveform under the time slider and the Graph Editor, and the time slider's Audio menu.
 */
namespace lila::ui::audio
{
/** Registers create.audio and the sound listeners. Call once the QApplication exists. */
LILA_UI_API void install();

/** Draws the heard sounds' waveform across rect, rect's left edge at scene time t0 and its right at t1 seconds. */
LILA_UI_API void drawWaveform(QPainter& p, const QRectF& rect, double t0, double t1, const QColor& color);
/** Draws the given voices' waveform the same way. */
LILA_UI_API void drawVoices(QPainter& p, const QRectF& rect, double t0, double t1, const QColor& color,
                            const std::vector<media::Voice>& voices);
/** Whether anything would draw: some audible sound has loaded. */
LILA_UI_API bool hasWaveform();

/** The time slider's right-click Audio menu: All, each audio node, Import Audio... */
LILA_UI_API void fillMenu(QMenu* menu);
/** Asks for a sound file and makes an audio node from it, at the playback range's start. */
LILA_UI_API void importAudio(QWidget* parent);

/** Playback: starts and stops the sound with the playhead. dir is 1 or -1; speed the real-time multiplier. */
LILA_UI_API void playbackStarted(double sceneTime, int dir, double speed);
LILA_UI_API void playbackStopped();
/** The scene time being heard while sound drives playback; < 0 when it doesn't. */
LILA_UI_API double audioClock();
/** The playhead jumped (a loop wrapped) while playing: the sound follows. */
LILA_UI_API void playbackJumped(double sceneTime);
/** The playhead moved from t0 to t1 while playing forward: fires the sound cues of the events between. */
LILA_UI_API void playbackAdvanced(double t0, double t1);
/** A scrub (time slider drag, frame step) landed on sceneTime: plays the frame's sound. */
LILA_UI_API void scrubbed(double sceneTime);
} // namespace lila::ui::audio
