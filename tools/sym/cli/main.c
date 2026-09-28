/**
 * @file main.c
 * Entry point: the process environment and the real streams, nothing else.
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"

/**
 * Copy `CWSYM_TOKEN` out of the environment and remove it, so no child
 * process, such as vswhere spawned by the PE reader, inherits it. One
 * trailing newline is dropped: a token pasted from a file often has one.
 *
 * @return The token, or `NULL` when the variable is unset.
 */
static const char*
take_token(void) {
	static char token[4096];
	const char* env = getenv("CWSYM_TOKEN");
	if (env == NULL) {
		return NULL;
	}
	snprintf(token, sizeof(token), "%s", env);
#ifdef _WIN32
	_putenv_s("CWSYM_TOKEN", "");
#else
	unsetenv("CWSYM_TOKEN");
#endif
	size_t len = strlen(token);
	while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r')) {
		token[--len] = '\0';
	}
	return token;
}

int
main(int argc, char* argv[]) {
	cwsym_cli_t cli = {
		.out = stdout,
		.err = stderr,
		.transport = &cw_transport_http,
		.token = take_token(),
	};
	return cwsym_cli_main(&cli, argc, (const char**)argv);
}
