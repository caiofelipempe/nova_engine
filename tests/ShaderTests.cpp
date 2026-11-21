// Headless tests of game shaders: runtime compilation, includes, disk cache, hot reload,
// precompiled SPIR-V lookup and scene references to shaders. No GPU needed.
#include <nova/Nova.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

using namespace nova;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Rewrites a file and moves its timestamp forward so polling notices even on coarse clocks.
void editFile(const fs::path& p, const std::string& text) {
    const auto before = fs::last_write_time(p);
    writeFile(p, text);
    fs::last_write_time(p, before + std::chrono::seconds(2));
}

fs::path tempRoot(const char* name) {
    fs::path p = fs::temp_directory_path() / "nova_shader_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

bool isSpirv(const rhi::ShaderDesc& d) { return d.spirv.size() > 5 && d.spirv[0] == 0x07230203u; }

const char* kSimpleFrag = R"(#version 460
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0); }
)";

const char* kMaterialFrag = R"(#version 460
#include "nova/material.glsl"
void material(inout Surface s) { s.albedo *= s.custom.rgb; s.emissive += vec3(nova_time()); }
)";

const char* kPostFrag = R"(#version 460
#include "nova/postprocess.glsl"
vec3 postProcess(vec2 uv) { return nova_scene(uv) * nova_params().x; }
)";

// ------------------------------------------------------------------- tests --
void stages() {
    ShaderStage s{};
    CHECK(ShaderLibrary::stageFromPath("water.frag", s) && s == ShaderStage::Fragment);
    CHECK(ShaderLibrary::stageFromPath("a/b/blur.comp.glsl", s) && s == ShaderStage::Compute);
    CHECK(ShaderLibrary::stageFromPath("sky.VERT.spv", s) && s == ShaderStage::Vertex);
    CHECK(ShaderLibrary::stageFromPath("water.frag.rq.spv", s) && s == ShaderStage::Fragment);
    CHECK(ShaderLibrary::stageFromPath("shadow.rchit", s) && s == ShaderStage::ClosestHit);
    CHECK(!ShaderLibrary::stageFromPath("common.glsl", s));
}

void memoryCompile() {
    ShaderLibrary lib;
    ShaderHandle h = lib.compile("simple.frag", kSimpleFrag, ShaderStage::Fragment);
    CHECK(lib.valid(h) && lib.ready(h));
    CHECK(isSpirv(lib.desc(h)));
    CHECK(lib.version(h) == 1 && lib.error(h).empty());
    CHECK(lib.source(h) == ShaderSource::Memory);

    // A broken shader keeps a handle and reports the error; recompile() fixes it in place.
    ShaderHandle bad = lib.compile("bad.frag", "#version 460\nvoid main() { undefined_call(); }\n", ShaderStage::Fragment);
    CHECK(lib.valid(bad) && !lib.ready(bad));
    CHECK(lib.error(bad).find("undefined_call") != std::string::npos);
    CHECK(lib.recompile(bad, kSimpleFrag));
    CHECK(lib.ready(bad) && lib.error(bad).empty() && lib.version(bad) == 1);

    // Defines reach the preprocessor.
    ShaderHandle d = lib.compile("def.frag", "#version 460\n#ifndef FOO\n#error FOO missing\n#endif\nvoid main(){}\n",
                                 ShaderStage::Fragment, {"FOO=2"});
    CHECK(lib.ready(d));
}

void engineIncludes() {
    // nova/*.glsl are deployed to <exe>/shaders/include by the build (nova_copy_shaders).
    ShaderLibrary lib;
    ShaderHandle m = lib.compile("mat.frag", kMaterialFrag, ShaderStage::Fragment);
    CHECK(lib.ready(m));
    if (!lib.ready(m)) std::printf("  %s\n", lib.error(m).c_str());
    // The renderer's ray-traced-shadow variant compiles from the same source.
    ShaderHandle rq = lib.variant(m, {"NOVA_RAY_QUERY=1"}, "rq");
    CHECK(rq && rq != m && lib.ready(rq));
    CHECK(lib.desc(rq).spirv != lib.desc(m).spirv);
    CHECK(lib.variant(m, {"NOVA_RAY_QUERY=1"}, "rq") == rq); // cached

    ShaderHandle post = lib.compile("post.frag", kPostFrag, ShaderStage::Fragment);
    CHECK(lib.ready(post));
    ShaderHandle comp = lib.compile("fill.comp", R"(#version 460
#include "nova/common.glsl"
layout(local_size_x = 64) in;
layout(buffer_reference, std430) buffer Out { float v[]; };
layout(push_constant) uniform P { Out out_; } pc;
void main() { pc.out_.v[gl_GlobalInvocationID.x] = 1.0; }
)", ShaderStage::Compute);
    CHECK(lib.ready(comp));
}

void filesCacheAndIncludes() {
    const fs::path root = tempRoot("cache");
    const fs::path cache = root / "cache";
    writeFile(root / "src/lib/tint.glsl", "vec3 tint() { return vec3(0.5); }\n");
    writeFile(root / "src/a.frag", R"(#version 460
#include "lib/tint.glsl"
layout(location = 0) out vec4 o;
void main() { o = vec4(tint(), 1.0); }
)");

    {
        ShaderLibrary lib;
        lib.setCacheDir(cache);
        ShaderHandle h = lib.load(root / "src/a.frag");
        CHECK(lib.ready(h) && lib.source(h) == ShaderSource::Runtime && !lib.fromCache(h));
        CHECK(lib.load(root / "src/a.frag") == h); // same file + options → same handle
        CHECK(lib.load(root / "src/a.frag", {.defines = {"X"}}) != h);
        CHECK(!lib.load(root / "src/missing.frag")); // nothing to load at all
    }
    {
        ShaderLibrary lib; // a new session: served from the disk cache
        lib.setCacheDir(cache);
        ShaderHandle h = lib.load(root / "src/a.frag");
        CHECK(lib.ready(h) && lib.fromCache(h));
    }
    writeFile(root / "src/lib/tint.glsl", "vec3 tint() { return vec3(0.25); }\n");
    {
        ShaderLibrary lib; // an include changed: the cache entry is stale
        lib.setCacheDir(cache);
        ShaderHandle h = lib.load(root / "src/a.frag");
        CHECK(lib.ready(h) && !lib.fromCache(h));
    }
}

void hotReload() {
    const fs::path root = tempRoot("reload");
    const fs::path file = root / "live.frag";
    writeFile(root / "inc.glsl", "const float K = 1.0;\n");
    writeFile(file, "#version 460\n#include \"inc.glsl\"\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(K); }\n");

    ShaderLibrary lib;
    lib.setCacheDir({});
    lib.setPollInterval(std::chrono::milliseconds(0));
    ShaderHandle h = lib.load(file);
    CHECK(lib.ready(h) && lib.version(h) == 1);
    CHECK(lib.update() == 0); // nothing changed

    editFile(file, "#version 460\n#include \"inc.glsl\"\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(K * 2.0); }\n");
    CHECK(lib.update() == 1);
    CHECK(lib.version(h) == 2);

    // Changing an include reloads too.
    editFile(root / "inc.glsl", "const float K = 3.0;\n");
    CHECK(lib.update() == 1 && lib.version(h) == 3);

    // A broken edit keeps the last good SPIR-V and reports the error.
    const std::vector<u32> good = lib.desc(h).spirv;
    editFile(file, "#version 460\nvoid main(){ syntax error }\n");
    CHECK(lib.update() == 0);
    CHECK(lib.ready(h) && lib.version(h) == 3 && !lib.error(h).empty());
    CHECK(lib.desc(h).spirv == good);

    editFile(file, kSimpleFrag);
    CHECK(lib.update() == 1 && lib.version(h) == 4 && lib.error(h).empty());

    lib.setHotReload(false);
    editFile(file, "#version 460\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.5); }\n");
    CHECK(lib.update() == 0 && lib.version(h) == 4);
    CHECK(lib.reload(h) && lib.version(h) == 5); // explicit reloads still work

    // A first compile that fails still yields a handle: fixing the file brings it to life.
    lib.setHotReload(true);
    const fs::path broken = root / "broken.frag";
    writeFile(broken, "#version 460\nvoid main(){ nope }\n");
    ShaderHandle b = lib.load(broken);
    CHECK(lib.valid(b) && !lib.ready(b));
    editFile(broken, kSimpleFrag);
    CHECK(lib.update() == 1 && lib.ready(b));
}

void precompiled() {
    const fs::path root = tempRoot("precompiled");
    const fs::path spvDir = root / "bin/shaders";
    // Produce .spv files the way the build would (compiled once, written to disk).
    {
        ShaderLibrary compiler;
        ShaderHandle h = compiler.compile("p.frag", kSimpleFrag, ShaderStage::Fragment);
        const auto& words = compiler.desc(h).spirv;
        fs::create_directories(spvDir);
        std::ofstream(spvDir / "p.frag.spv", std::ios::binary)
            .write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
    }
    writeFile(spvDir / "junk.frag.spv", "not spir-v at all, definitely");

    ShaderLibrary lib;
    lib.setCacheDir({});
    lib.setPollInterval(std::chrono::milliseconds(0));
    lib.addPrecompiledDir(spvDir);

    // Found by file name wherever the source would be; no source file exists at all.
    ShaderHandle h = lib.load("shaders/p.frag", {.source = ShaderSource::Precompiled});
    CHECK(lib.ready(h) && lib.source(h) == ShaderSource::Precompiled && isSpirv(lib.desc(h)));
    // Auto falls back to precompiled when the source is missing.
    ShaderHandle a = lib.load(root / "nowhere/p.frag");
    CHECK(lib.ready(a) && lib.source(a) == ShaderSource::Precompiled);
    // Loading the .spv directly works too.
    ShaderHandle direct = lib.load(spvDir / "p.frag.spv");
    CHECK(lib.ready(direct) && lib.source(direct) == ShaderSource::Precompiled);
    // No "rq" variant was built: the variant is reported missing (the renderer falls back).
    CHECK(!lib.variant(h, {"NOVA_RAY_QUERY=1"}, "rq"));
    // Invalid files are rejected.
    ShaderHandle junk = lib.load("junk.frag", {.source = ShaderSource::Precompiled});
    CHECK(lib.valid(junk) && !lib.ready(junk));
    CHECK(!lib.load("absent.frag", {.source = ShaderSource::Precompiled}));

    // A rebuilt .spv is hot reloaded.
    const auto words = lib.desc(h).spirv;
    {
        const auto before = fs::last_write_time(spvDir / "p.frag.spv");
        std::ofstream(spvDir / "p.frag.spv", std::ios::binary)
            .write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
        fs::last_write_time(spvDir / "p.frag.spv", before + std::chrono::seconds(2));
    }
    CHECK(lib.update() >= 1 && lib.version(h) == 2);
}

void sceneReferences() {
    // Material::shader is reflected: scenes store it through SerializeOptions (asset refs in files).
    Scene scene;
    Entity e = scene.createEntity("Holo");
    auto& mesh = e.add<MeshComponent>();
    mesh.material.shader = ShaderHandle{7};
    mesh.material.customParams = {1.0f, 2.0f, 3.0f, 4.0f};

    SerializeOptions opt;
    opt.shaderName = [](ShaderHandle s) { return s.id == 7 ? std::string("asset:00000000000000aa#0") : std::string{}; };
    opt.findShader = [](const std::string& ref) { return ref == "asset:00000000000000aa#0" ? ShaderHandle{42} : ShaderHandle{}; };
    const std::string json = serializeScene(scene, opt);
    CHECK(json.find("asset:00000000000000aa#0") != std::string::npos);

    Scene loaded;
    CHECK(deserializeScene(loaded, json, opt));
    bool found = false;
    loaded.registry().view<MeshComponent>().each([&](auto, MeshComponent& m) {
        found = m.material.shader == ShaderHandle{42} && m.material.customParams == glm::vec4(1, 2, 3, 4);
    });
    CHECK(found);

    // Without options the id is written (exact within a session, e.g. editor Play/Stop).
    Scene copy;
    CHECK(deserializeScene(copy, serializeScene(scene), {}));
    copy.registry().view<MeshComponent>().each([&](auto, MeshComponent& m) { CHECK(m.material.shader == ShaderHandle{7}); });
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::off); // the tests trigger expected errors
    std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"stage from file name", stages},
        {"scene references", sceneReferences},
    };
    if (ShaderLibrary::compilerAvailable()) {
        tests.insert(tests.end(), {
            {"compile from memory", memoryCompile},
            {"engine includes (material, post-process, compute)", engineIncludes},
            {"files, includes and disk cache", filesCacheAndIncludes},
            {"hot reload", hotReload},
            {"precompiled SPIR-V", precompiled},
        });
    } else {
        std::printf("(runtime compiler disabled: compile tests skipped)\n");
    }
    for (auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    fs::remove_all(fs::temp_directory_path() / "nova_shader_tests");
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
