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
import { binary, Collector, type CrashesPage, run, runLogging } from "./harness.mts";

/** The crash page's JSON, as far as this test reads it. */
interface CrashPage {
	sample: {
		frames: { module: string; offset?: number }[];
		locations: { function: string; file: string | null; line: number }[][];
	} | null;
}

/** `cwsym symbolize` on the sample's own binary, parsed: one location per indented line. */
function symbolize(offset: number): { function: string; file: string | null; line: number }[] {
	const r = run(binary("cwsym"), ["symbolize", binary("crashme"), `0x${offset.toString(16)}`]);
	assert.equal(r.status, 0, r.output);
	return r.output
		.split("\n")
		.filter((l) => l.startsWith("  "))
		.map((l) => {
			const m = /^  (.*?)(?: at (.*):(\d+))?$/.exec(l)!;
			return { function: m[1]!, file: m[2] ?? null, line: m[3] === undefined ? 0 : Number(m[3]) };
		});
}

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
it("uploads a null dereference with its core", async () => {
	const log = await crash("null");
	assert.match(log, /attachment \S+\.dmp uploaded/);
});
it("uploads an abort", async () => {
	await crash("assert");
});
it("leaves nothing pending and had both reports accepted", async () => {
	const pending = await readdir(path.join(reportDir, "pending")).catch(() => [] as string[]);
	assert.deepEqual(pending.filter((f) => f.endsWith(".json")), []);
	const served = await collector.log();
	const accepted = served.match(new RegExp(`POST /v1/${APP}/report 201`, "g")) ?? [];
	assert.equal(accepted.length, 2);
	const stored = served.match(new RegExp(`POST /v1/${APP}/attach 201`, "g")) ?? [];
	assert.equal(stored.length, 2, "both cores were stored");
});
it("lists both crashes by their frames and message", async () => {
	const page = await collector.json<CrashesPage>(`/dashboard/apps/${APP}`, 200);
	const titles = page.crashes.map((g) => g.title).sort();
	assert.deepEqual(titles, ["ASSERT in main: mode != assert", "memory in crashme:crash_here, from crashme:level_two"]);
	const memory = page.crashes.find((g) => g.fault === "memory")!;
	assert.equal(memory.count, 1);
	assert.equal(memory.recent_users, 1, "the install id reached the shard");
	assert.equal(memory.urgency, 1);
	assert.equal(memory.message, null);
	const abort = page.crashes.find((g) => g.fault === "ASSERT")!;
	assert.equal(abort.message, "mode != assert");
	for (const g of page.crashes) {
		assert.ok(g.frames.some((f) => f.module === "crashme"), `${g.title} keeps a raw frame in the sample`);
	}
});
it("locates the sample's frames as the tool does", async () => {
	const list = await collector.json<CrashesPage>(`/dashboard/apps/${APP}`, 200);
	const memory = list.crashes.find((g) => g.fault === "memory")!;
	const page = await collector.json<CrashPage>(`/dashboard/apps/${APP}/crashes/${memory.id}`, 200);
	assert.ok(page.sample, "the report was sampled");
	let compared = 0;
	let located = 0;
	for (const [i, f] of page.sample.frames.entries()) {
		if (f.module !== "crashme" || f.offset === undefined) continue;
		// Frames past the first hold return addresses and are looked up one byte back.
		const expected = symbolize(i === 0 ? f.offset : f.offset - 1);
		assert.deepEqual(page.sample.locations[i], expected, `frame ${i} at 0x${f.offset.toString(16)}`);
		compared += 1;
		// The executable's start-up code has no lines; the sample's own functions do.
		if (expected[0]!.file !== null) located += 1;
	}
	assert.ok(compared >= 2, "at least crash_here and level_two were compared");
	assert.ok(located >= 2, "the sample's own frames have a file and line");
});
