// Web backend of the host collector, an Emscripten JS library.
//
// The init half reads the navigator of the page. The report half runs in
// the watcher, which cannot look at the page: it reads what the shim
// kept of the game's last message.
addToLibrary({
	$cwHost__deps: ['$cwWeb', '$stringToUTF8', '$lengthBytesUTF8'],
	$cwHost: {
		// The user agent in two parts: the first comment, where every
		// browser names the operating system, and the product tokens after
		// it, which name the browser. `Mozilla/5.0` before the comment says
		// nothing and is dropped.
		userAgent() {
			const ua = String(navigator.userAgent || '');
			const open = ua.indexOf('(');
			const close = open >= 0 ? ua.indexOf(')', open) : -1;
			if (close < 0) {
				return { platform: '', browser: ua.trim() };
			}
			return { platform: ua.slice(open + 1, close).trim(), browser: ua.slice(close + 1).trim() };
		},

		// The renderer of a WebGL context made for this and dropped at once.
		// Empty where no context can be made or the browser hides the name.
		gpu() {
			if (typeof OffscreenCanvas !== 'function') {
				return '';
			}
			let gl = null;
			try {
				gl = new OffscreenCanvas(1, 1).getContext('webgl');
			} catch (e) {
				return '';
			}
			if (!gl) {
				return '';
			}
			const info = gl.getExtension('WEBGL_debug_renderer_info');
			const name = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : '';
			gl.getExtension('WEBGL_lose_context')?.loseContext();
			return String(name || '');
		},

		fact(what) {
			switch (what) {
			case 0: return cwHost.userAgent().browser;
			case 1: return cwHost.userAgent().platform;
			case 2: return String(navigator.language || '');
			case 3: return cwHost.gpu();
			default: return '';
			}
		},
	},

	cw_host_web_fact__deps: ['$cwHost'],
	cw_host_web_fact: (what, out, cap) => {
		const s = cwHost.fact(what);
		if (s === '' || lengthBytesUTF8(s) >= cap) {
			return false;
		}
		stringToUTF8(s, out, cap);
		return true;
	},

	cw_host_web_cpu_cores: () => navigator.hardwareConcurrency || 0,

	// Chromium alone offers it, already rounded and capped.
	cw_host_web_ram_mb: () => Math.round((navigator.deviceMemory || 0) * 1024),

	cw_host_web_game_rss_mb__deps: ['$cwWeb'],
	cw_host_web_game_rss_mb: () => Math.floor((cwWeb.report?.['memory'] || 0) / 1048576),
});
