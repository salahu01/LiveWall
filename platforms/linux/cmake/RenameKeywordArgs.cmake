# Renames C++ keywords used as parameter names in a wayland-scanner header.
#
# `zwlr_layer_shell_v1_get_layer_surface` takes an argument the protocol calls
# `namespace`. The generated header is valid C and does not compile as C++.
# Only parameter names are affected, so a whole-word rename is safe: these
# headers contain no C++ constructs for the word to collide with.

file(READ "${FILE}" contents)
string(REGEX REPLACE "([^A-Za-z0-9_])namespace([^A-Za-z0-9_])" "\\1name_space\\2" contents "${contents}")
file(WRITE "${FILE}" "${contents}")
