// Test platform for the web, an Emscripten JS library linked into cw_test.
//
// The page sets Module.testRole:
//   'runner'  runs the suites; test_spawn_self() suspends it while a child runs.
//   'child'   runs one scenario and reports how it ended to the page above.
// The watcher of a child needs no role of its own here: it answers one
// message, with the directories and files of the run directory.
addToLibrary({
	$testWeb__deps: ['$cwWeb', '$ENV', '$FS', '$wasmTable', '$stringToNewUTF8', '$UTF8ToString', 'malloc'],
	$testWeb__postset: () => 'testWeb.boot();',
	$testWeb: {
		// A trap has no signal number; the runner's expectations are written in them.
		SIGABRT: 6,
		SIGSEGV: 11,
		SIGNALED: 0x10000,
		// How long a child's watcher may take to hand over the run directory.
		FILES_MS: 5000,
		// How long a child may run. One that neither exits nor dies would hold the suite forever.
		SPAWN_MS: 30000,

		boot() {
			if (Module['cwRole'] === 'watcher') {
				testWeb.bootWatcher();
			} else if (Module['testRole'] === 'runner') {
				testWeb.bootRunner();
			} else if (Module['testRole'] === 'child') {
				testWeb.bootChild();
			}
		},

		// Runner.

		bootRunner() {
			Module['instantiateWasm'] = (imports, receive) => {
				// By identity: optimized builds rename the imports.
				for (const ns of Object.values(imports)) {
					for (const k of Object.keys(ns)) {
						if (ns[k] === _test_web_spawn) {
							ns[k] = new WebAssembly.Suspending(_test_web_spawn);
						}
					}
				}
				WebAssembly.instantiateStreaming(fetch(findWasmBinary(), { credentials: 'same-origin' }), imports)
					.then((r) => receive(r.instance, r.module));
				return {};
			};
			// Resolves to what main() returned.
			Module['testRun'] = (args) => {
				const argv = _malloc(4 * (args.length + 2));
				['cw_test', ...args].forEach((arg, i) => {
					HEAPU32[(argv >> 2) + i] = stringToNewUTF8(arg);
				});
				HEAPU32[(argv >> 2) + args.length + 1] = 0;
				// The table holds the function itself; an export may be wrapped.
				const main = wasmTable.get(_test_web_main());
				return WebAssembly.promising(main)(args.length + 1, argv);
			};
		},

		spawn(env) {
			return new Promise((resolve) => {
				const frame = document.createElement('iframe');
				frame.style.display = 'none';
				const timer = setTimeout(() => {
					err(`child did not end within ${testWeb.SPAWN_MS} ms`);
					removeEventListener('message', onMessage);
					frame.remove();
					resolve(-1);
				}, testWeb.SPAWN_MS);
				const onMessage = (e) => {
					if (e.source !== frame.contentWindow || !e.data['exit']) {
						return;
					}
					clearTimeout(timer);
					removeEventListener('message', onMessage);
					// Directories too: one the watcher emptied must still exist here.
					for (const dir of e.data['dirs']) {
						FS.mkdirTree(dir);
					}
					for (const [path, bytes] of Object.entries(e.data['files'])) {
						FS.writeFile(path, bytes);
						Module['testOnFile']?.(path, bytes);
					}
					// Removing the frame ends the game and its watcher.
					frame.remove();
					const exit = e.data['exit'];
					resolve((exit['signaled'] ? testWeb.SIGNALED : 0) | (exit['code'] & 0xffff));
				};
				addEventListener('message', onMessage);
				frame.src = 'child.html#' + encodeURIComponent(env);
				document.body.appendChild(frame);
			});
		},

		// Child.

		bootChild() {
			const env = Module['testEnv'];
			// First, so the page's own hooks and the shim see the variables.
			Module['preRun'] = [() => Object.assign(ENV, env)].concat(Module['preRun'] || []);

			let exit = null;
			let finished = false;
			const send = (tree) => {
				if (!finished) {
					finished = true;
					parent.postMessage({ 'exit': exit, 'dirs': tree['dirs'], 'files': tree['files'] }, '*');
				}
			};
			const nothing = { 'dirs': [], 'files': {} };
			const finish = () => {
				if (!cwWeb.watcher) {
					return send(nothing);
				}
				cwWeb.watcher.addEventListener('message', (e) => {
					if (e.data['testTree']) {
						send(e.data['testTree']);
					}
				});
				cwWeb.watcher.postMessage({ 'testPublish': env['CW_TEST_OUT'] });
				setTimeout(() => send(nothing), testWeb.FILES_MS);
			};
			// An error nobody catches ends a process. A page lives on, so the
			// child says it died, unless the shim took the trap and will say so.
			const died = (e) => {
				if (exit || e === 'unwind' || e?.name === 'ExitStatus') {
					return;
				}
				const abort = /^Aborted/.test(String(e?.message));
				exit = { 'signaled': true, 'code': abort ? testWeb.SIGABRT : testWeb.SIGSEGV };
				// Once every listener has run, the shim's among them.
				setTimeout(() => {
					if (cwWeb.pending === 0) {
						finish();
					}
				});
			};
			addEventListener('error', (e) => died(e.error));
			addEventListener('unhandledrejection', (e) => died(e.reason));
			Module['onExit'] = (code) => {
				exit ??= { 'signaled': false, 'code': code };
				// A shutdown still with the watcher ends the child when it is done.
				if (cwWeb.pending === 0) {
					finish();
				}
			};
			Module['cwOnDone'] = (done) => {
				if (done['kind'] === 'trap') {
					const abort = /^Aborted/.test(done['message']);
					exit = { 'signaled': true, 'code': abort ? testWeb.SIGABRT : testWeb.SIGSEGV };
				}
				if (exit) {
					finish();
				}
			};
		},

		// Watcher of a child.

		bootWatcher() {
			addEventListener('message', (e) => {
				const root = e.data['testPublish'];
				if (!root) {
					return;
				}
				const dirs = [];
				const files = {};
				const walk = (dir) => {
					dirs.push(dir);
					for (const name of FS.readdir(dir)) {
						if (name === '.' || name === '..') {
							continue;
						}
						const path = dir + '/' + name;
						if (FS.isDir(FS.stat(path).mode)) {
							walk(path);
						} else {
							files[path] = FS.readFile(path);
						}
					}
				};
				if (FS.analyzePath(root).exists) {
					walk(root);
				}
				postMessage({ 'testTree': { 'dirs': dirs, 'files': files } });
			});
		},
	},

	test_web_spawn__deps: ['$testWeb'],
	test_web_spawn: (env) => testWeb.spawn(UTF8ToString(env)),
});
