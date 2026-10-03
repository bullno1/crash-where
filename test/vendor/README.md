# Vendored third-party code

Files here are copied verbatim from upstream and keep their own formatting.
Update by replacing the file and the line below.

| File | Source | Commit | License |
|------|--------|--------|---------|
| `autolist.h` | https://github.com/bullno1/libs | eb56fc8dcc6a (2026-09-23) | Unlicense (public domain) |
| `btest.h` | https://github.com/bullno1/libs | eb56fc8dcc6a (2026-09-23) | Unlicense (public domain) |
| `blog.h` | https://github.com/bullno1/libs | eb56fc8dcc6a (2026-09-23) | Unlicense (public domain) |
| `yyjson.h`, `yyjson.c` | https://github.com/ibireme/yyjson | b21c02904188 (0.9.0, 2024-04-08) | MIT |
| `wby.h` | https://github.com/bullno1/slopnet (`src/wby.h`) | 5670f498fdb6 (2025-12-18) | BSD-2-Clause |
| `sinfl.h` | https://github.com/vurtun/lib | 5a3f3aba052e (2025-03-19) | MIT or Unlicense (public domain) |

`wby.h` carries two local fixes to its Windows build, pending upstream:
`wby_start` used `socklen_t`, which Winsock lacks (now `wby_socklen`), and
`errno` without `<errno.h>`.
