#pragma once

/// @file
/// @brief The editor's extension registry: panels, commands, component editors, plugins and UI hooks.

#include "nova/assets/AssetDatabase.h"
#include "nova/core/Log.h"
#include "nova/editor/Command.h"
#include "nova/editor/Panel.h"
#include "nova/editor/Tool.h"
#include "nova/reflect/Reflect.h"
#include "nova/scene/Entity.h"

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace nova::editor {

class EditorContext;
class Registry;

/// @brief Inspector support for one component type: how to detect, add, remove and draw it.
///
/// Usually created with Registry::component(). Every field except @ref name and @ref has is optional.
struct ComponentEditor {
    std::string name;                                 ///< Section title and "Add Component" label.
    std::function<bool(Entity)> has;                  ///< Whether the entity has the component (required).
    std::function<void(Entity)> add;                  ///< Adds it ("Add Component"); null = not addable.
    std::function<void(Entity)> remove;               ///< Removes it (the section's × button); null = not removable.
    std::function<bool(Entity)> canAdd;               ///< Extra condition to offer it in "Add Component".
    std::function<void(Entity, EditorContext&)> draw; ///< Draws the section's widgets.
    bool defaultOpen = true;                          ///< Section starts expanded.
};

/// @brief Something the Assets panel can do with an asset (context menu; the first one runs on double-click).
struct AssetAction {
    std::string type;  ///< Asset type it applies to ("Texture", "Model"... or "*" for every type).
    std::string label; ///< Menu label.
    std::function<void(EditorContext&, const AssetRecord&)> run; ///< The action.
};

/// @brief Custom inspector widget for a reflected field.
/// @return True if the value changed.
using FieldDrawer = std::function<bool(void* field, const reflect::FieldInfo& info, EditorContext& ctx)>;

/// @brief A bundle of registrations (panels, commands, component editors...) enabled as a unit.
///
/// The editor's own windows are plugins too; a game can split its tooling the same way.
/// @code
/// struct NavmeshPlugin : nova::editor::Plugin {
///     void onRegister(nova::editor::Registry& r) override {
///         r.panel<NavmeshPanel>();
///         r.command("Tools/Bake Navmesh", &bake, "Ctrl+B");
///     }
/// };
/// registry.plugin<NavmeshPlugin>();
/// @endcode
class Plugin {
public:
    virtual ~Plugin() = default;
    /// @brief Registers the plugin's extensions.
    virtual void onRegister(Registry& registry) = 0;
};

/// @brief Everything the editor is made of. Games add, replace or remove entries here.
///
/// Available in EditorApp::onSetup() and later through EditorContext::registry().
class Registry {
public:
    /// @param context Context passed to panels and commands.
    explicit Registry(EditorContext& context) : m_context(context) {}
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /// @name Plugins
    /// @{
    /// @brief Creates a plugin and calls its Plugin::onRegister immediately.
    /// @return The plugin, owned by the registry.
    template <typename T, typename... Args>
    T& plugin(Args&&... args) {
        static_assert(std::is_base_of_v<Plugin, T>, "T must derive from nova::editor::Plugin");
        auto p = makeScope<T>(std::forward<Args>(args)...);
        T& ref = *p;
        m_plugins.push_back(std::move(p));
        ref.onRegister(*this);
        return ref;
    }
    /// @}

    /// @name Panels
    /// @{
    /// @brief Creates and registers a panel. A panel with the same title is replaced.
    /// @return The panel, owned by the registry.
    template <typename T, typename... Args>
    T& panel(Args&&... args) {
        static_assert(std::is_base_of_v<Panel, T>, "T must derive from nova::editor::Panel");
        auto p = makeScope<T>(std::forward<Args>(args)...);
        T& ref = *p;
        addPanel(std::move(p));
        return ref;
    }
    /// @brief Removes (and detaches) the panel with this title. @return False if not found.
    bool removePanel(const std::string& title);
    /// @brief The panel with this title, or `nullptr`.
    Panel* findPanel(const std::string& title);
    /// @brief The first panel of type `T`, or `nullptr`.
    template <typename T>
    T* findPanel() {
        for (auto& p : m_panels)
            if (auto* t = dynamic_cast<T*>(p.get())) return t;
        return nullptr;
    }
    /// @brief Panels in registration order.
    const std::vector<Scope<Panel>>& panels() const { return m_panels; }
    /// @}

    /// @name Viewport tools
    /// @{
    /// @brief Creates and registers a viewport tool (a tool with the same name is replaced).
    ///
    /// Also registers the command `View/Tools/<name>` (with the tool's shortcut) that activates it.
    /// The first registered tool is active when the editor starts.
    /// @return The tool, owned by the registry.
    template <typename T, typename... Args>
    T& tool(Args&&... args) {
        static_assert(std::is_base_of_v<Tool, T>, "T must derive from nova::editor::Tool");
        auto t = makeScope<T>(std::forward<Args>(args)...);
        T& ref = *t;
        addTool(std::move(t));
        return ref;
    }
    /// @brief The tool with this name, or `nullptr`.
    Tool* findTool(const std::string& name);
    /// @brief Tools in registration order.
    const std::vector<Scope<Tool>>& tools() const { return m_tools; }
    /// @}

    /// @name Commands
    /// @{
    /// @brief Registers a command (replacing one with the same id). @return The stored command.
    Command& command(Command command);
    /// @brief Shorthand: id = `menuPath`, optional shortcut text (see KeyChord::parse).
    /// @code
    /// registry.command("Tools/Drop Ball", [](EditorContext& ctx) { ... }, "Ctrl+B");
    /// @endcode
    Command& command(const std::string& menuPath, std::function<void(EditorContext&)> execute,
                     std::string_view shortcut = {});
    /// @brief Removes a command. @return False if not found.
    bool removeCommand(const std::string& id);
    /// @brief The command with this id, or `nullptr`.
    Command* findCommand(const std::string& id);
    /// @brief Runs a command if it exists and is enabled. @return True if it ran.
    bool execute(const std::string& id);
    /// @brief Commands in registration order.
    const std::deque<Command>& commands() const { return m_commands; }
    /// @}

    /// @name Inspector
    /// @{
    /// @brief Registers inspector support for component `T` (replacing an editor with the same name).
    ///
    /// `draw` may take `(T&)`, `(T&, Entity)` or `(T&, Entity, EditorContext&)`. Adding uses
    /// `entity.add<T>()` and removing `entity.remove<T>()`; adjust the returned entry to customize.
    /// @code
    /// registry.component<Health>("Health", [](Health& h) {
    ///     ImGui::DragFloat("Current", &h.current, 1.0f, 0.0f, h.max);
    /// });
    /// @endcode
    template <typename T, typename Draw>
    ComponentEditor& component(std::string name, Draw draw) {
        ComponentEditor ed;
        ed.name = std::move(name);
        ed.has = [](Entity e) { return e.has<T>(); };
        if constexpr (std::is_default_constructible_v<T>) ed.add = [](Entity e) { e.add<T>(); };
        ed.remove = [](Entity e) { e.remove<T>(); };
        ed.draw = [d = std::move(draw)](Entity e, EditorContext& ctx) {
            T& c = e.get<T>();
            if constexpr (std::is_invocable_v<Draw&, T&, Entity, EditorContext&>) d(c, e, ctx);
            else if constexpr (std::is_invocable_v<Draw&, T&, Entity>) d(c, e);
            else d(c);
        };
        return addComponentEditor(std::move(ed));
    }
    /// @brief Registers an inspector generated from `T`'s reflection (see reflect::type).
    ///
    /// Every reflected component is registered automatically at startup; call this to control
    /// the order or to tweak the generated entry:
    /// @code
    /// r.component<Health>().canAdd = [](nova::Entity e) { return e.has<Enemy>(); };
    /// @endcode
    template <typename T>
    ComponentEditor& component() {
        const reflect::TypeInfo* type = reflect::types().find<T>();
        NOVA_VERIFY(type && type->isComponent, "Registry::component<T>(): T is not a reflected component");
        return addComponentEditor(reflectedComponentEditor(*type));
    }

    /// @brief Inspector entry generated from a reflected component type.
    static ComponentEditor reflectedComponentEditor(const reflect::TypeInfo& type);
    /// @brief Inspector entry for a reflected script type ("Script: Name"): add/remove/edit fields.
    static ComponentEditor reflectedScriptEditor(const reflect::TypeInfo& type);
    /// @brief Registers inspector entries for every reflected component/script not registered yet
    ///        (by name). Called by the editor after EditorApp::onSetup.
    void addReflectedEditors();

    /// @brief Registers a fully custom component editor (replacing one with the same name).
    ComponentEditor& addComponentEditor(ComponentEditor editor);
    /// @brief Removes a component editor by name. @return False if not found.
    bool removeComponentEditor(const std::string& name);
    /// @brief Component editors in inspector order.
    const std::deque<ComponentEditor>& componentEditors() const { return m_componentEditors; }
    /// @}

    /// @name Field drawers (custom widgets inside generated inspectors)
    /// @{
    /// @brief Custom widget for every reflected field of C++ type `F`.
    ///
    /// `draw` is `bool(F& value, const reflect::FieldInfo& info, EditorContext& ctx)` and returns true on change.
    /// @code
    /// r.fieldDrawer<Range>([](Range& v, const auto& f, auto&) {
    ///     return ImGui::DragFloatRange2(f.label.c_str(), &v.min, &v.max);
    /// });
    /// @endcode
    template <typename F, typename Draw>
    void fieldDrawer(Draw draw) {
        m_typeDrawers[std::type_index(typeid(F))] = [d = std::move(draw)](void* p, const reflect::FieldInfo& f,
                                                                         EditorContext& ctx) -> bool {
            return d(*static_cast<F*>(p), f, ctx);
        };
    }
    /// @brief Custom widget for one field of one reflected type (wins over type drawers).
    /// @param typeName Reflected type name (e.g. "Health").
    /// @param fieldName Field name (e.g. "current").
    void fieldDrawer(const std::string& typeName, const std::string& fieldName, FieldDrawer draw) {
        m_fieldDrawers[typeName + "." + fieldName] = std::move(draw);
    }
    /// @brief The drawer for a field (field-specific first, then by C++ type), or `nullptr`.
    const FieldDrawer* findFieldDrawer(const reflect::TypeInfo& owner, const reflect::FieldInfo& field) const;
    /// @}

    /// @name Asset actions
    /// @{
    /// @brief Adds an action for assets of `type` (replacing one with the same type and label).
    /// @code
    /// r.assetAction("Dialogue", "Preview", [](EditorContext& ctx, const nova::AssetRecord& a) { openPreview(ctx, a.guid); });
    /// @endcode
    AssetAction& assetAction(std::string type, std::string label, std::function<void(EditorContext&, const AssetRecord&)> run);
    /// @brief Actions for a type (type-specific first, then "*"), in registration order.
    std::vector<const AssetAction*> assetActions(std::string_view type) const;
    /// @brief Removes an action. @return False if not found.
    bool removeAssetAction(std::string_view type, std::string_view label);
    /// @}

    /// @name UI hooks
    /// @{
    /// @brief Called every frame after the panels (modal popups, overlays, floating tools).
    void uiHook(std::function<void(EditorContext&)> draw) { m_uiHooks.push_back(std::move(draw)); }
    /// @brief Called inside the main menu bar after the menus (status text, play buttons...).
    void menuBarWidget(std::function<void(EditorContext&)> draw) { m_menuBarWidgets.push_back(std::move(draw)); }
    /// @brief Registered UI hooks.
    const std::vector<std::function<void(EditorContext&)>>& uiHooks() const { return m_uiHooks; }
    /// @brief Registered menu bar widgets.
    const std::vector<std::function<void(EditorContext&)>>& menuBarWidgets() const { return m_menuBarWidgets; }
    /// @}

    /// @brief Attaches every panel registered so far (called once by the editor at startup).
    void attachAll();
    /// @brief Detaches every panel (called once by the editor at shutdown).
    void detachAll();

private:
    void addPanel(Scope<Panel> panel);
    void addTool(Scope<Tool> tool);

    EditorContext& m_context;
    bool m_attached = false;
    std::vector<Scope<Plugin>> m_plugins;
    std::vector<Scope<Panel>> m_panels;
    std::vector<Scope<Tool>> m_tools;
    std::deque<Command> m_commands;                 // deque: references stay valid on append
    std::deque<ComponentEditor> m_componentEditors;
    std::unordered_map<std::type_index, FieldDrawer> m_typeDrawers;
    std::map<std::string, FieldDrawer> m_fieldDrawers;
    std::vector<std::function<void(EditorContext&)>> m_uiHooks;
    std::deque<AssetAction> m_assetActions;
    std::vector<std::function<void(EditorContext&)>> m_menuBarWidgets;
};

/// @brief Splits a menu path (`"File/Recent/Scene.json"`) into its parts, ignoring empty ones.
std::vector<std::string> splitMenuPath(std::string_view path);

} // namespace nova::editor
