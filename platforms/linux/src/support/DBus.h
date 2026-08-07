// The smallest usable libdbus-1 wrapper.
//
// DBus is unavoidable on Linux for three things the other two ports get from a
// plain OS call: is the session locked, is the machine in a power-saving
// profile, and is there a tray to put an icon in. logind,
// power-profiles-daemon and StatusNotifierItem are all DBus and none has a
// non-DBus equivalent.
//
// libdbus rather than sd-bus, and dlopen rather than linking:
//
//   sd-bus has the nicer API and lives in libsystemd. Linking it would make the
//   app refuse to start on a musl or non-systemd machine over a feature — a
//   tray icon — that is decoration. libdbus is present wherever a desktop
//   session is, and absent exactly where the app should degrade rather than
//   fail.
//
//   dlopen because "absent" has to be survivable. Without a bus the app loses
//   the lock, idle-hint and power-profile gates and the tray, keeps every other
//   gate, and says so once at startup.
//
// The wrapper is thin on purpose. It makes a blocking call with a short
// timeout, reads a reply, subscribes to signals, and exports an object whose
// replies are drawn from a fixed set of shapes. That is all four things the app
// does with a bus, and the marshalling code is bounded by that list rather than
// by what DBus can express.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace livewall {
namespace dbus {

// The value shapes the app sends and receives. Anything richer — the nested
// `(ia{sv}av)` of com.canonical.dbusmenu, say — is deliberately not
// representable here; see the tray's known limits in the README.
class Value {
public:
    // `Unset` rather than the obvious `None`: X11/X.h defines None as a macro,
    // and any header that has seen Xlib — directly or four includes deep —
    // turns an enumerator of that name into a parse error a long way from its
    // cause.
    enum class Kind { Unset, Boolean, Int32, UInt32, String, ObjectPath, StringList };

    Value() = default;
    static Value boolean(bool v);
    static Value int32(std::int32_t v);
    static Value uint32(std::uint32_t v);
    static Value string(std::string v);
    static Value objectPath(std::string v);
    static Value stringList(std::vector<std::string> v);

    Kind kind() const { return kind_; }
    bool boolValue() const { return number_ != 0; }
    std::int32_t int32Value() const { return static_cast<std::int32_t>(number_); }
    std::uint32_t uint32Value() const { return static_cast<std::uint32_t>(number_); }
    const std::string& stringValue() const { return text_; }
    const std::vector<std::string>& listValue() const { return list_; }

    // The DBus signature character(s) for this value, for the marshaller.
    const char* signature() const;

private:
    Kind kind_ = Kind::Unset;
    std::int64_t number_ = 0;
    std::string text_;
    std::vector<std::string> list_;
};

using Dict = std::vector<std::pair<std::string, Value>>;

// A reply, or a reason there isn't one. Distinguishing "the call failed" from
// "the property is false" is the point: an absent power-profiles-daemon must
// not read as "power saver is on".
class Reply {
public:
    Reply() = default;
    Reply(bool ok, Value value) : ok_(ok), value_(std::move(value)) {}

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    // Reads the reply's single value, unwrapping one level of variant if there
    // is one — org.freedesktop.DBus.Properties.Get always wraps, and every
    // property read here goes through it.
    bool asBool(bool* out) const;
    bool asString(std::string* out) const;
    bool asInt32(std::int32_t* out) const;
    bool asUInt32(std::uint32_t* out) const;

private:
    bool ok_ = false;
    Value value_;
};

// Arguments for an outgoing call or signal, in order.
using Args = std::vector<Value>;

// An incoming method call being handled by an exported object.
class Call {
public:
    virtual ~Call() = default;

    // Positional arguments, as far as the app needs them: Properties.Get takes
    // two strings, Activate takes two int32s that are ignored.
    virtual std::string argString(int index) const = 0;

    // Exactly one of these is called per handled message.
    virtual void replyEmpty() = 0;
    virtual void replyString(const std::string& value) = 0;
    virtual void replyVariant(const Value& value) = 0;
    virtual void replyDict(const Dict& value) = 0;
    virtual void replyError(const char* name, const char* message) = 0;
};

// One of the two buses.
class Bus {
public:
    // Lazily connected on first use and never reconnected: a session that has
    // lost its bus daemon has larger problems than its wallpaper, and a
    // reconnect loop is a timer wakeup forever. Returns null when libdbus is
    // absent or the bus cannot be reached.
    static Bus* session();
    static Bus* system();

    // False when libdbus itself could not be loaded, which the caller reports
    // once rather than once per attempt.
    static bool available();

    ~Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // Deliberately short. Every call here is to a local service answering from
    // memory; anything slower is a service that is hung, and blocking the
    // render loop on it would be worse than doing without the answer.
    static constexpr int kTimeoutMs = 500;

    Reply call(const char* destination, const char* path, const char* interface,
               const char* method, const Args& args = {}, int timeoutMs = kTimeoutMs);

    // org.freedesktop.DBus.Properties.Get — most of what the app does.
    Reply property(const char* destination, const char* path, const char* interface,
                   const char* name);

    // Adds a match rule; anything matching is routed to the signal handler.
    bool subscribe(const std::string& matchRule);

    using SignalHandler = std::function<void(std::string_view interface, std::string_view member,
                                             std::string_view path)>;
    void setSignalHandler(SignalHandler handler);

    // The descriptor to poll for readability, or -1. The app has one event loop
    // and no thread parked on a bus, so the loop needs this rather than a
    // blocking dispatch.
    int fd() const;

    // Drains whatever is pending. Never blocks.
    void dispatch();
    void flush();

    // Claims a well-known name. `replaceExisting` is what a second tray icon
    // after a panel restart needs.
    bool requestName(const char* name, bool replaceExisting);

    using MethodHandler =
        std::function<bool(std::string_view interface, std::string_view member, Call& call)>;
    bool exportObject(const char* path, MethodHandler handler);

    bool emitSignal(const char* path, const char* interface, const char* name,
                    const Args& args = {});

    struct Impl;

private:
    Bus();

    // `busType` is a DBusBusType, kept as an int so this header does not pull
    // in dbus.h — which is the point of the whole wrapper.
    static Bus* connect(int busType, const char* label);

    std::unique_ptr<Impl> impl_;
};

}  // namespace dbus
}  // namespace livewall
