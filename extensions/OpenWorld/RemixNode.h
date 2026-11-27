#pragma once

#include "Remix.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace openworld
{
/** What a World Remix node evaluated last, by its modifier's scene node id: the overlay and the commands read it. */
struct Published
{
    RemixSpec spec;
    std::shared_ptr<const RemixResult> result;
    bool before = false;
};

bool publishedRemix(uint64_t modifier, Published* out);
std::vector<std::pair<uint64_t, Published>> publishedRemixes();
/** Forgets the remixes of modifiers no longer in the scene. */
void forgetGoneRemixes();
void forgetRemix(uint64_t modifier);

bool registerRemixNode(lila::ext::Context& ctx);
} // namespace openworld
