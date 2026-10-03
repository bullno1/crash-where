# crash-where [![CI](https://github.com/bullno1/crash-where/actions/workflows/ci.yml/badge.svg)](https://github.com/bullno1/crash-where/actions/workflows/ci.yml)

A painless crash reporter.

(still under development)

## Motivation

Crash reporting should be as simple as:

```c
#include <cw.h>  // The library
#include <cw_http.h>

int
main(int argc, char** argv) {
    // Initialize the library
    cw_config_t cfg = {
        .app = "my-app",  // Name of the application
        .version = "0.0.1",  // Version
        .endpoint = "http://localhost",  // Where to report
        .transport = &cw_transport_http,  // How to reach it
    };
    cw_init(&cfg);

    // The rest of the application code

    // Shutdown
    cw_shutdown(exit_code);
    return exit_code;
}
```

That's it.
No separate binary to bundle, your executable is the crash handler and uploader (TODO: explain why).
No separate platform-specific code path, just `cw_init` and `cw_shutdown`.

Symbolication and aggregation is handled in a separate service (TODO: link).
