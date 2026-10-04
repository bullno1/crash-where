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
