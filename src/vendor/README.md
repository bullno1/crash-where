# Vendored third-party code

Files here are copied verbatim from upstream and keep their own formatting.
Update by replacing the file and the line below.

| File | Source | Commit | License |
|------|--------|--------|---------|
| `barray.h` | https://github.com/bullno1/libs | eb56fc8dcc6a (2026-09-23) | Unlicense (public domain) |
| `bhash.h` | https://github.com/bullno1/libs | eb56fc8dcc6a (2026-09-23) | Unlicense (public domain) |
| `barg.h` | https://github.com/bullno1/libs | e58514f6989f (2026-09-05) | Unlicense (public domain) |
| `sdefl.h` | https://github.com/vurtun/lib | d66260b1e7eb (2025-03-15) | MIT or Unlicense (public domain) |

`sdefl.h` carries one local change, pending upstream: an `SDEFL_API`
macro, `extern` by default, on its three public functions, so the one
file that compiles the implementation can make them `static` and keep
them out of the library's symbols.
