#include "app/TrayIcon.h"

#include <unistd.h>
#include <algorithm>

#include "app/WallpaperEngine.h"
#include "support/DBus.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kItemPath = "/StatusNotifierItem";

// Installed into the hicolor theme by packaging/. A run straight out of a
// build directory has not installed it, and the host falls back to a blank
// square — which is a cosmetic cost of not installing, not a bug to work
// around by shipping pixmaps over the bus.
constexpr const char* kIconName = "livewall";

}  // namespace

bool TrayIcon::start(WallpaperEngine& engine) {
    engine_ = &engine;

    dbus::Bus* bus = dbus::Bus::session();
    if (bus == nullptr) {
        Log::info("no session bus — no tray icon (the CLI is unaffected)");
        return false;
    }

    // The name the watcher expects, and the reason a second instance would
    // collide: the pid makes it unique.
    serviceName_ = format("org.kde.StatusNotifierItem-%d-1", static_cast<int>(::getpid()));

    if (!bus->requestName(serviceName_.c_str(), true)) {
        Log::info("could not take " + serviceName_ + " — no tray icon");
        return false;
    }

    if (!exported_) {
        exported_ = bus->exportObject(
            kItemPath, [this](std::string_view interface, std::string_view member,
                              dbus::Call& call) -> bool {
                if (interface == "org.freedesktop.DBus.Properties") {
                    if (member == "Get") {
                        const std::string name = call.argString(1);
                        if (name == "Category") {
                            call.replyVariant(dbus::Value::string("SystemServices"));
                        } else if (name == "Id") {
                            call.replyVariant(dbus::Value::string("livewall"));
                        } else if (name == "Title") {
                            call.replyVariant(dbus::Value::string("LiveWall"));
                        } else if (name == "Status") {
                            call.replyVariant(dbus::Value::string("Active"));
                        } else if (name == "IconName") {
                            call.replyVariant(dbus::Value::string(kIconName));
                        } else if (name == "OverlayIconName") {
                            call.replyVariant(dbus::Value::string(""));
                        } else if (name == "AttentionIconName") {
                            call.replyVariant(dbus::Value::string(""));
                        } else if (name == "IconThemePath") {
                            call.replyVariant(dbus::Value::string(""));
                        } else if (name == "ItemIsMenu") {
                            // False, so a host sends Activate on a left click
                            // rather than trying to open the menu this item
                            // does not have.
                            call.replyVariant(dbus::Value::boolean(false));
                        } else if (name == "Menu") {
                            // Hosts ask for this before deciding what a click
                            // does. An empty path is the honest answer and is
                            // what makes ItemIsMenu=false meaningful.
                            call.replyVariant(dbus::Value::objectPath("/"));
                        } else {
                            // ToolTip is the notable absentee: its signature is
                            // a struct with an embedded icon pixmap array, and
                            // the status text lives in the title instead.
                            call.replyError("org.freedesktop.DBus.Error.UnknownProperty",
                                            "not published");
                        }
                        return true;
                    }

                    if (member == "GetAll") {
                        dbus::Dict properties;
                        properties.emplace_back("Category",
                                                dbus::Value::string("SystemServices"));
                        properties.emplace_back("Id", dbus::Value::string("livewall"));
                        properties.emplace_back("Title", dbus::Value::string(currentTitle()));
                        properties.emplace_back("Status", dbus::Value::string("Active"));
                        properties.emplace_back("IconName", dbus::Value::string(kIconName));
                        properties.emplace_back("ItemIsMenu", dbus::Value::boolean(false));
                        properties.emplace_back("Menu", dbus::Value::objectPath("/"));
                        call.replyDict(properties);
                        return true;
                    }
                    return false;
                }

                if (interface != kItemInterface) return false;

                if (member == "Activate") {
                    cycleWallpaper();
                    call.replyEmpty();
                    return true;
                }
                if (member == "SecondaryActivate" || member == "Scroll") {
                    cycleFitMode();
                    call.replyEmpty();
                    return true;
                }
                if (member == "ContextMenu") {
                    // Nothing to show. Replying rather than erroring keeps the
                    // host from logging a failure on every right click.
                    call.replyEmpty();
                    return true;
                }
                return false;
            });
    }

    if (!exported_) return false;

    // A watcher that is not there yet is the normal case during login: the bar
    // and this daemon start in the same batch. NameOwnerChanged brings us back.
    bus->subscribe(format("type='signal',interface='org.freedesktop.DBus',"
                          "member='NameOwnerChanged',arg0='%s'",
                          kWatcherName));

    registered_ = registerWithWatcher();
    return registered_;
}

std::string TrayIcon::currentTitle() const {
    if (engine_ == nullptr) return "LiveWall";
    return "LiveWall — " + engine_->statusLine();
}

bool TrayIcon::registerWithWatcher() {
    dbus::Bus* bus = dbus::Bus::session();
    if (bus == nullptr) return false;

    const dbus::Reply reply =
        bus->call(kWatcherName, kWatcherPath, kWatcherName, "RegisterStatusNotifierItem",
                  {dbus::Value::string(serviceName_)});
    if (!reply.ok()) {
        Log::info("no StatusNotifierWatcher on this session — no tray icon");
        return false;
    }

    Log::info("tray icon registered as " + serviceName_);
    return true;
}

void TrayIcon::handleBusSignal(std::string_view interface, std::string_view member) {
    if (interface != "org.freedesktop.DBus" || member != "NameOwnerChanged") return;

    // The signal's arguments are not surfaced by the wrapper, so this fires on
    // both the watcher appearing and disappearing. Re-registering when it has
    // gone is a call that fails harmlessly; not re-registering when it has
    // arrived would lose the icon until the next restart.
    const bool nowRegistered = registerWithWatcher();
    if (nowRegistered && !registered_) Log::info("a tray host appeared — icon restored");
    registered_ = nowRegistered;
}

void TrayIcon::refresh() {
    if (!registered_ || engine_ == nullptr) return;

    const std::string title = currentTitle();
    if (title == lastTitle_) return;
    lastTitle_ = title;

    dbus::Bus* bus = dbus::Bus::session();
    if (bus == nullptr) return;
    // NewTitle carries no payload; the host re-reads the property. That is the
    // protocol, and it is why the title is computed in the Get handler too.
    bus->emitSignal(kItemPath, kItemInterface, "NewTitle");
}

void TrayIcon::cycleWallpaper() {
    if (engine_ == nullptr) return;

    Library& library = engine_->library();
    const std::vector<WallpaperItem>& items = library.items();
    if (items.empty()) {
        Log::info("tray click with an empty library — nothing to cycle to");
        return;
    }

    const std::string current = library.selectedId();
    const auto hit = std::find_if(items.begin(), items.end(), [&current](const WallpaperItem& item) {
        return item.id == current;
    });

    // Past the end wraps to the gradient rather than straight back to the first
    // item, so the procedural mode stays reachable by click.
    if (hit == items.end()) {
        engine_->select(items.front().id);
        return;
    }
    const size_t next = static_cast<size_t>(hit - items.begin()) + 1;
    engine_->select(next < items.size() ? items[next].id : std::string());
}

void TrayIcon::cycleFitMode() {
    if (engine_ == nullptr) return;

    switch (engine_->library().fitMode()) {
        case FitMode::Fill: engine_->setFitMode(FitMode::Fit); break;
        case FitMode::Fit: engine_->setFitMode(FitMode::Stretch); break;
        case FitMode::Stretch: engine_->setFitMode(FitMode::Fill); break;
    }
}

}  // namespace livewall
