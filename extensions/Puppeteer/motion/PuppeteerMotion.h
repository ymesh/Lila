#pragma once

#include <lila/Extension.h>

/** Generated motion in Puppeteer: fill between keys, paths, spots and live driving, with the Motion panel. */
namespace puppeteer::motionfill
{
/** motionFill. */
void registerCommands(lila::ext::Context& ctx);
} // namespace puppeteer::motionfill

namespace puppeteer::motionui
{
/** The drive commands, the Drive tool, the Motion panel, its actions and menus. */
void registerUi(lila::ext::Context& ctx);
} // namespace puppeteer::motionui
