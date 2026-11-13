#pragma once

#include "ops/Command.h"

#include <cmath>
#include <string>
#include <vector>

/** Helpers the Puppeteer command tests share: scene at 30 fps, keys by frame, stacker rows by frame. */
namespace animtest
{
inline nlohmann::json run(const char* name, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(name, call); }
inline nlohmann::json v3(double x, double y, double z) { return nlohmann::json::array({x, y, z}); }
inline double sec(double frame) { return frame / 30.0; }
inline void key(const std::string& plug, double frame, double v) { run("setKeyframe", {{"args", {plug}}, {"time", sec(frame)}, {"value", v}}); }

inline std::vector<double> frames(const nlohmann::json& stack)
{
    std::vector<double> out;
    for (const nlohmann::json& r : stack["rows"])
        out.push_back(std::round(r["frame"].get<double>() * 1000.0) / 1000.0);
    return out;
}
} // namespace animtest
