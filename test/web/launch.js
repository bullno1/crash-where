// Runs cw_test in a headless browser and exits with its status.
//
// usage: node launch.js --bin <dir> --work <dir> --browser <name>[=<command>] [suite [test]]
//
// Serves the build over the loopback interface, prints what the page
// logs as it arrives, and keeps the run directories the page hands back
// under <work>. Needs nothing beyond Node.
const fs = require('fs');
const http = require('http');
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
const [browser, command = browser] = opts.browser.split('=');

const PAGES = { '/runner.html': 'runner.html', '/child.html': 'child.html' };
const TYPES = {
	'.html': 'text/html', '.js': 'text/javascript',
	'.wasm': 'application/wasm', '.data': 'application/octet-stream',
};

const profile = path.join(opts.work, `profile-${browser}`);
fs.rmSync(profile, { recursive: true, force: true });
fs.mkdirSync(profile, { recursive: true });

let child;
let finished = false;
function finish(code) {
	if (finished) {
		return;
	}
	finished = true;
	clearTimeout(idle);
	child?.kill('SIGKILL');
	// The browser may still be writing its profile.
	setTimeout(() => {
		fs.rmSync(profile, { recursive: true, force: true });
		process.exit(code);
	}, 300);
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

const server = http.createServer((req, res) => {
	alive();
	const url = new URL(req.url, 'http://localhost');
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
	});
	res.end(content);
});

server.listen(0, '127.0.0.1', () => {
	// The runner sees the test variables of this process, as a native one would.
	const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => k.startsWith('CW_TEST_')));
	const query = new URLSearchParams({ args: JSON.stringify(opts.args), env: JSON.stringify(env) });
	const url = `http://127.0.0.1:${server.address().port}/runner.html?${query}`;
	const flags = browser === 'firefox'
		? ['--headless', '--no-remote', '--profile', profile, url]
		// No sandbox: containers and CI runners cannot create one.
		: ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run', `--user-data-dir=${profile}`, url];
	child = spawn(command, flags, { stdio: 'ignore' });
	child.on('error', (e) => {
		console.error(`cannot start ${command}: ${e.message}`);
		finish(EXIT_NO_BROWSER);
	});
	alive();
});
