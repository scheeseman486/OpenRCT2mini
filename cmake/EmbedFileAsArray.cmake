# OPENRCT2MINI defaults-export P3: helper for embedding a data file
# into the binary as a C++ byte array. Used at build time to bake the
# per-build defaults (config.ini, shortcuts.json, rumble.json under
# config/{appimage,miyoo_mini}/save/) into the openrct2 binary so the
# corresponding loaders can construct defaults from in-memory blobs
# instead of source-baked code paths (DefaultIniReader,
# register*Default chains, hardcoded seedDefaults()).
#
# The actual hex-conversion runs in EmbedFileAsArrayScript.cmake via
# `cmake -P`, dispatched from a per-output add_custom_command. The
# command re-runs whenever the INPUT_FILE changes (or the script
# itself), so editing a seed file under config/<build>/save/ triggers
# a recompile of whatever TU includes the generated header.
#
# Caller contract:
#   embed_file_as_array(input_file output_header symbol_name)
# Effects:
#   * Registers a custom_command producing ${output_header}.
#   * Generated header contains:
#       namespace OpenRCT2 {
#           inline constexpr unsigned char ${symbol_name}[] = { ... };
#           inline constexpr std::size_t ${symbol_name}Size = N;
#       }
#   * Internal-linkage-by-default (inline constexpr in namespace
#     scope), so multiple TUs may include it without ODR trouble.

function(embed_file_as_array INPUT_FILE OUTPUT_HEADER SYMBOL_NAME)
    add_custom_command(
        OUTPUT  "${OUTPUT_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
                "-DINPUT_FILE=${INPUT_FILE}"
                "-DOUTPUT_HEADER=${OUTPUT_HEADER}"
                "-DSYMBOL_NAME=${SYMBOL_NAME}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFileAsArrayScript.cmake"
        DEPENDS "${INPUT_FILE}"
                "${CMAKE_SOURCE_DIR}/cmake/EmbedFileAsArrayScript.cmake"
        COMMENT "Embedding ${INPUT_FILE} as ${SYMBOL_NAME}[]"
        VERBATIM
    )
endfunction()
