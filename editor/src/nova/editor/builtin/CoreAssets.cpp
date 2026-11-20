#include "nova/editor/builtin/CoreAssets.h"

#include "nova/core/Application.h"
#include "nova/core/Log.h"
#include "nova/editor/EditorContext.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace nova::editor::builtin {

namespace fs = std::filesystem;

namespace {

bool containsNoCase(const std::string& haystack, const char* needle) {
    if (!*needle) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle, needle + std::strlen(needle), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

std::string parentOf(const AssetRecord& r) { return r.path.parent_path().generic_string(); }

const char* typeTag(const std::string& type) {
    if (type == "Texture") return "[T]";
    if (type == "Model") return "[M]";
    if (type == "Prefab") return "[P]";
    if (type == "Scene") return "[S]";
    return "[?]";
}

// Every folder that contains assets, plus its ancestors ("a/b" → "a", "a/b").
std::set<std::string> folders(const AssetDatabase& db) {
    std::set<std::string> out;
    for (const AssetRecord& r : db.assets())
        for (fs::path p = r.path.parent_path(); !p.empty(); p = p.parent_path()) out.insert(p.generic_string());
    return out;
}

void runAction(EditorContext& ctx, const AssetAction& action, AssetGuid guid) {
    // Copy the record: the action may rescan the database (invalidating references into it).
    if (const AssetRecord* r = ctx.assets().find(guid)) {
        const AssetRecord copy = *r;
        action.run(ctx, copy);
    }
}

void applyTexture(EditorContext& ctx, const std::vector<Entity>& targets, rhi::TextureHandle texture) {
    std::vector<Entity> meshes;
    for (Entity e : targets)
        if (e.valid() && e.has<MeshComponent>()) meshes.push_back(e);
    if (meshes.empty()) {
        NOVA_CORE_WARN("Apply texture: no mesh entity selected");
        return;
    }
    ctx.edit("Apply texture", meshes, [&] {
        for (Entity e : meshes) e.get<MeshComponent>().material.baseColorTexture = texture;
    });
}

rhi::TextureHandle firstTexture(EditorContext& ctx, AssetGuid guid) {
    const AssetData* data = ctx.assets().load(guid);
    return data && !data->textures.empty() ? data->textures[0] : rhi::TextureHandle{};
}

} // namespace

// ------------------------------------------------------------------- panel --
AssetBrowserPanel::AssetBrowserPanel() : Panel("Assets", DockSlot::Bottom) {}

void AssetBrowserPanel::onUpdate(EditorContext& ctx, f32 dt) {
    m_sinceRefresh += dt;
    if (autoRefreshInterval > 0.0f && m_sinceRefresh >= autoRefreshInterval) {
        ctx.assets().refresh();
        m_sinceRefresh = 0.0f;
    }
}

void AssetBrowserPanel::onImGui(EditorContext& ctx) {
    AssetDatabase& db = ctx.assets();
    if (ImGui::Button("Refresh")) db.refresh();
    ImGui::SameLine();
    if (ImGui::Button("Import...")) ctx.registry().execute("assets.import");
    ImGui::SameLine();
    ImGui::BeginDisabled(ctx.selection().empty() || ctx.isPlaying());
    if (ImGui::Button("Create Prefab")) ctx.registry().execute("assets.createPrefab");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##filter", "Search all folders...", m_filter, sizeof(m_filter));
    ImGui::SameLine();
    ImGui::TextDisabled("%s  (%zu assets)", db.root().string().c_str(), db.assets().size());
    ImGui::Separator();

    if (db.root().empty()) {
        ImGui::TextDisabled("No asset folder (EditorDesc::assetsDir).");
        return;
    }
    ImGui::BeginChild("##folders", {200.0f, 0.0f}, ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    drawFolderTree(ctx);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##files");
    drawFiles(ctx);
    ImGui::EndChild();
}

void AssetBrowserPanel::drawFolderTree(EditorContext& ctx) {
    const std::set<std::string> all = folders(ctx.assets());
    std::map<std::string, std::vector<std::string>> children; // parent → child folders
    for (const std::string& f : all) children[fs::path(f).parent_path().generic_string()].push_back(f);

    auto draw = [&](auto& self, const std::string& path, const char* label) -> void {
        const auto it = children.find(path);
        const bool leaf = it == children.end();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
        if (path.empty()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (folder == path) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(path.empty() ? "##root" : path.c_str(), flags, "%s", label);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) folder = path;
        if (open) {
            if (!leaf)
                for (const std::string& c : it->second) self(self, c, fs::path(c).filename().string().c_str());
            ImGui::TreePop();
        }
    };
    draw(draw, "", "assets");
}

void AssetBrowserPanel::drawFiles(EditorContext& ctx) {
    AssetDatabase& db = ctx.assets();
    const bool searching = m_filter[0] != '\0';
    AssetGuid runFirstAction = 0;

    if (!searching) {
        if (!folder.empty() && ImGui::Selectable("[..]", false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            folder = fs::path(folder).parent_path().generic_string();
        for (const std::string& f : folders(db)) {
            if (fs::path(f).parent_path().generic_string() != folder) continue;
            const std::string label = "[/] " + fs::path(f).filename().string();
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                folder = f;
        }
    }

    for (const AssetRecord& r : db.assets()) {
        if (searching ? !containsNoCase(r.path.generic_string(), m_filter) : parentOf(r) != folder) continue;
        ImGui::PushID(static_cast<int>(r.guid ^ (r.guid >> 32)));
        const std::string label = std::string(typeTag(r.type)) + " " + (searching ? r.path.generic_string() : r.fileName());
        if (ImGui::Selectable(label.c_str(), m_selected == r.guid, ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selected = r.guid;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) runFirstAction = r.guid;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kAssetDragPayload, &r.guid, sizeof(AssetGuid));
            ImGui::Text("%s %s", typeTag(r.type), r.fileName().c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s  (%s)", r.path.generic_string().c_str(), r.type.c_str());
            ImGui::TextDisabled("%s%s", AssetDatabase::makeRef(r.guid).c_str(), db.isLoaded(r.guid) ? "  loaded" : "");
            if (r.type == "Texture")
                if (rhi::TextureHandle tex = firstTexture(ctx, r.guid); tex && ctx.app().imgui())
                    ImGui::Image(static_cast<ImTextureID>(ctx.app().imgui()->textureId(tex)), {128.0f, 128.0f});
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem()) {
            m_selected = r.guid;
            for (const AssetAction* a : ctx.registry().assetActions(r.type))
                if (ImGui::MenuItem(a->label.c_str())) {
                    runAction(ctx, *a, r.guid);
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    return; // the action may have rescanned the database
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x - 70.0f, 260.0f));
        ImGui::TextDisabled("%s", r.type.c_str());
        ImGui::PopID();
    }

    if (runFirstAction) {
        const AssetRecord* r = db.find(runFirstAction);
        const auto actions = r ? ctx.registry().assetActions(r->type) : std::vector<const AssetAction*>{};
        if (!actions.empty()) runAction(ctx, *actions.front(), runFirstAction);
    }
}

// ------------------------------------------------------------------ plugin --
void CoreAssetsPlugin::onRegister(Registry& r) {
    m_panel = &r.panel<AssetBrowserPanel>();

    auto addToScene = [](EditorContext& ctx, const AssetRecord& a) { ctx.instantiateAsset(a.guid, ctx.spawnPoint()); };
    r.assetAction("Model", "Add to Scene", addToScene);
    r.assetAction("Prefab", "Add to Scene", addToScene);
    r.assetAction("Scene", "Open", [](EditorContext& ctx, const AssetRecord& a) {
        if (ctx.isPlaying()) ctx.stop();
        std::string error;
        if (!ctx.openScene(ctx.assets().absolutePath(a), &error)) NOVA_CORE_ERROR("Open failed: {}", error);
    });
    r.assetAction("Texture", "Apply to Selection", [](EditorContext& ctx, const AssetRecord& a) {
        if (rhi::TextureHandle tex = firstTexture(ctx, a.guid)) applyTexture(ctx, ctx.selection(), tex);
    });
    r.assetAction("*", "Copy Reference", [](EditorContext&, const AssetRecord& a) {
        ImGui::SetClipboardText(AssetDatabase::makeRef(a.guid).c_str());
    });

    Command import;
    import.id = "assets.import";
    import.menuPath = "File/Import Asset...";
    import.shortcut = *KeyChord::parse("Ctrl+I");
    import.order = 10;
    import.separatorBefore = true;
    import.execute = [this](EditorContext&) {
        m_prompt = Prompt::Import;
        m_openPrompt = true;
        m_error.clear();
        m_path[0] = '\0';
        std::snprintf(m_folder, sizeof(m_folder), "%s", m_panel ? m_panel->folder.c_str() : "");
    };
    r.command(std::move(import));

    Command prefab;
    prefab.id = "assets.createPrefab";
    prefab.menuPath = "Assets/Create Prefab from Selection";
    prefab.isEnabled = [](EditorContext& ctx) { return !ctx.selection().empty() && !ctx.isPlaying(); };
    prefab.execute = [this](EditorContext& ctx) {
        m_prompt = Prompt::CreatePrefab;
        m_openPrompt = true;
        m_error.clear();
        std::snprintf(m_path, sizeof(m_path), "prefabs/%s.prefab.json", ctx.selected().name().c_str());
    };
    r.command(std::move(prefab));

    Command refresh;
    refresh.id = "assets.refresh";
    refresh.menuPath = "Assets/Refresh";
    refresh.execute = [](EditorContext& ctx) { ctx.assets().refresh(); };
    r.command(std::move(refresh));

    r.uiHook([this](EditorContext& ctx) { drawPrompt(ctx); });
}

void CoreAssetsPlugin::drawPrompt(EditorContext& ctx) {
    const char* title = m_prompt == Prompt::Import ? "Import Asset" : "Create Prefab";
    if (m_openPrompt) {
        ImGui::OpenPopup(title);
        m_openPrompt = false;
    }
    if (m_prompt == Prompt::None || !ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    bool enter = false;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (m_prompt == Prompt::Import) {
        ImGui::TextUnformatted("File to import (copied into the asset folder with the files it references):");
        ImGui::SetNextItemWidth(520.0f);
        enter |= ImGui::InputText("##path", m_path, sizeof(m_path), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextUnformatted("Destination folder (relative to the asset folder):");
        ImGui::SetNextItemWidth(520.0f);
        enter |= ImGui::InputText("##folder", m_folder, sizeof(m_folder), ImGuiInputTextFlags_EnterReturnsTrue);
    } else {
        ImGui::Text("Prefab file for %zu selected entities (relative to the asset folder):", ctx.selection().size());
        ImGui::SetNextItemWidth(520.0f);
        enter |= ImGui::InputText("##path", m_path, sizeof(m_path), ImGuiInputTextFlags_EnterReturnsTrue);
    }
    if (!m_error.empty()) ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_error.c_str());

    if (ImGui::Button(m_prompt == Prompt::Import ? "Import" : "Create", {120, 0}) || enter) {
        std::string error;
        std::optional<AssetGuid> guid;
        if (m_prompt == Prompt::Import) {
            std::string path = m_path; // tolerate quotes from "Copy as path"
            std::erase(path, '"');
            guid = ctx.assets().import(path, m_folder, &error);
            if (guid && m_panel) {
                if (const AssetRecord* rec = ctx.assets().find(*guid)) m_panel->folder = parentOf(*rec);
            }
        } else {
            guid = ctx.assets().createPrefab(ctx.selection(), m_path, &error);
            if (guid) NOVA_CORE_INFO("Prefab saved: {}", ctx.assets().find(*guid)->path.generic_string());
        }
        if (guid) {
            m_prompt = Prompt::None;
            ImGui::CloseCurrentPopup();
        } else {
            m_error = error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_prompt = Prompt::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace nova::editor::builtin
