#include "support/Strings.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace livewall {

std::string formatBytes(std::int64_t bytes) {
    if (bytes < 1000) return std::to_string(bytes) + " bytes";

    static const char* const kUnits[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes) / 1000.0;
    int unit = 0;
    while (value >= 1000.0 && unit + 1 < 4) {
        value /= 1000.0;
        ++unit;
    }
    // One decimal below 100, none above — "9.4 MB" but "418 MB", which is what
    // both of the other two platforms' formatters do.
    return format(value < 100 ? "%.1f %s" : "%.0f %s", value, kUnits[unit]);
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') left = static_cast<char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = static_cast<char>(right - 'A' + 'a');
        if (left != right) return false;
    }
    return true;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string_view trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t hit = text.find(separator, start);
        const size_t stop = hit == std::string_view::npos ? text.size() : hit;
        if (stop > start) parts.emplace_back(text.substr(start, stop - start));
        if (hit == std::string_view::npos) break;
        start = hit + 1;
    }
    return parts;
}

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);

    std::string out;
    if (needed > 0) {
        out.resize(static_cast<size_t>(needed));
        std::vsnprintf(out.data(), static_cast<size_t>(needed) + 1, fmt, args);
    }
    va_end(args);
    return out;
}

std::string iso8601Now() {
    const std::time_t now = std::time(nullptr);
    std::tm utc = {};
    ::gmtime_r(&now, &utc);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

}  // namespace livewall
