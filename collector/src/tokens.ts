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

/** A token just minted: the clear text, returned once and never stored, and its row. */
export interface MintedToken {
	token: string;
	row: TokenRow;
}

function mintToken(): string {
	return PREFIX + base64url(crypto.getRandomValues(new Uint8Array(32)));
}

/** Mints a token for the app and stores its hash under the label. */
export async function createToken(db: Db, appId: number, label: string, who: Identity, now: number): Promise<MintedToken> {
	const token = mintToken();
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

/**
 * Replaces the app's token with a new one under the same label, for a
 * token that leaked or whose holder rotates it on a schedule. The old
 * token stops working the moment the new one exists. The row keeps its id
 * and becomes the new token's: minted now, by the caller, never used. Null
 * when the app has no such token.
 */
export async function regenerateToken(
	db: Db, appId: number, id: number, who: Identity, now: number
): Promise<MintedToken | null> {
	const token = mintToken();
	const row = await db
		.updateTable("upload_tokens")
		.set({ hash: await hashToken(token), created_at: now, created_by: who.email ?? who.sub, last_used_at: null })
		.where("app_id", "=", appId)
		.where("id", "=", id)
		.returningAll()
		.executeTakeFirst();
	return row ? { token, row } : null;
}

/** Every token of the app, newest first. */
export function listTokens(db: Db, appId: number): Promise<TokenRow[]> {
	return db.selectFrom("upload_tokens").selectAll().where("app_id", "=", appId).orderBy("id", "desc").execute();
}

/** Deletes the app's token; false when the app has no such token. */
export async function revokeToken(db: Db, appId: number, id: number): Promise<boolean> {
	const result = await db.deleteFrom("upload_tokens").where("app_id", "=", appId).where("id", "=", id).executeTakeFirst();
	return result.numDeletedRows > 0n;
}

/** The token row the bearer names, with its use recorded, or null. */
export async function authenticateToken(db: Db, token: string, now: number): Promise<TokenRow | null> {
	const row = await db
		.updateTable("upload_tokens")
		.set({ last_used_at: now })
		.where("hash", "=", await hashToken(token))
		.returningAll()
		.executeTakeFirst();
	return row ?? null;
}
