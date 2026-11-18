#pragma once

#include "core/Export.h"

#include "tex/PaintSurface.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * Particle brushes: each brush dab emits a few particles that move over the surface and leave dabs behind (drips running
 * down, rain spattering, sparks flying out). Deterministic: a dab, its index in the stroke and the seed always give the
 * same particles, so a stroke painted live and the same stroke replayed agree, and the stroke record stores the dabs.
 */
namespace lila::tex::paint
{
struct LILA_SCENE_API ParticleSpec
{
    /**
     * drips, rain or sparks; or flow, a small simulation on the surface: particles fall with gravity in world space, crawl
     * along the surface (their velocity kept in its tangent plane), stick on slopes flatter than friction, and pool where
     * they stop. Presets of flow: rustRun, leak, rainStreaks, dustSettle, burn, scorch.
     */
    std::string kind = "drips";
    /** Particles per brush dab. */
    int count = 3;
    /** How far particles travel, in brush radii. */
    float length = 4.0f;
    /** Particle size as a fraction of the brush radius. */
    float size = 0.25f;
    /** Spread of the start points, in brush radii. */
    float spread = 1.0f;
    /** Fraction of the brush dabs that emit at all (the rest only paint themselves). */
    float rate = 0.35f;
    uint32_t seed = 0;
    /** Object-space down, for drips, rain and flow. */
    glm::vec3 down{0.0f, -1.0f, 0.0f};
    /** flow: how strongly gravity pulls (0 none, 1 full), the slope below which particles stick (0 to 1, as the pull along
     *  the surface over gravity), how much they wander, how big the pool they leave where they stop (in particle sizes),
     *  and where they start: dab (at the brush dab), area (over the spread) or ring (on its edge). */
    float gravity = 1.0f;
    float friction = 0.15f;
    float wander = 0.1f;
    float pool = 1.5f;
    std::string emitter = "area";
    /** flow: the opacity left at the end of the run (1 keeps it, 0 fades it out). */
    float fade = 1.0f;

    nlohmann::json toJson() const;
    /** Throws std::invalid_argument on an unknown kind. */
    static ParticleSpec fromJson(const nlohmann::json& j);
};

/** The dabs the particles of one brush dab leave (not the brush dab itself). */
LILA_SCENE_API std::vector<Dab> emitParticles(const PaintMesh& mesh, const Dab& from, uint32_t index, const ParticleSpec& spec);

/** Every dab followed by its particles' dabs. */
LILA_SCENE_API std::vector<Dab> withParticles(const PaintMesh& mesh, std::span<const Dab> dabs, uint32_t firstIndex, const ParticleSpec& spec);
} // namespace lila::tex::paint
