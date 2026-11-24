# Third-party notices — Handkerchief Vita

- **sh3vita**, matfsouza: [source](https://github.com/matfsouza/sh3vita), GPL version 3.
  `recomp/x86rec.py`, `recomp/pe.py`, `recomp/difftest.py` and `port/rt/x86rt.h` start from
  sh3vita commit `2e43f62`. Local changes are described in each file header. See `LICENSE`.
- **sh4vita**, vallfrr: [source](https://github.com/vallfrr/sh4vita), GPL version 3. sh3vita's
  recompiler and runtime start from sh4vita.

These notices do not claim ownership of Handkerchief, its original executable, resources, or
generated translations. The C generated from `HDKWIN.EXE` (`build/hk-recomp/gen/`) is derived
from the commercial game and is never committed or distributed. Players supply their own copy.

- **Liberation Sans**, Red Hat and contributors: [source](https://github.com/liberationfonts/liberation-fonts),
  SIL Open Font License 1.1. `port/hk/font_data.h` holds 1-bit glyph bitmaps rendered from Liberation Sans
  Regular by `tools/make_font.py`, standing in for the MS Sans Serif text the game draws through GDI.
