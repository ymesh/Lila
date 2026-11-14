# Overnight decisions (autoremesh branch, 2026-11-13)

Scope from Jared: UV reuse items 1-4 from the retopo rebase, plus the remaining Quad Remesh gaps, unattended, on `autoremesh`, decisions mine.

## 1. Flow-oriented UV islands

- Field: Lila's own `retopo::solveCrossField` on the chart's triangles (curvature-aligned, borders and creases as features). It is Surface-based and already exported; the AutoRemesher field would need a full Parameterizer per island for the same direction.
- Turn: per triangle the field direction mapped into UV through the triangle's affine map, folded four ways, area-weighted, like `gridAngle`.
- Flat islands have no flow of their own (the field there is arbitrary and cost 0.3-0.6% pack density in the 200/2000-island benchmark), so an uncreased island needs mean |k1 - k2| times its size of at least 0.25 before its flow counts.
- Order for a fresh or packed island: edge grid (agreement 0.4, unchanged) beats flow; flow needs agreement 0.5 and may cost at most 15% more box area than the smallest box, else the smallest box. Long side along U either way. Quad meshes therefore keep the grid rule; flow acts on triangle meshes and sculpts.
- Measured: pack benchmark densities identical to before; uvAuto on triangulated Mochi 71.86% vs 71.75%, head and quad meshes identical. The gain is texel direction (limbs and panels along U/V), not density.
- `uvOrient mode=flow` and `mode=auto` (the upright rule) added; the Orient UVs action keeps bbox as its default.

## 2. Flow singularities as cones and darts

- `flowCones(chart)`: the singular triangles of the chart's flow (same solver as item 1, no flatness gate), each at its most curved corner off the border, ranked by the angle defect summed over two rings (per-vertex defect reads 0.005 rad on a fine mesh and was useless as a threshold).
- `uvBff coneSource=flow`: cones from the flow instead of the greedy stretch picker; `cones` caps the count, 0 takes every one bending at least 0.05 rad, kept three edges apart. On a pointed cap it lands on the tip and halves area stretch (1.21 -> 0.51), same as the greedy pick there; it needs no repeated scale solves.
- Auto seams darts (`AutoSeamOptions::darts`, flag `darts` on uvAutoSeams/uvAuto): an island over the distortion limit first tries cuts from its +1 flow points (caps) to its border, cheapest by seam cost, up to 4, clustered within 0.15 sqrt(area), total under 35% of the border; kept only when the darted island flattens under the limit, else it splits as before. An island darts once.
- Measured on Mochi and the head (quad and triangulated, 8 runs): seams 10-22% shorter and 2-6 fewer islands where darts apply, but coverage 4-8% lower (notched islands pack looser) and about 3x the time (2-7 s). So darts are off by default and offered as the flag; the default output is unchanged.
- Found while testing: BFF cones only cut along polygon faces, so a chart with no faces gets cones that do nothing. Real charts have faces; only the test helper needed them.

## 3. Flow Unwrap (straight UVs for hard surface and trims)

- New `uvFlowUnwrap` (Flow Unwrap) rather than feeding the AutoRemesher cover into UVs: its integer lattice and cut graph are built for whole closed surfaces, and per island a combed field plus one least-squares solve does the same job without lattice rounding. Per island: the cross field held to the border and creases (soft crease pull at 15 degrees for bevel segments), darts from each singular point to the border (from the singular triangle's most curved corner), the field combed across the cut island with transport over folds, then u and v solved so their gradients follow it at 3D scale.
- Straighten: border and crease edges within 12 degrees (option) of U or V are held to one line by a penalty (1000x) in a second solve.
- Flat islands (all faces within 1 degree of a plane) are projected onto their plane and turned so the border runs along U/V: zero stretch, and round caps no longer need four darts.
- Folds: the folded triangles plus three rings are re-solved by the existing pinned LSCM+SLIM unwrap with the rest held; a repair that moves the held part is dropped. An island still folded or overlapping itself is unwrapped for least stretch instead and counted in `fellBack`, with a note in the result.
- Two bugs found and fixed on the way: combing projected directions across a 90 degree crease (zeroed them; now rotated about the fold), and per-triangle directions projected the vertex field from averaged normals at crease junctions (wrong quarter turns, a 180 degree jump at a cube corner; now transported from each vertex's tangent plane).
- Measured: folded panel exact 2x1 rectangle; cube corner unfolds with one 90 degree dart, area 2.99 of 3; bevelled box 2 islands as clean nets, 0 overlap, mean angle distortion 0.012. Organic meshes (head, Mochi) fall back on about half their islands, reported; Flow Unwrap is for hard surface and trims, Auto UV stays the organic default.
- UI: Flow Unwrap in the UV menu and the Unwrap section's overflow (weekly tool, the section already has 11 entries); BFF Unwrap with Flow Cones in the Conformal split row; Auto Seams with Darts in the Seams split row; option labels "Straighten Within" (deg) and "Cones From" (Stretch, Flow).

## Quad Remesh gaps

- Cylinder fold (flow layout test): a row of cap points beside the rim crease was taken onto it, leaving a quad lying flat along the rim. `freeCreaseFlaps` frees the points of a face under 0.08 h² whose corners are all on features, except the two on its crease edge, and puts them back at their neighbours' mean on the surface. Test passes; angle deviation max 87 -> 49 degrees. Committed 3ff2e1e5.
- CAD corpus (cone, starPrism, combPrism, spiralFace, bentStar, twistedGrid, tent over hausdorff 0.035): not fixed. Found: tips sharper than 45 degrees collapse because both sides of the tip snap to the same lattice direction, and the isotropic pass leaves 10-22 short edges near tips bending sharply without feature flags. Tried and reverted: (a) forcing the two sides of a sector under 60 degrees onto different directions in the cover constraints (cone and tent pass, bentStar 0.07 -> 0.19); (b) the same propagated along the whole straight side (worse everywhere); (c) adding sharp-dihedral iso edges to the feature set (starPrism and twistedGrid better, cone and bentComb worse); (d) moving the nearest line point onto each unclaimed corner (no measurable change). The real fix is corner-aware field constraints: the field itself has to turn at a sharp corner (MIQ-style corner valences), so its singularities and the constraints agree. That is a stage of its own; the tests keep their 0.035 bar rather than being loosened.
- `LILA_CORPUS_ONLY=<name>` runs one corpus case, for this kind of chase.
- Weight proxy joint loop: fixed with `layLoopsOnGuides`, a post-pass for every closed edge loop curve. It traces the closed edge loops (straight through four-edge points) from the twelve points nearest the curve, takes the one lying closest on average (within 0.6 spacings), moves its points onto the curve and back onto the surface, and eases their neighbours a quarter of the way after them. The proxy's middle ring had spiralled on a coarse tube (about 9 points round); now all 189 rig tests pass and the torus loop test still does. Knife imprinting stays as the first step: it gets the flow close, the pass makes the loop exact.
- Density handles: not fixed. Measured on the test sphere: the size field and the isotropic triangles carry the handle (2.9 vs 4.7 asked for 0.5x; 7.7 vs 4.5 for 2x), but the cover's own uvs don't (4.6 vs 4.9). Tried and reverted: curl correction off (2x reaches 1.16 of the 1.25 needed, 0.5x nothing), unit energy weights instead of su*sv (with curl off, 0.5x reaches 0.85 of the 0.8 needed), and turning the flow with the size by the conformal rule n x grad log s (max turn 0.035 rad, no effect). Why: a local density change alters the curvature the layout sees (K - laplacian of log s), so by Gauss-Bonnet the layout needs extra poles round the handle, a ring of 3- and 5-edge pairs at the transition. A smooth field can't supply them. The fix is to prescribe those singularities from the integrated laplacian of log size (the `poles` path, prescribeSingularities, already takes positions), then solve the cover on that field.

## 4. Cage paint: own-fold clearance

- Nothing from the remesher fits the per-point solve, so this item is the gap found while looking: clearance ran only between lows. Each low now also traces its own surface: front faces only (the faces a ray leaves from look away), from a few epsilons out.
- Share: another low keeps 0.9 of the gap as before; across a fold of the same low both sides move into one gap, so each keeps 0.45.
- Measured: new fold test 20.6 cm -> 2.25 cm under the slab, open areas unchanged; benchmark miss rates identical (2.856% / 2.876%), time within noise. Open sheets seen from behind don't clamp (back faces).

# Quad Remesh back on AutoRemesher's own stages (main, 2026-11-14)

Jared's report: at the same settings the result was far worse than AutoRemesher 1.2.0 itself, and the symmetry centre line broke. Measured on the head (neusculpt2), same face count, symmetry off: AutoRemesher 5461 faces, 98 poles; Lila 5399 faces, 128 poles, flat density over the face, no eye loops.

## What was wrong

- The rebase had kept only AutoRemesher's field and cover and replaced the rest: its simplifier and isotropic pass (Lila's own), its curvature sizes (Lila's size field through setFaceScaling), its extraction (integer grid, which failed on the head anyway and fell back). Swapping stages one at a time, Lila's size field did the most damage, then the triangles.
- Symmetry laid out a clipped half with the field passed as guidance, which switches off the singularity simplifier, then welded the cut by distance.
- "Target Quads" in AutoRemesher's UI is half its triangle target, and its quads come out near a third of the number typed, so 16000 there is about 5400 faces. The earlier comparison was never at equal count.

## Decisions

- Vendored the missing upstream stages unchanged (autoremesher.cpp, its IsotropicRemesher wrapper, thirdparty isotropicremesher), linked meshoptimizer. One edit: resample and its helpers are public.
- Stock path: a closed shape with no creases (after Lila's own crease detection on the simplified surface), no pins, feature guides or asked poles runs AutoRemesher's stages as they come. Inside Lila this reproduces the app: 5482 faces, 99 poles on the head.
- Creases are Lila's to find. AutoRemesher's angle test at Lila's default 40 degrees wrecks a sculpt (205 poles, 50 n-gons); at its own 90 it gives 109; off gives 75. The stock stages run with it off and take Lila's feature set, which is empty on that path.
- Symmetry on the stock path: the working triangles are made mirror symmetric with an edge chain on the plane (points within 0.3 of an edge snap onto it, the rest is cut, the half is mirrored), the layout is solved on the closed whole with the chain as feature edges, then the positive half is kept and mirrored. The cut is found by topology (edges the whole had twice and the half has once). A triangle with an edge on the cut, or a corner of the loop with one face, closes up instead of mirroring into a kite or a doublet. Odd faces no longer walk onto the cut.
- Face count: Parameterizer takes a target quad count. It sets the scaling from the scaling field, measures the cover's uv area (one quad per unit), and searches the scaling (up to 3 solves, 5 on small meshes), keeping the closest. Head: 5554 for 5450, 15920 for 16000.
- Lila's size controls on the stock sizes: painted density, joints and handles as a factor; gaps and thin parts as a length limit, fitted from three sizes because the stock sizes span 1/3 to 3 times the mean. The limits' extra quads are counted on top of the target. Parts share the target by area.
- Joint Density default 0.5 -> 0. On the head it cost 25 to 80 poles and rows of triangles; the character presets still set it.
- Joint Pole Avoidance stays on for the stock path: head 84 -> 64 poles, Mochi 132 -> 102.
- Six working triangles per quad, AutoRemesher's own ratio. 3 gave 50 flaws at 16000; 9 and 12 were worse at 5450 (104 and 169 poles) and up to twice as slow.
- cleanSoup's short-edge threshold is relative to the face on the stock path; against the mean edge it collapsed legitimate small quads at the nostrils into triangle fans.
- Open or creased shapes keep Lila's own triangles, features and integer grid: the stock extraction fills a border in or leaves it ragged. Uncreased ones (crease length times edge under 2% of the area) now take the stock sizes there too: open bust 110 -> 88 poles and on count.

## Measured (defaults)

- Head, 5450, symmetry Z: 5554 faces, 2 n-gons, 64 poles, 8 s. AutoRemesher: 5461 faces, 16 non-quads, 98 poles, not symmetric.
- Head, 16000, symmetry Z, adaptivity 0.15: 15920 faces, 6 non-quads, 104 poles, 0 defects, 20 s.
- Mochi, 6000, symmetry X: 5898 faces, 102 poles (last night 106 at 7613 faces).
- Suites retopo, rig, uv, bake pass.

## Left

- At 16000 with adaptivity 0.25 a pinch at the nostril crease (thin quads converging) comes from the stock layout itself.
- Pairing odd faces adds about 1.5 poles per face it removes; a few triangles or pentagons remain.
- Density handles test still parked: the factor reaches the cover but the layout evens it out.
- Corpus (parked): cone now passes, pipe now fails (hausdorff 0.048); starPrism, combPrism, spiralFace, bentStar, twistedGrid, tent as before.
- Open sculpts and creased shapes are still on the old triangles and extraction.

# Quad Remesh follow-up: open meshes, pinch, pure quads, middle groove (main, 2026-11-14)

- Open meshes take the stock stages: each border loop of the working triangles is capped (a fan to its centre, remeshed by AutoRemesher's isotropic pass with the loop left whole), the border edges are feature edges so the layout runs a loop along them, and the quads on the caps are dropped. The working border is evened out first (12 passes along the loop): a border left by deleting faces is a sawtooth and the layout turned at every tooth. Cap faces are left out of the curvature sizing (setUnsizedFaces). Caps over 30% of the surface (a sheet) keep Lila's own path. Open bust 110 -> 84 poles, all quads; open Mochi torso 77 -> 44.
- Nostril pinch: the stock scaling field jumps from triangle to triangle and its anisotropy stretches quads 2.3 to 1 along a crease. Three smoothing passes over the field and a 1.5 to 1 cap: over five cases poles 369 -> 325, and at 16000 with adaptivity 0.25, 148 poles and 30 flaws -> 86 and none. Either change alone did less.
- Pure quads: the odd-face pairing searched breadth first, so paths wandered and left a pole pair at every turn (+50 poles for 8 faces at 16000). Now cheapest first with a turn costing four steps: an odd face carried along one row leaves nothing behind. Three rounds, each allowing less square quads. All five reference cases come out all quads; 16000 at 0.25: 136 poles -> 94.
- Two faces back to back on the same four points (left by pairing two pentagons side by side) are dropped.
- Thin-part limit fitted at 0.85 of the ask: with quads stretching less, the layout rounded 12 round an arm down to 10. Costs Mochi faces (6778 for 6000 before the count fit).
- Creased shapes compared with AutoRemesher 1.2.0 itself (sharp edge 90) on the corpus: Lila's own path is far ahead (cube 8 irregular points vs 76, cylinder 12 vs 58, bevelCube 52 vs 65, no non-quads vs 6 to 22). Only starPrism is behind (71 vs 58). Nothing ported.
- Mochi's hands: surface coverage equal to AutoRemesher's (max 1.07 vs 0.94 of a unit on a 180 unit figure, both at sampling noise).
- Middle Smoothing (flag middleSmoothing, 0 to 1, default 0, enabled only with symmetry): fills a groove along the middle loop. Averaging across the middle only (neighbours a ring nearer and further, six rings), and only straight outwards. Taubin kept the groove; plain smoothing softened the profile; inward moves flattened the nose; sliding along the surface squashed the lips.
- A test's corpus sources are dumped with LILA_RETOPO_DUMP (corpus_source_*), for running other remeshers on them.

Measured, defaults, count tolerance 0: head 5450 Z 5786 faces all quads 52 poles 9 s; head 5450 off 5436, 85; Mochi 6000 X 6778, 120; head 16000 Z adaptivity 0.15 16780, 88, 22 s; at 0.25 16276, 94. Suites retopo, rig, uv, bake pass.

# Quad Remesh follow-up 2: creased symmetry, pole reach, sharp corners, sheets (main, 2026-11-14)

- Mochi's poles: the thin-part limit at 0.85 of the ask was the cost. At 0.92 the test still holds (12 round the arm) and Mochi is back at 102 poles. Without the limit Mochi has 130, so the limit earns its place there.
- Pole pairs cancel across 12 faces, twice the stock 6. Totals over the five reference cases by reach: 6: 445, 12: 380, 16: 399, 24: 324, 48: 236. At 24 the unsymmetric head folds (12 flaws) and Mochi drifts off the surface (0.48 against 0.25); at 48 the nose flattens and holes open. 12 has no folds and the same distances as 6. Head with symmetry off: 85 -> 68 poles.
- Creased or open-sheet shapes with symmetry are laid out whole too (Lila's own triangles mirrored, the cut held as a chain), not as a half with the field as guidance, which switched the singularity simplifier off. Flat-based bust (a creased organic): 58 -> 56 poles and 5972 faces for 5450 instead of 6202. The size choice (stock or own) is made on the half, before the chain would count as a crease.
- Middle Smoothing acts only where the rows meet the middle in a V (8 degrees none, 25 full), so a groove the faces are fine enough to follow round (the philtrum, the nostril base) is left. Checked at 1.0: chest filled, nose and lips as at 0.
- Sharp corners: where two feature edges meet at under 45 degrees with no third, the field is turned through the corner's fan (the long way, a quarter turn short of the corner's angle) and the two sides are held to the lattice direction each is nearer (Parameterizer field overrides and forced corners). Crease detection no longer drops one of two creases meeting alone at a point. Tent: hausdorff 0.041 -> under 0.035 and 31 -> 10 irregular points; bentStar 0.148 -> 0.110. Not at prism tips, where a third crease runs down the side: turned there, starPrism went 71 -> 106 irregular. Not with edge loop curves: where one crosses a border at a slant the sector is no corner, and the weight proxy's joint loops broke.
- Sheets: tried closing a sheet with a back (a copy a quarter edge behind, joined at the border) so the stock layout applies. Worse everywhere on the corpus (spiralFace 17 faces, slivers 14, hemisphere and tent borders off by 11 and 38). Removed; sheets keep Lila's own path.
- Why the star prism still fails: its tips are in the working triangles with both creases now, but the layout near them folds (264 flipped triangles in the cover) and the grid extraction drops the tip quads; the result misses the tips by one to two quads.

Measured, defaults, count tolerance 0: head 5450 Z 6026 faces all quads 48 poles; off 5502, 68; Mochi 6000 X 6216, 102; head 16000 Z at adaptivity 0.15 15898, 78; at 0.25 16106, 84; open bust 4232, 74; flat-based bust Z 5972, 56; off 5725, 148. Suites retopo, rig, bake pass; uv fails on another session's uncommitted src/uv edits.
