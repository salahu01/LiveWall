#include "support/Settings.h"

#include "support/Log.h"
#include "support/Paths.h"

namespace livewall {

Settings::Settings() {
    const std::string text = paths::readFile(paths::settingsFile());
    if (text.empty()) return;

    JsonValue parsed = parseJson(text);
    if (!parsed.isObject()) {
        // Deliberately not fatal and deliberately not backed up the way the
        // index is: preferences are re-derivable by clicking four menu items,
        // whereas a lost index means lost transcodes.
        Log::error("settings.json is unreadable; starting from defaults");
        return;
    }
    root_ = std::move(parsed);
}

void Settings::save() const {
    paths::writeFileAtomically(paths::settingsFile(), root_.serialize(/*pretty=*/true));
}

std::string Settings::stringValue(std::string_view key, std::string fallback) const {
    if (!root_.has(key)) return fallback;
    const JsonValue& value = root_[key];
    return value.type() == JsonValue::Type::String ? value.stringValue() : fallback;
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

}  // namespace livewall
