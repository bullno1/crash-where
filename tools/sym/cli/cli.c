/**
 * @file cli.c
 * Dispatcher and the helpers every command shares.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BARG_IMPLEMENTATION
#include "command.h"

static const cwsym_cli_command_t* const commands[] = {
	&cwsym_cli_convert,
	&cwsym_cli_upload,
	&cwsym_cli_dump,
	&cwsym_cli_lookup,
	&cwsym_cli_symbolize,
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void
log_to_stream(void* user, const char* msg) {
	fprintf((FILE*)user, "cwsym: %s\n", msg);
}

cwsym_log_t
cwsym_cli_log(const cwsym_cli_t* cli) {
	return (cwsym_log_t){ .log = log_to_stream, .user = cli->err };
}

static void
print_commands(FILE* out) {
	fprintf(out, "\nCommands:\n");
	for (size_t i = 0; i < NUM_COMMANDS; ++i) {
		fprintf(out, "  %-10s %s\n", commands[i]->name, commands[i]->summary);
	}
	fprintf(out, "\n<input> is a PE, ELF, or Wasm file with debug information, or a cwsym table.\n");
}

cwsym_cli_status_t
cwsym_cli_main(const cwsym_cli_t* cli, int argc, const char* argv[]) {
	cwsym_cli_t ctx = *cli;
	ctx.read = (cwsym_read_options_t){ 0 };
	barg_opt_t opts[] = {
		{
			.name = "dia", .value_name = "dll",
			.summary = "Path to msdia140.dll",
			.description = "Default: located through VSINSTALLDIR or vswhere",
			.parser = barg_str(&ctx.read.dia),
		},
		{
			.name = "symbol-map", .value_name = "file",
			.summary = "Path to emscripten's --emit-symbol-map output for a Wasm module with a stripped name section",
			.description = "Default: <module>.symbols, <stem>.js.symbols or <stem>.html.symbols beside it",
			.parser = barg_str(&ctx.read.symbol_map),
		},
		{
			.name = "source-map", .value_name = "file",
			.summary = "Path to emscripten's -gsource-map output for a Wasm module without DWARF; ",
			.description = "Default: the file its sourceMappingURL section names, beside it",
			.parser = barg_str(&ctx.read.source_map),
		},
		barg_opt_help(),
	};
	barg_t barg = {
		.opts = opts, .num_opts = (int)(sizeof(opts) / sizeof(opts[0])),
		.allow_positional = true,
		.usage = "cwsym [options] <command> [command options] <input> ...",
		.summary = "Convert debug information to cwsym tables, inspect them, and upload them.\n"
			"The upload token is read from the CWSYM_TOKEN environment variable.",
	};
	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status == BARG_SHOW_HELP) {
		barg_print_result(&barg, result, ctx.out);
		print_commands(ctx.out);
		return CWSYM_CLI_OK;
	}
	if (result.status != BARG_OK) {
		return cwsym_cli_report(&ctx, &barg, result);
	}
	if (result.arg_index >= argc) {
		return cwsym_cli_bad_usage(&ctx, &barg, "expected a command");
	}
	const char* name = argv[result.arg_index];
	for (size_t i = 0; i < NUM_COMMANDS; ++i) {
		if (strcmp(commands[i]->name, name) == 0) {
			return commands[i]->run(&ctx, argc - result.arg_index, argv + result.arg_index);
		}
	}
	char msg[128];
	snprintf(msg, sizeof(msg), "unknown command '%s'", name);
	return cwsym_cli_bad_usage(&ctx, &barg, msg);
}

/* Diagnostics {{{ */

cwsym_cli_status_t
cwsym_cli_report(const cwsym_cli_t* cli, barg_t* barg, barg_result_t result) {
	if (result.status == BARG_SHOW_HELP) {
		barg_print_result(barg, result, cli->out);
		return CWSYM_CLI_OK;
	}
	fprintf(cli->err, "cwsym: ");
	barg_print_result(barg, result, cli->err);
	fprintf(cli->err, "Usage: %s\n", barg->usage);
	return CWSYM_CLI_USAGE;
}

cwsym_cli_status_t
cwsym_cli_bad_usage(const cwsym_cli_t* cli, const barg_t* barg, const char* msg) {
	fprintf(cli->err, "cwsym: %s\nUsage: %s\n", msg, barg->usage);
	return CWSYM_CLI_USAGE;
}

/* }}} */

/* Input and output {{{ */

/** Read the whole of an open file. */
static void*
slurp(FILE* f, size_t* len) {
	if (fseek(f, 0, SEEK_END) != 0) {
		return NULL;
	}
	long n = ftell(f);
	if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
		return NULL;
	}
	void* buf = malloc((size_t)n > 0 ? (size_t)n : 1);
	if (buf == NULL) {
		return NULL;
	}
	*len = fread(buf, 1, (size_t)n, f);
	if (*len != (size_t)n) {
		free(buf);
		return NULL;
	}
	return buf;
}

cwsym_cli_status_t
cwsym_cli_load(const cwsym_cli_t* cli, const char* path, bool lines, cwsym_cli_input_t* in) {
	*in = (cwsym_cli_input_t){ 0 };
	cwsym_log_t log = cwsym_cli_log(cli);
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(cli->err, "cwsym: %s: %s\n", path, strerror(errno));
		return CWSYM_CLI_FAILED;
	}
	static const char magic[6] = "CWSYM";
	char head[sizeof(magic)];
	bool is_table = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, magic, sizeof(magic)) == 0;
	if (is_table) {
		size_t len = 0;
		in->buf = slurp(f, &len);
		fclose(f);
		if (in->buf == NULL) {
			fprintf(cli->err, "cwsym: %s: cannot read\n", path);
			return CWSYM_CLI_FAILED;
		}
		if (cwsym_table_parse(in->buf, len, &in->table, &log) != CWSYM_OK) {
			cwsym_cli_unload(in);
			return CWSYM_CLI_FAILED;
		}
		return CWSYM_CLI_OK;
	}
	fclose(f);

	cwsym_table_builder_t* builder = cwsym_table_begin();
	if (builder == NULL) {
		fprintf(cli->err, "cwsym: out of memory\n");
		return CWSYM_CLI_FAILED;
	}
	cwsym_sink_t sink = cwsym_table_sink(builder, lines);
	cwsym_status_t read = cwsym_read(path, &cli->read, &sink, &log);
	cwsym_status_t built = cwsym_table_end(builder, &in->table, read == CWSYM_OK ? &log : NULL);
	if (read != CWSYM_OK || built != CWSYM_OK) {
		cwsym_cli_unload(in);
		return CWSYM_CLI_FAILED;
	}
	return CWSYM_CLI_OK;
}

void
cwsym_cli_unload(cwsym_cli_input_t* in) {
	cwsym_table_free(&in->table);
	free(in->buf);
	*in = (cwsym_cli_input_t){ 0 };
}

cwsym_cli_status_t
cwsym_cli_write(const cwsym_cli_t* cli, const char* path, const cwsym_table_t* table) {
	FILE* f = fopen(path, "wb");
	if (f == NULL) {
		fprintf(cli->err, "cwsym: %s: %s\n", path, strerror(errno));
		return CWSYM_CLI_FAILED;
	}
	bool ok = fwrite(table->data, 1, table->len, f) == table->len;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		fprintf(cli->err, "cwsym: %s: %s\n", path, strerror(errno));
		return CWSYM_CLI_FAILED;
	}
	return CWSYM_CLI_OK;
}

bool
cwsym_cli_parse_offset(const char* s, uint32_t* out) {
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	}
	if (s[0] == '\0' || s[0] == '-' || s[0] == '+' || s[0] == ' ') {
		return false;
	}
	char* end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, base);
	if (*end != '\0' || errno != 0 || v > UINT32_MAX) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

/* }}} */
