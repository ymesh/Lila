#pragma once

// Pattern drafting on the flat pieces: edits that keep seams, notches and linked twins in step, darts, mirrored twins,
// grading and automatic sewing.

#include "TailorCore.h"

#include <utility>
#include <vector>

namespace tailor
{

/** Total outline length of a seam end's runs, metres. */
float seamSideLength(const Garment& g, const std::vector<SeamSide>& sides);

/** Inserts an outline point before index `index` (0..size). Seams keep sewing the same stretch of outline. */
void insertOutlinePoint(Garment& g, int panel, int index, const Vec2& at);
/** Deletes an outline point; seams ending there end at the next point, and a seam left without length is removed.
 *  Returns the number of seams removed. */
int deleteOutlinePoint(Garment& g, int panel, int index);
/** Replaces a panel's outline with one of the same point count. */
void setOutline(Garment& g, int panel, const std::vector<Vec2>& outline);
/** Removes panels and the seams sewing them. Returns the number of seams removed. */
int removePanels(Garment& g, std::vector<int> panels);

/** Marks a notch at the outline point nearest `at`, inserting a point there when none is within `snap`. Returns the
 *  point index. */
int addNotch(Garment& g, int panel, const Vec2& at, float snap = 0.005f);

/**
 * Cuts a dart into the outline edge from point `edge` to the next: legs `width` apart centred `t` along the edge, the
 * point `depth` in from it, legs sewn together. A seam along that edge keeps sewing the edge on both sides of the dart.
 * Returns the dart's seam index, or -1 when the edge is too short for it.
 */
int addDart(Garment& g, int panel, int edge, float t, float width, float depth);

/**
 * Adds a mirror image of a panel, placed mirrored across the plane x = planeX. Linked, every outline edit to either is
 * mirrored onto the other. Returns the new panel's index.
 */
int mirrorPanel(Garment& g, int panel, float planeX, bool link);

/**
 * Grades a pattern: widths by `width`, lengths by `length`. Set-in sleeve caps are then re-eased to their armholes:
 * each cap is widened or narrowed until it carries the same ease as before.
 */
void gradePattern(Garment& g, float width, float length);

/** An unsewn stretch of outline from corner to corner, as the panel is placed: five points along it and its outward direction. */
struct OpenEdge
{
    SeamSide side;
    float length = 0.f;
    std::array<Vec3, 5> at{};
    Vec3 out{};
};

/** Every unsewn outline edge of the active and frozen panels. */
std::vector<OpenEdge> openEdges(const Garment& g);

/**
 * Seams a pattern maker would sew next: unsewn outline edges (corner to corner) on placed panels whose lengths match
 * within 15% and that face each other within maxGap. Each edge is used once, closest pairs first. Given the body, edges
 * that both open upwards with the body under the line between them (a shoulder, a hood's crown) sew too, up to 45 cm
 * apart; hems, necklines and armholes stay open.
 */
std::vector<Seam> suggestSeams(const Garment& g, float maxGap, const Sdf* body = nullptr);

/** A seam split at its notches when both ends carry the same number, so notch meets notch. Otherwise the seam itself. */
std::vector<Seam> splitSeamAtNotches(const Garment& g, const Seam& seam);

// Details: pieces laid on a host panel and held by tacks, collars sewn to a neckline, cords held along a path.

/** Places piece so its pattern origin sits on host's pattern point `at`, lifted `lift` metres off it, following host's wrap. */
void placeOnPanel(const Panel& host, const Vec2& at, float lift, Panel& piece);
/** A patch pocket on host with its lower left corner at `at` (host pattern, metres), tacked round all but its mouth;
 *  with flap, a flap tacked above it hanging over the mouth. Returns the pocket panel. */
int addPocket(Garment& g, int host, const Vec2& at, float width, float height, bool flap);
/** A rounded patch (a knee or elbow reinforcement) centred on host's pattern point, tacked all the way round.
 *  Named after host plus `name`. Returns the patch panel. */
int addPatch(Garment& g, int host, const Vec2& centre, float width, float height, const std::string& name = "Patch");
/** count belt loops across host at pattern height y, each width wide spanning height, tacked at both ends. */
std::vector<int> addBeltLoops(Garment& g, int host, float y, int count, float width, float height);
/** The open edges round the top of the garment, opening upwards, chained through the seams between them: its neckline. */
std::vector<SeamSide> findNeckline(const Garment& g);
/** A collar strip as long as the neckline, sewn to it; closed into a ring when the neckline goes all the way round.
 *  fold presses a shirt collar's fall down over its stand. Returns the collar panel. */
int addCollar(Garment& g, const std::vector<SeamSide>& neckline, float height, bool fold);
/** A drawstring along a channel of outline: a cord tacked along it, gather times the channel's length so it draws the
 *  channel in, with tails hanging past both ends. Returns the cord panel. */
int addDrawstring(Garment& g, const std::vector<SeamSide>& channel, float tail, float gather, float width);
/** Zigzag lacing between two edges: eyelets inset from each, a lace tacked through them in turn, tails at the top.
 *  eyelets gets the eyelet points (panel, pattern point) in lacing order. Returns the lace panel. */
int addLacing(Garment& g, const std::vector<SeamSide>& sideA, const std::vector<SeamSide>& sideB, int pairs, float inset, float tail,
              float width, std::vector<std::pair<int, Vec2>>* eyelets = nullptr);
/** Opens a seam past `closed` (0 to 1 along side a): the rest of it unsewn, as a zip opened to its slider. */
bool openSeam(Garment& g, int seam, float closed);

/**
 * Wrinkle relief for a fabric: a tile-metre square of it, border pinned flat and drawn in by `compress` along U (axis 0)
 * or V (axis 1), simulated until it buckles. Returns size x size heights (metres, rows along V), tiling at the border.
 */
std::vector<float> bakeWrinkleHeight(const Fabric& fabric, int axis, float compress, int size, float tile, uint32_t seed = 1);
/** RGBA tangent-space normals (DirectX green) from a tiling height field over a tile-metre square. */
std::vector<float> heightToNormals(const std::vector<float>& height, int size, float tile);

} // namespace tailor
