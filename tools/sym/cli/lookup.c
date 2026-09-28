/**
 * @file lookup.c
 * `cwsym lookup`: the function covering each offset, as the Worker sees it.
 */
#include "command.h"

static cwsym_cli_status_t
run(const cwsym_cli_t* cli, int argc, const char* argv[]) {
	barg_opt_t opts[] = { barg_opt_help() };
	barg_t barg = {
		.opts = opts, .num_opts = 1,
		.allow_positional = true,
		.usage = "cwsym lookup <input> <offset>...",
		.summary = "Resolve module-relative offsets, 0x hex or decimal, to the function rows the Worker hashes with.\n"
			"Each line is the offset, the normalized name, the function start, and its size;\n"
			"an offset no row covers prints <unknown> and the exit status is 1.",
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
	cwsym_cli_status_t status = cwsym_cli_load(cli, argv[result.arg_index], false, &in);
	if (status != CWSYM_CLI_OK) {
		return status;
	}
	for (int i = first; i < argc; ++i) {
		uint32_t offset;
		cwsym_cli_parse_offset(argv[i], &offset);
		cwsym_hit_t hit;
		if (cwsym_lookup(&in.table, offset, &hit)) {
			fprintf(cli->out, "0x%08x %s 0x%08x 0x%08x\n", offset, hit.name, hit.start, hit.size);
		} else {
			fprintf(cli->out, "0x%08x <unknown>\n", offset);
			status = CWSYM_CLI_FAILED;
		}
	}
	cwsym_cli_unload(&in);
	return status;
}

const cwsym_cli_command_t cwsym_cli_lookup = {
	.name = "lookup",
	.summary = "Resolve offsets to the function rows the Worker hashes with",
	.run = run,
};
