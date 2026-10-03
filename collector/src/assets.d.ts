/** `.sql` and `.css` files are bundled as text by the Text rule in wrangler.toml. */
declare module "*.sql" {
	const text: string;
	export default text;
}

declare module "*.css" {
	const text: string;
	export default text;
}
