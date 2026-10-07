import { type Selectable, sql } from "kysely";
import type { Db } from "./db";
import { parseCorsOrigins } from "./cors";
import type { Apps } from "./db.generated";
import type { Identity } from "./env";
import { templateError } from "./source-link";

/** A row of the `apps` table as read back. */
export type AppRow = Selectable<Apps>;

/** What the create form submits. */
export interface AppInput {
	name: string;
	display_name: string;
}

/** What the settings form submits: strings, so a rejected form can show what was typed. */
export interface SettingsInput {
	display_name: string;
	sample_cap_trusted: string;
	sample_cap_untrusted: string;
	/** Empty for no links. */
	source_link_template: string;
	/** `*` for every origin, else one per line; empty for none. */
	cors_origins: string;
}

/** The settings as stored. */
export type AppSettings = Pick<
	AppRow, "display_name" | "sample_cap_trusted" | "sample_cap_untrusted" | "source_link_template" | "cors_origins"
>;

/** Why an input is unusable, attached to the field at fault. */
export interface AppError {
	field: keyof AppInput | keyof SettingsInput;
	message: string;
}

/** The slug grammar the client enforces at `cw_init`; the table's CHECK repeats it. */
export const NAME_PATTERN = /^[a-z0-9_-]{1,63}$/;

/** Longest display name accepted, to keep the listing readable. */
export const MAX_DISPLAY_NAME = 100;

/** Most full reports a sample bucket may be asked to keep. */
export const MAX_SAMPLE_CAP = 100;

function displayNameError(name: string): AppError | null {
	if (name.length === 0) {
		return { field: "display_name", message: "The display name is required." };
	}
	if (name.length > MAX_DISPLAY_NAME) {
		return { field: "display_name", message: `The display name must be at most ${MAX_DISPLAY_NAME} characters.` };
	}
	return null;
}

/** The reason an input is unusable, or null when it is fine. */
export function validateApp(input: AppInput): AppError | null {
	if (!NAME_PATTERN.test(input.name)) {
		return { field: "name", message: "The name must be 1 to 63 lowercase letters, digits, '-' or '_'." };
	}
	return displayNameError(input.display_name);
}

/** A sample cap as typed: a whole number of reports, zero for none. */
function sampleCap(field: "sample_cap_trusted" | "sample_cap_untrusted", text: string): { cap: number } | { error: AppError } {
	const cap = /^\d+$/.test(text) ? Number(text) : NaN;
	if (!(cap >= 0 && cap <= MAX_SAMPLE_CAP)) {
		return { error: { field, message: `The sample cap must be a whole number from 0 to ${MAX_SAMPLE_CAP}.` } };
	}
	return { cap };
}

/** The settings a form describes, or the error on the field at fault. */
export function parseSettings(input: SettingsInput): { settings: AppSettings } | { error: AppError } {
	const name = displayNameError(input.display_name);
	if (name) return { error: name };
	const trusted = sampleCap("sample_cap_trusted", input.sample_cap_trusted);
	if ("error" in trusted) return trusted;
	const untrusted = sampleCap("sample_cap_untrusted", input.sample_cap_untrusted);
	if ("error" in untrusted) return untrusted;
	const template = input.source_link_template === "" ? null : input.source_link_template;
	const bad = template === null ? null : templateError(template);
	if (bad !== null) return { error: { field: "source_link_template", message: bad } };
	const cors = parseCorsOrigins(input.cors_origins);
	if ("error" in cors) return { error: { field: "cors_origins", message: cors.error } };
	return {
		settings: {
			display_name: input.display_name,
			sample_cap_trusted: trusted.cap,
			sample_cap_untrusted: untrusted.cap,
			source_link_template: template,
			cors_origins: cors.origins,
		},
	};
}

/** Every app, ordered by display name. */
export function listApps(db: Db): Promise<AppRow[]> {
	return db
		.selectFrom("apps")
		.selectAll()
		.orderBy(sql`display_name COLLATE NOCASE`)
		.orderBy("name")
		.execute();
}

/** The app with this name, or undefined. */
export function getApp(db: Db, name: string): Promise<AppRow | undefined> {
	return db.selectFrom("apps").selectAll().where("name", "=", name).executeTakeFirst();
}

/**
 * Inserts an app and returns its row, or null when the name is taken.
 * The input must already have passed `validateApp`.
 */
export async function createApp(db: Db, input: AppInput, who: Identity): Promise<AppRow | null> {
	const row = await db
		.insertInto("apps")
		.values({
			name: input.name,
			display_name: input.display_name,
			created_at: Math.floor(Date.now() / 1000),
			created_by: who.email ?? who.sub,
		})
		.onConflict((oc) => oc.column("name").doNothing())
		.returningAll()
		.executeTakeFirst();
	return row ?? null;
}

/** Stores the settings of an app and returns its row, or null when there is no such app. */
export async function updateSettings(db: Db, id: number, settings: AppSettings): Promise<AppRow | null> {
	const row = await db.updateTable("apps").set(settings).where("id", "=", id).returningAll().executeTakeFirst();
	return row ?? null;
}

/**
 * Flips the kill switch: `at` is when the app was disabled, or null to
 * enable it. Returns the row, or null when there is no such app.
 */
export async function setDisabled(db: Db, id: number, at: number | null): Promise<AppRow | null> {
	const row = await db.updateTable("apps").set({ disabled_at: at }).where("id", "=", id).returningAll().executeTakeFirst();
	return row ?? null;
}
