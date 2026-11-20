#include "nova/core/Module.h"

namespace nova {

namespace {
ModuleId g_current = 0;
ModuleId g_next = 1;
} // namespace

ModuleId currentModule() { return g_current; }
ModuleId newModuleId() { return g_next++; }

ModuleScope::ModuleScope(ModuleId id) : m_previous(g_current) { g_current = id; }
ModuleScope::~ModuleScope() { g_current = m_previous; }

} // namespace nova
