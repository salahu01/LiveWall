# Turns a text file into `inline constexpr const char SYMBOL[]`.
#
# Run in script mode by livewall_embed_text(). Kept separate rather than done
# with configure_file() because the contents have to be escaped, and a raw
# string literal is not an option — GLSL contains `)"` often enough that the
# delimiter would have to be chosen per file.

file(READ "${INPUT}" contents)

string(REPLACE "\\" "\\\\" contents "${contents}")
string(REPLACE "\"" "\\\"" contents "${contents}")
string(REPLACE "\n" "\\n\"\n    \"" contents "${contents}")

get_filename_component(name "${INPUT}" NAME)

file(WRITE "${OUTPUT}"
"// Generated from ${name}. Do not edit.
#pragma once

namespace livewall {
inline constexpr const char ${SYMBOL}[] =
    \"${contents}\";
}  // namespace livewall
")
