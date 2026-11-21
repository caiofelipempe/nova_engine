#include "nova/platform/DynamicLibrary.h"

#if defined(_WIN32)
    #include <Windows.h>
#else
    #include <dlfcn.h>
#endif

namespace nova::platform {

bool DynamicLibrary::open(const std::filesystem::path& path, std::string* error) {
    close();
#if defined(_WIN32)
    // A broken or half-written module (e.g. a failed build) would make Windows show a modal
    // "Bad Image" dialog and block the process until someone closes it. Report it as an error
    // instead: suppress the system error boxes for this thread while loading.
    DWORD previousMode = 0;
    const BOOL modeSet = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previousMode);
    m_handle = ::LoadLibraryW(path.wstring().c_str());
    const DWORD loadError = ::GetLastError();
    if (modeSet) ::SetThreadErrorMode(previousMode, nullptr);
    if (!m_handle && error) *error = "LoadLibrary failed for " + path.string() + " (error " + std::to_string(loadError) + ")";
#else
    m_handle = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_handle && error) *error = ::dlerror();
#endif
    return m_handle != nullptr;
}

void DynamicLibrary::close() {
    if (!m_handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    ::dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const {
    if (!m_handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
    return ::dlsym(m_handle, name);
#endif
}

} // namespace nova::platform
