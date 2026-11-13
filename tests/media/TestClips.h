#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mediatest
{
/** A scratch folder for this run's clips. */
std::filesystem::path scratch();
/**
 * A gray video (MPEG-4 Part 2, B-frames, a keyframe every `gop`) whose frame i has luma 20 + 7 i, so a decoded frame
 * tells its own index. The container follows the extension (.mp4, .mkv, .avi).
 */
std::filesystem::path writeClip(const std::string& name, int frames, int fps, int gop);
/** The red value frame i decodes to (limited-range luma to full-range RGB). */
double expectedRed(int frame);
/** A 16-bit mono WAV at rate: silence with a full-scale click lasting 10 samples at clickSeconds. */
std::filesystem::path writeWav(const std::string& name, int rate, double seconds, double clickSeconds);
/**
 * A container (by extension) with videoFrames of gray video at 24 fps (none when 0) and one 16-bit mono sound track per
 * click, each silent but for a 10-sample click at its time.
 */
std::filesystem::path writeTracks(const std::string& name, int rate, double seconds, const std::vector<double>& clicks, int videoFrames);
} // namespace mediatest
