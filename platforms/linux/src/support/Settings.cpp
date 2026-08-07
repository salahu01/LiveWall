#include "support/Settings.h"

#include "support/Log.h"
#include "support/Paths.h"

namespace livewall {

Settings::Settings() {
    const std::string text = paths::readFile(paths::settingsFile());
    if (text.empty()) return;

    JsonValue parsed = parseJson(text);
    if (!parsed.isObject()) {
        // Unlike the index, a damaged settings file costs the user nothing to
        // recreate — six values they can set again — so it is replaced rather
        // than preserved. Saying so matters more than keeping it.
        Log::error("settings.json is not readable; starting from defaults");
        return;
    }
    root_ = std::move(parsed);
}

std::string Settings::stringValue(std::string_view key, std::string fallback) const {
    const JsonValue& value = root_[key];
    if (value.type() != JsonValue::Type::String) return fallback;
    return value.stringValue();
}

bool Settings::boolValue(std::string_view key, bool fallback) const {
    if (!root_.has(key)) return fallback;
    return root_[key].boolValue(fallback);
}

long long Settings::intValue(std::string_view key, long long fallback) const {
    if (!root_.has(key)) return fallback;
    return root_[key].intValue(fallback);
}

void Settings::setString(std::string_view key, std::string value) {
    root_.set(std::string(key), JsonValue(std::move(value)));
    save();
}

void Settings::setBool(std::string_view key, bool value) {
    root_.set(std::string(key), JsonValue(value));
    save();
}

void Settings::setInt(std::string_view key, long long value) {
    root_.set(std::string(key), JsonValue(static_cast<std::int64_t>(value)));
    save();
}

void Settings::remove(std::string_view key) {
    root_.erase(key);
    save();
}

void Settings::save() const {
    paths::writeFileAtomically(paths::settingsFile(), root_.serialize(true) + "\n");
}

}  // namespace livewall
