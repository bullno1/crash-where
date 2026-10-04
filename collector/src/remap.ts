import { fingerprint, selectFrames, type SkipList } from "./grouping";
import type { AppShard } from "./shard";
import { type RawFrame, resymbolicate } from "./symbols";

/**
 * Names again, from a table just stored, the frames of that build in
 * every group that holds some, and rehashes those groups. A group
 * created while the build had no table was hashed on its module alone;
 * with names it may become its own crash or the same as one already
 * seen, which the shard's remap settles.
 */
export async function remapForBuild(
	bucket: R2Bucket, shard: DurableObjectStub<AppShard>, app: string, buildId: string, skip: SkipList
): Promise<{ updated: number; merged: number }> {
	const changes = [];
	for (const g of await shard.listStoredGroups()) {
		const named = await resymbolicate(bucket, app, buildId, JSON.parse(g.frames) as RawFrame[]);
		if (named === null) continue;
		const frames = JSON.stringify(named);
		const hash = await fingerprint(g.fault, selectFrames(named, skip), g.message);
		if (frames !== g.frames || hash !== g.fingerprint) changes.push({ id: g.id, frames, fingerprint: hash });
	}
	return changes.length === 0 ? { updated: 0, merged: 0 } : shard.remap(changes);
}
