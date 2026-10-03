/**
 * End to end: a crash in the `crashme` sample reaches the collector. The
 * sample's own symbols are uploaded with `cwsym` first, so the frames the
 * collector groups on are the sample's functions. Consent is stored as
 * `always` before the first run, so the watcher uploads without a prompt.
 * A null dereference and a `cw_abort` are each run once, and the app page
 * must list both crashes by name.
 */
import assert from "node:assert/strict";
import { mkdir, readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { after, before, test } from "node:test";
import { setTimeout as sleep } from "node:timers/promises";
import { type AppPage, binary, Collector, run, runLogging } from "./harness.mts";

// The slug, version and channel the sample is built with.
const APP = "crashme";
const VERSION = "0.0.1";
const CHANNEL = "dev";

let collector: Collector;
let reportDir: string;
let failed = false;

before(async () => {
	collector = await Collector.start();
	reportDir = path.join(collector.work, "report");
	await mkdir(reportDir);
	await writeFile(path.join(reportDir, "consent"), "always\n");
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

/**
 * Runs the sample in `mode` and waits for its watcher, which outlives it
 * and inherits its log, to say the report was uploaded. Returns the log.
 */
async function crash(mode: string): Promise<string> {
	const logPath = path.join(collector.work, `crashme-${mode}.log`);
	// The sample dies by a signal, which is the point; its watcher inherits the log.
	runLogging(binary("crashme"), [mode, reportDir, collector.endpoint], logPath);
	let text = "";
	for (let i = 0; i < 60; ++i) {
		text = await readFile(logPath, "utf8");
		if (/report .* uploaded/.test(text)) return text;
		await sleep(500);
	}
	throw new Error(`mode ${mode}: the watcher did not report an upload:\n${text}`);
}

it("uploads the sample's symbols", async () => {
	const token = await collector.createApp(APP, "Crashme");
	const r = run(binary("cwsym"), [
		"upload", "--endpoint", collector.endpoint, "--app", APP, "--version", VERSION, "--channel", CHANNEL, binary("crashme"),
	], { CWSYM_TOKEN: token });
	assert.equal(r.status, 0, r.output);
});
it("uploads a null dereference", async () => {
	await crash("null");
});
it("uploads an abort", async () => {
	await crash("assert");
});
it("leaves nothing pending and had both reports accepted", async () => {
	const pending = await readdir(path.join(reportDir, "pending")).catch(() => [] as string[]);
	assert.deepEqual(pending.filter((f) => f.endsWith(".json")), []);
	const accepted = (await collector.log()).match(new RegExp(`POST /v1/${APP}/report 201`, "g")) ?? [];
	assert.equal(accepted.length, 2);
});
it("lists both crashes by their frames and message", async () => {
	const page = await collector.json<AppPage>(`/dashboard/apps/${APP}`, 200);
	const titles = page.crashes.map((g) => g.title).sort();
	assert.deepEqual(titles, ["ASSERT in main: mode != assert", "memory in crashme:crash_here, from crashme:level_two"]);
	const memory = page.crashes.find((g) => g.fault === "memory")!;
	assert.equal(memory.count, 1);
	assert.equal(memory.message, null);
	const abort = page.crashes.find((g) => g.fault === "ASSERT")!;
	assert.equal(abort.message, "mode != assert");
	for (const g of page.crashes) {
		assert.ok(g.frames.some((f) => f.module === "crashme"), `${g.title} keeps a raw frame in the sample`);
	}
});
