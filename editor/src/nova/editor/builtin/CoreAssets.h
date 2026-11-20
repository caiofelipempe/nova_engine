#pragma once

/// @file
/// @brief The Assets panel (asset browser), built-in asset actions and asset commands.

#include "nova/editor/Registry.h"

#include <string>

namespace nova::editor::builtin {

/// @brief Browses EditorContext::assets(): folders, filter, drag-and-drop and per-type actions.
///
/// - Drag an asset onto the Viewport: models/prefabs are placed where the mouse ray hits the
///   scene (or the ground plane); textures are applied to the mesh under the mouse.
/// - Drag a texture onto a texture field in the Inspector to assign it.
/// - Double-click runs the asset type's first action; right-click lists all of them
///   (Registry::assetAction).
class AssetBrowserPanel final : public Panel {
public:
    AssetBrowserPanel();
    void onUpdate(EditorContext& ctx, f32 dt) override;
    void onImGui(EditorContext& ctx) override;

    /// @brief Folder shown (relative to the asset root, generic separators; "" = root).
    std::string folder;
    /// @brief Seconds between automatic rescans of the asset folder (0 = manual only).
    f32 autoRefreshInterval = 2.0f;

private:
    void drawFolderTree(EditorContext& ctx);
    void drawFiles(EditorContext& ctx);

    char m_filter[128] = "";
    AssetGuid m_selected = 0;
    f32 m_sinceRefresh = 0.0f;
};

/// @brief Registers the Assets panel, the built-in asset actions and these commands:
///
/// | Id                   | Menu                                 | Shortcut |
/// |----------------------|--------------------------------------|----------|
/// | assets.import        | File/Import Asset...                 | Ctrl+I   |
/// | assets.createPrefab  | Assets/Create Prefab from Selection  |          |
/// | assets.refresh       | Assets/Refresh                       |          |
///
/// Actions: Model/Prefab "Add to Scene", Scene "Open", Texture "Apply to Selection",
/// every type "Copy Reference".
class CoreAssetsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;

private:
    enum class Prompt { None, Import, CreatePrefab };
    void drawPrompt(EditorContext& ctx);

    AssetBrowserPanel* m_panel = nullptr;
    Prompt m_prompt = Prompt::None;
    bool m_openPrompt = false;
    char m_path[512] = "";
    char m_folder[256] = "";
    std::string m_error;
};

} // namespace nova::editor::builtin
