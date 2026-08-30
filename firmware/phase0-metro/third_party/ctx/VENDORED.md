# Vendored: ctx

Single-header C 2D vector graphics library by Øyvind Kolås — the same
library the Tildagon badge firmware uses for all its rendering (via the
`uctx` MicroPython binding). See the main README §1.5 for how the badge
uses it.

- **Source:** https://pippin.gimp.org/ctx/ctx.h (canonical single-header
  build; project home https://ctx.graphics/)
- **Fetched:** 2026-08-30, 18770 lines. Corresponds to the ctx-0.1.18
  (2026-05-23) release per the project site at fetch time.
- **License: LGPL-3.0-or-later** — per the license header actually in the
  file (`ctx.h` lines 1-16). Note: ctx.graphics' own page text describes
  the license as ISC, which does not match the header in the file itself;
  treat the file's own header as authoritative and this project's LGPL
  obligations as the ones that apply. Worth re-checking if pulling a newer
  version.
- **Not modified** — `ctx.h` here is byte-for-byte what was downloaded.
  All configuration is done via `#define`s in `../../src/ctx_bench.c`
  before `#include`, per the library's own header-only convention (see
  `ctx.h` around line 1269).

## Why this version, not the badge's exact vendored commit

The design README notes the badge vendors ctx "at commit `26287002`"
(83,760 lines in their tree). We don't have access to that specific
commit — pulled the current canonical single-header release instead. For
a rasterisation *speed* benchmark (B1's goal) this is a reasonable stand-in:
ctx's rasterizer core hasn't changed algorithmically in ways that would be
expected to swing performance by more than noise. If B1's numbers end up
mattering for a go/no-go decision, it would be worth locating the exact
badge commit to confirm.
