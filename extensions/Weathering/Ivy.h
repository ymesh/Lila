#pragma once

namespace lila::ext
{
class Context;
}

namespace weathering
{
/** The live ivy node: hosts' meshes in, the ivy mesh out. */
inline constexpr const char* kIvyNodeType = "weatheringIvy";
/** The Ivy geometry node (Masonry's ivy runs it too). */
inline constexpr const char* kIvyGeometryNode = "IvyGrowth";

/** The Ivy brush, its palette, menu items and option boxes (IvyUi.cpp). */
void registerIvyUi(lila::ext::Context& ctx);
} // namespace weathering
