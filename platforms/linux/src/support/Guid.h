// Stable identifiers for library items.
//
// The macOS index stores Foundation UUIDs in their canonical uppercase
// hyphenated form. The same format is used here so that an index written by
// either app is readable by the other — the library format is deliberately
// portable even though the media files are not.
#pragma once

#include <string>
#include <string_view>

namespace livewall {

// Uppercase, hyphenated, no braces: "3F2504E0-4F89-41D3-9A0C-0305E82C3301".
std::string newGuidString();

// True when `text` is that exact shape. Anything else in the index is a row
// this build did not write and cannot key on.
bool isGuidString(std::string_view text);

}  // namespace livewall
