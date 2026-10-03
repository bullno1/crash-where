import { type Selectable, sql } from "kysely";
import type { Db } from "./db";
import type { Apps } from "./db.generated";
import type { Identity } from "./env";

/** A row of the `apps` table as read back. */
export type AppRow = Selectable<Apps>;

/** What the create form submits. */
export interface AppInput {
	name: string;
	display_name: string;
}

/** Why an input is unusable, attached to the field at fault. */
export interface AppError {
	field: keyof AppInput;
	message: string;
}

/** The slug grammar the client enforces at `cw_init`; the table's CHECK repeats it. */
export const NAME_PATTERN = /^[a-z0-9_-]{1,63}$/;

/** Longest display name accepted, to keep the listing readable. */
export const MAX_DISPLAY_NAME = 100;

/** The reason an input is unusable, or null when it is fine. */
export function validateApp(input: AppInput): AppError | null {
	if (!NAME_PATTERN.test(input.name)) {
		return { field: "name", message: "The name must be 1 to 63 lowercase letters, digits, '-' or '_'." };
	}
	if (input.display_name.length === 0) {
		return { field: "display_name", message: "The display name is required." };
	}
	if (input.display_name.length > MAX_DISPLAY_NAME) {
		return { field: "display_name", message: `The display name must be at most ${MAX_DISPLAY_NAME} characters.` };
	}
	return null;
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
