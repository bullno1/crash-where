/**
 * @file convert.c
 * `cwsym convert`: write the input's table to a file.
 */
#include "command.h"

static cwsym_cli_status_t
run(const cwsym_cli_t* cli, int argc, const char* argv[]) {
	const char* out_path = NULL;
	barg_opt_t opts[] = {
		{
			.name = "out", .short_name = 'o', .value_name = "file",
			.summary = "Where to write the table (required)",
			.parser = barg_str(&out_path),
		},
		barg_opt_help(),
	};
	barg_t barg = {
		.opts = opts, .num_opts = (int)(sizeof(opts) / sizeof(opts[0])),
		.allow_positional = true,
		.usage = "cwsym convert -o <file> <input>",
		.summary = "Read a debug file and write its cwsym table. A cwsym input is written back unchanged.",
	};
	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status != BARG_OK) {
		return cwsym_cli_report(cli, &barg, result);
	}
	if (argc - result.arg_index != 1) {
		return cwsym_cli_bad_usage(cli, &barg, "expected exactly one input");
	}
	if (out_path == NULL) {
		return cwsym_cli_bad_usage(cli, &barg, "--out is required");
	}

	cwsym_cli_input_t in;
	cwsym_cli_status_t status = cwsym_cli_load(cli, argv[result.arg_index], true, &in);
	if (status != CWSYM_CLI_OK) {
		return status;
	}
	status = cwsym_cli_write(cli, out_path, &in.table);
	cwsym_cli_unload(&in);
	return status;
}

const cwsym_cli_command_t cwsym_cli_convert = {
	.name = "convert",
	.summary = "Write the input's cwsym table to a file",
	.run = run,
};
