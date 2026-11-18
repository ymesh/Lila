#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lila::ui::uvToolkit
{
/** One button: a registered action (unregistered ones are skipped), a Tabler icon and short text, the action's label when empty. */
struct Item
{
    std::string action;
    std::string icon;
    std::string text;
};

/** A section an extension adds to the UV Toolkit, under Lila's own sections. */
struct Section
{
    std::string id;
    std::string title;
    /** Icon-and-label rows, two to a row, like the toolkit's own. */
    std::vector<Item> items;
    /** Actions in the section's overflow menu. */
    std::vector<std::string> rare;
    /** Lower comes first; equal orders keep registration order. */
    int order = 100;
};

LILA_UI_API Registry<Section>& registry();
/** Adds or replaces. */
LILA_UI_API void add(Section s, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t add(ext::Context& ctx, Section s);
} // namespace lila::ui::uvToolkit
