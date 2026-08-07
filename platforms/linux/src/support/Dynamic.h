// dlopen, wrapped just enough to make an optional dependency ordinary.
//
// Six of the libraries this app talks to are optional, and "optional" has to
// mean the app starts and explains itself rather than dying on a missing
// symbol at load time. That is the whole reason this file exists.
//
// Two things it does that a bare dlopen call site would get wrong:
//
//   Version-suffixed sonames. There is no libavcodec.so on a user's machine —
//   that symlink is in the -dev package. What is there is libavcodec.so.61, or
//   .60, or .58, depending on the distribution and its age. `open()` takes a
//   list and tries them in order, newest first.
//
//   All-or-nothing binding. A library that opens but is missing one function
//   is worse than one that is absent, because the absence is discovered at the
//   first call rather than at startup. `bind()` latches a failure and
//   `complete()` reports it, so a caller checks once and then uses the
//   pointers without checking again.
//
// Nothing is ever dlclose()d. Unloading a library that has registered atexit
// handlers, thread-local destructors or — in libva's case — a driver .so of
// its own is a well-known way to crash at shutdown, and the process is about
// to exit anyway.
#pragma once

#include <initializer_list>
#include <string>

namespace livewall {

class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary() = default;

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    // Tries each soname in turn. Returns true on the first that loads.
    // `label` names the dependency in log lines: "libavcodec", not a soname.
    //
    // `quiet` suppresses the not-found line, for callers that are searching
    // through several candidate sets and for which a miss is expected rather
    // than notable.
    bool open(const char* label, std::initializer_list<const char*> sonames, bool quiet = false);

    bool isOpen() const { return handle_ != nullptr; }

    // Resolves `symbol` into `out`. A failure is remembered; later calls still
    // run, so one report lists the first missing symbol rather than the last.
    template <typename Fn>
    void bind(Fn*& out, const char* symbol) {
        out = reinterpret_cast<Fn*>(resolve(symbol));
    }

    // Optional symbols — a function added in a later release of the library
    // that the app can do without. Not counted against `complete()`.
    template <typename Fn>
    void bindOptional(Fn*& out, const char* symbol) {
        out = reinterpret_cast<Fn*>(resolveQuietly(symbol));
    }

    // True when the library opened and every `bind()` resolved.
    bool complete() const { return handle_ != nullptr && missing_.empty(); }

    // The first symbol that failed to resolve, for the log line.
    const std::string& missingSymbol() const { return missing_; }

    // The soname that actually loaded.
    const std::string& soname() const { return soname_; }

private:
    void* resolve(const char* symbol);
    void* resolveQuietly(const char* symbol) const;

    void* handle_ = nullptr;
    std::string soname_;
    std::string missing_;
};

}  // namespace livewall
