# collector

Cloudflare Worker that receives crash reports from the `cw` client and serves the dashboard.

[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/bullno1/crash-where/tree/collector)

The button clones the `collector` branch into your GitHub or GitLab account and deploys it with Workers Builds.
Afterwards, attach your dashboard hostname to the Worker as a custom domain, and keep the clone current with `./update`.

```sh
npm install
npm run dev      # serves on http://localhost:8787
npm run check    # type-check
npm test         # unit tests
npm run deploy   # needs a logged-in wrangler
```

## Updating a deployment

The [`collector`](https://github.com/bullno1/crash-where/tree/collector) branch of the upstream repository holds this directory as its root.
To update, inside this repository, run:

```sh
./update
```

A conflict in `wrangler.toml` keeps the deployment's copy, and prints what upstream changed there so a new binding can be added by hand.
Any other conflict aborts the merge and names the files.
`CW_UPSTREAM` and `CW_UPSTREAM_BRANCH` override the upstream repository and branch.

## Dashboard access

Every route outside `/v1/` sits behind Cloudflare Access.
The Worker verifies the JWT that Access attaches to each request, so a request that reaches the Worker by any other path is refused.

To view the dashboard under `wrangler dev`, run, with the dev server up:

```sh
npm run login -- https://crash-where.bullno1.com
```

It fetches a token for the deployed dashboard with `cloudflared`, logging in through the browser first when there is none, and opens the dev server's `/dev/login` route, which stores the token as the cookie Access itself would set.
That route answers only on localhost, and the Worker verifies the cookie on every request as it would a token from the edge.
`CW_DASHBOARD_URL` replaces the argument, and a second argument picks another port.
