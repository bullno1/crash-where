// Runs cw_test in a browser and exits with its status.
//
// usage: node launch.js --bin <dir> --work <dir> --browser <name>[=<command>] [suite [test]]
//
// Serves the build over the loopback interface, prints what the page
// logs as it arrives, and keeps the run directories the page hands back
// under <work>. Also starts the HTTP servers the transport tests ask
// for. Needs nothing beyond Node.
//
// Chromium and Firefox run headless, from the command line. Safari has
// no headless mode and takes no URL on its command line: it is driven
// over WebDriver through safaridriver, whose <command> is then the
// driver, not the browser.
const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');
const { spawn } = require('child_process');

// How long the page may stay silent. Longer than a child may run.
const IDLE_MS = 60000;
const EXIT_TIMEOUT = 124;
const EXIT_NO_BROWSER = 127;

const opts = { args: [] };
for (let i = 2; i < process.argv.length; ++i) {
	const arg = process.argv[i];
	if (arg.startsWith('--')) {
		opts[arg.slice(2)] = process.argv[++i];
	} else {
		opts.args.push(arg);
	}
}
if (!opts.bin || !opts.work || !opts.browser) {
	console.error('usage: node launch.js --bin <dir> --work <dir> --browser <name>[=<command>] [suite [test]]');
	process.exit(2);
}
const [browser, command = browser === 'safari' ? 'safaridriver' : browser] = opts.browser.split('=');

const PAGES = { '/runner.html': 'runner.html', '/child.html': 'child.html' };
const TYPES = {
	'.html': 'text/html', '.js': 'text/javascript',
	'.wasm': 'application/wasm', '.data': 'application/octet-stream',
};

const profile = path.join(opts.work, `profile-${browser}`);
fs.rmSync(profile, { recursive: true, force: true });
fs.mkdirSync(profile, { recursive: true });

let child;
// Closes the browser before the process it came from is killed; set by
// the way the browser was started, when a kill alone does not close it.
let close = async () => {};
let finished = false;
function finish(code) {
	if (finished) {
		return;
	}
	finished = true;
	clearTimeout(idle);
	// Bounded: the browser is killed either way.
	const grace = new Promise((resolve) => setTimeout(resolve, 3000));
	Promise.race([close().catch(() => {}), grace]).then(() => {
		child?.kill('SIGKILL');
		// The browser may still be writing its profile.
		setTimeout(() => {
			fs.rmSync(profile, { recursive: true, force: true });
			process.exit(code);
		}, 300);
	});
}

function body(req, fn) {
	const chunks = [];
	req.on('data', (c) => chunks.push(c));
	req.on('end', () => fn(Buffer.concat(chunks)));
}

let idle;
function alive() {
	clearTimeout(idle);
	idle = setTimeout(() => {
		console.error(`${browser}: silent for ${IDLE_MS} ms, no exit status`);
		finish(EXIT_TIMEOUT);
	}, IDLE_MS);
}

// Servers a test starts to send requests to. Each listens on a port of
// its own, so it is another origin than the page, as a real endpoint is;
// it answers every request alike and remembers what arrived.
const PEER_MAX_REQUESTS = 4;
const peers = new Map();
let nextPeer = 1;

function startPeer(reply, done) {
	const peer = { count: 0, requests: [] };
	peer.server = http.createServer((req, res) => {
		alive();
		const cors = { 'Access-Control-Allow-Origin': '*', 'Connection': 'close' };
		if (req.method === 'OPTIONS') {
			// The browser asking leave to send; not a request of the test.
			res.writeHead(204, {
				...cors,
				'Access-Control-Allow-Methods': 'GET, POST, PUT',
				'Access-Control-Allow-Headers': req.headers['access-control-request-headers'] || '',
			});
			return res.end();
		}
		body(req, (b) => {
			const url = new URL(req.url, 'http://localhost');
			if (peer.requests.length < PEER_MAX_REQUESTS) {
				const header = (name) => req.headers[name] || '';
				peer.requests.push({
					method: req.method,
					uri: decodeURIComponent(url.pathname),
					query: url.search.slice(1),
					content_type: header('content-type'),
					content_length: header('content-length'),
					authorization: header('authorization'),
					user_agent: header('user-agent'),
					expect: header('expect'),
					body: b.toString('base64'),
				});
			}
			peer.count++;
			const content = Buffer.from(reply.body || '', 'base64');
			const headers = { ...cors, 'Content-Length': content.length };
			if (reply.location) {
				headers['Location'] = reply.location;
			}
			if (reply.body !== null) {
				headers['Content-Type'] = 'text/plain';
			}
			res.writeHead(reply.status || 200, headers);
			res.end(content);
		});
	});
	peer.server.listen(0, '127.0.0.1', () => {
		const id = nextPeer++;
		peers.set(id, peer);
		done({ id, url: `http://127.0.0.1:${peer.server.address().port}` });
	});
}

// POST /http/start, GET /http/<id>/count, GET /http/<id>/request/<n>, POST /http/<id>/stop
function control(req, res, parts) {
	if (parts[0] === 'start') {
		return body(req, (b) => startPeer(JSON.parse(b.toString()), (started) => res.end(JSON.stringify(started))));
	}
	const peer = peers.get(Number(parts[0]));
	if (!peer) {
		res.writeHead(404);
		return res.end();
	}
	if (parts[1] === 'count') {
		return res.end(String(peer.count));
	}
	if (parts[1] === 'request' && peer.requests[Number(parts[2])]) {
		return res.end(JSON.stringify(peer.requests[Number(parts[2])]));
	}
	if (parts[1] === 'stop') {
		// Kept-alive connections too: the port must refuse from now on.
		peer.server.close();
		peer.server.closeAllConnections();
		peers.delete(Number(parts[0]));
		return res.end();
	}
	res.writeHead(404);
	res.end();
}

const server = http.createServer((req, res) => {
	alive();
	const url = new URL(req.url, 'http://localhost');
	if (url.pathname.startsWith('/http/')) {
		return control(req, res, url.pathname.slice('/http/'.length).split('/'));
	}
	if (req.method === 'POST' && url.pathname === '/log') {
		return body(req, (b) => { console.log(b.toString()); res.end(); });
	}
	if (req.method === 'POST' && url.pathname === '/exit') {
		res.end();
		return finish(Number(url.searchParams.get('code')));
	}
	if (req.method === 'POST' && url.pathname.startsWith('/file/')) {
		return body(req, (b) => {
			const file = path.resolve(opts.work, url.pathname.slice('/file/'.length));
			if (file.startsWith(path.resolve(opts.work) + path.sep)) {
				fs.mkdirSync(path.dirname(file), { recursive: true });
				fs.writeFileSync(file, b);
			}
			res.end();
		});
	}
	const page = PAGES[url.pathname];
	const file = page ? path.join(__dirname, page) : path.join(opts.bin, path.basename(url.pathname));
	if (!fs.existsSync(file)) {
		res.writeHead(404);
		return res.end();
	}
	const content = fs.readFileSync(file);
	res.writeHead(200, {
		'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream',
		'Content-Length': content.length,
		'Cache-Control': 'no-store',
		// Cross-origin isolation, which a build with threads needs for its
		// shared memory. On every page, since a frame must match its parent.
		'Cross-Origin-Opener-Policy': 'same-origin',
		'Cross-Origin-Embedder-Policy': 'require-corp',
	});
	res.end(content);
});

// One request to a WebDriver server; resolves to the `value` it answers.
function webdriver(port, method, route, data) {
	return new Promise((resolve, reject) => {
		const req = http.request({ host: '127.0.0.1', port, method, path: route, headers: { 'Content-Type': 'application/json' } }, (res) => {
			body(res, (b) => {
				const reply = b.length ? JSON.parse(b.toString()) : {};
				if (res.statusCode >= 400) {
					reject(new Error(reply.value?.message || `${method} ${route}: ${res.statusCode}`));
				} else {
					resolve(reply.value);
				}
			});
		});
		req.on('error', reject);
		req.end(data === undefined ? undefined : JSON.stringify(data));
	});
}

// A port nothing listens on right now.
function freePort() {
	return new Promise((resolve, reject) => {
		const probe = net.createServer();
		probe.on('error', reject);
		probe.listen(0, '127.0.0.1', () => {
			const { port } = probe.address();
			probe.close(() => resolve(port));
		});
	});
}

// Opens `url` in a WebDriver session of safaridriver. The session's
// window starts clean and shares nothing with the user's own browsing,
// like a private window; deleting the session closes it. Safari must
// have been allowed once: `safaridriver --enable`.
async function startSafari(url) {
	const port = await freePort();
	child = spawn(command, ['-p', String(port)], { stdio: 'ignore' });
	const died = new Promise((resolve) => {
		child.on('error', (e) => resolve(`cannot start ${command}: ${e.message}`));
		child.on('exit', (code) => resolve(`${command} exited with ${code}`));
	});
	// Until the driver answers, or dies.
	for (let tries = 0; ; ++tries) {
		const result = await Promise.race([
			webdriver(port, 'GET', '/status').then(() => null, (e) => e),
			died,
		]);
		if (result === null) {
			break;
		}
		if (typeof result === 'string' || tries >= 100) {
			throw new Error(typeof result === 'string' ? result : `${command} does not answer on port ${port}`);
		}
		await new Promise((resolve) => setTimeout(resolve, 100));
	}
	const session = await webdriver(port, 'POST', '/session', {
		capabilities: { alwaysMatch: { browserName: 'safari' } },
	});
	close = () => webdriver(port, 'DELETE', `/session/${session.sessionId}`);
	await webdriver(port, 'POST', `/session/${session.sessionId}/url`, { url });
}

function startHeadless(url) {
	const flags = browser === 'firefox'
		? ['--headless', '--no-remote', '--profile', profile, url]
		// No sandbox: containers and CI runners cannot create one.
		: ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run', `--user-data-dir=${profile}`, url];
	child = spawn(command, flags, { stdio: 'ignore' });
	child.on('error', (e) => {
		console.error(`cannot start ${command}: ${e.message}`);
		finish(EXIT_NO_BROWSER);
	});
}

server.listen(0, '127.0.0.1', () => {
	// The runner sees the test variables of this process, as a native one would.
	const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => k.startsWith('CW_TEST_')));
	const query = new URLSearchParams({ args: JSON.stringify(opts.args), env: JSON.stringify(env) });
	const url = `http://127.0.0.1:${server.address().port}/runner.html?${query}`;
	if (browser === 'safari') {
		startSafari(url).catch((e) => {
			console.error(`cannot start safari: ${e.message}`);
			finish(EXIT_NO_BROWSER);
		});
	} else {
		startHeadless(url);
	}
	alive();
});
