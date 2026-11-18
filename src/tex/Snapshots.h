#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Node;
}

/**
 * Texture set snapshots: the evaluated channels of a set at one moment, for comparing against later work. Pixels are kept
 * as pages of kPage x kPage texels shared by content hash, so a snapshot of a set that mostly didn't change costs only
 * the pages that did. Snapshots last the session, by set and name.
 */
namespace lila::tex::snapshots
{
inline constexpr int kPage = 64;

/** kPage x kPage RGBA floats (edge pages padded with zeros). */
struct Page
{
    std::vector<float> rgba;
    uint64_t hash = 0;
};

struct ChannelShot
{
    std::string channel;
    int udim = 1001;
    int width = 0;
    int height = 0;
    /** Row by row, (width + kPage - 1) / kPage per row. */
    std::vector<std::shared_ptr<const Page>> pages;
};

struct Snapshot
{
    std::string set;
    std::string name;
    uint64_t id = 0; // unique across the session
    std::vector<ChannelShot> channels;
    /** Pages this snapshot added to the store; the rest it shares with earlier snapshots. */
    size_t newPages = 0;
};

/**
 * Evaluates every channel of the set (all its UDIM tiles) at resolution (0: the tiles' own sizes) and keeps it under
 * name, replacing a snapshot of that name. An empty name picks "Snapshot N". Throws std::invalid_argument.
 */
LILA_SCENE_API std::shared_ptr<const Snapshot> take(scene::Node& set, std::string name, int resolution = 1024, const std::string& backend = {});
LILA_SCENE_API std::shared_ptr<const Snapshot> find(std::string_view set, std::string_view name);
/** Oldest first. */
LILA_SCENE_API std::vector<std::shared_ptr<const Snapshot>> list(std::string_view set);
LILA_SCENE_API bool remove(std::string_view set, std::string_view name);
/** Drops every snapshot (a new scene). */
LILA_SCENE_API void clear();

/** A channel's pixels put back together from the pages, exactly as evaluated; empty when the snapshot lacks it. */
LILA_SCENE_API image::Image image(const Snapshot& snap, std::string_view channel, int udim = 1001);

/** Distinct pages alive across every snapshot, and their bytes. */
LILA_SCENE_API size_t storedPages();
LILA_SCENE_API size_t storedBytes();

/** texSnapshot, texSnapshots, texDeleteSnapshot. Called by tex::registerCommands. */
LILA_SCENE_API void registerCommands();
} // namespace lila::tex::snapshots
