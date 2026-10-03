export default {
	async fetch(request: Request): Promise<Response> {
		return new Response("Hello world", {
			headers: { "content-type": "text/plain; charset=utf-8" },
		});
	},
} satisfies ExportedHandler;
