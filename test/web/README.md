# Web test harness

Runs `cw_test`, the same test binary as on every other platform, inside a headless browser.
Nothing here is a second test suite: the tests are the shared ones under `test/`, and this directory supplies what `test/platform.h` needs from a browser.

```
cmd/emscripten/test [suite [test]]
```

## Files

| File | Role |
|------|------|
| `launch.js` | Node script. Serves the build, starts the browser, relays output, runs the servers the transport tests send to, exits with the page's status. |
| `runner.html` | The page that runs the suites. |
| `child.html` | The page that runs one scenario, in a frame of the runner. |
| `testlib.js` | Emscripten JS library linked into `cw_test`: spawn, exit detection, files. |
| `http_server.c` | `test/http_server.h` for the web, on top of the launcher's servers. |
| `skip` | Tests the run script leaves out, with reasons. |
| `../web.c` | The C side: `test/platform.h` for the web. |

The library's own shim is `src/web/shim.js`.
It is not test code, but a child cannot run without it.

## One run

```
cmd/emscripten/test
└─ node launch.js              one per browser
   ├─ HTTP server on 127.0.0.1, any free port
   └─ headless browser
      └─ runner.html           cw_test as the runner, shim off
         └─ child.html         cw_test as the game, one frame per scenario
            └─ Worker          cw_test as the watcher, started by the shim
```

1. `cmd/emscripten/test` builds, then starts `launch.js` once for each browser.
2. `launch.js` opens `runner.html` in the browser.
   The suite and test filters and every `CW_TEST_*` variable of its own environment travel in the URL.
3. The runner page loads `cw_test.js` with `cwRole: 'none'`, which keeps the shim out, and `noInitialRun`.
   Once the runtime is up it calls `main` itself through `Module.testRun`.
4. `main` runs the suites as on any platform.
   A test that does not spawn a child runs entirely inside this one instance.
5. When `main` returns, the page posts the value to the launcher, which exits with it.
   That value is the number of failed tests.

## How the runner blocks

`test_spawn_self` must not return until the child is gone, and a page cannot block.
The runner uses JavaScript Promise Integration:

- `testlib.js` replaces the import `test_web_spawn` with a `WebAssembly.Suspending` wrapper when it instantiates the runner.
- `Module.testRun` enters `main` through `WebAssembly.promising`.
  It takes `main` from the function table, by way of `test_web_main()`, because an export may be wrapped in JS and a wrapper cannot be made promising.

The C code sees an ordinary blocking call.
The page's event loop keeps running meanwhile, which is how the child's messages arrive.

No JS frame may sit between `main` and the suspending import.
That is why `cw_test` is built with `-sSUPPORT_LONGJMP=wasm`: the test framework uses `setjmp`, and Emscripten's default implementation routes every call under it through JS.

## One child

`test_web_spawn` creates a hidden frame on `child.html`, with the extra environment variables as JSON in the URL fragment.
The child:

1. Puts those variables into `ENV` before `main`.
2. Runs `main`, which sees `CW_TEST_SCENARIO` and becomes the fixture.
3. Calls `cw_init`.
   The shim has already started a watcher, a Worker running this same script, and passes it the child's `ENV`.

A child gets only the variables the runner passes.
It does not inherit the runner's, unlike a child process.

A child ends in one of four ways:

| What happens | Reported as |
|---|---|
| It calls `exit`, or `main` returns | That exit status |
| It traps and the shim reports it | A signal, once the watcher is done |
| An error nobody catches kills it | A signal |
| None of these within 30 s | The spawn fails |

A page has no signals, and the shared tests expect them.
A trap whose message starts with `Aborted` is reported as `SIGABRT`, any other as `SIGSEGV`.

An exit waits for the watcher when a `cw_shutdown` is still with it.
Exit statuses reach the page through `Module.onExit`, which Emscripten only calls when built with `-sEXIT_RUNTIME=1`.

## Files

The runner, the game and the watcher are three instances with three separate in-memory filesystems.
The shared tests assume one disk: the watcher writes `events.jsonl` and the report directory, and the runner reads them.

When a child ends, it asks its watcher for every directory and file under `CW_TEST_OUT`, allowing it 5 s, and sends them to the runner with the exit status.
The runner recreates them in its own filesystem at the same paths, so `read_events`, `test_run_has` and `test_run_pending` work unchanged.

A child run with `CW_DISABLE=1` has no watcher, so nothing comes back.

Only the watcher's files come back.
The game's own copy of the store does not.
Nothing travels the other way either: a child's store always starts empty, so tests that keep a report directory from one child to the next cannot pass yet.

The fixtures under `test/fixtures` is preloaded into the test.

The runner also posts each file to the launcher, which writes it under `.build/emscripten/<config>/test/work/<browser>/`.
For inspection, the event log of a test is then at `.build/emscripten/<config>/test/work/<browser>/work/<test>/events.jsonl`.

## HTTP servers

The launcher contains a http server for all transport-related tests.
The requests are recorded so tests can retrieve and assert on them.

## Output

| From | Path |
|---|---|
| Runner | `print` and `printErr` post to the launcher |
| Game | `print` and `printErr` post to the runner page, which posts to the launcher |
| Watcher | The shim forwards each line to its game, then as above |

Lines are posted one at a time through a queue, so they arrive in order.
The launcher prints every one on its **stdout**.
The distinction between `stdout` and `stderr` is lost on the way, although the test log itself is written to `stderr` in C.
The launcher's own messages go to its stderr.

## Launcher

```
node launch.js --bin <dir> --work <dir> --browser <name>[=<command>] [suite [test]]
```

It needs nothing beyond Node.
It serves `runner.html` and `child.html` from this directory and everything else by base name from `<dir>` of `--bin`, with caching off.
The page talks back through these requests:

| Request | Effect |
|---|---|
| `POST /log` | Print the body as one line |
| `POST /file/<path>` | Write the body to `<work>/<path>` |
| `POST /exit?code=<n>` | Stop the browser and exit with `<n>` |
| `POST /http/start` | Start a server that answers as the JSON body says; reply with its id and URL |
| `GET /http/<id>/count` | Number of requests that server has answered |
| `GET /http/<id>/request/<n>` | The `<n>`th request it recorded, as JSON |
| `POST /http/<id>/stop` | Close that server |

The browser gets a fresh profile under `<work>`, removed afterwards.
Chromium runs without its sandbox, since containers and CI runners cannot create one.

| Exit status | Meaning |
|---|---|
| 0 | Every test that ran passed |
| 1 and up | Number of failed tests |
| 70 | The runner itself threw |
| 124 | The page was silent for 60 s and never posted a status |
| 127 | The browser could not be started |

## Environment

| Variable | Meaning |
|---|---|
| `CW_TEST_BROWSERS` | Browsers to run in, separated by spaces: `chromium`, `firefox`, each optionally `<name>=<command>`. Unset: each of them found on `PATH`. |
| `CW_TEST_SKIP` | Tests to leave out, as `suite/test` separated by spaces. Unset: the contents of `skip`. Set to nothing: run everything. |
| `CW_TEST_WEB_TRANSPORT` | `xhr` makes the transport send synchronously, the way it does in a browser that cannot suspend. Unset: `fetch`. |

`CW_TEST_SKIP` is read by `test/main.c` and works on every platform.
A test named on the command line runs even when listed.

With several browsers the script runs them in turn and exits with the last nonzero status.

A run of everything, with no suite named, also runs the `http` suite a second time per browser with `CW_TEST_WEB_TRANSPORT=xhr`.

## What the platform cannot do

`test/web.c` answers `false` where the web has no equivalent, and the tests that depend on it are in `skip`:

- `test_run_thread`: the build has no threads.
- `test_sockets_init`: a page has no sockets.
  The HTTP server the tests need comes from the launcher instead.
- `test_stop_self` and `test_debug_self`: a page can neither stop itself nor attach a debugger to itself.
- `test_image_base` and `TEST_RETURN_ADDRESS` are 0.
  A Wasm function cannot read its return address, so frames are not checked against recorded addresses.

## Requirements and limits

- Node, and Chromium or Firefox.
  The browser must support JavaScript Promise Integration: Chrome 137 or Firefox 153, and later.
  The launcher knows how to start no other browser.
- A browser that dies takes its status with it.
  The launcher then waits out the 60 s and exits with 124.
- There is no mode that leaves the page open for developer tools.
