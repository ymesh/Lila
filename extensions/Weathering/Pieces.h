#pragma once

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/**
 * Pieces: the meshes a weathering step builds from (scatter items, ivy leaves, roof sheets, decals, insulators). The user's
 * own pieces always come first; the shipped placeholders only fill a role nobody has supplied.
 *
 * A pieces value is a name or a list of them, each one of:
 *   - a scene mesh, or a group (every mesh under it is a piece), or an object set (each member)
 *   - a Stagehand library asset id ("Debris/StoneChunkM1")
 *   - "tag:<word>": every library asset carrying that tag
 *   - {"piece": <any of the above>, "weight": w}: picked w times as often (default 1)
 *
 * Library tags for roles are "weathering" plus the role ("rubble", "plank", "plant", "ivy-leaf", "roof-sheet", ...), and a
 * style tag "realistic" or "toon". Placeholders live in the "placeholder" tier.
 */
namespace weathering
{
using json = nlohmann::json;
using lila::scene::Graph;
using lila::scene::NodeId;

struct Piece
{
    NodeId mesh = lila::scene::kNoNode; // the shape: its own space is the piece's space
    NodeId root = lila::scene::kNoNode; // what a realized copy duplicates (the mesh itself, or a library master root)
    std::string name;                   // as given, for stats and messages
    double weight = 1.0;
    bool placeholder = false;
};

enum class Style
{
    Realistic,
    Toon,
};

/** Resolves a pieces value. Throws CommandError naming cmd when an entry is unknown or has no mesh. */
std::vector<Piece> resolvePieces(Graph& g, const json& value, const char* cmd);

/**
 * The pieces in a flag when it is given, else the library's pieces for a role: assets tagged "weathering" and role, in
 * the given style when any exist in it. Assets the user supplied (any tier but placeholder) win: when one carries the
 * role, placeholders are left out. Throws when there are none at all, saying how to give some.
 */
std::vector<Piece> piecesOrDefault(Graph& g, const lila::Args& a, const char* flag, const std::string& role, Style style, const char* cmd);

/** Library asset ids for a role, the same choice piecesOrDefault makes. */
std::vector<std::string> roleAssets(const std::string& role, Style style);

/** A weighted pick: u in [0, 1) maps to a piece index. */
size_t pickPiece(std::span<const Piece> pieces, double u);

/** A piece mesh's output in its own space. */
lila::geo::Mesh pieceMesh(Graph& g, const Piece& p);

/** The style flag every weathering command takes, and its value. */
lila::FlagDef styleFlag();
Style styleArg(const lila::Args& a);
const char* styleName(Style s);

/** A pieces flag (T::Any: a name, a list, or weighted entries). */
lila::FlagDef piecesFlag(const char* name, const char* shortName, const char* doc);

/** A small deterministic hash to [0, 1) from a seed and counters. */
double unitHash(uint32_t seed, uint32_t a, uint32_t b = 0);
} // namespace weathering
