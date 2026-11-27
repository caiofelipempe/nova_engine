#pragma once

/// @file
/// @brief Streaming of a large world by chunks: the grid, its settings and what the game plugs in.
///
/// The world is cut into chunks (256 m by default) along the axes you choose (ChunkSettings::axes:
/// X only, X and Z, all three...). Around the focus (the player, the camera: Scene::setPhysicsFocus)
/// the scene keeps three rings, in chunks on every subdivided axis:
/// - **physics** (`physicsRadius`, 1 = 3×3): bodies, characters and joints simulate. Farther ones
///   are **dormant**: out of the physics engine, their pose kept in their transform and their
///   velocities saved, and back as they were when the focus comes near;
/// - **loaded** (`loadRadius`, 2 = 5×5): every chunk in it is loaded (ChunkSource::load), nearest
///   first, at most `loadsPerFrame` a frame;
/// - **kept** (`unloadRadius`, 3 = 7×7): a loaded chunk is unloaded (ChunkSource::unload) only
///   after staying out of it for `unloadDelay`.
/// The focus chunk itself changes only when the focus is 1/8 of a chunk past its border, so walking
/// back and forth on a border loads, unloads and wakes nothing.
///
/// The engine owns the mechanism; the **content** is the game's: ChunkSource::load creates the
/// chunk's entities (procedurally, from prefabs or files) and marks them with StreamedComponent.
/// When the chunk unloads, the streamed entities that are in it then (where they are, not where
/// they were made) are destroyed, after ChunkSource::unload (to save what changed). Entities
/// without StreamedComponent (the player) are never unloaded.

#include "nova/core/Base.h"
#include "nova/math/Scalar.h"
#include "nova/math/Time.h"

#include <array>
#include <functional>

namespace nova {

class Scene;

/// @brief A chunk of the grid: its index on each axis (0 on the axes that are not subdivided).
struct ChunkCoord {
    i64 x = 0, y = 0, z = 0;

    i64& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    i64 operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
    friend bool operator==(const ChunkCoord&, const ChunkCoord&) = default;
    friend auto operator<=>(const ChunkCoord&, const ChunkCoord&) = default;
};

/// @brief How the world is streamed (Scene::setChunkStreaming).
struct ChunkSettings {
    bool enabled = false;                        ///< Off: everything is loaded and simulated.
    Length size = Length(256.0);                 ///< Edge of a chunk (m). Should divide the world's axes.
    std::array<bool, 3> axes{true, false, true}; ///< The axes cut into chunks (default X and Z).
    i32 physicsRadius = 1;                       ///< Chunks around the focus that simulate (1 = 3×3).
    i32 loadRadius = 2;                          ///< Chunks around the focus that are loaded (2 = 5×5).
    i32 unloadRadius = 3;                        ///< Loaded chunks are kept up to here (3 = 7×7)...
    Duration unloadDelay = Duration::seconds(2); ///< ...and unloaded after being out this long.
    u32 loadsPerFrame = 2;                       ///< Most chunks loaded in one update (0: no limit).
};

/// @brief What the game plugs in to fill and empty chunks (Scene::setChunkSource).
struct ChunkSource {
    /// @brief Creates the chunk's content (mark its entities with StreamedComponent).
    std::function<void(Scene&, const ChunkCoord&)> load;
    /// @brief Called before the chunk's streamed entities are destroyed (to save what changed).
    std::function<void(Scene&, const ChunkCoord&)> unload;
};

/// @brief Marks an entity as part of the streamed world: destroyed when the chunk it is in unloads
///        (or when it goes beyond the kept ring). Entities without it are never unloaded.
struct StreamedComponent {
    bool unload = true; ///< False: kept loaded for now (carried by the player...); true again to stream it.
};

} // namespace nova
