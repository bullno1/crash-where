/** Name of the envelope object under a sample's prefix. */
export const ENVELOPE_OBJECT = "envelope.json";

/** The prefix a sampled report's objects share. */
export function sampleKey(app: string, reportId: string): string {
	return `samples/${app}/${reportId}/`;
}

/** Deletes every object of a sample, however many of its attachments arrived. */
export async function deleteSample(bucket: R2Bucket, prefix: string): Promise<void> {
	const listed = await bucket.list({ prefix });
	if (listed.objects.length > 0) await bucket.delete(listed.objects.map((o) => o.key));
}

/** What the client may attach, by extension: the type stored and the largest body accepted. */
export const ATTACHMENTS: Record<string, { contentType: string; maxBytes: number }> = {
	dmp: { contentType: "application/octet-stream", maxBytes: 64 * 1024 * 1024 },
	snap: { contentType: "application/octet-stream", maxBytes: 16 * 1024 * 1024 },
	log: { contentType: "text/plain", maxBytes: 1024 * 1024 },
};

const SIDECAR = /^[0-9]{1,20}_[a-z]_([A-Za-z0-9-]{1,64})\.([a-z]+)$/;

/**
 * The kind of a sidecar named `<ts>_<kind>_<report id>.<ext>`, or null
 * when the name is not one, names another report or has an extension
 * the client never sends.
 */
export function attachmentKind(name: string, reportId: string): { contentType: string; maxBytes: number } | null {
	const m = SIDECAR.exec(name);
	if (m === null || m[1] !== reportId) return null;
	return ATTACHMENTS[m[2]!] ?? null;
}
