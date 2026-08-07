#include "support/Guid.h"

#include <windows.h>
#include <objbase.h>

#include <cctype>

#include "support/Strings.h"

namespace livewall {

std::string newGuidString() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};

    return format("%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                  guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                  guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

bool isGuidString(std::string_view text) {
    if (text.size() != 36) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const bool hyphenPosition = (i == 8 || i == 13 || i == 18 || i == 23);
        if (hyphenPosition) {
            if (text[i] != '-') return false;
        } else if (std::isxdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace livewall
