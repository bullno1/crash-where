# crash-where-collector

Cloudflare Worker that receives crash reports from the `cw` client and serves the dashboard.

## Deployment

[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/bullno1/crash-where/tree/collector)

The button clones the `collector` branch into your GitHub or GitLab account and deploys it with Workers Builds.
Afterwards, attach your dashboard hostname to the Worker as a custom domain, and keep the clone current with `./update`.

### Updating a deployment

The [`collector`](https://github.com/bullno1/crash-where/tree/collector) branch of the upstream repository holds this directory as its root.
To update, inside this directory, run:

```sh
./update
```

A conflict in `wrangler.toml` keeps the deployment's copy, and prints what upstream changed there so a new binding can be added by hand.
Any other conflict aborts the merge and names the files.
`CW_UPSTREAM` and `CW_UPSTREAM_BRANCH` override the upstream repository and branch.

## Dashboard access

The dashboard lives under `/dashboard`, and the root redirects there.
Everything under that prefix needs a login, in one of two modes chosen by which secrets are set; `/` and `/v1/` stay outside it.

With `DASHBOARD_PASSWORD` set and nothing else, the browser asks for a name and that password.
This is the first-run mode: one shared password, no second factor, no record of who did what, and a logout only when the browser closes.
Generate the password rather than invent it, with `openssl rand -base64 24`; the Worker refuses to serve with one shorter than 16 characters.
A rate limiting rule on the dashboard hostname, which the free plan includes, blunts password guessing.

With `ACCESS_TEAM_DOMAIN` and `ACCESS_AUD` set, the prefix sits behind Cloudflare Access, and the password is ignored.
Scope the Access application to `<host>/dashboard` so that it guards the same paths the Worker does.
The Worker verifies the JWT that Access attaches to each request, so a request that reaches the Worker by any other path is refused.
Moving from the password to Access is setting those two secrets and deleting the password.

To view the dashboard under `wrangler dev`, run, with the dev server up:

```sh
npm run login -- https://crash-where.bullno1.com  # Replace with your domain
```

It fetches a token for the deployed dashboard with `cloudflared`, logging in through the browser first when there is none, and opens the dev server's `/dev/login` route, which stores the token as the cookie Access itself would set.
That route answers only on localhost, and the Worker verifies the cookie on every request as it would a token from the edge.
`CW_DASHBOARD_URL` replaces the argument, and a second argument picks another port.

## Development

```sh
npm install
npm run dev    # Apply migrations locally and run a dev server
npm run check  # Type check
npm test       # Run tests
npm run deploy # Apply migrations remotely and deploy
```

Tests run inside the Workers runtime  with the migrations applied to a fresh database before each test file.

### Database

Apps and crash metadata live in a D1 database bound as `DB`.
Its schema is the numbered SQL files under `migrations/`, applied with Wrangler's D1 migrations, which record what has run in the `d1_migrations` table.
`npm run dev` applies them to the local database before starting the dev server, and `npm run deploy` applies them to the deployed one before deploying the Worker, so a migration must be safe to run against the version of the Worker that is live while it runs.
Add a migration with `npx wrangler d1 migrations create DB <name>` and never edit one that has been applied anywhere.
Queries go through Kysely with its D1 dialect, typed by `src/db.generated.ts`.
`npm run db:types` regenerates that file by applying the migrations locally and introspecting the result with `kysely-codegen`, so run it after adding a migration and commit the output.

`wrangler.toml` names the database but carries no id.
The Deploy button creates the database and writes the id into the clone it deploys from.
A deployment made by hand needs the database created once, with `npx wrangler d1 create crash-where`, before the first `npm run deploy`.
Workers Builds runs `npx wrangler deploy` by default, which skips the migrations; set its deploy command to `npm run deploy`.
