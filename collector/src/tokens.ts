import type { Selectable } from "kysely";
import { base64url, hex } from "./bytes";
import type { Db } from "./db";
import type { UploadTokens } from "./db.generated";
import type { Identity } from "./env";

/** A row of the `upload_tokens` table as read back. */
export type TokenRow = Selectable<UploadTokens>;

const PREFIX = "cwu_";
/** Longest label accepted, to keep the listing readable. */
export const MAX_LABEL = 100;

/** The trimmed label when it is usable, else null. */
export function validLabel(label: string): string | null {
	const trimmed = label.trim();
	return trimmed.length >= 1 && trimmed.length <= MAX_LABEL ? trimmed : null;
}

export async function hashToken(token: string): Promise<string> {
	return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token))));
}

/** Mints a token for the app and stores its hash under the label. The token is returned once and never stored. */
export async function createToken(
	db: Db, appId: number, label: string, who: Identity, now: number
): Promise<{ token: string; row: TokenRow }> {
	const bytes = crypto.getRandomValues(new Uint8Array(32));
	const token = PREFIX + base64url(bytes);
	const row = await db
		.insertInto("upload_tokens")
		.values({
			app_id: appId,
			hash: await hashToken(token),
			label,
			created_at: now,
			created_by: who.email ?? who.sub,
		})
		.returningAll()
		.executeTakeFirstOrThrow();
	return { token, row };
}

/** Every token of the app, newest first, revoked ones included. */
export function listTokens(db: Db, appId: number): Promise<TokenRow[]> {
	return db.selectFrom("upload_tokens").selectAll().where("app_id", "=", appId).orderBy("id", "desc").execute();
}

/** Revokes the app's token; false when there is no such usable token. */
export async function revokeToken(db: Db, appId: number, id: number, now: number): Promise<boolean> {
	const result = await db
		.updateTable("upload_tokens")
		.set({ revoked_at: now })
		.where("app_id", "=", appId)
		.where("id", "=", id)
		.where("revoked_at", "is", null)
		.executeTakeFirst();
	return result.numUpdatedRows > 0n;
}

/** The usable token row the bearer names, with its use recorded, or null. */
export async function authenticateToken(db: Db, token: string, now: number): Promise<TokenRow | null> {
	const row = await db
		.updateTable("upload_tokens")
		.set({ last_used_at: now })
		.where("hash", "=", await hashToken(token))
		.where("revoked_at", "is", null)
		.returningAll()
		.executeTakeFirst();
	return row ?? null;
}
