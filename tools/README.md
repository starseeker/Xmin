# Maintenance tools

Generated output is committed, so normal builds need neither Python nor any
upstream source tree.

Regenerate the protocol contract and core opcode declarations after changing
`profiles/protocol.json` or `protocol/xcb/*.xml`:

```sh
python3 tools/generate-protocol-coverage.py \
  profiles/protocol.json protocol/xcb profiles/protocol-coverage.json \
  --cpp-header src/server/include/xmin/server/generated/core_protocol.hpp
```

Policy entries use `status`, optional `partial_opcodes`, and an optional
`compatibility_status` for compatibility opcode slices. Configure verifies
the policy/XML hashes and fails on stale output.

Regenerate compact constexpr font tables from the retained PCF data with:

```sh
python3 tools/generate-core-fonts.py data/fonts/xmin_builtin_fonts.c \
  src/server/include/xmin/server/generated/core_fonts.hpp
```

`generate-builtin-fonts.py` recreates the intermediate font data from the
pinned libXfont2 release. `import-pixman.sh` and `import-osmesa.sh` refresh the
two source dependencies recorded in `UPSTREAM.toml`.

Refresh the optional toolkit client's parser and embedded font files from
verified upstream checkouts with:

```sh
tools/import-struetype.sh /path/to/struetype
tools/import-go-fonts.sh /path/to/go-image
```

`import-go-fonts.sh` verifies the exact eight-file hash manifest in
`tools/go-font-files.sha256`. The normal build embeds those checked-in TTFs
with `cmake/EmbedGoFonts.cmake`; it performs no network or font search.
