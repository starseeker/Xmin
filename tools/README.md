# Maintenance tools

Source refresh, XKB generation, embedded-font generation, and provenance-checking
tools belong here. Their output must be committed so the normal build remains
offline and requires only CMake and the selected compilers.

`generate-protocol-coverage.py` expands `profiles/protocol.json` and the
retained xcb-proto XML into the committed request-level coverage report. Run:

```sh
python3 tools/generate-protocol-coverage.py \
  profiles/protocol.json protocol/xcb profiles/protocol-coverage.json \
  --cpp-header src/next/include/xmin/next/generated/core_protocol.hpp
```

The generated header is a normal committed C++17 input; Python is not needed by
configure or build.
Extension policy entries may set an extension-wide `next_status` and may list
`next_partial_opcodes`; explicit partial request slices override the extension default.
Compatibility subsets may use `next_compatibility_status` when their migration state
differs from the extension's required request set.

`generate-core-fonts.py` converts the pinned uncompressed PCF arrays into the
compact glyph/metric tables used by the C++ server, avoiding a runtime PCF
parser and libXfont dependency:

```sh
python3 tools/generate-core-fonts.py data/fonts/xmin_builtin_fonts.c \
  src/next/include/xmin/next/generated/core_fonts.hpp
```

`capture-next-core-keymap.c` converts the legacy oracle's core view of the pinned
`xmin-us.xkb` map and input defaults into the committed C++17 keymap header.  It is
a migration-time maintenance utility, not a product build dependency.  Start the
legacy server with its embedded map, compile the utility against libxcb, and write
its standard output to
`src/next/include/xmin/next/generated/core_keymap.hpp`.
