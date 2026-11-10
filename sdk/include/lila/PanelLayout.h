#pragma once

/**
 * Panel Layout: lines drawn on a mesh's surface (the Layout Pencil) and the panels they bound. Read a layout through
 * Resolved (lineAt, panelAt, panels, runs), edit one with the functions in layout/Edit.h, cut its lines into a mesh with
 * cutRun. panelLayoutModifier nodes give their resolved layout on outLayout.
 */
#include "layout/Edit.h"
#include "layout/Layout.h"
#include "layout/LayoutNode.h"
#include "layout/Lines.h"
#include "layout/Maps.h"
#include "layout/Resolve.h"
