/**
 * @file command.h
 * What the command files share: the option parser, the input loader,
 * and the diagnostics helpers.
 */
#ifndef CWSYM_CLI_COMMAND_H
#define CWSYM_CLI_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "cli.h"
#include "vendor/barg.h"

/**
 * A loaded input: the table plus the file bytes a parsed table borrows.
 */
typedef struct {
	cwsym_table_t table;
	void* buf; /**< File image behind a parsed table, or `NULL` when the table owns its bytes. */
} cwsym_cli_input_t;

/**
 * Log sink that writes each message to `cli->err` with the tool prefix.
 */
cwsym_log_t
cwsym_cli_log(const cwsym_cli_t* cli);

/**
 * Load `path`: a `cwsym` file is parsed in place; anything else goes
 * through the reader and the builder with `cli->read`.
 *
 * @param lines  `false` skips lines and inline sites when reading a debug file.
 * @return ::CWSYM_CLI_OK, or ::CWSYM_CLI_FAILED with the reason logged.
 */
cwsym_cli_status_t
cwsym_cli_load(const cwsym_cli_t* cli, const char* path, bool lines, cwsym_cli_input_t* in);

void
cwsym_cli_unload(cwsym_cli_input_t* in);

/**
 * Write a table's bytes to `path`, replacing any file there.
 *
 * @return ::CWSYM_CLI_OK, or ::CWSYM_CLI_FAILED with the reason logged.
 */
cwsym_cli_status_t
cwsym_cli_write(const cwsym_cli_t* cli, const char* path, const cwsym_table_t* table);

/**
 * Parse a module-relative offset: `0x` hex, else decimal.
 *
 * @return `false` for anything else or a value above `UINT32_MAX`.
 */
bool
cwsym_cli_parse_offset(const char* s, uint32_t* out);

/**
 * Report a parse that did not succeed: the help text to `cli->out` for
 * `--help`, the error and usage line to `cli->err` otherwise.
 *
 * @return ::CWSYM_CLI_OK after help, ::CWSYM_CLI_USAGE after an error.
 */
cwsym_cli_status_t
cwsym_cli_report(const cwsym_cli_t* cli, barg_t* barg, barg_result_t result);

/**
 * Print `msg` and the usage line to `cli->err`.
 *
 * @return ::CWSYM_CLI_USAGE.
 */
cwsym_cli_status_t
cwsym_cli_bad_usage(const cwsym_cli_t* cli, const barg_t* barg, const char* msg);

#endif /* CWSYM_CLI_COMMAND_H */
