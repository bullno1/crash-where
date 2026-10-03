/** `.sql` files are bundled as text by the Text rule in wrangler.toml. */
declare module "*.sql" {
	const text: string;
	export default text;
}
