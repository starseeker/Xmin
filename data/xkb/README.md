# Embedded keyboard maps

`xmin-us.xkb` is the reviewed recipe for Xmin's fixed US PC keyboard. The import
workflow runs xkbcomp 1.5.0 against pinned xkeyboard-config 2.47, emits XKM, and
converts those bytes into the committed `xmin-us-map.h` array.

At runtime the project-owned loader opens that array as a read-only libc memory
stream and passes it to Xorg's XKM reader. The resulting map uses ordinary XKB heap
ownership and can be queried, copied, modified, and freed normally. No `xkbcomp`,
keyboard database, host X11 library, or temporary keymap file is required by the
normal build or installed server.

To regenerate only this artifact:

```sh
tools/generate-xkb-map.sh /path/to/xkeyboard-config-2.47 \
  data/xkb/xmin-us-map.h
```
