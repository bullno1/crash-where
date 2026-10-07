/**
 * @file upload.c
 * `cwsym upload`: register a release and upload its table.
 */
#include "command.h"

static cwsym_cli_status_t
run(const cwsym_cli_t* cli, int argc, const char* argv[]) {
	cwsym_upload_t up = { .token = cli->token };
	const char* out_path = NULL;
	barg_opt_t opts[] = {
		{
			.name = "endpoint", .value_name = "url",
			.summary = "Base URL of the ingest service, without a trailing slash (required)",
			.parser = barg_str(&up.endpoint),
		},
		{
			.name = "app", .value_name = "slug",
			.summary = "Application slug the release belongs to (required)",
			.parser = barg_str(&up.app),
		},
		{
			.name = "version", .value_name = "version",
			.summary = "Release version compiled into the build (required)",
			.parser = barg_str(&up.version),
		},
		{
			.name = "channel", .value_name = "channel",
			.summary = "Build stream compiled into the build, such as stable or beta (required)",
			.parser = barg_str(&up.channel),
		},
		{
			.name = "commit", .value_name = "ref",
			.summary = "Commit the build was made from, for the dashboard's source links",
			.parser = barg_str(&up.commit),
		},
		{
			.name = "source-root", .value_name = "dir",
			.summary = "Checkout root the build's source paths start with, such as $GITHUB_WORKSPACE",
			.parser = barg_str(&up.source_root),
		},
		{
			.name = "out", .short_name = 'o', .value_name = "file",
			.summary = "Also write the uploaded table to this file",
			.parser = barg_str(&out_path),
		},
		barg_opt_help(),
	};
	barg_t barg = {
		.opts = opts, .num_opts = (int)(sizeof(opts) / sizeof(opts[0])),
		.allow_positional = true,
		.usage = "cwsym upload --endpoint <url> --app <slug> --version <version> --channel <channel> [--commit <ref>] [--source-root <dir>] [-o <file>] <input>",
		.summary = "Register the release and upload the input's table in one request.\n"
			"The bearer token is taken from the CWSYM_TOKEN environment variable.",
	};
	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status != BARG_OK) {
		return cwsym_cli_report(cli, &barg, result);
	}
	if (argc - result.arg_index != 1) {
		return cwsym_cli_bad_usage(cli, &barg, "expected exactly one input");
	}
	if (up.endpoint == NULL) {
		return cwsym_cli_bad_usage(cli, &barg, "--endpoint is required");
	}
	if (up.app == NULL) {
		return cwsym_cli_bad_usage(cli, &barg, "--app is required");
	}
	if (up.version == NULL) {
		return cwsym_cli_bad_usage(cli, &barg, "--version is required");
	}
	if (up.channel == NULL) {
		return cwsym_cli_bad_usage(cli, &barg, "--channel is required");
	}
	if (up.token == NULL || up.token[0] == '\0') {
		return cwsym_cli_bad_usage(cli, &barg, "CWSYM_TOKEN is not set");
	}

	cwsym_cli_input_t in;
	cwsym_cli_status_t status = cwsym_cli_load(cli, argv[result.arg_index], true, &in);
	if (status != CWSYM_CLI_OK) {
		return status;
	}
	if (out_path != NULL) {
		status = cwsym_cli_write(cli, out_path, &in.table);
	}
	if (status == CWSYM_CLI_OK) {
		cwsym_log_t log = cwsym_cli_log(cli);
		status = cwsym_upload(&up, &in.table, cli->transport, &log) == CWSYM_OK ? CWSYM_CLI_OK : CWSYM_CLI_FAILED;
	}
	cwsym_cli_unload(&in);
	return status;
}

const cwsym_cli_command_t cwsym_cli_upload = {
	.name = "upload",
	.summary = "Register a release and upload its table",
	.run = run,
};
