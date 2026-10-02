// Web shim of the crash reporter, an Emscripten JS library.
//
// The game and the watcher are two instances of the same binary. The game
// runs on the page. The watcher runs in a worker that loads only the
// game's script, never the page.
//
// CW_DISABLE=1 in the page's ENV stops the watcher before main().
//
// What a page may set on Module:
//   cwRole    'none' keeps the shim out of this instance.
//   cwOnDone  called with { kind, ... } once the watcher has dealt with a
//             trap ('trap', with `message`) or an exit ('shutdown', with
//             `result`), or has died with work outstanding ('lost').
//
// Message fields are quoted throughout: the watcher's bootstrap is a
// string, which a minifier does not rename.
addToLibrary({
	$cwWeb__deps: [
		'$addRunDependency', '$removeRunDependency', '$ENV', '$wasmTable',
		'$stringToNewUTF8', '$stringToUTF8', '$UTF8ToString', 'malloc', 'free',
	],
	$cwWeb__postset: () => {
		// Emitted into preRun(), after the entries of Module.preRun.
		addAtPreRun('cwWeb.preRun();');
		// Emitted at the top level: after every pre-js, before instantiation starts.
		return 'cwWeb.boot();';
	},
	$cwWeb: {
		// How long main() may wait for the watcher before running without it.
		HOLD_MS: 3000,

		role: 'game',
		module: null,
		// Imports that return a promise, as { stub, run }: `run` replaces
		// `stub` where an instance can suspend. Other libraries add theirs.
		suspending: [],

		// Game.
		watcher: null,
		snapshot: null,
		region: 0,
		regionLen: 0,
		held: false,
		released: false,
		trapped: false,
		pending: 0,

		// Watcher.
		entries: null,
		waiting: null,
		announced: null,
		canSuspend: false,
		queue: null,

		boot() {
			if (Module['cwRole'] === 'none') {
				cwWeb.role = 'none';
			} else if (Module['cwRole'] === 'watcher') {
				cwWeb.role = 'watcher';
				cwWeb.bootWatcher();
			} else {
				cwWeb.bootGame();
			}
		},

		instantiate: (imports) => WebAssembly.instantiateStreaming(
			fetch(findWasmBinary(), { credentials: 'same-origin' }), imports
		),

		// Let the registered imports suspend the instance about to be made
		// from `imports`. Its entry points must then be called as promising.
		suspend(imports) {
			if (typeof WebAssembly.Suspending !== 'function') {
				return false;
			}
			// By identity: optimized builds rename the imports.
			for (const ns of Object.values(imports)) {
				for (const k of Object.keys(ns)) {
					const s = cwWeb.suspending.find((s) => s.stub === ns[k]);
					if (s) {
						ns[k] = new WebAssembly.Suspending(s.run);
					}
				}
			}
			return true;
		},

		// Game.

		spawn() {
			const dir = _scriptName.slice(0, _scriptName.lastIndexOf('/') + 1);
			// Runs before the game's script, so before its asset loader starts a download.
			const src = `
				self.Module = {
					'cwRole': 'watcher',
					'getPreloadedPackage': () => new ArrayBuffer(0),
					'locateFile': (p) => ${JSON.stringify(dir)} + p,
					'print': (s) => postMessage({ 'log': s, 'stream': 'out' }),
					'printErr': (s) => postMessage({ 'log': s, 'stream': 'err' }),
				};
				importScripts(${JSON.stringify(_scriptName)});`;
			return new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
		},

		bootGame() {
			const watcher = cwWeb.watcher = cwWeb.spawn();
			watcher.addEventListener('message', (e) => {
				const m = e.data;
				if (m['log'] !== undefined) {
					(m['stream'] === 'out' ? out : err)(m['log']);
				} else if (m['snapshot']) {
					cwWeb.snapshot = m['snapshot'];
					cwWeb.release();
				} else if (m['done']) {
					cwWeb.pending--;
					Module['cwOnDone']?.(m['done']);
				}
			});
			watcher.addEventListener('error', (e) => {
				e.preventDefault();
				// One the shim stopped itself may still report its interrupted start.
				if (cwWeb.watcher !== watcher) {
					return;
				}
				err(`crash reporter: watcher failed: ${e.message}`);
				cwWeb.snapshot = null;
				cwWeb.watcher = null;
				cwWeb.release();
				if (cwWeb.pending > 0) {
					cwWeb.pending = 0;
					Module['cwOnDone']?.({ 'kind': 'lost' });
				}
			});
			// Listeners, never assignments: the page may own window.onerror.
			addEventListener('error', (e) => cwWeb.trap(e.error));
			addEventListener('unhandledrejection', (e) => cwWeb.trap(e.reason));

			// The page may instantiate by itself, for a loading bar. Its hook stays in charge.
			const page = Module['instantiateWasm'];
			Module['instantiateWasm'] = (imports, receive) => {
				const done = (inst, mod) => {
					// No module means the page kept it; the watcher then compiles its own.
					cwWeb.post({ 'module': mod || null });
					receive(inst, mod);
				};
				if (page) {
					return page(imports, done);
				}
				cwWeb.instantiate(imports).then((r) => done(r.instance, r.module));
				return {};
			};
		},

		post(m) {
			cwWeb.watcher?.postMessage(m);
		},

		release() {
			if (cwWeb.released) {
				return;
			}
			cwWeb.released = true;
			if (cwWeb.held) {
				removeRunDependency('cw-watcher');
			}
		},

		// After the trap. Reads memory; never calls into the instance.
		trap(e) {
			if (cwWeb.trapped || !cwWeb.watcher || !cwWeb.regionLen || !(e instanceof WebAssembly.RuntimeError)) {
				return;
			}
			cwWeb.trapped = true;
			cwWeb.pending++;
			cwWeb.post({ 'report': {
				'message': String(e.message),
				'stack': String(e.stack),
				'region': HEAPU8.slice(cwWeb.region, cwWeb.region + cwWeb.regionLen),
			} });
		},


		// Watcher.

		bootWatcher() {
			let onModule;
			const gotModule = new Promise((resolve) => { onModule = resolve; });
			cwWeb.waiting = [];
			cwWeb.announced = [];
			// Not a member initializer: library members are serialized at link time.
			cwWeb.queue = Promise.resolve();
			// main() needs the game's environment, which the game knows only in its preRun().
			addRunDependency('cw-env');
			addEventListener('message', (e) => {
				const m = e.data;
				if (m['module'] !== undefined) {
					onModule(m['module']);
				} else if (m['env']) {
					Object.assign(ENV, m['env']);
					ENV['CW_WATCHER'] = '1';
					removeRunDependency('cw-env');
				} else if (cwWeb.entries) {
					cwWeb.enqueue(m);
				} else {
					cwWeb.waiting.push(m);
				}
			});
			Module['instantiateWasm'] = (imports, receive) => {
				cwWeb.canSuspend = cwWeb.suspend(imports);
				gotModule.then(async (mod) => {
					if (mod) {
						// A compiled module resolves to the instance alone.
						cwWeb.module = mod;
						receive(await WebAssembly.instantiate(mod, imports));
					} else {
						// Bytes resolve to the module and the instance.
						const r = await cwWeb.instantiate(imports);
						cwWeb.module = r.module;
						receive(r.instance);
					}
				});
				return {};
			};
		},

		// One call into the instance at a time: a suspended call leaves its
		// frames on the C stack, and a second call would run over them.
		enqueue(m) {
			cwWeb.queue = cwWeb.queue.then(() => cwWeb.handle(m)).catch((e) => {
				// Out of the promise, so that the game hears of a watcher that died.
				setTimeout(() => { throw e; });
			});
		},

		async handle(m) {
			// The table holds the function itself; an export may be wrapped.
			const call = (index, ...args) => {
				const fn = wasmTable.get(index);
				return cwWeb.canSuspend ? WebAssembly.promising(fn)(...args) : fn(...args);
			};
			if (m['report']) {
				const r = m['report'];
				const message = stringToNewUTF8(r['message']);
				const stack = stringToNewUTF8(r['stack']);
				const region = _malloc(r['region'].length);
				HEAPU8.set(r['region'], region);
				await call(cwWeb.entries.report, message, stack, region);
				_free(region);
				_free(stack);
				_free(message);
				postMessage({ 'done': { 'kind': 'trap', 'message': r['message'] } });
			} else if (m['consent'] !== undefined) {
				await call(cwWeb.entries.consent, m['consent']);
			} else if (m['auth']) {
				const a = m['auth'];
				const copy = (bytes) => {
					const ptr = _malloc(bytes.length);
					HEAPU8.set(bytes, ptr);
					return ptr;
				};
				const token = copy(a['token']);
				const proof = copy(a['proof']);
				await call(cwWeb.entries.auth, a['what'], token, a['token'].length, proof, a['proof'].length);
				_free(proof);
				_free(token);
			} else if (m['shutdown'] !== undefined) {
				await call(cwWeb.entries.shutdown, m['shutdown']);
				postMessage({ 'done': { 'kind': 'shutdown', 'result': m['shutdown'] } });
			}
		},


		preRun() {
			if (cwWeb.role !== 'game') {
				return;
			}
			// A disabled game has no watcher. This one was started before the
			// page could say so, and would obey the variable too if it got it.
			if (ENV['CW_DISABLE'] === '1') {
				const watcher = cwWeb.watcher;
				cwWeb.watcher = null;
				watcher?.terminate();
				return;
			}
			cwWeb.post({ 'env': Object.assign({}, ENV) });
			if (!cwWeb.released) {
				// Nothing blocks: main() is simply not called until the count drops.
				cwWeb.held = true;
				addRunDependency('cw-watcher');
				setTimeout(() => {
					if (!cwWeb.released) {
						err(`crash reporter: no watcher after ${cwWeb.HOLD_MS} ms, starting without it`);
						cwWeb.release();
					}
				}, cwWeb.HOLD_MS);
			}
		},
	},

	cw_web_game_start__deps: ['$cwWeb'],
	cw_web_game_start: (region, len) => {
		if (cwWeb.role !== 'game' || !cwWeb.snapshot) {
			return false;
		}
		cwWeb.region = region;
		cwWeb.regionLen = len;
		return true;
	},

	cw_web_game_consent__deps: ['$cwWeb'],
	cw_web_game_consent: () => cwWeb.snapshot['consent'],

	cw_web_game_pending_count__deps: ['$cwWeb'],
	cw_web_game_pending_count: () => cwWeb.snapshot['pending'].length,

	cw_web_game_pending__deps: ['$cwWeb', '$stringToUTF8', '$lengthBytesUTF8'],
	cw_web_game_pending: (i, name, cap, approved) => {
		const p = cwWeb.snapshot['pending'][i];
		if (!p || lengthBytesUTF8(p['name']) >= cap) {
			return false;
		}
		stringToUTF8(p['name'], name, cap);
		HEAPU8[approved] = p['approved'] ? 1 : 0;
		return true;
	},

	cw_web_game_notify_consent__deps: ['$cwWeb'],
	cw_web_game_notify_consent: (choice) => {
		cwWeb.post({ 'consent': choice });
	},

	cw_web_game_notify_auth__deps: ['$cwWeb'],
	cw_web_game_notify_auth: (what, token, tokenLen, proof, proofLen) => {
		cwWeb.post({ 'auth': {
			'what': what,
			'token': HEAPU8.slice(token, token + tokenLen),
			'proof': HEAPU8.slice(proof, proof + proofLen),
		} });
	},

	cw_web_game_notify_shutdown__deps: ['$cwWeb'],
	cw_web_game_notify_shutdown: (result) => {
		if (cwWeb.watcher) {
			cwWeb.pending++;
			cwWeb.post({ 'shutdown': result });
		}
	},

	cw_web_watcher_pending__deps: ['$cwWeb'],
	cw_web_watcher_pending: (name, approved) => {
		cwWeb.announced.push({ 'name': UTF8ToString(name), 'approved': !!approved });
	},

	cw_web_watcher_ready__deps: ['$cwWeb'],
	cw_web_watcher_ready: (consent, report, consentFn, auth, shutdown) => {
		cwWeb.entries = { report, consent: consentFn, auth, shutdown };
		postMessage({ 'snapshot': { 'consent': consent, 'pending': cwWeb.announced } });
		// The handlers run from the event loop, never from inside main().
		const waiting = cwWeb.waiting;
		cwWeb.waiting = [];
		setTimeout(() => waiting.forEach(cwWeb.enqueue));
	},

	cw_web_build_id__deps: ['$cwWeb'],
	cw_web_build_id: (out, cap) => {
		const sections = cwWeb.module ? WebAssembly.Module.customSections(cwWeb.module, 'build_id') : [];
		if (sections.length === 0) {
			return 0;
		}
		// One length byte, then the id.
		const bytes = new Uint8Array(sections[0]);
		const len = Math.min(bytes[0], bytes.length - 1, cap);
		HEAPU8.set(bytes.subarray(1, 1 + len), out);
		return len;
	},

	cw_web_module_name__deps: ['$stringToUTF8', '$lengthBytesUTF8'],
	cw_web_module_name: (out, cap) => {
		const path = findWasmBinary();
		const name = path.slice(path.lastIndexOf('/') + 1).split('?')[0];
		stringToUTF8(lengthBytesUTF8(name) < cap ? name : '', out, cap);
	},
});
