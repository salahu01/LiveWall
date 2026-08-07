#include "app/WallpaperEngine.h"

#include <algorithm>
#include <ctime>

#include "render/EglDevice.h"
#include "render/GradientSource.h"
#include "render/VideoSource.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

std::int64_t monotonicMs() {
    timespec now = {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// Long enough that an idle app is genuinely idle, short enough that the power
// gates and the visibility latch are re-evaluated at a human timescale even
// when nothing else wakes the loop.
constexpr int kMaxSleepMs = 1000;

}  // namespace

WallpaperEngine::WallpaperEngine(Backend& backend, EglDevice& egl)
    : backend_(backend), egl_(egl) {}

WallpaperEngine::~WallpaperEngine() = default;

void WallpaperEngine::start() {
    fitMode_ = library_.fitMode();

    power_.setPauseOnBattery(library_.pauseOnBattery());
    power_.displayAsleepProbe = [this]() { return backend_.displayAsleep(); };
    power_.idleMillisProbe = [this]() { return backend_.idleMillis(); };
    power_.onChange = [this]() {
        const std::int64_t now = monotonicMs();
        for (auto& controller : controllers_) controller->evaluate(now);
        if (onStateChange) onStateChange();
    };
    power_.start();

    syncOutputs();
    applySelection();

    if (!backend_.supportsOcclusion()) {
        Log::info("this backend cannot see other windows — the wallpaper will keep decoding "
                  "while covered unless the compositor stops asking for frames");
    }
}

void WallpaperEngine::syncOutputs() {
    const std::vector<OutputInfo> current = backend_.outputs();

    // Drop controllers whose output is gone.
    controllers_.erase(
        std::remove_if(controllers_.begin(), controllers_.end(),
                       [&current](const std::unique_ptr<OutputController>& controller) {
                           const bool stillThere =
                               std::any_of(current.begin(), current.end(),
                                           [&controller](const OutputInfo& info) {
                                               return info.id == controller->info().id;
                                           });
                           if (!stillThere) Log::info(controller->info().id + " went away");
                           return !stillThere;
                       }),
        controllers_.end());

    std::vector<OutputController*> added;

    for (const OutputInfo& info : current) {
        const auto hit = std::find_if(controllers_.begin(), controllers_.end(),
                                      [&info](const std::unique_ptr<OutputController>& controller) {
                                          return controller->info().id == info.id;
                                      });
        if (hit != controllers_.end()) {
            // Geometry only. The decoder keeps running.
            if (!((*hit)->info() == info)) (*hit)->updateInfo(info);
            continue;
        }

        auto controller = std::make_unique<OutputController>(info, backend_, egl_, power_);
        if (!controller->createSurface()) {
            Log::error("could not create a wallpaper surface on " + info.id);
            continue;
        }
        Log::info(format("%s: %dx%d at %d,%d, %d Hz, scale %.0f", info.id.c_str(), info.pixelWidth,
                         info.pixelHeight, info.x, info.y, info.refreshHz, info.scale));
        controllers_.push_back(std::move(controller));
        added.push_back(controllers_.back().get());
    }

    for (OutputController* controller : added) {
        auto source = makeSource(*controller);
        if (source) controller->setSource(std::move(source));
    }

    if (onStateChange) onStateChange();
}

std::unique_ptr<WallpaperSource> WallpaperEngine::makeSource(OutputController& target) {
    if (!target.makeContextCurrent()) {
        Log::error("no GL context on " + target.info().id + " — nothing can be drawn there");
        return nullptr;
    }

    const std::string selected = library_.selectedId();

    if (!selected.empty()) {
        if (const WallpaperItem* item = library_.find(selected); item != nullptr) {
            const std::string path = library_.pathFor(*item);
            auto video = std::make_unique<VideoSource>(path, item->fps, item->bitDepth);
            if (video->prepare(egl_)) {
                fellBackToProcedural_ = false;
                return video;
            }
            Log::error("could not prepare " + item->title + "; falling back to the gradient");
            fellBackToProcedural_ = true;
        } else {
            Log::error("the selected wallpaper is not in the library any more");
            library_.setSelectedId({});
        }
    }

    auto gradient = std::make_unique<GradientSource>(library_.proceduralFps());
    if (!gradient->prepare(egl_)) {
        Log::error("even the gradient will not compile — there will be no wallpaper");
        return nullptr;
    }
    return gradient;
}

void WallpaperEngine::applySelection() {
    for (auto& controller : controllers_) {
        auto source = makeSource(*controller);
        if (source) controller->setSource(std::move(source));
    }
    if (onStateChange) onStateChange();
}

void WallpaperEngine::select(const std::string& itemId) {
    library_.setSelectedId(itemId);
    fellBackToProcedural_ = false;
    applySelection();
}

void WallpaperEngine::setFitMode(FitMode mode) {
    fitMode_ = mode;
    library_.setFitMode(mode);
    // Applied to live sources in place — no reload, no decode interruption.
    // Every controller picks it up on its next frame.
    if (onStateChange) onStateChange();
}

void WallpaperEngine::setPauseOnBattery(bool value) {
    library_.setPauseOnBattery(value);
    power_.setPauseOnBattery(value);
    if (onStateChange) onStateChange();
}

int WallpaperEngine::tick() {
    const std::int64_t now = monotonicMs();

    power_.poll();

    bool anyRenderingBefore = false;
    for (auto& controller : controllers_) {
        anyRenderingBefore = anyRenderingBefore || controller->isRendering();
        controller->evaluate(now);
        controller->tick(now, fitMode_);
    }

    // The state line is what the tray shows; recomputing it on every tick would
    // be a string built ten times a second for nobody. Only a change in whether
    // anything is rendering can change it without some other event already
    // having fired.
    bool anyRenderingAfter = false;
    for (auto& controller : controllers_) {
        anyRenderingAfter = anyRenderingAfter || controller->isRendering();
    }
    if (anyRenderingBefore != anyRenderingAfter && onStateChange) onStateChange();

    std::int64_t earliest = 0;
    for (auto& controller : controllers_) {
        const std::int64_t deadline = controller->nextDeadlineMs();
        if (deadline == 0) continue;
        earliest = earliest == 0 ? deadline : std::min(earliest, deadline);
    }

    const int powerInterval = power_.pollIntervalMs();
    if (earliest == 0) return std::min(powerInterval, kMaxSleepMs);

    const std::int64_t wait = earliest - monotonicMs();
    return static_cast<int>(std::clamp<std::int64_t>(wait, 0, std::min(powerInterval, kMaxSleepMs)));
}

bool WallpaperEngine::isAnyRendering() const {
    return std::any_of(controllers_.begin(), controllers_.end(),
                       [](const std::unique_ptr<OutputController>& c) { return c->isRendering(); });
}

std::string WallpaperEngine::statusLine() const {
    if (const std::optional<std::string> reason = power_.blockReason(); reason.has_value()) {
        return "Paused — " + *reason;
    }
    if (controllers_.empty()) return "No displays";
    if (isAnyRendering()) {
        return fellBackToProcedural_ ? "Rendering — the selected video could not be opened"
                                     : "Rendering";
    }

    const bool nearlyCovered =
        std::any_of(controllers_.begin(), controllers_.end(),
                    [](const std::unique_ptr<OutputController>& c) { return c->isNearlyCovered(); });
    return nearlyCovered ? "Paused — desktop nearly covered" : "Paused — desktop covered";
}

DisplayTarget WallpaperEngine::displayTarget() const {
    DisplayTarget target;
    std::int64_t largest = 0;

    for (const auto& controller : controllers_) {
        const OutputInfo& info = controller->info();
        const std::int64_t pixels = static_cast<std::int64_t>(info.pixelWidth) * info.pixelHeight;
        if (pixels <= largest) continue;
        largest = pixels;
        target.pixelWidth = info.pixelWidth;
        target.pixelHeight = info.pixelHeight;
        target.refreshHz = info.refreshHz > 0 ? info.refreshHz : 60;
    }
    return target;
}

}  // namespace livewall
