# collector

Cloudflare Worker that receives crash reports from the `cw` client and serves the dashboard.

```sh
npm install
npm run dev      # serves on http://localhost:8787
npm run check    # type-check
npm test         # unit tests
npm run deploy   # needs a logged-in wrangler
```

## Dashboard access

Every route outside `/v1/` sits behind Cloudflare Access.
The Worker verifies the JWT that Access attaches to each request, so a request that reaches the Worker by any other path is refused.
Set the two vars in `wrangler.toml` from the Zero Trust dashboard: the team domain, and the AUD tag of the application that protects the dashboard hostname.

To view the dashboard under `wrangler dev`, run, with the dev server up:

```sh
npm run login -- https://crash-where.bullno1.com
```

It fetches a token for the deployed dashboard with `cloudflared`, logging in through the browser first when there is none, and opens the dev server's `/dev/login` route, which stores the token as the cookie Access itself would set.
That route answers only on localhost, and the Worker verifies the cookie on every request as it would a token from the edge.
`CW_DASHBOARD_URL` replaces the argument, and a second argument picks another port.
