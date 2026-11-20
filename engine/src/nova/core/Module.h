#pragma once

/// @file
/// @brief Code-module ownership of registrations (for game code hot reload).
///
/// Everything a module registers — reflected types, asset importers, editor panels/commands —
/// is tagged with currentModule() so it can be removed before the module's DLL is unloaded
/// (its functions and vtables disappear with it). Code in the executable is module 0.

#include "nova/core/Base.h"

namespace nova {

/// @brief Identifies a loaded code module; 0 = the host executable and the engine.
using ModuleId = u32;

/// @brief The module whose code is registering things right now.
ModuleId currentModule();

/// @brief A new, never reused module id.
ModuleId newModuleId();

/// @brief Makes `id` the current module for its lifetime (nestable).
/// @code
/// { nova::ModuleScope scope(id); module->onLoad(registry); } // registrations tagged with `id`
/// @endcode
class ModuleScope : NonCopyable {
public:
    explicit ModuleScope(ModuleId id);
    ~ModuleScope();

private:
    ModuleId m_previous;
};

} // namespace nova
