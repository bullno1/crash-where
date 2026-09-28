/**
 * @file symbolize.c
 * `cwsym symbolize`: source locations of each offset, as the dashboard shows them.
 */
#include "command.h"

typedef struct {
	FILE* out;
	bool missed;
} print_state_t;

static void
print_location(void* user, const cwsym_location_t* loc) {
	print_state_t* st = user;
	if (loc->function == NULL) {
		fprintf(st->out, "  <unknown>\n");
		st->missed = true;
	} else if (loc->file != NULL) {
		fprintf(st->out, "  %s at %s:%u\n", loc->function, loc->file, loc->line);
	} else {
		fprintf(st->out, "  %s\n", loc->function);
	}
}

static cwsym_cli_status_t
run(const cwsym_cli_t* cli, int argc, const char* argv[]) {
	barg_opt_t opts[] = { barg_opt_help() };
	barg_t barg = {
		.opts = opts, .num_opts = 1,
		.allow_positional = true,
		.usage = "cwsym symbolize <input> <offset>...",
		.summary = "Resolve module-relative offsets, 0x hex or decimal, to source locations.\n"
			"Each offset is followed by one indented line per inline level, innermost first,\n"
			"as `function at file:line`; an offset no row covers prints <unknown> and the exit status is 1.",
	};
	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status != BARG_OK) {
		return cwsym_cli_report(cli, &barg, result);
	}
	int first = result.arg_index + 1;
	if (argc - result.arg_index < 2) {
		return cwsym_cli_bad_usage(cli, &barg, "expected an input and at least one offset");
	}
	for (int i = first; i < argc; ++i) {
		uint32_t offset;
		if (!cwsym_cli_parse_offset(argv[i], &offset)) {
			char msg[128];
			snprintf(msg, sizeof(msg), "'%s' is not an offset", argv[i]);
			return cwsym_cli_bad_usage(cli, &barg, msg);
		}
	}

	cwsym_cli_input_t in;
	cwsym_cli_status_t status = cwsym_cli_load(cli, argv[result.arg_index], true, &in);
	if (status != CWSYM_CLI_OK) {
		return status;
	}
	print_state_t st = { .out = cli->out };
	cwsym_location_sink_t sink = { .location = print_location, .user = &st };
	for (int i = first; i < argc; ++i) {
		uint32_t offset;
		cwsym_cli_parse_offset(argv[i], &offset);
		fprintf(cli->out, "0x%08x\n", offset);
		cwsym_symbolize(&in.table, offset, &sink);
	}
	cwsym_cli_unload(&in);
	return st.missed ? CWSYM_CLI_FAILED : CWSYM_CLI_OK;
}

const cwsym_cli_command_t cwsym_cli_symbolize = {
	.name = "symbolize",
	.summary = "Resolve offsets to source locations, innermost inline level first",
	.run = run,
};
