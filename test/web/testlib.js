// Test platform for the web, an Emscripten JS library linked into cw_test.
//
// The page sets Module.testRole:
//   'runner'  runs the suites; test_spawn_self() suspends it while a child runs.
//   'child'   runs one scenario and reports how it ended to the page above.
// The watcher of a child needs no role of its own here: it answers one
// message, with the directories and files of the run directory.
addToLibrary({
	$testWeb__deps: [
		'$cwWeb', '$cwStore', '$ENV', '$FS', '$PATH_FS', '$wasmTable', '$stringToNewUTF8',
		'$UTF8ToString', 'malloc', 'cw_http_web_fetch',
	],
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

		// The event log of a run, written by the game and by its watcher.
		EVENTS: '/events.jsonl',

		// Hand every append to the event log to `send` as it happens. Game and
		// watcher each write their own copy; the runner's is the one in order.
		streamEvents(send) {
			const write = FS.write;
			FS.write = (stream, buffer, offset, length, ...rest) => {
				const n = write(stream, buffer, offset, length, ...rest);
				if (stream.path?.endsWith(testWeb.EVENTS)) {
					send(new Uint8Array(buffer.buffer, buffer.byteOffset + offset, n).slice());
				}
				return n;
			};
		},

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
			cwWeb.suspending.push({ stub: _test_web_spawn, run: _test_web_spawn });
			Module['instantiateWasm'] = (imports, receive) => {
				if (Module['testTransport'] === 'xhr') {
					// Send as a browser that cannot suspend would.
					cwWeb.suspending = cwWeb.suspending.filter((s) => s.stub !== _cw_http_web_fetch);
				}
				// The spawn, and whatever else registered itself, such as the transport.
				cwWeb.suspend(imports);
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

		// Put the runner's copy of a directory where the child's watcher will
		// load its store from, in place of whatever an earlier child left.
		// The runner's files stand in for a disk the children would share.
		async pushStore(dir) {
			if (!navigator.storage?.getDirectory) {
				return;
			}
			const path = PATH_FS.resolve(dir);
			const parts = path.split('/').filter(Boolean);
			const name = parts.pop();
			let parent = await (await navigator.storage.getDirectory()).getDirectoryHandle(cwStore.ROOT, { create: true });
			for (const part of parts) {
				parent = await parent.getDirectoryHandle(part, { create: true });
			}
			await parent.removeEntry(name, { recursive: true }).catch(() => {});
			if (!FS.analyzePath(path).exists) {
				return;
			}
			const copy = async (from, into, as) => {
				const to = await into.getDirectoryHandle(as, { create: true });
				for (const entry of FS.readdir(from)) {
					if (entry === '.' || entry === '..') {
						continue;
					}
					const source = from + '/' + entry;
					if (FS.isDir(FS.stat(source).mode)) {
						await copy(source, to, entry);
					} else {
						const out = await (await to.getFileHandle(entry, { create: true })).createWritable();
						await out.write(FS.readFile(source));
						await out.close();
					}
				}
			};
			await copy(path, parent, name);
		},

		// Whether the stored copy of a directory holds exactly `files`, a map
		// of path to bytes: what the watcher had in memory when it ended.
		async storeMatches(dir, files) {
			if (!navigator.storage?.getDirectory) {
				return true;
			}
			const path = PATH_FS.resolve(dir);
			const stored = {};
			const walk = async (handle, at) => {
				for await (const [name, entry] of handle.entries()) {
					if (entry.kind === 'directory') {
						await walk(entry, at + '/' + name);
					} else {
						stored[at + '/' + name] = new Uint8Array(await (await entry.getFile()).arrayBuffer());
					}
				}
			};
			try {
				let handle = await (await navigator.storage.getDirectory()).getDirectoryHandle(cwStore.ROOT);
				for (const part of path.split('/').filter(Boolean)) {
					handle = await handle.getDirectoryHandle(part);
				}
				await walk(handle, path);
			} catch (e) {
				// No stored directory: right only when memory has no file either.
			}
			const kept = {};
			for (const [name, bytes] of Object.entries(files)) {
				const full = PATH_FS.resolve(name);
				if (full.startsWith(path + '/')) {
					kept[full] = bytes;
				}
			}
			let same = true;
			for (const name of new Set([...Object.keys(stored), ...Object.keys(kept)])) {
				const a = stored[name];
				const b = kept[name];
				if (!a || !b || a.length !== b.length || a.some((byte, i) => byte !== b[i])) {
					err(`store: ${name} is ${a ? `${a.length} bytes` : 'absent'} on disk, ${b ? `${b.length} bytes` : 'absent'} in the watcher`);
					same = false;
				}
			}
			return same;
		},

		removeTree(path) {
			if (!FS.analyzePath(path).exists) {
				return;
			}
			for (const entry of FS.readdir(path)) {
				if (entry === '.' || entry === '..') {
					continue;
				}
				const child = path + '/' + entry;
				if (FS.isDir(FS.stat(child).mode)) {
					testWeb.removeTree(child);
				} else {
					FS.unlink(child);
				}
			}
			FS.rmdir(path);
		},

		async spawn(env) {
			const store = JSON.parse(env)['CW_TEST_REPORT_DIR'];
			await testWeb.pushStore(store);
			return new Promise((resolve) => {
				const frame = document.createElement('iframe');
				frame.style.display = 'none';
				const timer = setTimeout(() => {
					err(`child did not end within ${testWeb.SPAWN_MS} ms`);
					removeEventListener('message', onMessage);
					frame.remove();
					resolve(-1);
				}, testWeb.SPAWN_MS);
				const events = PATH_FS.resolve(JSON.parse(env)['CW_TEST_OUT'] + testWeb.EVENTS);
				const onMessage = (e) => {
					if (e.source !== frame.contentWindow) {
						return;
					}
					if (e.data['event']) {
						FS.writeFile(events, e.data['event'], { flags: 'a' });
						return;
					}
					if (!e.data['exit']) {
						return;
					}
					clearTimeout(timer);
					removeEventListener('message', onMessage);
					// The watcher's store replaces the copy here: what it deleted is gone.
					if (e.data['dirs'].length > 0) {
						testWeb.removeTree(PATH_FS.resolve(store));
					}
					// Directories too: one the watcher emptied must still exist here.
					for (const dir of e.data['dirs']) {
						FS.mkdirTree(dir);
					}
					for (const [path, bytes] of Object.entries(e.data['files'])) {
						// The watcher's event log is its half; the whole one is already here.
						if (PATH_FS.resolve(path) !== events) {
							FS.writeFile(path, bytes);
							Module['testOnFile']?.(path, bytes);
						}
					}
					Module['testOnFile']?.(events, FS.readFile(events));
					const exit = e.data['exit'];
					const status = (exit['signaled'] ? testWeb.SIGNALED : 0) | (exit['code'] & 0xffff);
					// A watcher that answered must have stored what it holds.
					const stored = e.data['dirs'].length > 0 ? testWeb.storeMatches(store, e.data['files']) : Promise.resolve(true);
					stored.then((same) => {
						// Removing the frame ends the game and its watcher.
						frame.remove();
						resolve(same ? status : -1);
					});
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
			Module['preRun'] = [() => {
				Object.assign(ENV, env);
				testWeb.streamEvents((bytes) => parent.postMessage({ 'event': bytes }, '*'));
				// The watcher writes nothing before it has the environment, sent after this.
				cwWeb.watcher?.addEventListener('message', (e) => {
					if (e.data['testEvent']) {
						parent.postMessage({ 'event': e.data['testEvent'] }, '*');
					}
				});
			}].concat(Module['preRun'] || []);

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
			Module['preRun'] = [() => {
				testWeb.streamEvents((bytes) => postMessage({ 'testEvent': bytes }));
			}].concat(Module['preRun'] || []);
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

	// The servers the transport tests send to live in the launcher. The
	// runner reaches them with synchronous requests, so that this channel
	// shares nothing with the transport under test.
	$testHttp__deps: ['$stringToUTF8', '$UTF8ToString'],
	$testHttp: {
		// Requests read back so far, by server id and index.
		seen: {},

		control(method, path, body) {
			const x = new XMLHttpRequest();
			x.open(method, '/http/' + path, false);
			x.send(body);
			return x.status === 200 ? x.responseText : null;
		},
	},

	test_web_http_start__deps: ['$testHttp'],
	test_web_http_start: (status, body, bodyLen, location, url, urlCap) => {
		const bytes = HEAPU8.subarray(body, body + bodyLen);
		const started = testHttp.control('POST', 'start', JSON.stringify({
			status,
			body: body ? btoa(String.fromCharCode(...bytes)) : null,
			location: location ? UTF8ToString(location) : null,
		}));
		if (started === null) {
			return -1;
		}
		const { id, url: base } = JSON.parse(started);
		stringToUTF8(base, url, urlCap);
		testHttp.seen[id] = {};
		return id;
	},

	test_web_http_count__deps: ['$testHttp'],
	test_web_http_count: (id) => Number(testHttp.control('GET', `${id}/count`)),

	test_web_http_load__deps: ['$testHttp'],
	test_web_http_load: (id, index) => {
		const text = testHttp.control('GET', `${id}/request/${index}`);
		if (text === null) {
			return false;
		}
		testHttp.seen[id][index] = JSON.parse(text);
		return true;
	},

	test_web_http_field__deps: ['$testHttp'],
	test_web_http_field: (id, index, name, out, cap) => {
		stringToUTF8(testHttp.seen[id][index][UTF8ToString(name)], out, cap);
	},

	test_web_http_body__deps: ['$testHttp'],
	test_web_http_body: (id, index, out, cap) => {
		const body = atob(testHttp.seen[id][index]['body']);
		const len = Math.min(body.length, cap);
		for (let i = 0; i < len; ++i) {
			HEAPU8[out + i] = body.charCodeAt(i);
		}
		return len;
	},

	test_web_http_stop__deps: ['$testHttp'],
	test_web_http_stop: (id) => {
		testHttp.control('POST', `${id}/stop`);
		delete testHttp.seen[id];
	},
});
