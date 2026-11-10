#pragma once

#include "core/Export.h"
#include "groom/Style.h"

#include <optional>
#include <vector>

#include <string>

namespace lila::groom
{
/** The groom graph node whose guides become the locks mesh. */
inline constexpr const char* kLocksType = "Locks";

/** The groom graph node type of a lock style ("LockCurl"). */
std::string lockStyleType(Style kind);

/** A lock style's param, for editors. */
struct LockStyleParam
{
    std::string name, doc;
    /** 'f' a number, 'i' an integer, 's' text (a ramp's keys). */
    char kind = 'f';
    double defaultValue = 0.0;
    std::optional<double> min, max;
};
LILA_SCENE_API std::vector<LockStyleParam> lockStyleParams(Style kind);

/** Registers Locks and the Lock<Style> operators with GroomLab: Lila's lock styles on a graph's guides. */
void registerLockOps();
} // namespace lila::groom
