/**
 * @file host/web.c
 * Web backend: what the browser tells about itself and the machine.
 *
 * The facts are as the browser words them, which is already coarse. The
 * GPU is recorded at init, from a WebGL context made for the purpose,
 * because the watcher is a worker and has no view of the page.
 */
#include <stdbool.h>

#include "backend.h"
#include "cw.h"
#include "internal.h"

/** A string fact of the page, for cw_host_web_fact(). */
typedef enum {
	CW_HOST_WEB_BROWSER  = 0, /**< Product tokens of the user agent after its first comment. */
	CW_HOST_WEB_PLATFORM = 1, /**< First comment of the user agent: the operating system. */
	CW_HOST_WEB_LOCALE   = 2, /**< Preferred language, as a language tag. */
	CW_HOST_WEB_GPU      = 3, /**< Renderer of a WebGL context. */
} cw_host_web_fact_t;

/**
 * Shim: one string fact.
 *
 * @return `false` when the browser has none or it does not fit.
 */
bool
cw_host_web_fact(int what, char* out, size_t cap);

/** Shim: logical processors; 0 when unknown. */
uint32_t
cw_host_web_cpu_cores(void);

/** Shim: memory of the device as the browser rounds it; 0 when it says nothing. */
uint32_t
cw_host_web_ram_mb(void);

/** Shim, watcher side: linear memory of the game when it trapped; 0 when unknown. */
uint32_t
cw_host_web_game_rss_mb(void);

static void
put_fact(const char* key, cw_host_web_fact_t what) {
	char buf[128];
	if (cw_host_web_fact((int)what, buf, sizeof(buf))) {
		cw_set_env(key, buf);
	}
}

void
cw_host_backend_init(void) {
	cw_set_env("os", "web");
	/* The browser stands between the binary and the operating system, as Wine does. */
	put_fact("compat", CW_HOST_WEB_BROWSER);
	put_fact("compat_host", CW_HOST_WEB_PLATFORM);
#if defined(__wasm64__)
	cw_set_env("arch", "wasm64");
#else
	cw_set_env("arch", "wasm32");
#endif
	uint32_t cores = cw_host_web_cpu_cores();
	if (cores > 0) {
		cw_host_set_u64("cpu_cores", cores);
	}
	uint32_t ram = cw_host_web_ram_mb();
	if (ram > 0) {
		cw_host_set_u64("ram_mb", ram);
	}
	put_fact("locale", CW_HOST_WEB_LOCALE);
	if (!cw_env_has("gpu")) {
		put_fact("gpu", CW_HOST_WEB_GPU);
	}
}

void
cw_host_backend_report(uint32_t pid) {
	(void)pid;
	uint32_t rss = cw_host_web_game_rss_mb();
	if (rss > 0) {
		cw_host_set_u64("rss_mb", rss);
	}
}
