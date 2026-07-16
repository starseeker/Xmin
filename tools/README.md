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
