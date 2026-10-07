import { describe, expect, it } from "vitest";
import { relativeSource, sourceLink, templateError, validCommit, validSourceRoot } from "../src/source-link";

const github = "https://github.com/org/repo/blob/{commit}/{+file}#L{line}";
const vars = { commit: "abc123", version: "1.4.2", file: "src/render.c", line: "41" };

describe("source link template", () => {
	it("accepts the forge templates and refuses what is not one", () => {
		expect(templateError(github)).toBeNull();
		expect(templateError("https://gitlab.com/org/repo/-/blob/{commit}/{+file}#L{line}")).toBeNull();
		expect(templateError("https://github.com/org/repo/blob/v{version}/{+file}")).toBeNull();
		expect(templateError("https://example.com/")).toBeNull();
		expect(templateError("https://example.com/{branch}/{+file}")).toMatch(/not \{branch\}/);
		expect(templateError("https://example.com/{file")).toMatch(/unmatched brace/);
		expect(templateError("https://example.com/file}")).toMatch(/unmatched brace/);
		expect(templateError("https://example.com/{#file}")).toMatch(/not \{#file\}/);
		expect(templateError("ftp://example.com/{+file}")).toMatch(/http or https/);
		expect(templateError("org/repo/{+file}")).toMatch(/http or https/);
		expect(templateError("")).toMatch(/http or https/);
		expect(templateError(`https://example.com/${"x".repeat(500)}`)).toMatch(/at most 500/);
	});
	it("expands as level 2 does: simple encodes, reserved keeps the path characters", () => {
		expect(sourceLink(github, vars)).toBe("https://github.com/org/repo/blob/abc123/src/render.c#L41");
		expect(sourceLink("https://x.example/{file}", vars)).toBe("https://x.example/src%2Frender.c");
		expect(sourceLink("https://x.example/{+file}", { ...vars, file: "a b/c'd!.c" })).toBe("https://x.example/a%20b/c'd!.c");
		expect(sourceLink("https://x.example/{file}", { ...vars, file: "a b/c'd!.c" })).toBe("https://x.example/a%20b%2Fc%27d%21.c");
		expect(sourceLink("https://x.example/{+file}", { ...vars, file: "é.c" })).toBe("https://x.example/%C3%A9.c");
		expect(sourceLink("https://x.example/{commit}/{version}/{line}", vars)).toBe("https://x.example/abc123/1.4.2/41");
		expect(sourceLink("https://x.example/{+file}/{+file}", vars)).toBe("https://x.example/src/render.c/src/render.c");
	});
	it("gives no link when a named variable is undefined, or the template is bad", () => {
		expect(sourceLink(github, { ...vars, commit: undefined })).toBeNull();
		expect(sourceLink(github, { ...vars, file: undefined })).toBeNull();
		expect(sourceLink(github, { ...vars, line: undefined })).toBeNull();
		expect(sourceLink("https://x.example/{+file}", { ...vars, commit: undefined, line: undefined })).toBe("https://x.example/src/render.c");
		expect(sourceLink("https://x.example/{branch}", vars)).toBeNull();
	});
});

describe("relative source", () => {
	it("strips a POSIX root, exactly and case-sensitively", () => {
		expect(relativeSource("/home/ci/game/src/render.c", "/home/ci/game")).toBe("src/render.c");
		expect(relativeSource("/home/ci/game/src/render.c", "/home/ci/game/")).toBe("src/render.c");
		expect(relativeSource("/home/ci/game/src/render.c", "/home/ci/Game")).toBeUndefined();
		expect(relativeSource("/home/ci/game-2/src/render.c", "/home/ci/game")).toBeUndefined();
		expect(relativeSource("/usr/include/stdio.h", "/home/ci/game")).toBeUndefined();
		expect(relativeSource("/home/ci/game", "/home/ci/game")).toBeUndefined();
	});
	it("strips a Windows root without regard to case or separators", () => {
		expect(relativeSource("D:\\a\\game\\game\\src\\render.c", "D:\\a\\game\\game")).toBe("src/render.c");
		expect(relativeSource("d:\\A\\Game\\game\\src\\Render.c", "D:/a/game/game/")).toBe("src/Render.c");
		expect(relativeSource("C:\\Program Files\\SDK\\x.h", "D:\\a\\game\\game")).toBeUndefined();
	});
	it("keeps a relative path as it is and refuses one that climbs out", () => {
		expect(relativeSource("src/render.c", null)).toBe("src/render.c");
		expect(relativeSource("src\\render.c", null)).toBe("src/render.c");
		expect(relativeSource("./src//render.c", "/home/ci/game")).toBe("src/render.c");
		expect(relativeSource("../other/x.c", null)).toBeUndefined();
		expect(relativeSource("/home/ci/game/../sdk/x.h", "/home/ci/game")).toBeUndefined();
		expect(relativeSource("/home/ci/game/src/render.c", null)).toBeUndefined();
	});
});

describe("upload grammars", () => {
	it("take a hash, a tag or a branch as the commit", () => {
		expect(validCommit("8f3c2a1")).toBe(true);
		expect(validCommit("v1.4.2")).toBe(true);
		expect(validCommit("release/1.4")).toBe(true);
		expect(validCommit("")).toBe(false);
		expect(validCommit("a b")).toBe(false);
		expect(validCommit("x".repeat(101))).toBe(false);
	});
	it("take any short printable root", () => {
		expect(validSourceRoot("/home/ci/game")).toBe(true);
		expect(validSourceRoot("D:\\a\\game game")).toBe(true);
		expect(validSourceRoot("")).toBe(false);
		expect(validSourceRoot("/a\nb")).toBe(false);
		expect(validSourceRoot("x".repeat(501))).toBe(false);
	});
});
