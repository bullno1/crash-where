import tseslint from "typescript-eslint";

// Pages reach the response only through `render` in src/page.ts, which takes
// the escaped-string type the `html` tag produces. These rules keep the two
// ways around that, calling `c.html` directly and `raw`, out of the code.
export default tseslint.config(
	{ ignores: ["node_modules/", ".wrangler/", "src/*.generated.ts"] },
	{
		files: ["**/*.ts", "**/*.mts"],
		languageOptions: { parser: tseslint.parser },
		rules: {
			"no-restricted-syntax": [
				"error",
				{
					selector: 'CallExpression > MemberExpression.callee[property.name="html"]',
					message: "Call render() from src/page.ts instead of c.html(), so only escaped HTML is sent.",
				},
			],
			"no-restricted-imports": [
				"error",
				{
					paths: [
						{ name: "hono/html", importNames: ["raw"], message: "Build markup with the html tag; raw() bypasses escaping." },
						{ name: "hono/utils/html", importNames: ["raw"], message: "Build markup with the html tag; raw() bypasses escaping." },
					],
				},
			],
		},
	},
);
