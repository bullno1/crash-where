# crash-where-collector

Cloudflare Worker that receives crash reports from the `cw` client and serves the dashboard.

## Deployment

[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/bullno1/crash-where/tree/collector)

The button clones the `collector` branch into your GitHub or GitLab account and deploys it with Workers Builds.
Afterwards, attach your dashboard hostname to the Worker as a custom domain, and keep the clone current with `./update`.

## Updating a deployment

The [`collector`](https://github.com/bullno1/crash-where/tree/collector) branch of the upstream repository holds this directory as its root.
To update, inside this directory, run:

```sh
./update
```

A conflict in `wrangler.toml` keeps the deployment's copy, and prints what upstream changed there so a new binding can be added by hand.
Any other conflict aborts the merge and names the files.
`CW_UPSTREAM` and `CW_UPSTREAM_BRANCH` override the upstream repository and branch.

## Dashboard access

Every route outside `/v1/` needs a login, in one of two modes chosen by which secrets are set.

With `DASHBOARD_PASSWORD` set and nothing else, the browser asks for a name and that password.
This is the first-run mode: one shared password, no second factor, no record of who did what, and a logout only when the browser closes.
Generate the password rather than invent it, with `openssl rand -base64 24`; the Worker refuses to serve with one shorter than 16 characters.
A rate limiting rule on the dashboard hostname, which the free plan includes, blunts password guessing.

With `ACCESS_TEAM_DOMAIN` and `ACCESS_AUD` set, every route outside `/v1/` sits behind Cloudflare Access, and the password is ignored.
The Worker verifies the JWT that Access attaches to each request, so a request that reaches the Worker by any other path is refused.
Moving from the password to Access is setting those two secrets and deleting the password.

To view the dashboard under `wrangler dev`, run, with the dev server up:

```sh
npm run login -- https://crash-where.bullno1.com  # Replace with your domain
```

It fetches a token for the deployed dashboard with `cloudflared`, logging in through the browser first when there is none, and opens the dev server's `/dev/login` route, which stores the token as the cookie Access itself would set.
That route answers only on localhost, and the Worker verifies the cookie on every request as it would a token from the edge.
`CW_DASHBOARD_URL` replaces the argument, and a second argument picks another port.

# Development

```sh
npm install
npm run dev    # Run a dev server
npm run check  # Type check
npm test       # Run tests
npm run deploy # Deploy
```
