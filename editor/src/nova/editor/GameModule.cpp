#include "nova/editor/GameModule.h"

#include "nova/core/Log.h"
#include "nova/editor/EditorContext.h"
#include "nova/scene/SceneSerializer.h"

#include <chrono>
#include <cstdio>

#if defined(_WIN32)
    #include <process.h>
    #define NOVA_POPEN _popen
    #define NOVA_PCLOSE _pclose
    #define NOVA_GETPID _getpid
#else
    #include <unistd.h>
    #define NOVA_POPEN popen
    #define NOVA_PCLOSE pclose
    #define NOVA_GETPID getpid
#endif

namespace nova::editor {

namespace fs = std::filesystem;

namespace {
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool contains(const std::string& s, const char* word) { return s.find(word) != std::string::npos; }
} // namespace

GameModuleHost::GameModuleHost(EditorContext& ctx, Registry& registry) : m_ctx(ctx), m_registry(registry) {}

GameModuleHost::~GameModuleHost() {
    if (m_buildThread.joinable()) m_buildThread.join();
    if (isLoaded()) {
        unloadInternal();
        std::error_code ec;
        fs::remove(m_loadedCopy, ec);
    }
}

// ------------------------------------------------------------- load/unload --
fs::path GameModuleHost::shadowCopy(std::string* error) {
    std::error_code ec;
    if (!fs::exists(m_library, ec)) {
        if (error) *error = "game module not found: " + m_library.string();
        return {};
    }
    const fs::path dir = fs::temp_directory_path(ec) / "nova_modules";
    fs::create_directories(dir, ec);
    const fs::path copy = dir / (m_library.stem().string() + "-" + std::to_string(NOVA_GETPID()) + "-" +
                                 std::to_string(++m_copies) + m_library.extension().string());
    // The linker may still be writing the file: retry briefly.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (fs::copy_file(m_library, copy, fs::copy_options::overwrite_existing, ec)) return copy;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (error) *error = "cannot copy " + m_library.string() + ": " + ec.message();
    return {};
}

bool GameModuleHost::loadInternal(const fs::path& file, std::string* error) {
    std::string err;
    if (!m_lib.open(file, &err)) {
        if (error) *error = err;
        return false;
    }
    auto version = m_lib.function<int (*)()>("novaGameModuleApiVersion");
    auto create = m_lib.function<GameModule* (*)()>("novaCreateGameModule");
    if (!version || !create) {
        m_lib.close();
        if (error) *error = m_library.filename().string() + " is not a game module (NOVA_GAME_MODULE missing)";
        return false;
    }
    if (version() != kGameModuleApiVersion) {
        m_lib.close();
        if (error) *error = "game module built for API version " + std::to_string(version()) + ", editor uses " +
                            std::to_string(kGameModuleApiVersion);
        return false;
    }
    m_id = newModuleId();
    {
        ModuleScope scope(m_id); // everything registered now belongs to the module
        m_module = create();
        m_module->onLoad(m_registry);
    }
    m_registry.addReflectedEditors(); // inspectors for the module's reflected types
    return true;
}

std::string GameModuleHost::unloadInternal() {
    m_ctx.stop();
    std::string json = serializeScene(m_ctx.scene()); // ids: meshes/textures stay alive in this session
    m_ctx.clearSelection();
    m_ctx.scene().resetStorage();  // entities, scripts and ECS pools may run the module's code
    m_ctx.undoStack().clear();     // undo steps may hold the module's code too
    {
        ModuleScope scope(m_id);
        m_module->onUnload(m_ctx);
    }
    delete m_module;
    m_module = nullptr;
    const size_t removed = m_registry.removeModule(m_id) + reflect::types().removeModule(m_id);
    m_ctx.assets().removeModule(m_id);
    m_lib.close();
    NOVA_CORE_INFO("Game module unloaded ({} registrations removed)", removed);
    return json;
}

void GameModuleHost::restoreScene(const std::string& json, const std::vector<u64>& selection) {
    std::string error;
    if (!deserializeScene(m_ctx.scene(), json, {}, &error)) NOVA_CORE_ERROR("Game module: scene restore failed: {}", error);
    std::vector<Entity> selected;
    for (u64 uuid : selection)
        if (Entity e = m_ctx.scene().findEntityByUuid(uuid)) selected.push_back(e);
    m_ctx.setSelection(std::move(selected));
    m_ctx.sceneReloaded.emit();
}

bool GameModuleHost::load(const fs::path& library, std::string* error) {
    if (isLoaded()) unload();
    m_library = library;
    std::error_code ec;
    const auto time = fs::last_write_time(library, ec);
    const fs::path copy = shadowCopy(error);
    if (copy.empty()) return false;
    if (!loadInternal(copy, error)) {
        fs::remove(copy, ec);
        return false;
    }
    m_loadedCopy = copy;
    m_loadedTime = time;
    NOVA_CORE_INFO("Game module loaded: {}", library.filename().string());
    reloaded.emit();
    return true;
}

bool GameModuleHost::reload(std::string* error) {
    if (!isLoaded()) return !m_library.empty() && load(m_library, error);
    std::error_code ec;
    const auto time = fs::last_write_time(m_library, ec);
    const fs::path copy = shadowCopy(error); // before unloading: on failure nothing changes
    if (copy.empty()) return false;

    std::vector<u64> selection;
    for (Entity e : m_ctx.selection()) selection.push_back(e.uuid());
    const fs::path previous = m_loadedCopy;
    const std::string scene = unloadInternal();

    std::string err;
    const bool ok = loadInternal(copy, &err);
    if (ok) {
        fs::remove(previous, ec);
        m_loadedCopy = copy;
        m_loadedTime = time;
        ++m_reloads;
        NOVA_CORE_INFO("Game module reloaded: {} (#{})", m_library.filename().string(), m_reloads);
    } else {
        NOVA_CORE_ERROR("Game module reload failed: {} (restoring the previous code)", err);
        if (error) *error = err;
        fs::remove(copy, ec);
        std::string again;
        if (!loadInternal(previous, &again)) NOVA_CORE_ERROR("Game module: previous code failed too: {}", again);
        m_loadedTime = time; // do not retry the same broken file in a loop
    }
    restoreScene(scene, selection);
    if (ok) reloaded.emit();
    return ok;
}

void GameModuleHost::unload() {
    if (!isLoaded()) return;
    std::vector<u64> selection;
    for (Entity e : m_ctx.selection()) selection.push_back(e.uuid());
    const std::string scene = unloadInternal();
    std::error_code ec;
    fs::remove(m_loadedCopy, ec);
    m_loadedCopy.clear();
    restoreScene(scene, selection);
}

// ---------------------------------------------------------- build / watch --
void GameModuleHost::build() {
    if (m_building || buildCommand.empty()) {
        if (buildCommand.empty()) NOVA_CORE_WARN("Game module: no build command configured");
        return;
    }
    if (m_buildThread.joinable()) m_buildThread.join();
    m_building = true;
    m_buildFinished = false;
    NOVA_CORE_INFO("Building game module: {}", buildCommand);
    m_buildThread = std::thread([this, command = buildCommand] {
        int code = -1;
        // cmd.exe strips one pair of outer quotes: wrap so quoted paths inside survive.
        const std::string shell = "\"" + command + " 2>&1\"";
        if (FILE* pipe = NOVA_POPEN(shell.c_str(), "r")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), pipe)) {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                std::lock_guard lock(m_buildMutex);
                m_buildLines.push_back(std::move(text));
            }
            code = NOVA_PCLOSE(pipe);
        }
        m_buildExitCode = code;
        m_buildFinished = true;
    });
}

void GameModuleHost::update() {
    {
        std::lock_guard lock(m_buildMutex);
        for (const std::string& line : m_buildLines) {
            if (contains(line, " error ") || contains(line, ": error")) NOVA_CORE_ERROR("[build] {}", line);
            else if (contains(line, " warning ") || contains(line, ": warning")) NOVA_CORE_WARN("[build] {}", line);
            else NOVA_CORE_TRACE("[build] {}", line);
        }
        m_buildLines.clear();
    }
    if (m_buildFinished.exchange(false)) {
        if (m_buildThread.joinable()) m_buildThread.join();
        m_building = false;
        m_lastBuildOk = m_buildExitCode == 0;
        if (!m_lastBuildOk) {
            NOVA_CORE_ERROR("Game module build failed (exit code {})", m_buildExitCode);
        } else {
            std::error_code ec;
            const auto time = fs::last_write_time(m_library, ec);
            if (!isLoaded()) load(m_library);
            else if (!ec && time != m_loadedTime) reload();
            else NOVA_CORE_INFO("Game module is up to date");
        }
    }

    if (!watch || m_building || !isLoaded()) return;
    const double now = nowSeconds();
    if (now - m_lastPoll < 0.5) return;
    m_lastPoll = now;
    std::error_code ec;
    const auto time = fs::last_write_time(m_library, ec);
    if (ec || time == m_loadedTime) return;
    if (time != m_pendingTime) { // changed: wait one more poll, the linker may still be writing
        m_pendingTime = time;
        return;
    }
    reload();
}

} // namespace nova::editor
