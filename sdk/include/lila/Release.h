#pragma once

/**
 * Freeing big data off the main thread without slowing frames: release::later(std::move(x)) and
 * release::laterEach(std::move(vector)) free on one background thread that holds off while a viewport builds a frame.
 */
#include "core/Release.h"
