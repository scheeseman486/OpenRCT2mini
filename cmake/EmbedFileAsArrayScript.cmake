# OPENRCT2MINI defaults-export P3: worker for EmbedFileAsArray.cmake.
# Invoked as `cmake -P` with -D vars INPUT_FILE / OUTPUT_HEADER /
# SYMBOL_NAME. Reads the input file as hex, formats as a C++ unsigned
# char array, writes the result to OUTPUT_HEADER.
#
# Why this is its own script (vs. inline in EmbedFileAsArray.cmake):
# add_custom_command needs an actual command to invoke at BUILD time,
# not CMake configuration time. `cmake -P scriptfile` is the
# portable way to ship a tiny build-time script that uses CMake's
# own primitives (file(READ ... HEX), string regex) without needing
# a separate language or xxd / bin2c.

if (NOT INPUT_FILE OR NOT OUTPUT_HEADER OR NOT SYMBOL_NAME)
    message(FATAL_ERROR "EmbedFileAsArrayScript requires INPUT_FILE, OUTPUT_HEADER, SYMBOL_NAME")
endif ()

# file(READ ... HEX) returns lowercase hex with no separators —
# e.g. an input "AB" produces "4142". HEX is the toolchain-agnostic
# alternative to xxd / bin2c and works on every CMake we'd target.
# The HEX keyword is positional and MUST follow the output variable
# name; omitting it (which I did the first pass) causes file(READ)
# to read text instead, and the regex below silently truncates.
file(READ "${INPUT_FILE}" _HEX HEX)
string(LENGTH "${_HEX}" _HEX_LEN)
math(EXPR _BYTE_COUNT "${_HEX_LEN} / 2")

# Two-pass formatting: first inject "0x" prefix + ", " separator per
# byte, then chunk into 16-byte lines so diffs of the generated
# header are readable when someone reviews the build artifact. The
# generated file lives under ${CMAKE_BINARY_DIR}/embedded_defaults/
# and isn't committed — line-wrapping is purely for sanity when
# debugging the embedding itself.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _BYTES "${_HEX}")
# Strip trailing ", " on the last byte.
string(REGEX REPLACE ", $" "" _BYTES "${_BYTES}")
# Wrap every 16 bytes (16 * "0xXX, " = 96 chars) onto its own line.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f], ){16})" "\\1\n    " _BYTES "${_BYTES}")

get_filename_component(_IN_NAME "${INPUT_FILE}" NAME)

file(WRITE "${OUTPUT_HEADER}"
"// AUTO-GENERATED — DO NOT EDIT.\n"
"// Source: ${_IN_NAME} (${_BYTE_COUNT} bytes)\n"
"// Generator: cmake/EmbedFileAsArrayScript.cmake\n"
"// Regenerated whenever the source file changes; see\n"
"// cmake/EmbedFileAsArray.cmake for the add_custom_command wiring.\n"
"#pragma once\n"
"#include <cstddef>\n"
"namespace OpenRCT2 {\n"
"inline constexpr unsigned char ${SYMBOL_NAME}[] = {\n"
"    ${_BYTES}\n"
"};\n"
"inline constexpr std::size_t ${SYMBOL_NAME}Size = ${_BYTE_COUNT};\n"
"} // namespace OpenRCT2\n"
)
