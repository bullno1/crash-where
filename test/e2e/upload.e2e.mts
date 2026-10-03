/**
 * End to end: `cwsym upload` against the collector. The app and its upload
 * token are created through the dashboard, then the tool uploads the
 * golden table, uploads it again, uploads it under another version, and
 * tries a bad token. The app page is checked last.
 */
import assert from "node:assert/strict";
import path from "node:path";
import { after, before, test } from "node:test";
import { type AppPage, binary, Collector, ROOT, run } from "./harness.mts";

const FIXTURE = path.join(ROOT, "test/fixtures/synthetic.cwsym");
const BUILD_ID = "0102030405060708090a0b0c0d0e0f1011121314";
const APP = "e2e-game";

let collector: Collector;
let token: string;
let failed = false;

before(async () => {
	collector = await Collector.start();
	token = await collector.createApp(APP, "E2E Game");
});
after(() => collector?.stop(failed));

/** A test whose failure makes the collector print its log when it stops. */
function it(name: string, fn: () => Promise<void>): void {
	test(name, async () => {
		try {
			await fn();
		} catch (e) {
			failed = true;
			throw e;
		}
	});
}

/** Uploads the fixture as `version` with `token`; checks the exit status and one line of the output. */
function upload(version: string, token: string, status: number, line: string): void {
	const r = run(binary("cwsym"), [
		"upload", "--endpoint", collector.endpoint, "--app", APP, "--version", version, "--channel", "stable", FIXTURE,
	], { CWSYM_TOKEN: token });
	assert.equal(r.status, status, r.output);
	assert.ok(r.output.includes(line), `no line '${line}' in:\n${r.output}`);
}

it("uploads a table and registers the release", () => {
	upload("1.0.0", token, 0, `uploaded ${BUILD_ID} as 1.0.0 (stable): HTTP 201`);
	return Promise.resolve();
});
it("is a no-op when run again", () => {
	upload("1.0.0", token, 0, `uploaded ${BUILD_ID} as 1.0.0 (stable): HTTP 200`);
	return Promise.resolve();
});
it("refuses the build under another version", () => {
	upload("1.0.1", token, 1, `HTTP 409: Build ${BUILD_ID} is already registered under version 1.0.0`);
	return Promise.resolve();
});
it("refuses a bad token", () => {
	upload("1.0.0", "cwu_not-a-token", 1, "HTTP 401: The upload token is not valid");
	return Promise.resolve();
});
it("lists the release and the used token on the app page", async () => {
	const page = await collector.json<AppPage>(`/dashboard/apps/${APP}`, 200);
	assert.equal(page.versions.length, 1);
	const [v] = page.versions;
	assert.equal(v!.version, "1.0.0");
	assert.deepEqual(v!.builds, [BUILD_ID]);
	assert.ok(v!.channels.some((c) => c.channel === "stable" && c.supported_until === null), "current on stable");
	assert.equal(page.tokens.length, 1);
	assert.equal(page.tokens[0]!.label, "e2e");
	assert.notEqual(page.tokens[0]!.last_used_at, null, "the token records its use");
});
