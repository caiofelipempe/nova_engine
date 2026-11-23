#include "nova/editor/EditorContext.h"

#include "nova/core/Application.h"
#include "nova/core/Log.h"
#include "nova/editor/Tool.h"

#include <algorithm>

namespace nova::editor {

EditorContext::EditorContext(Scene& scene) : m_scene(&scene) {
    // Any history movement (undo, redo, History panel jumps) changes entities behind the tracker's back.
    m_undo.changed.connect([this] { m_trackValid = false; });
    // Model animations saved by reference play again after loading (and after Stop).
    m_scene->setAnimationSource(m_assets.animationSource());
}

EditorContext::~EditorContext() {
    if (m_scene) m_scene->setAnimationSource({}); // it pointed at this context's assets
}

Application& EditorContext::app() { return Application::get(); }
Renderer& EditorContext::renderer() { return Application::get().renderer(); }

// --------------------------------------------------------------- selection --
bool EditorContext::isSelected(Entity entity) const {
    return std::find(m_selection.begin(), m_selection.end(), entity) != m_selection.end();
}

void EditorContext::setSelection(std::vector<Entity> entities) {
    std::vector<Entity> clean;
    for (Entity e : entities) {
        if (!e.valid()) continue;
        auto dup = std::find(clean.begin(), clean.end(), e);
        if (dup != clean.end()) clean.erase(dup); // re-adding moves it to primary
        clean.push_back(e);
    }
    if (clean == m_selection) return;
    m_selection = std::move(clean);
    selectionChanged.emit(selected());
}

void EditorContext::select(Entity entity) { setSelection(entity.valid() ? std::vector<Entity>{entity} : std::vector<Entity>{}); }

void EditorContext::addToSelection(Entity entity) {
    auto s = m_selection;
    s.push_back(entity);
    setSelection(std::move(s));
}

void EditorContext::removeFromSelection(Entity entity) {
    auto s = m_selection;
    std::erase(s, entity);
    setSelection(std::move(s));
}

void EditorContext::toggleSelection(Entity entity) {
    if (isSelected(entity)) removeFromSelection(entity);
    else addToSelection(entity);
}

void EditorContext::validateSelection() {
    if (std::all_of(m_selection.begin(), m_selection.end(), [](Entity e) { return e.valid(); })) return;
    auto s = m_selection;
    std::erase_if(s, [](Entity e) { return !e.valid(); });
    m_selection.clear(); // force a change notification even if the remainder looks equal
    setSelection(std::move(s));
    if (m_selection.empty()) selectionChanged.emit({});
}

// ---------------------------------------------------------------- snapshots --
EditorContext::Snapshot EditorContext::snapshot(const std::vector<Entity>& entities) const {
    Snapshot s;
    s.reserve(entities.size());
    for (Entity e : entities)
        if (e.valid()) s.emplace_back(e.uuid(), serializeEntity(e)); // mesh ids: exact within the session
    return s;
}

void EditorContext::restore(const Snapshot& s) {
    for (const auto& [uuid, json] : s) restoreEntity(*m_scene, json);
}

void EditorContext::destroyByUuid(const std::vector<u64>& uuids) {
    for (u64 id : uuids) {
        Entity e = m_scene->findEntityByUuid(id);
        if (!e.valid()) continue;
        removeFromSelection(e);
        m_scene->destroyEntity(e);
    }
}

std::unordered_set<u64> EditorContext::allUuids() const {
    std::unordered_set<u64> ids;
    for (auto [e, id] : m_scene->registry().view<IdComponent>().each()) ids.insert(id.uuid);
    return ids;
}

void EditorContext::pushRecorded(UndoAction action) {
    m_trackValid = false; // the tracker must not record this change a second time
    if (isPlaying() || m_undo.isApplying()) return;
    m_undo.push(std::move(action));
}

// --------------------------------------------------------------- undo/redo --
void EditorContext::undo() {
    if (isPlaying()) return;
    m_undo.undo();
    m_trackValid = false;
    validateSelection();
}

void EditorContext::redo() {
    if (isPlaying()) return;
    m_undo.redo();
    m_trackValid = false;
    validateSelection();
}

namespace {
std::string nameFor(Scene& scene, const std::string& verb, const std::vector<std::pair<u64, std::string>>& snap) {
    if (snap.size() == 1) {
        Entity e = scene.findEntityByUuid(snap[0].first);
        if (e.valid()) return verb + " " + e.name();
    }
    return verb + " " + std::to_string(snap.size()) + " entities";
}
} // namespace

EditTransaction EditorContext::beginEdit(std::string name, const std::vector<Entity>& entities) {
    EditTransaction tx;
    tx.m_ctx = this;
    tx.m_name = std::move(name);
    tx.m_before = snapshot(entities);
    return tx;
}

bool EditTransaction::commit() {
    if (!m_ctx) return false;
    EditorContext& ctx = *std::exchange(m_ctx, nullptr);
    std::vector<Entity> entities;
    for (auto& [uuid, json] : m_before) entities.push_back(ctx.m_scene->findEntityByUuid(uuid));
    auto after = ctx.snapshot(entities);
    ctx.m_trackValid = false;
    if (after == m_before) return false;
    auto before = std::move(m_before);
    ctx.pushRecorded({m_name, [&ctx, before] { ctx.restore(before); }, [&ctx, after] { ctx.restore(after); }});
    return true;
}

void EditorContext::edit(std::string name, const std::vector<Entity>& entities, const std::function<void()>& change) {
    EditTransaction tx = beginEdit(std::move(name), entities);
    change();
    tx.commit();
}

CreationTransaction EditorContext::beginCreation(std::string name) {
    CreationTransaction tx;
    tx.m_ctx = this;
    tx.m_name = std::move(name);
    tx.m_before = allUuids();
    return tx;
}

std::vector<Entity> CreationTransaction::commit() {
    if (!m_ctx) return {};
    EditorContext& ctx = *std::exchange(m_ctx, nullptr);
    std::vector<Entity> created;
    std::vector<u64> ids;
    for (auto [e, id] : ctx.m_scene->registry().view<IdComponent>().each())
        if (!m_before.contains(id.uuid)) {
            created.push_back({e, ctx.m_scene});
            ids.push_back(id.uuid);
        }
    ctx.m_trackValid = false;
    if (created.empty()) return created;
    std::reverse(created.begin(), created.end()); // creation order
    auto snap = ctx.snapshot(created);
    ctx.pushRecorded({m_name, [&ctx, ids] { ctx.destroyByUuid(ids); }, [&ctx, snap] { ctx.restore(snap); }});
    return created;
}

std::vector<Entity> EditorContext::recordCreation(std::string name, const std::function<void()>& create) {
    CreationTransaction tx = beginCreation(std::move(name));
    create();
    return tx.commit();
}

void EditorContext::destroyEntities(std::string name, const std::vector<Entity>& entities) {
    // Their descendants go with them: in the snapshot too, so undo brings the whole tree back.
    std::vector<Entity> all;
    for (Entity e : entities) {
        if (!e.valid() || std::find(all.begin(), all.end(), e) != all.end()) continue;
        all.push_back(e);
        for (Entity d : m_scene->descendantsOf(e))
            if (std::find(all.begin(), all.end(), d) == all.end()) all.push_back(d);
    }
    auto snap = snapshot(all);
    if (snap.empty()) return;
    std::vector<u64> ids;
    for (auto& [uuid, json] : snap) ids.push_back(uuid);
    destroyByUuid(ids);
    pushRecorded({std::move(name), [this, snap] { restore(snap); }, [this, ids] { destroyByUuid(ids); }});
}

std::vector<Entity> EditorContext::selectionRoots() const {
    std::vector<Entity> roots;
    for (Entity e : m_selection) {
        if (!e.valid()) continue;
        bool ancestorSelected = false;
        int guard = 0;
        for (Entity up = m_scene->parentOf(e); up.valid() && !ancestorSelected && guard++ < 1024; up = m_scene->parentOf(up))
            ancestorSelected = isSelected(up);
        if (!ancestorSelected) roots.push_back(e);
    }
    return roots;
}

void EditorContext::updateEditTracking(bool interacting) {
    if (isPlaying() || m_undo.isApplying()) {
        m_trackValid = false;
        return;
    }
    if (interacting) return; // wait: a drag or a held widget becomes a single step
    Snapshot current = snapshot(m_selection);
    const bool sameEntities =
        current.size() == m_trackBaseline.size() &&
        std::equal(current.begin(), current.end(), m_trackBaseline.begin(),
                   [](const auto& a, const auto& b) { return a.first == b.first; });
    if (!m_trackValid || !sameEntities) {
        m_trackBaseline = std::move(current);
        m_trackValid = true;
        return;
    }
    if (current == m_trackBaseline) return;
    Snapshot before = std::move(m_trackBaseline);
    m_trackBaseline = current;
    m_undo.push({nameFor(*m_scene, "Edit", current), [this, before] { restore(before); },
                 [this, after = std::move(current)] { restore(after); }});
}

// ---------------------------------------------------------------- play mode --
void EditorContext::play() {
    if (m_playState == PlayState::Play) return;
    if (m_playState == PlayState::Edit) {
        m_snapshot = serializeScene(*m_scene); // mesh ids: exact within this session
        m_scene->startSimulation();
    }
    m_playState = PlayState::Play;
    m_trackValid = false;
    playStateChanged.emit(m_playState);
}

void EditorContext::pause() {
    if (m_playState != PlayState::Play) return;
    m_playState = PlayState::Pause;
    playStateChanged.emit(m_playState);
}

void EditorContext::stop() {
    if (m_playState == PlayState::Edit) return;
    std::vector<u64> selectedIds;
    for (Entity e : m_selection)
        if (e.valid()) selectedIds.push_back(e.uuid());
    m_scene->stopSimulation();
    std::string error;
    if (!deserializeScene(*m_scene, m_snapshot, {}, &error)) NOVA_CORE_ERROR("Editor: failed to restore scene: {}", error);
    m_snapshot.clear();
    m_playState = PlayState::Edit;
    m_trackValid = false;

    // Handles changed: re-select by UUID.
    std::vector<Entity> reselect;
    for (u64 id : selectedIds)
        if (Entity e = m_scene->findEntityByUuid(id); e.valid()) reselect.push_back(e);
    m_selection.clear();
    setSelection(std::move(reselect));
    if (m_selection.empty() && !selectedIds.empty()) selectionChanged.emit({});
    sceneReloaded.emit();
    playStateChanged.emit(m_playState);
}

// ------------------------------------------------------------------- files --
void EditorContext::newScene() {
    stop();
    clearSelection();
    m_scene->clear();
    m_scene->setName("Untitled");
    m_scenePath.clear();
    m_undo.clear();
    m_trackValid = false;
    sceneReloaded.emit();
}

bool EditorContext::openScene(const std::filesystem::path& path, std::string* error) {
    stop();
    clearSelection();
    const bool ok = loadScene(*m_scene, path, m_assets.serializeOptions(), error);
    if (ok) m_scenePath = path;
    m_undo.clear();
    m_trackValid = false;
    sceneReloaded.emit();
    return ok;
}

bool EditorContext::saveScene(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (!nova::saveScene(*m_scene, path, m_assets.serializeOptions(), error)) return false;
    m_scenePath = path;
    m_scene->setName(sceneNameFromPath(path));
    m_assets.refresh(); // the scene may be inside the asset folder
    return true;
}

// ------------------------------------------------------------------ assets --
std::vector<Entity> EditorContext::instantiateAsset(AssetGuid guid, const GlobalPosition& position,
                                                    const std::function<void(std::vector<Entity>&)>& adjust) {
    const AssetRecord* record = m_assets.find(guid);
    if (!record || !m_assets.canInstantiate(*record)) return {};
    std::vector<Entity> created;
    auto create = [&] {
        created = m_assets.instantiate(guid, *m_scene, position);
        if (adjust && !created.empty()) adjust(created);
    };
    if (isPlaying()) create(); // undone by Stop
    else recordCreation("Add " + record->fileName(), create);
    if (!created.empty()) setSelection(created);
    return created;
}

GlobalPosition EditorContext::spawnPoint() const {
    GlobalPosition p = m_camera.position + m_camera.forward() * 6.0f;
    const f64 y = p.toMeters().y;
    if (y < 0.5) p += GlobalOffset::fromMeters(glm::dvec3(0.0, 0.5 - y, 0.0)); // not below the ground
    return p;
}

// ------------------------------------------------------------------- tools --
void EditorContext::setActiveTool(Tool* tool) {
    if (tool == m_activeTool) return;
    if (m_activeTool) m_activeTool->onDeactivate(*this);
    m_activeTool = tool;
    if (m_activeTool) m_activeTool->onActivate(*this);
}

} // namespace nova::editor
