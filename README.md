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

### Source links

An app's settings page takes a source link template, a URI template (RFC 6570) that turns each source location on a crash page into a link to the page that holds the file, for example `https://github.com/org/repo/blob/{commit}/{+file}#L{line}` or `https://gitlab.com/org/repo/-/blob/{commit}/{+file}#L{line}`.
The template may name `{commit}` and `{version}` as the symbol upload recorded them, `{+file}` for the path relative to the checkout root, and `{line}`.
A location is linked only when every one it names is known.
For the first two, CI passes the commit and the checkout root to the upload:

```sh
cwsym upload --endpoint https://crash.example.com --app forest-quest --version 1.4.2 --channel stable \
  --commit "$GITHUB_SHA" --source-root "$GITHUB_WORKSPACE" game.pdb
```

A build compiled with `-ffile-prefix-map` already records relative paths and needs no `--source-root`, and a project that tags its versions can write `blob/v{version}/{+file}` and pass no `--commit`.

### Web origins

A web build sends its reports from a page on another origin than the collector.
An app's settings page lists the origins allowed, one per line as the browser sends them in the `Origin`header.
Each should have the form: `scheme://host[:port]`, with `*` matching any run of characters: `https://game.example.com`, `https://*.itch.io`, `http://localhost:*`.

### Scripting the dashboard

Every dashboard route answers in JSON when the request prefers it, with the same login.
Send `Accept: application/json`; a browser, or `curl` without the header, gets the page.
A GET returns the object the page is rendered from.
A POST takes a form body or a JSON object and answers with a status and the created row instead of a redirect; a new upload token comes back in the body.
A form body still needs an `Origin` header matching the dashboard, as a browser sends; a JSON body does not.

```sh
curl -u "alice:$PASSWORD" -H 'Accept: application/json' https://crash.example.com/dashboard/apps/forest-quest
curl -u "alice:$PASSWORD" -H 'Accept: application/json' -H 'Content-Type: application/json' \
  -d '{"label":"GitHub Actions"}' https://crash.example.com/dashboard/apps/forest-quest/tokens
curl -u "alice:$PASSWORD" -H 'Accept: application/json' -X POST https://crash.example.com/dashboard/apps/forest-quest/tokens/<token id>/regenerate
curl -u "alice:$PASSWORD" -H 'Accept: application/json' https://crash.example.com/dashboard/apps/forest-quest/versions
curl -u "alice:$PASSWORD" -H 'Accept: application/json' -H 'Content-Type: application/json' \
  -d '{"display_name":"Forest Quest","sample_cap_trusted":"5","sample_cap_untrusted":"2","source_link_template":"","cors_origins":"https://game.example.com"}' https://crash.example.com/dashboard/apps/forest-quest/settings
curl -u "alice:$PASSWORD" -H 'Accept: application/json' -X POST https://crash.example.com/dashboard/apps/forest-quest/settings/disable
curl -u "alice:$PASSWORD" -H 'Accept: application/json' https://crash.example.com/dashboard/apps/forest-quest/crashes/3?sample=<report id>
```

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
npm run e2e    # Run the end-to-end tests against built binaries
npm run deploy # Apply migrations remotely and deploy
```

Tests run inside the Workers runtime  with the migrations applied to a fresh database before each test file.
The end-to-end tests under `test/e2e/` run in Node itself, start a dev server in a temporary directory, and drive the native `cwsym` and `crashme` binaries from the directory `CW_BIN_DIR` names; `cmd/<toolchain>/e2e` at the repository root builds them and runs this.

### Database

Schema changes are numbered SQL files under `migrations/`: `migrations/root` for the collector's own D1 database and `migrations/shard` for the per-app storage.
`npm run dev` and `npm run deploy` apply the root migrations before starting or deploying; the per-app storage migrates itself.
Add a root migration with `npx wrangler d1 migrations create DB <name>`, a shard migration by adding the next numbered file and listing it in `migrations/shard/index.ts`, and never edit a migration that has been applied anywhere.
After adding one, run `npm run db:types` and commit its output.

`wrangler.toml` names the database but carries no id.
The Deploy button creates the database and writes the id into the clone it deploys from.
A deployment made by hand needs the database created once, with `npx wrangler d1 create crash-where`, before the first `npm run deploy`.
The same goes for the bucket, with `npx wrangler r2 bucket create crash-where-symbols`.
Workers Builds runs `npx wrangler deploy` by default, which skips the migrations; set its deploy command to `npm run deploy`.
