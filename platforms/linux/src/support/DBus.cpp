#include "support/DBus.h"

#include <dbus/dbus.h>

#include <cstring>

#include "support/Dynamic.h"
#include "support/Log.h"

namespace livewall {
namespace dbus {
namespace {

// ---------------------------------------------------------------------------
// The libdbus entry points, resolved at runtime.
//
// Twenty-eight symbols, which is the whole of libdbus this app touches. Listed
// explicitly rather than pulled in by linking so that a machine without the
// library loses the tray and three gates instead of failing to start.
// ---------------------------------------------------------------------------
struct Api {
    decltype(::dbus_error_init)* error_init = nullptr;
    decltype(::dbus_error_free)* error_free = nullptr;
    decltype(::dbus_error_is_set)* error_is_set = nullptr;
    decltype(::dbus_bus_get_private)* bus_get_private = nullptr;
    decltype(::dbus_bus_request_name)* bus_request_name = nullptr;
    decltype(::dbus_bus_add_match)* bus_add_match = nullptr;
    decltype(::dbus_connection_set_exit_on_disconnect)* set_exit_on_disconnect = nullptr;
    decltype(::dbus_connection_close)* connection_close = nullptr;
    decltype(::dbus_connection_unref)* connection_unref = nullptr;
    decltype(::dbus_connection_get_unix_fd)* get_unix_fd = nullptr;
    decltype(::dbus_connection_read_write)* connection_read_write = nullptr;
    decltype(::dbus_connection_dispatch)* connection_dispatch = nullptr;
    decltype(::dbus_connection_flush)* connection_flush = nullptr;
    decltype(::dbus_connection_send)* connection_send = nullptr;
    decltype(::dbus_connection_send_with_reply_and_block)* send_with_reply_and_block = nullptr;
    decltype(::dbus_connection_add_filter)* add_filter = nullptr;
    decltype(::dbus_connection_register_object_path)* register_object_path = nullptr;
    decltype(::dbus_message_new_method_call)* new_method_call = nullptr;
    decltype(::dbus_message_new_method_return)* new_method_return = nullptr;
    decltype(::dbus_message_new_error)* new_error = nullptr;
    decltype(::dbus_message_new_signal)* new_signal = nullptr;
    decltype(::dbus_message_unref)* message_unref = nullptr;
    decltype(::dbus_message_get_type)* message_get_type = nullptr;
    decltype(::dbus_message_get_interface)* get_interface = nullptr;
    decltype(::dbus_message_get_member)* get_member = nullptr;
    decltype(::dbus_message_get_path)* get_path = nullptr;
    decltype(::dbus_message_iter_init)* iter_init = nullptr;
    decltype(::dbus_message_iter_init_append)* iter_init_append = nullptr;
    decltype(::dbus_message_iter_append_basic)* iter_append_basic = nullptr;
    decltype(::dbus_message_iter_open_container)* iter_open_container = nullptr;
    decltype(::dbus_message_iter_close_container)* iter_close_container = nullptr;
    decltype(::dbus_message_iter_get_arg_type)* iter_get_arg_type = nullptr;
    decltype(::dbus_message_iter_get_basic)* iter_get_basic = nullptr;
    decltype(::dbus_message_iter_recurse)* iter_recurse = nullptr;
    decltype(::dbus_message_iter_next)* iter_next = nullptr;
};

Api g_api;
SharedLibrary g_library;
bool g_attempted = false;

bool loadApi() {
    if (g_attempted) return g_library.complete();
    g_attempted = true;

    // libdbus-1.so.3 has been the soname since 2006; there is no second one to
    // try, which is unusual for this list and worth not pretending otherwise.
    if (!g_library.open("libdbus-1", {"libdbus-1.so.3"})) return false;

    g_library.bind(g_api.error_init, "dbus_error_init");
    g_library.bind(g_api.error_free, "dbus_error_free");
    g_library.bind(g_api.error_is_set, "dbus_error_is_set");
    g_library.bind(g_api.bus_get_private, "dbus_bus_get_private");
    g_library.bind(g_api.bus_request_name, "dbus_bus_request_name");
    g_library.bind(g_api.bus_add_match, "dbus_bus_add_match");
    g_library.bind(g_api.set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
    g_library.bind(g_api.connection_close, "dbus_connection_close");
    g_library.bind(g_api.connection_unref, "dbus_connection_unref");
    g_library.bind(g_api.get_unix_fd, "dbus_connection_get_unix_fd");
    g_library.bind(g_api.connection_read_write, "dbus_connection_read_write");
    g_library.bind(g_api.connection_dispatch, "dbus_connection_dispatch");
    g_library.bind(g_api.connection_flush, "dbus_connection_flush");
    g_library.bind(g_api.connection_send, "dbus_connection_send");
    g_library.bind(g_api.send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
    g_library.bind(g_api.add_filter, "dbus_connection_add_filter");
    g_library.bind(g_api.register_object_path, "dbus_connection_register_object_path");
    g_library.bind(g_api.new_method_call, "dbus_message_new_method_call");
    g_library.bind(g_api.new_method_return, "dbus_message_new_method_return");
    g_library.bind(g_api.new_error, "dbus_message_new_error");
    g_library.bind(g_api.new_signal, "dbus_message_new_signal");
    g_library.bind(g_api.message_unref, "dbus_message_unref");
    g_library.bind(g_api.message_get_type, "dbus_message_get_type");
    g_library.bind(g_api.get_interface, "dbus_message_get_interface");
    g_library.bind(g_api.get_member, "dbus_message_get_member");
    g_library.bind(g_api.get_path, "dbus_message_get_path");
    g_library.bind(g_api.iter_init, "dbus_message_iter_init");
    g_library.bind(g_api.iter_init_append, "dbus_message_iter_init_append");
    g_library.bind(g_api.iter_append_basic, "dbus_message_iter_append_basic");
    g_library.bind(g_api.iter_open_container, "dbus_message_iter_open_container");
    g_library.bind(g_api.iter_close_container, "dbus_message_iter_close_container");
    g_library.bind(g_api.iter_get_arg_type, "dbus_message_iter_get_arg_type");
    g_library.bind(g_api.iter_get_basic, "dbus_message_iter_get_basic");
    g_library.bind(g_api.iter_recurse, "dbus_message_iter_recurse");
    g_library.bind(g_api.iter_next, "dbus_message_iter_next");

    if (!g_library.complete()) {
        Log::error("libdbus-1 is missing " + g_library.missingSymbol() +
                   " — DBus features are off");
        return false;
    }
    return true;
}

// --- marshalling ------------------------------------------------------------

void appendValue(DBusMessageIter* iter, const Value& value) {
    switch (value.kind()) {
        case Value::Kind::Boolean: {
            const dbus_bool_t v = value.boolValue() ? TRUE : FALSE;
            g_api.iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &v);
            break;
        }
        case Value::Kind::Int32: {
            const dbus_int32_t v = value.int32Value();
            g_api.iter_append_basic(iter, DBUS_TYPE_INT32, &v);
            break;
        }
        case Value::Kind::UInt32: {
            const dbus_uint32_t v = value.uint32Value();
            g_api.iter_append_basic(iter, DBUS_TYPE_UINT32, &v);
            break;
        }
        case Value::Kind::String:
        case Value::Kind::ObjectPath: {
            const char* v = value.stringValue().c_str();
            g_api.iter_append_basic(
                iter, value.kind() == Value::Kind::String ? DBUS_TYPE_STRING : DBUS_TYPE_OBJECT_PATH,
                &v);
            break;
        }
        case Value::Kind::StringList: {
            DBusMessageIter array;
            g_api.iter_open_container(iter, DBUS_TYPE_ARRAY, "s", &array);
            for (const std::string& entry : value.listValue()) {
                const char* v = entry.c_str();
                g_api.iter_append_basic(&array, DBUS_TYPE_STRING, &v);
            }
            g_api.iter_close_container(iter, &array);
            break;
        }
        case Value::Kind::Unset:
            break;
    }
}

void appendVariant(DBusMessageIter* iter, const Value& value) {
    DBusMessageIter variant;
    g_api.iter_open_container(iter, DBUS_TYPE_VARIANT, value.signature(), &variant);
    appendValue(&variant, value);
    g_api.iter_close_container(iter, &variant);
}

void appendDict(DBusMessageIter* iter, const Dict& dict) {
    DBusMessageIter array;
    g_api.iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &array);
    for (const auto& [key, value] : dict) {
        DBusMessageIter entry;
        g_api.iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char* name = key.c_str();
        g_api.iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
        appendVariant(&entry, value);
        g_api.iter_close_container(&array, &entry);
    }
    g_api.iter_close_container(iter, &array);
}

// Reads one value, stepping through a variant if it finds one. Anything richer
// than the shapes in `Value` reads as None, which the callers treat as "the
// property was not the type I expected" rather than as a false.
Value readValue(DBusMessageIter* iter, int depth = 0) {
    if (depth > 2) return {};

    switch (g_api.iter_get_arg_type(iter)) {
        case DBUS_TYPE_BOOLEAN: {
            dbus_bool_t v = FALSE;
            g_api.iter_get_basic(iter, &v);
            return Value::boolean(v != FALSE);
        }
        case DBUS_TYPE_INT32: {
            dbus_int32_t v = 0;
            g_api.iter_get_basic(iter, &v);
            return Value::int32(v);
        }
        case DBUS_TYPE_UINT32: {
            dbus_uint32_t v = 0;
            g_api.iter_get_basic(iter, &v);
            return Value::uint32(v);
        }
        case DBUS_TYPE_DOUBLE: {
            double v = 0;
            g_api.iter_get_basic(iter, &v);
            // No Double kind: the only double the app reads is a battery
            // percentage it does not use — /sys is the source for that.
            return Value::int32(static_cast<std::int32_t>(v));
        }
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH: {
            const char* v = nullptr;
            g_api.iter_get_basic(iter, &v);
            return Value::string(v != nullptr ? v : "");
        }
        case DBUS_TYPE_VARIANT: {
            DBusMessageIter inner;
            g_api.iter_recurse(iter, &inner);
            return readValue(&inner, depth + 1);
        }
        default:
            return {};
    }
}

}  // namespace

// --- Value ------------------------------------------------------------------

Value Value::boolean(bool v) {
    Value value;
    value.kind_ = Kind::Boolean;
    value.number_ = v ? 1 : 0;
    return value;
}

Value Value::int32(std::int32_t v) {
    Value value;
    value.kind_ = Kind::Int32;
    value.number_ = v;
    return value;
}

Value Value::uint32(std::uint32_t v) {
    Value value;
    value.kind_ = Kind::UInt32;
    value.number_ = v;
    return value;
}

Value Value::string(std::string v) {
    Value value;
    value.kind_ = Kind::String;
    value.text_ = std::move(v);
    return value;
}

Value Value::objectPath(std::string v) {
    Value value;
    value.kind_ = Kind::ObjectPath;
    value.text_ = std::move(v);
    return value;
}

Value Value::stringList(std::vector<std::string> v) {
    Value value;
    value.kind_ = Kind::StringList;
    value.list_ = std::move(v);
    return value;
}

const char* Value::signature() const {
    switch (kind_) {
        case Kind::Boolean: return "b";
        case Kind::Int32: return "i";
        case Kind::UInt32: return "u";
        case Kind::String: return "s";
        case Kind::ObjectPath: return "o";
        case Kind::StringList: return "as";
        case Kind::Unset: return "s";
    }
    return "s";
}

// --- Reply ------------------------------------------------------------------

bool Reply::asBool(bool* out) const {
    if (!ok_ || value_.kind() != Value::Kind::Boolean) return false;
    *out = value_.boolValue();
    return true;
}

bool Reply::asString(std::string* out) const {
    if (!ok_ || (value_.kind() != Value::Kind::String && value_.kind() != Value::Kind::ObjectPath)) {
        return false;
    }
    *out = value_.stringValue();
    return true;
}

bool Reply::asInt32(std::int32_t* out) const {
    if (!ok_ || value_.kind() != Value::Kind::Int32) return false;
    *out = value_.int32Value();
    return true;
}

bool Reply::asUInt32(std::uint32_t* out) const {
    if (!ok_ || value_.kind() != Value::Kind::UInt32) return false;
    *out = value_.uint32Value();
    return true;
}

// --- Bus --------------------------------------------------------------------

struct Bus::Impl {
    DBusConnection* connection = nullptr;
    Bus::SignalHandler signalHandler;
};

namespace {

// The filter sees every message that reaches the connection and is where
// subscribed signals are dispatched from. Method calls are handled by the
// object vtable instead, so this passes them through.
DBusHandlerResult signalFilter(DBusConnection* connection, DBusMessage* message, void* user) {
    (void)connection;
    auto* impl = static_cast<Bus::Impl*>(user);
    if (g_api.message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (impl->signalHandler) {
        const char* interface = g_api.get_interface(message);
        const char* member = g_api.get_member(message);
        const char* path = g_api.get_path(message);
        impl->signalHandler(interface != nullptr ? interface : "", member != nullptr ? member : "",
                            path != nullptr ? path : "");
    }
    // Not "handled": a signal may legitimately interest more than one filter,
    // and swallowing it here would make a second subscriber impossible.
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

class MessageCall final : public Call {
public:
    MessageCall(DBusConnection* connection, DBusMessage* message)
        : connection_(connection), message_(message) {}

    ~MessageCall() override {
        // A handler that returned true without replying would leave the caller
        // blocked until its own timeout. Cheaper to notice here than to debug
        // from the other end.
        if (!replied_) {
            replyError(DBUS_ERROR_FAILED, "handler produced no reply");
        }
    }

    std::string argString(int index) const override {
        DBusMessageIter iter;
        if (g_api.iter_init(message_, &iter) == FALSE) return {};
        for (int i = 0; i < index; ++i) {
            if (g_api.iter_next(&iter) == FALSE) return {};
        }
        const Value value = readValue(&iter);
        return value.kind() == Value::Kind::String ? value.stringValue() : std::string();
    }

    void replyEmpty() override {
        DBusMessage* reply = g_api.new_method_return(message_);
        if (reply == nullptr) return;
        finish(reply);
    }

    void replyString(const std::string& value) override {
        DBusMessage* reply = g_api.new_method_return(message_);
        if (reply == nullptr) return;
        DBusMessageIter iter;
        g_api.iter_init_append(reply, &iter);
        const char* text = value.c_str();
        g_api.iter_append_basic(&iter, DBUS_TYPE_STRING, &text);
        finish(reply);
    }

    void replyVariant(const Value& value) override {
        DBusMessage* reply = g_api.new_method_return(message_);
        if (reply == nullptr) return;
        DBusMessageIter iter;
        g_api.iter_init_append(reply, &iter);
        appendVariant(&iter, value);
        finish(reply);
    }

    void replyDict(const Dict& value) override {
        DBusMessage* reply = g_api.new_method_return(message_);
        if (reply == nullptr) return;
        DBusMessageIter iter;
        g_api.iter_init_append(reply, &iter);
        appendDict(&iter, value);
        finish(reply);
    }

    void replyError(const char* name, const char* message) override {
        DBusMessage* reply = g_api.new_error(message_, name, message);
        if (reply == nullptr) return;
        finish(reply);
    }

private:
    void finish(DBusMessage* reply) {
        replied_ = true;
        g_api.connection_send(connection_, reply, nullptr);
        g_api.message_unref(reply);
    }

    DBusConnection* connection_ = nullptr;
    DBusMessage* message_ = nullptr;
    bool replied_ = false;
};

DBusHandlerResult objectDispatch(DBusConnection* connection, DBusMessage* message, void* user) {
    auto* handler = static_cast<Bus::MethodHandler*>(user);
    if (g_api.message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* interface = g_api.get_interface(message);
    const char* member = g_api.get_member(message);
    MessageCall call(connection, message);
    const bool handled = (*handler)(interface != nullptr ? interface : "",
                                    member != nullptr ? member : "", call);
    if (!handled) {
        call.replyError(DBUS_ERROR_UNKNOWN_METHOD, "not implemented");
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

}  // namespace

Bus::Bus() : impl_(std::make_unique<Impl>()) {}

Bus::~Bus() {
    if (impl_ && impl_->connection != nullptr) {
        // A private connection must be closed explicitly; the shared one from
        // dbus_bus_get must not be. Using the private variant everywhere makes
        // that rule one rule instead of two.
        g_api.connection_close(impl_->connection);
        g_api.connection_unref(impl_->connection);
    }
}

bool Bus::available() { return loadApi(); }

Bus* Bus::connect(int busType, const char* label) {
    if (!loadApi()) return nullptr;

    // Leaked on purpose: the connection outlives every caller, and closing it
    // at static-destruction time races with whatever libdbus has running. The
    // process is exiting either way.
    auto* bus = new Bus();

    DBusError error;
    g_api.error_init(&error);
    DBusConnection* connection =
        g_api.bus_get_private(static_cast<DBusBusType>(busType), &error);
    if (connection == nullptr) {
        Log::info(std::string("no ") + label + " bus: " +
                  (g_api.error_is_set(&error) ? error.message : "unavailable"));
        g_api.error_free(&error);
        delete bus;
        return nullptr;
    }
    g_api.error_free(&error);

    // Without this, the bus daemon going away takes the whole app with it —
    // libdbus calls exit(1) from inside the dispatch. Losing the tray is the
    // correct outcome; losing the wallpaper is not.
    g_api.set_exit_on_disconnect(connection, FALSE);

    bus->impl_->connection = connection;
    g_api.add_filter(connection, signalFilter, bus->impl_.get(), nullptr);
    return bus;
}

Bus* Bus::session() {
    static Bus* instance = connect(DBUS_BUS_SESSION, "session");
    return instance;
}

Bus* Bus::system() {
    static Bus* instance = connect(DBUS_BUS_SYSTEM, "system");
    return instance;
}

Reply Bus::call(const char* destination, const char* path, const char* interface,
                const char* method, const Args& args, int timeoutMs) {
    if (impl_->connection == nullptr) return {};

    DBusMessage* message = g_api.new_method_call(destination, path, interface, method);
    if (message == nullptr) return {};

    DBusMessageIter iter;
    g_api.iter_init_append(message, &iter);
    for (const Value& value : args) appendValue(&iter, value);

    DBusError error;
    g_api.error_init(&error);
    DBusMessage* reply =
        g_api.send_with_reply_and_block(impl_->connection, message, timeoutMs, &error);
    g_api.message_unref(message);

    if (reply == nullptr) {
        if (g_api.error_is_set(&error)) {
            // Info, not error: "the service is not running" is the normal
            // answer on a machine that does not have it, and this fires once
            // per gate evaluation.
            Log::info(std::string(interface) + "." + method + ": " + error.message);
        }
        g_api.error_free(&error);
        return {};
    }
    g_api.error_free(&error);

    Value value;
    DBusMessageIter replyIter;
    if (g_api.iter_init(reply, &replyIter) != FALSE) value = readValue(&replyIter);
    g_api.message_unref(reply);
    return Reply(true, std::move(value));
}

Reply Bus::property(const char* destination, const char* path, const char* interface,
                    const char* name) {
    return call(destination, path, "org.freedesktop.DBus.Properties", "Get",
                {Value::string(interface), Value::string(name)});
}

bool Bus::subscribe(const std::string& matchRule) {
    if (impl_->connection == nullptr) return false;

    DBusError error;
    g_api.error_init(&error);
    // Non-blocking: the reply is not interesting and waiting for it would put a
    // round trip in the startup path for each of the four rules.
    g_api.bus_add_match(impl_->connection, matchRule.c_str(), &error);
    const bool failed = g_api.error_is_set(&error) != FALSE;
    if (failed) Log::info("match rule rejected: " + std::string(error.message));
    g_api.error_free(&error);
    return !failed;
}

void Bus::setSignalHandler(SignalHandler handler) { impl_->signalHandler = std::move(handler); }

int Bus::fd() const {
    if (impl_->connection == nullptr) return -1;
    int descriptor = -1;
    if (g_api.get_unix_fd(impl_->connection, &descriptor) == FALSE) return -1;
    return descriptor;
}

void Bus::dispatch() {
    if (impl_->connection == nullptr) return;

    // Timeout 0: move whatever has arrived into the incoming queue and return.
    // The event loop already decided this descriptor was readable, so there is
    // nothing to wait for.
    g_api.connection_read_write(impl_->connection, 0);

    // One read can carry several messages, and each dispatch handles one. The
    // obvious `while (read_write_dispatch(...))` spelling does not do this: it
    // returns true for as long as the connection is *open*, so it never
    // terminates.
    while (g_api.connection_dispatch(impl_->connection) == DBUS_DISPATCH_DATA_REMAINS) {
    }
}

void Bus::flush() {
    if (impl_->connection != nullptr) g_api.connection_flush(impl_->connection);
}

bool Bus::requestName(const char* name, bool replaceExisting) {
    if (impl_->connection == nullptr) return false;

    DBusError error;
    g_api.error_init(&error);
    const unsigned flags = replaceExisting ? DBUS_NAME_FLAG_REPLACE_EXISTING : 0;
    const int result = g_api.bus_request_name(impl_->connection, name, flags, &error);
    if (g_api.error_is_set(&error)) {
        Log::info(std::string("could not take ") + name + ": " + error.message);
        g_api.error_free(&error);
        return false;
    }
    g_api.error_free(&error);
    return result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
           result == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
}

bool Bus::exportObject(const char* path, MethodHandler handler) {
    if (impl_->connection == nullptr) return false;

    // Heap-allocated rather than stored in a container: libdbus keeps the raw
    // pointer for the life of the registration, and any container that can
    // reallocate would hand it a dangling one the moment a second object is
    // exported. Freed only on failure; a live registration outlives the app.
    auto* stored = new MethodHandler(std::move(handler));

    static DBusObjectPathVTable vtable = {};
    vtable.message_function = objectDispatch;

    DBusError error;
    g_api.error_init(&error);
    const dbus_bool_t ok =
        g_api.register_object_path(impl_->connection, path, &vtable, stored);
    if (ok == FALSE || g_api.error_is_set(&error)) {
        Log::error(std::string("could not export ") + path);
        g_api.error_free(&error);
        delete stored;
        return false;
    }
    g_api.error_free(&error);
    return true;
}

bool Bus::emitSignal(const char* path, const char* interface, const char* name, const Args& args) {
    if (impl_->connection == nullptr) return false;

    DBusMessage* message = g_api.new_signal(path, interface, name);
    if (message == nullptr) return false;

    DBusMessageIter iter;
    g_api.iter_init_append(message, &iter);
    for (const Value& value : args) appendValue(&iter, value);

    const dbus_bool_t sent = g_api.connection_send(impl_->connection, message, nullptr);
    g_api.message_unref(message);
    g_api.connection_flush(impl_->connection);
    return sent != FALSE;
}

}  // namespace dbus
}  // namespace livewall
