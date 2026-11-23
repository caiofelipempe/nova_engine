#include "nova/renderer/ShaderLibrary.h"

#include "nova/core/Log.h"
#include "nova/platform/FileSystem.h"

#if NOVA_SHADER_COMPILER
#include <shaderc/shaderc.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace nova {

namespace fsys = std::filesystem;

namespace {

// Bump when the compile options or the engine's GLSL layouts (nova/common.glsl) change: invalidates
// every cached .spv.
constexpr const char* kCacheVersion = "nova-shader-cache-2"; // 2: FrameData::wrapSize, DrawPush::indices

u64 fnv1a(std::string_view data, u64 h = 1469598103934665603ull) {
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex(u64 v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string stripSuffix(std::string name, std::string_view suffix) {
    if (lower(name).ends_with(suffix)) name.resize(name.size() - suffix.size());
    return name;
}

bool isSpirvFile(const fsys::path& file) { return lower(file.filename().string()).ends_with(".spv"); }

fsys::file_time_type mtime(const fsys::path& file) {
    std::error_code ec;
    auto t = fsys::last_write_time(file, ec);
    return ec ? fsys::file_time_type{} : t;
}

bool toWords(const std::vector<u8>& bytes, std::vector<u32>& out) {
    if (bytes.size() < 20 || bytes.size() % 4 != 0) return false;
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out[0] == 0x07230203u; // SPIR-V magic number
}

const char* sourceName(ShaderSource s) {
    switch (s) {
        case ShaderSource::Runtime: return "runtime";
        case ShaderSource::Precompiled: return "precompiled";
        case ShaderSource::Memory: return "memory";
        default: return "auto";
    }
}

// ----------------------------------------------------------------- compiler --
struct CompileResult {
    bool ok = false;
    std::vector<u32> spirv;
    std::string error;
    std::vector<fsys::path> includes; // resolved files pulled in by #include
};

#if NOVA_SHADER_COMPILER

shaderc_shader_kind kindOf(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return shaderc_vertex_shader;
        case ShaderStage::Fragment: return shaderc_fragment_shader;
        case ShaderStage::Compute: return shaderc_compute_shader;
        case ShaderStage::RayGen: return shaderc_raygen_shader;
        case ShaderStage::Miss: return shaderc_miss_shader;
        case ShaderStage::ClosestHit: return shaderc_closesthit_shader;
        case ShaderStage::AnyHit: return shaderc_anyhit_shader;
    }
    return shaderc_glsl_infer_from_source;
}

// Resolves `#include "x"` next to the including file, then in the include folders;
// `#include <x>` only in the include folders. Records every file it opens.
class Includer final : public shaderc::CompileOptions::IncluderInterface {
public:
    Includer(const std::vector<fsys::path>& dirs, std::vector<fsys::path>& opened) : m_dirs(dirs), m_opened(opened) {}

    shaderc_include_result* GetInclude(const char* requested, shaderc_include_type type, const char* requesting,
                                       size_t) override {
        auto* r = new Result;
        std::vector<fsys::path> candidates;
        if (type == shaderc_include_type_relative)
            candidates.push_back(fsys::path(requesting).parent_path() / requested);
        for (const fsys::path& d : m_dirs) candidates.push_back(d / requested);
        for (const fsys::path& c : candidates) {
            std::error_code ec;
            if (!fsys::is_regular_file(c, ec)) continue;
            if (auto text = fs::readText(c)) {
                r->name = c.lexically_normal().generic_string();
                r->content = std::move(*text);
                if (std::find(m_opened.begin(), m_opened.end(), c.lexically_normal()) == m_opened.end())
                    m_opened.push_back(c.lexically_normal());
                break;
            }
        }
        if (r->name.empty()) r->content = std::string("cannot find include file '") + requested + "'";
        r->result = {r->name.c_str(), r->name.size(), r->content.c_str(), r->content.size(), r};
        return &r->result;
    }

    void ReleaseInclude(shaderc_include_result* data) override { delete static_cast<Result*>(data->user_data); }

private:
    struct Result {
        std::string name, content;
        shaderc_include_result result{};
    };
    const std::vector<fsys::path>& m_dirs;
    std::vector<fsys::path>& m_opened;
};

CompileResult compileGlsl(std::string_view source, const std::string& name, ShaderStage stage,
                          const std::vector<std::string>& defines, const std::vector<fsys::path>& includeDirs) {
    CompileResult out;
    static shaderc::Compiler compiler; // thread-compatible; the library is used from one thread
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetGenerateDebugInfo(); // readable names in RenderDoc / Nsight
    for (const std::string& d : defines) {
        const size_t eq = d.find('=');
        if (eq == std::string::npos) options.AddMacroDefinition(d);
        else options.AddMacroDefinition(d.substr(0, eq), d.substr(eq + 1));
    }
    options.SetIncluder(std::make_unique<Includer>(includeDirs, out.includes));

    auto result = compiler.CompileGlslToSpv(source.data(), source.size(), kindOf(stage), name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        out.error = result.GetErrorMessage();
        while (!out.error.empty() && (out.error.back() == '\n' || out.error.back() == '\r')) out.error.pop_back();
        if (out.error.empty()) out.error = "compilation failed";
        return out;
    }
    out.spirv.assign(result.cbegin(), result.cend());
    out.ok = true;
    return out;
}

#else

CompileResult compileGlsl(std::string_view, const std::string&, ShaderStage, const std::vector<std::string>&,
                          const std::vector<fsys::path>&) {
    CompileResult out;
    out.error = "this build has no shader compiler (NOVA_SHADER_COMPILER=OFF); ship precompiled .spv files";
    return out;
}

#endif

} // namespace

// ----------------------------------------------------------------- library --
ShaderLibrary::ShaderLibrary() {
    const fsys::path exe = fs::executableDir();
    m_includeDirs.push_back(exe / "shaders" / "include");
    m_precompiledDirs.push_back(exe / "shaders");
    m_cacheDir = exe / "shader_cache";
}

ShaderLibrary::~ShaderLibrary() = default;

bool ShaderLibrary::compilerAvailable() { return NOVA_SHADER_COMPILER != 0; }

bool ShaderLibrary::stageFromPath(const fsys::path& file, ShaderStage& stage) {
    std::string name = stripSuffix(stripSuffix(lower(file.filename().string()), ".spv"), ".glsl");
    // Precompiled variants: "water.frag.rq" → look one extension further.
    for (int pass = 0; pass < 2; ++pass) {
        const fsys::path p(name);
        const std::string ext = p.extension().string();
        static const std::pair<const char*, ShaderStage> kStages[] = {
            {".vert", ShaderStage::Vertex},   {".frag", ShaderStage::Fragment},     {".comp", ShaderStage::Compute},
            {".rgen", ShaderStage::RayGen},   {".rmiss", ShaderStage::Miss},        {".rchit", ShaderStage::ClosestHit},
            {".rahit", ShaderStage::AnyHit},
        };
        for (auto& [e, s] : kStages)
            if (ext == e) {
                stage = s;
                return true;
            }
        name = p.stem().string();
    }
    return false;
}

void ShaderLibrary::addIncludeDir(const fsys::path& dir) { m_includeDirs.push_back(dir); }
void ShaderLibrary::addPrecompiledDir(const fsys::path& dir) { m_precompiledDirs.insert(m_precompiledDirs.begin(), dir); }

fsys::path ShaderLibrary::resolveFile(const fsys::path& file) const {
    std::error_code ec;
    if (file.is_absolute()) return file.lexically_normal();
    if (fsys::exists(file, ec)) return fsys::absolute(file, ec).lexically_normal();
    const fsys::path exe = fs::executableDir() / file;
    if (fsys::exists(exe, ec)) return exe.lexically_normal();
    return fsys::absolute(file, ec).lexically_normal(); // may not exist: a precompiled file can stand in
}

fsys::path ShaderLibrary::findPrecompiled(const Entry& e) const {
    // "water.frag", "water.frag.glsl" and "water.frag.spv" all map to "water.frag[.<variant>].spv".
    const std::string base = stripSuffix(stripSuffix(e.path.filename().string(), ".spv"), ".glsl");
    const std::string name = base + (e.options.variant.empty() ? "" : "." + e.options.variant) + ".spv";
    std::error_code ec;
    for (const fsys::path& dir : m_precompiledDirs)
        if (fsys::is_regular_file(dir / name, ec)) return dir / name;
    if (fsys::is_regular_file(e.path.parent_path() / name, ec)) return e.path.parent_path() / name;
    return {};
}

ShaderHandle ShaderLibrary::load(const fsys::path& file, const ShaderLoadOptions& options) {
    Entry e;
    e.path = resolveFile(file);
    e.options = options;
    if (isSpirvFile(e.path) && e.options.source == ShaderSource::Auto) e.options.source = ShaderSource::Precompiled;
    if (!stageFromPath(e.path, e.stage) && e.options.source != ShaderSource::Precompiled) {
        NOVA_CORE_ERROR("Shaders: cannot infer the stage of '{}' (use .vert/.frag/.comp...)", file.generic_string());
        return {};
    }

    std::string key = e.path.generic_string() + "|" + std::to_string(static_cast<int>(e.options.source)) + "|" +
                      e.options.variant;
    for (const std::string& d : e.options.defines) key += "|" + d;
    if (auto it = m_byKey.find(key); it != m_byKey.end()) return {it->second};
    e.key = key;

    // Decide where the SPIR-V comes from.
    std::error_code ec;
    const bool sourceExists = !isSpirvFile(e.path) && fsys::is_regular_file(e.path, ec);
    switch (e.options.source) {
        case ShaderSource::Runtime: e.resolved = ShaderSource::Runtime; break;
        case ShaderSource::Precompiled: e.resolved = ShaderSource::Precompiled; break;
        default: e.resolved = compilerAvailable() && sourceExists ? ShaderSource::Runtime : ShaderSource::Precompiled; break;
    }
    if (e.resolved == ShaderSource::Runtime && !sourceExists) {
        NOVA_CORE_ERROR("Shaders: source '{}' not found", e.path.generic_string());
        return {};
    }
    if (e.resolved == ShaderSource::Precompiled && findPrecompiled(e).empty()) {
        NOVA_CORE_ERROR("Shaders: no precompiled SPIR-V for '{}'{} (add it with nova_add_game_shaders{})",
                        e.path.filename().string(), e.options.variant.empty() ? "" : " variant '" + e.options.variant + "'",
                        compilerAvailable() ? "" : "; this build cannot compile GLSL");
        return {};
    }

    const u32 id = static_cast<u32>(m_entries.size());
    m_entries.push_back(std::move(e));
    m_byKey.emplace(std::move(key), id);
    Entry& entry = m_entries.back();
    if (!loadEntry(entry))
        NOVA_CORE_ERROR("Shaders: {}:\n{}", entry.path.filename().string(), entry.error);
    return {id};
}

ShaderHandle ShaderLibrary::compile(std::string_view name, std::string_view source, ShaderStage stage,
                                    const std::vector<std::string>& defines) {
    if (!compilerAvailable()) {
        NOVA_CORE_ERROR("Shaders: cannot compile '{}': this build has no shader compiler", name);
        return {};
    }
    Entry e;
    e.path = fsys::path(std::string(name));
    e.stage = stage;
    e.options.source = ShaderSource::Memory;
    e.options.defines = defines;
    e.resolved = ShaderSource::Memory;
    e.memorySource = std::string(source);
    const u32 id = static_cast<u32>(m_entries.size());
    m_entries.push_back(std::move(e));
    Entry& entry = m_entries.back();
    if (!compileEntry(entry, entry.memorySource, false))
        NOVA_CORE_ERROR("Shaders: {}:\n{}", std::string(name), entry.error);
    return {id};
}

bool ShaderLibrary::recompile(ShaderHandle shader, std::string_view source) {
    if (!valid(shader) || m_entries[shader.id].resolved != ShaderSource::Memory) return false;
    Entry& e = m_entries[shader.id];
    e.memorySource = std::string(source);
    return compileEntry(e, e.memorySource, false);
}

ShaderHandle ShaderLibrary::variant(ShaderHandle base, const std::vector<std::string>& defines,
                                    std::string_view variantName) {
    if (!valid(base)) return base;
    const Entry& b = m_entries[base.id];
    if (b.resolved == ShaderSource::Memory) {
        std::vector<std::string> all = b.options.defines;
        all.insert(all.end(), defines.begin(), defines.end());
        const std::string name = b.path.generic_string() + "." + std::string(variantName);
        const std::string key = "memory|" + std::to_string(base.id) + "|" + std::string(variantName);
        if (auto it = m_byKey.find(key); it != m_byKey.end()) return {it->second};
        const ShaderHandle h = compile(name, b.memorySource, b.stage, all);
        if (h) m_byKey.emplace(key, h.id);
        return h;
    }
    ShaderLoadOptions o = b.options;
    o.defines.insert(o.defines.end(), defines.begin(), defines.end());
    o.variant = b.options.variant.empty() ? std::string(variantName) : b.options.variant + "." + std::string(variantName);
    const fsys::path file = b.path; // copy: load() may reallocate m_entries
    return load(file, o);
}

bool ShaderLibrary::reload(ShaderHandle shader) {
    if (!valid(shader)) return false;
    Entry& e = m_entries[shader.id];
    if (e.resolved == ShaderSource::Memory) return compileEntry(e, e.memorySource, false);
    const bool ok = loadEntry(e);
    if (!ok) NOVA_CORE_ERROR("Shaders: {}:\n{}", e.path.filename().string(), e.error);
    return ok;
}

bool ShaderLibrary::loadEntry(Entry& e) {
    if (e.resolved == ShaderSource::Precompiled) return loadPrecompiled(e);
    auto text = fs::readText(e.path);
    if (!text) {
        e.error = "cannot read " + e.path.generic_string();
        watch(e, {e.path});
        return false;
    }
    return compileEntry(e, *text, true);
}

bool ShaderLibrary::loadPrecompiled(Entry& e) {
    const fsys::path spv = findPrecompiled(e);
    if (spv.empty()) {
        e.error = "precompiled file not found";
        return false;
    }
    watch(e, {spv});
    auto bytes = fs::readBinary(spv);
    std::vector<u32> words;
    if (!bytes || !toWords(*bytes, words)) {
        e.error = "invalid SPIR-V file " + spv.generic_string();
        return false;
    }
    e.desc.spirv = std::move(words);
    e.error.clear();
    e.fromCache = false;
    ++e.version;
    ++m_changeCounter;
    return true;
}

bool ShaderLibrary::compileEntry(Entry& e, std::string_view source, bool useCache) {
    const std::string name = e.path.generic_string();
    std::string definesKey;
    for (const std::string& d : e.options.defines) definesKey += d + ";";

    // ---- cache lookup: <key>.spv + <key>.deps ("<content hash> <file>" per dependency)
    const bool cache = useCache && !m_cacheDir.empty();
    const std::string cacheKey =
        hex(fnv1a(std::string(kCacheVersion) + "|" + name + "|" + std::to_string(static_cast<int>(e.stage)) + "|" + definesKey));
    const fsys::path spvFile = m_cacheDir / (cacheKey + ".spv");
    const fsys::path depsFile = m_cacheDir / (cacheKey + ".deps");
    const u64 sourceHash = fnv1a(source);

    if (cache) {
        if (auto deps = fs::readText(depsFile)) {
            std::istringstream in(*deps);
            std::string line;
            bool fresh = true, first = true;
            std::vector<fsys::path> files;
            while (fresh && std::getline(in, line)) {
                if (line.size() < 18) continue;
                const std::string expected = line.substr(0, 16);
                const fsys::path file = fsys::path(line.substr(17));
                files.push_back(file);
                if (first) { // the main source: compare with the text we were given
                    fresh = expected == hex(sourceHash);
                    first = false;
                } else {
                    auto text = fs::readText(file);
                    fresh = text && expected == hex(fnv1a(*text));
                }
            }
            std::vector<u32> words;
            if (fresh && !first) {
                if (auto bytes = fs::readBinary(spvFile); bytes && toWords(*bytes, words)) {
                    e.desc.spirv = std::move(words);
                    e.error.clear();
                    e.fromCache = true;
                    ++e.version;
                    ++m_changeCounter;
                    watch(e, files);
                    return true;
                }
            }
        }
    }

    CompileResult r = compileGlsl(source, name, e.stage, e.options.defines, m_includeDirs);
    std::vector<fsys::path> files;
    if (e.resolved != ShaderSource::Memory) files.push_back(e.path);
    files.insert(files.end(), r.includes.begin(), r.includes.end());
    if (e.resolved != ShaderSource::Memory) watch(e, files);
    if (!r.ok) {
        e.error = r.error;
        return false;
    }
    e.desc.spirv = std::move(r.spirv);
    e.error.clear();
    e.fromCache = false;
    ++e.version;
    ++m_changeCounter;

    if (cache) {
        std::error_code ec;
        fsys::create_directories(m_cacheDir, ec);
        std::ofstream spv(spvFile, std::ios::binary);
        spv.write(reinterpret_cast<const char*>(e.desc.spirv.data()), std::streamsize(e.desc.spirv.size() * 4));
        std::ofstream deps(depsFile, std::ios::binary);
        deps << hex(sourceHash) << ' ' << e.path.generic_string() << '\n';
        for (const fsys::path& inc : r.includes)
            if (auto text = fs::readText(inc)) deps << hex(fnv1a(*text)) << ' ' << inc.generic_string() << '\n';
    }
    return true;
}

void ShaderLibrary::watch(Entry& e, const std::vector<fsys::path>& files) {
    e.watched.clear();
    for (const fsys::path& f : files) e.watched.push_back({f, mtime(f)});
}

u32 ShaderLibrary::update() {
    if (!m_hotReload || m_entries.empty()) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (m_pollInterval.count() > 0 && now - m_lastPoll < m_pollInterval) return 0;
    m_lastPoll = now;

    u32 reloaded = 0;
    for (u32 i = 0; i < m_entries.size(); ++i) {
        Entry& e = m_entries[i];
        if (e.resolved == ShaderSource::Memory) continue;
        bool changed = false;
        for (Watched& w : e.watched) {
            const auto t = mtime(w.file);
            if (t != w.time) {
                w.time = t;
                changed = true;
            }
        }
        if (!changed) continue;
        if (loadEntry(e)) {
            ++reloaded;
            NOVA_CORE_INFO("Shaders: reloaded {} ({})", e.path.filename().string(), sourceName(e.resolved));
        } else {
            NOVA_CORE_ERROR("Shaders: {} (keeping the previous version):\n{}", e.path.filename().string(), e.error);
        }
    }
    return reloaded;
}

const rhi::ShaderDesc& ShaderLibrary::desc(ShaderHandle shader) const {
    static const rhi::ShaderDesc empty;
    return valid(shader) ? m_entries[shader.id].desc : empty;
}

const std::string& ShaderLibrary::error(ShaderHandle shader) const {
    static const std::string none = "invalid shader handle";
    return valid(shader) ? m_entries[shader.id].error : none;
}

const fsys::path& ShaderLibrary::path(ShaderHandle shader) const {
    static const fsys::path none;
    return valid(shader) ? m_entries[shader.id].path : none;
}

std::vector<ShaderHandle> ShaderLibrary::shaders() const {
    std::vector<ShaderHandle> out;
    for (u32 i = 0; i < m_entries.size(); ++i) out.push_back({i});
    return out;
}

} // namespace nova
