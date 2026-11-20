#include "nova/editor/Registry.h"

#include "nova/core/Log.h"
#include "nova/editor/EditorContext.h"
#include "nova/editor/Widgets.h"

#include <algorithm>

namespace nova::editor {

Registry::~Registry() { detachAll(); }

void Registry::addPanel(Scope<Panel> panel) {
    removePanel(panel->title()); // same title = replace
    Panel& ref = *panel;
    m_owners[&ref] = currentModule();
    m_panels.push_back(std::move(panel));
    if (m_attached) ref.onAttach(m_context);
}

void Registry::addTool(Scope<Tool> tool) {
    const std::string name = tool->name();
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it)
        if ((*it)->name() == name) {
            if (m_context.activeTool() == it->get()) m_context.setActiveTool(nullptr);
            m_owners.erase(it->get());
            m_tools.erase(it);
            break;
        }
    Tool* raw = tool.get();
    m_owners[raw] = currentModule();
    m_tools.push_back(std::move(tool));

    Command c;
    c.id = "tool." + name;
    c.menuPath = "View/Tools/" + name;
    c.order = static_cast<int>(m_tools.size());
    if (auto chord = KeyChord::parse(raw->shortcut())) c.shortcut = *chord;
    c.execute = [this, name](EditorContext& ctx) { ctx.setActiveTool(findTool(name)); };
    c.isChecked = [this, name](EditorContext& ctx) { return ctx.activeTool() == findTool(name); };
    command(std::move(c));
    if (m_attached && !m_context.activeTool()) m_context.setActiveTool(raw);
}

Tool* Registry::findTool(const std::string& name) {
    for (auto& t : m_tools)
        if (t->name() == name) return t.get();
    return nullptr;
}

bool Registry::removePanel(const std::string& title) {
    auto it = std::find_if(m_panels.begin(), m_panels.end(), [&](const Scope<Panel>& p) { return p->title() == title; });
    if (it == m_panels.end()) return false;
    if (m_attached) (*it)->onDetach(m_context);
    m_owners.erase(it->get());
    m_panels.erase(it);
    return true;
}

Panel* Registry::findPanel(const std::string& title) {
    for (auto& p : m_panels)
        if (p->title() == title) return p.get();
    return nullptr;
}

Command& Registry::command(Command command) {
    NOVA_VERIFY(command.execute, "Command '" + command.id + "' has no execute function");
    if (command.id.empty()) command.id = command.menuPath;
    command.module = currentModule();
    if (Command* existing = findCommand(command.id)) {
        *existing = std::move(command);
        return *existing;
    }
    return m_commands.emplace_back(std::move(command));
}

Command& Registry::command(const std::string& menuPath, std::function<void(EditorContext&)> execute,
                           std::string_view shortcut) {
    Command c;
    c.id = menuPath;
    c.menuPath = menuPath;
    c.execute = std::move(execute);
    if (!shortcut.empty()) {
        auto chord = KeyChord::parse(shortcut);
        if (chord) c.shortcut = *chord;
        else NOVA_CORE_WARN("Editor: invalid shortcut '{}' for '{}'", shortcut, menuPath);
    }
    return command(std::move(c));
}

bool Registry::removeCommand(const std::string& id) {
    auto it = std::find_if(m_commands.begin(), m_commands.end(), [&](const Command& c) { return c.id == id; });
    if (it == m_commands.end()) return false;
    m_commands.erase(it);
    return true;
}

Command* Registry::findCommand(const std::string& id) {
    for (auto& c : m_commands)
        if (c.id == id) return &c;
    return nullptr;
}

bool Registry::execute(const std::string& id) {
    Command* c = findCommand(id);
    if (!c || !c->enabled(m_context)) return false;
    c->execute(m_context);
    return true;
}

namespace {
Script* findScript(Entity e, std::type_index type) {
    auto* sc = e.tryGet<ScriptComponent>();
    if (!sc) return nullptr;
    for (auto& s : sc->scripts)
        if (std::type_index(typeid(*s)) == type) return s.get();
    return nullptr;
}
} // namespace

ComponentEditor Registry::reflectedComponentEditor(const reflect::TypeInfo& type) {
    ComponentEditor ed;
    ed.name = type.displayName;
    ed.has = type.has;
    ed.add = type.add;
    ed.remove = type.remove;
    ed.draw = [&type](Entity e, EditorContext& ctx) { widgets::reflected(type, type.get(e), ctx); };
    return ed;
}

ComponentEditor Registry::reflectedScriptEditor(const reflect::TypeInfo& type) {
    ComponentEditor ed;
    ed.name = "Script: " + type.displayName;
    ed.has = [&type](Entity e) { return findScript(e, type.type) != nullptr; };
    ed.add = [&type](Entity e) { e.attachScript(type.createScript()); };
    ed.remove = [&type](Entity e) {
        auto& scripts = e.get<ScriptComponent>().scripts;
        auto it = std::find_if(scripts.begin(), scripts.end(),
                               [&](const Scope<Script>& s) { return std::type_index(typeid(*s)) == type.type; });
        if (it == scripts.end()) return;
        scripts.erase(it);
        if (scripts.empty()) e.remove<ScriptComponent>();
    };
    ed.draw = [&type](Entity e, EditorContext& ctx) {
        if (Script* s = findScript(e, type.type)) widgets::reflected(type, s, ctx);
    };
    return ed;
}

void Registry::addReflectedEditors() {
    auto registered = [this](const std::string& name) {
        return std::any_of(m_componentEditors.begin(), m_componentEditors.end(),
                           [&](const ComponentEditor& e) { return e.name == name; });
    };
    // Generated editors belong to the module of their type: they go away with it.
    for (const reflect::TypeInfo* t : reflect::types().components())
        if (!t->hideInInspector && !registered(t->displayName)) {
            ComponentEditor ed = reflectedComponentEditor(*t);
            ed.module = t->module;
            addComponentEditor(std::move(ed));
        }
    for (const reflect::TypeInfo* t : reflect::types().scripts())
        if (!registered("Script: " + t->displayName)) {
            ComponentEditor ed = reflectedScriptEditor(*t);
            ed.module = t->module;
            addComponentEditor(std::move(ed));
        }
}

const FieldDrawer* Registry::findFieldDrawer(const reflect::TypeInfo& owner, const reflect::FieldInfo& field) const {
    if (!m_fieldDrawers.empty()) {
        auto it = m_fieldDrawers.find(owner.name + "." + field.name);
        if (it != m_fieldDrawers.end()) return &it->second.draw;
    }
    auto it = m_typeDrawers.find(field.type);
    return it != m_typeDrawers.end() ? &it->second.draw : nullptr;
}

ComponentEditor& Registry::addComponentEditor(ComponentEditor editor) {
    NOVA_VERIFY(editor.has, "ComponentEditor '" + editor.name + "' needs a `has` function");
    if (editor.module == 0) editor.module = currentModule();
    for (auto& e : m_componentEditors)
        if (e.name == editor.name) return e = std::move(editor);
    return m_componentEditors.emplace_back(std::move(editor));
}

bool Registry::removeComponentEditor(const std::string& name) {
    auto it = std::find_if(m_componentEditors.begin(), m_componentEditors.end(),
                           [&](const ComponentEditor& e) { return e.name == name; });
    if (it == m_componentEditors.end()) return false;
    m_componentEditors.erase(it);
    return true;
}

void Registry::attachAll() {
    if (m_attached) return;
    m_attached = true;
    for (size_t i = 0; i < m_panels.size(); ++i) m_panels[i]->onAttach(m_context); // panels may register panels
    if (!m_context.activeTool() && !m_tools.empty()) m_context.setActiveTool(m_tools.front().get());
}

void Registry::detachAll() {
    if (!m_attached) return;
    m_context.setActiveTool(nullptr);
    for (auto it = m_panels.rbegin(); it != m_panels.rend(); ++it) (*it)->onDetach(m_context);
    m_attached = false;
}

size_t Registry::removeModule(ModuleId module) {
    if (module == 0) return 0;
    auto owned = [&](const void* p) {
        auto it = m_owners.find(p);
        return it != m_owners.end() && it->second == module;
    };
    size_t removed = 0;

    // Tools (deactivating the active one first) and panels (detached like removePanel does).
    removed += std::erase_if(m_tools, [&](const Scope<Tool>& t) {
        if (!owned(t.get())) return false;
        if (m_context.activeTool() == t.get()) m_context.setActiveTool(nullptr);
        m_owners.erase(t.get());
        return true;
    });
    for (auto it = m_panels.begin(); it != m_panels.end();) {
        if (!owned(it->get())) {
            ++it;
            continue;
        }
        if (m_attached) (*it)->onDetach(m_context);
        m_owners.erase(it->get());
        it = m_panels.erase(it);
        ++removed;
    }

    removed += std::erase_if(m_commands, [&](const Command& c) { return c.module == module; });
    removed += std::erase_if(m_componentEditors, [&](const ComponentEditor& e) { return e.module == module; });
    removed += std::erase_if(m_typeDrawers, [&](const auto& kv) { return kv.second.module == module; });
    removed += std::erase_if(m_fieldDrawers, [&](const auto& kv) { return kv.second.module == module; });
    removed += std::erase_if(m_uiHooks, [&](const UiHook& h) { return h.module == module; });
    removed += std::erase_if(m_menuBarWidgets, [&](const UiHook& h) { return h.module == module; });
    removed += std::erase_if(m_assetActions, [&](const AssetAction& a) { return a.module == module; });
    // Plugins last: their objects may be referenced by the entries above.
    removed += std::erase_if(m_plugins, [&](const Scope<Plugin>& p) {
        if (!owned(p.get())) return false;
        m_owners.erase(p.get());
        return true;
    });

    if (m_attached && !m_context.activeTool() && !m_tools.empty()) m_context.setActiveTool(m_tools.front().get());
    return removed;
}

std::vector<std::string> splitMenuPath(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        if (end > start) parts.emplace_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// ----------------------------------------------------------- asset actions --
AssetAction& Registry::assetAction(std::string type, std::string label,
                                   std::function<void(EditorContext&, const AssetRecord&)> run) {
    for (AssetAction& a : m_assetActions)
        if (a.type == type && a.label == label) {
            a.run = std::move(run);
            a.module = currentModule();
            return a;
        }
    m_assetActions.push_back({std::move(type), std::move(label), std::move(run), currentModule()});
    return m_assetActions.back();
}

std::vector<const AssetAction*> Registry::assetActions(std::string_view type) const {
    std::vector<const AssetAction*> out;
    for (const AssetAction& a : m_assetActions)
        if (a.type == type) out.push_back(&a);
    for (const AssetAction& a : m_assetActions)
        if (a.type == "*") out.push_back(&a);
    return out;
}

bool Registry::removeAssetAction(std::string_view type, std::string_view label) {
    auto it = std::find_if(m_assetActions.begin(), m_assetActions.end(),
                           [&](const AssetAction& a) { return a.type == type && a.label == label; });
    if (it == m_assetActions.end()) return false;
    m_assetActions.erase(it);
    return true;
}
} // namespace nova::editor
