#include "support/Dynamic.h"

#include <dlfcn.h>

#include "support/Log.h"

namespace livewall {

bool SharedLibrary::open(const char* label, std::initializer_list<const char*> sonames,
                         bool quiet) {
    if (handle_ != nullptr) return true;

    for (const char* soname : sonames) {
        // RTLD_LOCAL so a symbol from, say, libavcodec cannot satisfy a later
        // lookup meant for a different library. RTLD_LAZY because the app binds
        // by name through this class and never relies on the loader's own
        // relocation of these libraries' internals.
        void* handle = ::dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
        if (handle != nullptr) {
            handle_ = handle;
            soname_ = soname;
            Log::info(std::string("opened ") + label + " (" + soname + ")");
            return true;
        }
    }

    // dlerror() here would report only the last attempt, which is the oldest
    // soname in the list and the least informative of the set.
    if (!quiet) Log::info(std::string("no ") + label + " on this system");
    return false;
}

void* SharedLibrary::resolve(const char* symbol) {
    if (handle_ == nullptr) return nullptr;

    ::dlerror();  // clear any stale error before the lookup
    void* address = ::dlsym(handle_, symbol);
    if (address == nullptr && ::dlerror() != nullptr) {
        if (missing_.empty()) missing_ = symbol;
        return nullptr;
    }
    return address;
}

void* SharedLibrary::resolveQuietly(const char* symbol) const {
    if (handle_ == nullptr) return nullptr;
    ::dlerror();
    return ::dlsym(handle_, symbol);
}

}  // namespace livewall
