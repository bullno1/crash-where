// Web shim of the crash reporter, an Emscripten JS library.
//
// The game and the watcher are two instances of the same binary. The game
// runs on the page. The watcher runs in a worker that loads only the
// game's script, never the page.
//
// CW_DISABLE=1 in the page's ENV stops the watcher before main().
//
// The watcher owns the store. Where it can suspend, the store is kept in
// the origin private file system; elsewhere it lives in memory.
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
	// The store on disk: a tree in the origin private file system that
	// mirrors the watcher's memory filesystem below one directory.
	//
	// Memory is what the C code reads and writes. A file is copied out when
	// it is renamed into place and deleted when it is removed, and the C
	// caller is suspended until that is done.
	$cwStore__deps: ['$FS', '$PATH', '$PATH_FS', '$UTF8ToString'],
	$cwStore: {
		ROOT: 'crash-where',
		// How long the load may take before the watcher starts without it.
		LOAD_MS: 2000,
		// A file being copied out carries this suffix until it is whole.
		PARTIAL: '.partial',

		root: null,
		abandoned: false,

		// Before main(): every stored file into memory, at its own path. The
		// whole tree, since the report directory is not known before cw_init().
		async open() {
			if (!navigator.storage?.getDirectory) {
				return;
			}
			const top = await navigator.storage.getDirectory();
			const root = await top.getDirectoryHandle(cwStore.ROOT, { create: true });
			const files = [];
			const load = async (dir, path) => {
				const jobs = [];
				for await (const [name, handle] of dir.entries()) {
					if (handle.kind === 'directory') {
						jobs.push(load(handle, path + name + '/'));
					} else if (name.endsWith(cwStore.PARTIAL)) {
						// Left by a watcher that ended in the middle of a copy.
						jobs.push(dir.removeEntry(name));
					} else {
						jobs.push(handle.getFile().then((f) => f.arrayBuffer()).then((bytes) => {
							files.push({ dir: path, path: path + name, bytes: new Uint8Array(bytes) });
						}));
					}
				}
				await Promise.all(jobs);
			};
			await load(root, '/');
			// All or nothing: a watcher that started without the store keeps none.
			if (cwStore.abandoned) {
				return;
			}
			for (const file of files) {
				FS.mkdirTree(file.dir);
				FS.writeFile(file.path, file.bytes);
			}
			cwStore.root = root;
		},

		// The directory of an absolute path, and the name within it.
		async locate(path, create) {
			const parts = path.split('/').filter(Boolean);
			const name = parts.pop();
			let dir = cwStore.root;
			for (const part of parts) {
				dir = await dir.getDirectoryHandle(part, { create });
			}
			return { dir, name };
		},

		async put(pathPtr) {
			if (!cwStore.root) {
				return;
			}
			const path = PATH_FS.resolve(UTF8ToString(pathPtr));
			try {
				// Read now: memory is this call's until it returns.
				const bytes = FS.readFile(path);
				const { dir, name } = await cwStore.locate(path, true);
				// Whole under another name first, then renamed, as it was in memory.
				// Without a rename the file is written in place.
				const probe = await dir.getFileHandle(name + cwStore.PARTIAL, { create: true });
				const renames = typeof probe.move === 'function';
				const file = renames ? probe : await dir.getFileHandle(name, { create: true });
				const handle = await file.createSyncAccessHandle();
				handle.truncate(0);
				handle.write(bytes, { at: 0 });
				handle.flush();
				handle.close();
				if (renames) {
					// With the directory: the name alone is not accepted everywhere.
					await file.move(dir, name);
				} else {
					await dir.removeEntry(name + cwStore.PARTIAL);
				}
			} catch (e) {
				err(`crash reporter: ${path} not saved: ${e}`);
			}
		},

		async remove(pathPtr) {
			if (!cwStore.root) {
				return;
			}
			const path = PATH_FS.resolve(UTF8ToString(pathPtr));
			try {
				const { dir, name } = await cwStore.locate(path, false);
				await dir.removeEntry(name);
			} catch (e) {
				// Never stored, or gone already.
				if (e.name !== 'NotFoundError') {
					err(`crash reporter: ${path} not deleted: ${e}`);
				}
			}
		},

		lock(pathPtr) {
			if (!navigator.locks) {
				return 1;
			}
			const name = cwStore.ROOT + PATH_FS.resolve(UTF8ToString(pathPtr));
			return new Promise((resolve) => {
				navigator.locks.request(name, { ifAvailable: true }, (lock) => {
					resolve(lock ? 1 : 0);
					// Held for as long as this promise is pending: until the watcher ends.
					return lock ? new Promise(() => {}) : undefined;
				});
			});
		},
	},

	cw_web_store_put__deps: ['$cwStore'],
	cw_web_store_put: (path) => {},
	cw_web_store_delete__deps: ['$cwStore'],
	cw_web_store_delete: (path) => {},
	cw_web_store_lock__deps: ['$cwStore'],
	cw_web_store_lock: (path) => 1,

	$cwWeb__deps: [
		'$addRunDependency', '$removeRunDependency', '$ENV', '$wasmTable',
		'$stringToNewUTF8', '$stringToUTF8', '$UTF8ToString', 'malloc', 'free',
		'$cwStore', 'cw_web_store_put', 'cw_web_store_delete', 'cw_web_store_lock',
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
		// One marked `watcher` is replaced in the watcher alone.
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
					if (s && (!s.watcher || cwWeb.role === 'watcher')) {
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
			// And the store, when this watcher will be able to keep one.
			if (typeof WebAssembly.Suspending === 'function') {
				cwWeb.suspending.push(
					{ stub: _cw_web_store_put, run: cwStore.put, watcher: true },
					{ stub: _cw_web_store_delete, run: cwStore.remove, watcher: true },
					{ stub: _cw_web_store_lock, run: cwStore.lock, watcher: true },
				);
				addRunDependency('cw-store');
				const giveUp = new Promise((resolve) => setTimeout(resolve, cwStore.LOAD_MS, 'late'));
				Promise.race([cwStore.open(), giveUp])
					.then((late) => {
						if (late) {
							cwStore.abandoned = true;
							err('crash reporter: the store did not load in time, reports are kept in memory');
						}
					})
					.catch((e) => err(`crash reporter: no store, reports are kept in memory: ${e}`))
					.then(() => removeRunDependency('cw-store'));
			}
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

		// Function index to the file offset of the function's first
		// instruction, for an engine that prints no offset. From the binary
		// the game already downloaded, read again on the first report that
		// needs it; null when it cannot be read.
		async loadOffsets() {
			if (cwWeb.offsets !== undefined) {
				return;
			}
			cwWeb.offsets = null;
			try {
				const bytes = Module['wasmBinary']
					? new Uint8Array(Module['wasmBinary'])
					: new Uint8Array(await (await fetch(findWasmBinary(), { credentials: 'same-origin' })).arrayBuffer());
				cwWeb.offsets = cwWeb.parseOffsets(bytes);
			} catch (e) {
				err(`crash reporter: function offsets unknown: ${e}`);
			}
		},

		// Walks the import and code sections. Throws on a shape it does
		// not know, rather than guess at offsets.
		parseOffsets(b) {
			let p = 8;
			const leb = () => {
				let v = 0;
				let shift = 0;
				let c;
				do {
					c = b[p++];
					v += (c & 0x7f) * 2 ** shift;
					shift += 7;
				} while (c & 0x80);
				return v;
			};
			// A value type: one byte, or a reference type with a heap type after it.
			const type = () => {
				const t = b[p++];
				if (t === 0x63 || t === 0x64) {
					leb();
				}
			};
			const limits = () => {
				const flags = b[p++];
				leb();
				if (flags & 1) {
					leb();
				}
			};
			let imports = 0;
			while (p < b.length) {
				const id = b[p++];
				const size = leb();
				const end = p + size;
				if (id === 2) {
					for (let n = leb(); n > 0; --n) {
						// Module and field names.
						for (let k = 0; k < 2; ++k) {
							const len = leb();
							p += len;
						}
						const kind = b[p++];
						if (kind === 0) {
							leb();
							++imports;
						} else if (kind === 1) {
							type();
							limits();
						} else if (kind === 2) {
							limits();
						} else if (kind === 3) {
							type();
							++p;
						} else if (kind === 4) {
							++p;
							leb();
						} else {
							throw new Error(`import kind ${kind}`);
						}
					}
				} else if (id === 10) {
					const offsets = [];
					for (let n = leb(); n > 0; --n) {
						const size = leb();
						const start = p;
						// Past the locals.
						for (let l = leb(); l > 0; --l) {
							leb();
							type();
						}
						offsets.push(p);
						p = start + size;
					}
					return { imports, offsets };
				}
				p = end;
			}
			throw new Error('no code section');
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
				// An engine that names a frame by function index alone.
				if (/wasm-function\[\d+\](?!:0x)/.test(r['stack'])) {
					await cwWeb.loadOffsets();
				}
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
			cwWeb.post({ 'env': Object.assign({}, ENV) });
			// If disabled, do not wait for the waitcher
			if (ENV['CW_DISABLE'] === '1') {
				cwWeb.watcher = null;
				return;
			}
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
	cw_web_watcher_quit__deps: ['$cwWeb'],
	cw_web_watcher_quit: () => close(),

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

	cw_web_function_offset__deps: ['$cwWeb'],
	cw_web_function_offset: (index) => {
		const table = cwWeb.offsets;
		const i = table ? index - table.imports : -1;
		return i >= 0 && i < table.offsets.length ? table.offsets[i] : 0;
	},

	cw_web_module_name__deps: ['$stringToUTF8', '$lengthBytesUTF8'],
	cw_web_module_name: (out, cap) => {
		const path = findWasmBinary();
		const name = path.slice(path.lastIndexOf('/') + 1).split('?')[0];
		stringToUTF8(lengthBytesUTF8(name) < cap ? name : '', out, cap);
	},
});
