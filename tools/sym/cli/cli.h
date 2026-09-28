/**
 * @file cli.h
 * The `cwsym` command line: a context, a command table, and a dispatcher.
 *
 * `main` is the only piece that reads the process environment or names
 * the real streams; everything a command touches arrives through
 * @ref cwsym_cli_t, so a test drives the dispatcher in-process with a
 * loopback transport and files it reads back.
 */
#ifndef CWSYM_CLI_H
#define CWSYM_CLI_H

#include <stdio.h>

#include <cw_http.h>
#include "sym.h"

/**
 * Exit status of the tool and return value of every command.
 */
typedef enum {
	CWSYM_CLI_OK     = 0,
	CWSYM_CLI_FAILED = 1, /**< The command ran and failed: unreadable input, a refused upload, an offset no row covers. */
	CWSYM_CLI_USAGE  = 2, /**< The command line is wrong; a message and the usage line were printed. */
} cwsym_cli_status_t;

/**
 * What a command runs with. `main` fills it from the process; a test
 * from its own streams and transport.
 */
typedef struct {
	FILE* out;                       /**< Command output. */
	FILE* err;                       /**< Diagnostics, usage, and help. */
	const cw_transport_t* transport; /**< What `upload` sends through. */
	const char* token;               /**< CI bearer token, or `NULL` when none was given. */
	cwsym_read_options_t read;       /**< Reader inputs from the global options; set by the dispatcher. */
} cwsym_cli_t;

/**
 * One command.
 */
typedef struct {
	const char* name;
	const char* summary; /**< One line for the command list. */
	/**
	 * Run the command. `argv[0]` is the command name; the rest are its
	 * own options and positionals.
	 */
	cwsym_cli_status_t (*run)(const cwsym_cli_t* cli, int argc, const char* argv[]);
} cwsym_cli_command_t;

extern const cwsym_cli_command_t cwsym_cli_convert;
extern const cwsym_cli_command_t cwsym_cli_upload;
extern const cwsym_cli_command_t cwsym_cli_dump;
extern const cwsym_cli_command_t cwsym_cli_lookup;
extern const cwsym_cli_command_t cwsym_cli_symbolize;

/**
 * Parse the global options, pick the command named by the first
 * positional, and run it. `argv[0]` is the program name.
 *
 * `cli->read` is overwritten with the parsed global options.
 */
cwsym_cli_status_t
cwsym_cli_main(const cwsym_cli_t* cli, int argc, const char* argv[]);

#endif /* CWSYM_CLI_H */
