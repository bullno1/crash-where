/**
 * @file dump.c
 * `cwsym dump`: print every section of the input's table.
 */
#include "command.h"

static cwsym_cli_status_t
run(const cwsym_cli_t* cli, int argc, const char* argv[]) {
	barg_opt_t opts[] = { barg_opt_help() };
	barg_t barg = {
		.opts = opts, .num_opts = 1,
		.allow_positional = true,
		.usage = "cwsym dump <input>",
		.summary = "Print the header and every row of the input's table. The first line is the key it uploads under.",
	};
	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status != BARG_OK) {
		return cwsym_cli_report(cli, &barg, result);
	}
	if (argc - result.arg_index != 1) {
		return cwsym_cli_bad_usage(cli, &barg, "expected exactly one input");
	}

	cwsym_cli_input_t in;
	cwsym_cli_status_t status = cwsym_cli_load(cli, argv[result.arg_index], true, &in);
	if (status != CWSYM_CLI_OK) {
		return status;
	}
	cwsym_dump(&in.table, cli->out);
	cwsym_cli_unload(&in);
	return CWSYM_CLI_OK;
}

const cwsym_cli_command_t cwsym_cli_dump = {
	.name = "dump",
	.summary = "Print every section of the input's table",
	.run = run,
};
