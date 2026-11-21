#pragma once

/// @file
/// @brief Game shaders: runtime GLSL → SPIR-V compilation (cached, hot reloaded) or precompiled SPIR-V.

#include "nova/renderer/Mesh.h"
#include "nova/rhi/RHITypes.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nova {

/// @brief Pipeline stage of a shader. Inferred from the file name: `.vert`, `.frag`, `.comp`,
///        `.rgen`, `.rmiss`, `.rchit`, `.rahit` (optionally followed by `.glsl` and/or `.spv`).
enum class ShaderStage : u8 { Vertex, Fragment, Compute, RayGen, Miss, ClosestHit, AnyHit };

/// @brief Where a shader's SPIR-V comes from.
enum class ShaderSource : u8 {
    /// Runtime when the engine has the compiler and the source file exists; otherwise precompiled.
    Auto,
    /// Compile the GLSL source at load time (disk cache + hot reload). Fails without the compiler.
    Runtime,
    /// Load `<file name>[.<variant>].spv` built by `nova_add_game_shaders` (no compiler needed).
    Precompiled,
    /// Compiled from a string with ShaderLibrary::compile() (reported only, not requested).
    Memory,
};

/// @brief Options of ShaderLibrary::load().
struct ShaderLoadOptions {
    ShaderSource source = ShaderSource::Auto; ///< Runtime or precompiled (per shader).
    std::vector<std::string> defines;         ///< Preprocessor macros, `"NAME"` or `"NAME=VALUE"` (runtime only).
    /// @brief Name of this define set for precompiled files: `water.frag` + `"rq"` → `water.frag.rq.spv`.
    std::string variant;
};

/// @brief Loads, compiles and hot reloads shaders. Owned by the Renderer (Renderer::shaders()),
///        usable on its own (it needs no GPU).
///
/// @code
/// auto& shaders = renderer().shaders();
/// nova::ShaderHandle water = shaders.load("assets/shaders/water.frag");        // source or .spv, per availability
/// nova::ShaderHandle blur  = shaders.load("blur.comp", {.source = nova::ShaderSource::Precompiled});
/// rhi::PipelineHandle p    = renderer().computePipeline(blur);                  // rebuilt after reloads
/// @endcode
///
/// - **Runtime** shaders compile with shaderc (GLSL, Vulkan 1.3). `#include "nova/material.glsl"`
///   and the other engine headers resolve from `<exe>/shaders/include`; `#include "x.glsl"` also
///   resolves next to the including file. Results are cached in `<exe>/shader_cache` and
///   revalidated by content hash (source and every include).
/// - **Precompiled** shaders are read from `<exe>/shaders/<file name>.spv` (or next to the file).
/// - update() (called by the Renderer every frame) recompiles shaders whose source or includes
///   changed, and reloads `.spv` files that were rebuilt. A failed compile keeps the previous
///   SPIR-V and records the error; version() increments on every successful (re)load.
class ShaderLibrary : NonCopyable {
public:
    /// @brief Default search folders next to the executable (see the class description).
    ShaderLibrary();
    ~ShaderLibrary();

    /// @brief True if this build of the engine can compile GLSL at runtime (`NOVA_SHADER_COMPILER`).
    static bool compilerAvailable();
    /// @brief Stage from a file name (`water.frag`, `blur.comp.glsl`, `sky.frag.spv`), or false if unknown.
    static bool stageFromPath(const std::filesystem::path& file, ShaderStage& stage);

    /// @name Loading
    /// @{
    /// @brief Loads a shader file; the same file + options returns the same handle.
    ///
    /// Relative paths are tried against the working directory, then the executable's folder.
    /// A runtime shader whose first compile fails still gets a valid handle (ready() is false,
    /// error() says why) so that fixing the file hot reloads it.
    /// @return Invalid handle if neither the source nor a precompiled file can be found.
    ShaderHandle load(const std::filesystem::path& file, const ShaderLoadOptions& options = {});
    /// @brief Compiles GLSL from memory (generated shaders). Needs the compiler.
    /// @param name Name for errors and `#include` resolution (relative includes use its folder).
    /// @param source GLSL source.
    /// @param stage Pipeline stage.
    /// @param defines Preprocessor macros.
    /// @return Valid handle even if compilation fails (see error()); invalid without the compiler.
    ShaderHandle compile(std::string_view name, std::string_view source, ShaderStage stage,
                         const std::vector<std::string>& defines = {});
    /// @brief Replaces the source of a compile() shader and recompiles it in place.
    /// @return True on success (on failure the previous SPIR-V stays).
    bool recompile(ShaderHandle shader, std::string_view source);
    /// @brief The same shader with extra defines and a variant name (e.g. the renderer's ray query
    ///        variant `{"NOVA_RAY_QUERY=1"}, "rq"`). Returns `base` if it is invalid.
    ShaderHandle variant(ShaderHandle base, const std::vector<std::string>& defines, std::string_view variantName);
    /// @brief Reloads one shader now (recompile or re-read). @return True on success.
    bool reload(ShaderHandle shader);
    /// @brief Polls watched files and reloads changed shaders (throttled by setPollInterval()).
    /// @return Number of shaders reloaded successfully.
    u32 update();
    /// @}

    /// @name Queries
    /// @{
    /// @brief True for handles returned by this library.
    bool valid(ShaderHandle shader) const { return shader.id < m_entries.size(); }
    /// @brief True if the shader has SPIR-V (compiled or loaded at least once).
    bool ready(ShaderHandle shader) const { return valid(shader) && m_entries[shader.id].desc.valid(); }
    /// @brief The SPIR-V to create pipelines with (empty if not ready()).
    const rhi::ShaderDesc& desc(ShaderHandle shader) const;
    /// @brief Increments on every successful (re)load; pipelines built from an older version are stale.
    u32 version(ShaderHandle shader) const { return valid(shader) ? m_entries[shader.id].version : 0; }
    /// @brief Last compile/load error, "" when the current SPIR-V is up to date.
    const std::string& error(ShaderHandle shader) const;
    /// @brief The resolved file (or the compile() name).
    const std::filesystem::path& path(ShaderHandle shader) const;
    /// @brief Pipeline stage.
    ShaderStage stage(ShaderHandle shader) const { return valid(shader) ? m_entries[shader.id].stage : ShaderStage::Fragment; }
    /// @brief Where the current SPIR-V came from (Runtime, Precompiled or Memory).
    ShaderSource source(ShaderHandle shader) const { return valid(shader) ? m_entries[shader.id].resolved : ShaderSource::Auto; }
    /// @brief True if the last runtime load was served from the disk cache.
    bool fromCache(ShaderHandle shader) const { return valid(shader) && m_entries[shader.id].fromCache; }
    /// @brief Every shader loaded so far.
    std::vector<ShaderHandle> shaders() const;
    /// @brief Increments whenever any shader is (re)loaded: a cheap "did anything change?" check.
    u64 changeCounter() const { return m_changeCounter; }
    /// @}

    /// @name Configuration
    /// @{
    /// @brief Adds a folder searched by `#include` (after the including file's folder).
    void addIncludeDir(const std::filesystem::path& dir);
    /// @brief Adds a folder searched for precompiled `.spv` files (searched first).
    void addPrecompiledDir(const std::filesystem::path& dir);
    /// @brief Folder of the compile cache; empty disables caching.
    void setCacheDir(const std::filesystem::path& dir) { m_cacheDir = dir; }
    /// @brief Current cache folder.
    const std::filesystem::path& cacheDir() const { return m_cacheDir; }
    /// @brief Enables/disables polling in update() (on by default).
    void setHotReload(bool enabled) { m_hotReload = enabled; }
    /// @brief Whether update() polls files.
    bool hotReload() const { return m_hotReload; }
    /// @brief Minimum time between two polls (default 250 ms; 0 = every update()).
    void setPollInterval(std::chrono::milliseconds interval) { m_pollInterval = interval; }
    /// @}

private:
    struct Watched {
        std::filesystem::path file;
        std::filesystem::file_time_type time;
    };
    struct Entry {
        std::filesystem::path path;   // resolved source/.spv file, or the compile() name
        std::string key;              // dedupe key of load()
        ShaderStage stage = ShaderStage::Fragment;
        ShaderLoadOptions options;
        ShaderSource resolved = ShaderSource::Auto;
        std::string memorySource;     // compile() shaders
        rhi::ShaderDesc desc;
        u32 version = 0;
        std::string error;
        bool fromCache = false;
        std::vector<Watched> watched; // files whose changes trigger a reload
    };

    bool loadEntry(Entry& e);
    bool compileEntry(Entry& e, std::string_view source, bool useCache);
    bool loadPrecompiled(Entry& e);
    std::filesystem::path findPrecompiled(const Entry& e) const;
    std::filesystem::path resolveFile(const std::filesystem::path& file) const;
    void watch(Entry& e, const std::vector<std::filesystem::path>& files);

    std::vector<Entry> m_entries;
    std::unordered_map<std::string, u32> m_byKey;
    std::vector<std::filesystem::path> m_includeDirs;
    std::vector<std::filesystem::path> m_precompiledDirs;
    std::filesystem::path m_cacheDir;
    bool m_hotReload = true;
    std::chrono::milliseconds m_pollInterval{250};
    std::chrono::steady_clock::time_point m_lastPoll{};
    u64 m_changeCounter = 0;
};

} // namespace nova
