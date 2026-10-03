// Web backend of the HTTP transport, an Emscripten JS library.
//
// fetch() is asynchronous and the transport's send() is not, so the
// request runs behind a suspending import. An instance that cannot
// suspend gets the plain function at the bottom instead, which sends a
// synchronous request.
addToLibrary({
	$cwHttp__deps: ['$cwWeb', '$UTF8ToString'],
	$cwHttp__postset: () => 'cwWeb.suspending.push({ stub: _cw_http_web_fetch, run: cwHttp.fetch });',
	$cwHttp: {
		RESPONDED: 0,
		FAILED: 1,
		INVALID_URL: 2,
		UNSUPPORTED: 3,
		// The status of a redirect is hidden when it is not followed, and gone
		// when it was. Which one it was does not matter to the caller, that it
		// was one does.
		REDIRECT: 302,

		// What both ways of sending start from, or null for a URL neither can use.
		prepare(method, url, contentType, contentEncoding, token) {
			let target;
			try {
				// No base: a relative URL would otherwise name the page itself.
				target = new URL(UTF8ToString(url));
			} catch (e) {
				return null;
			}
			if (target.protocol !== 'http:' && target.protocol !== 'https:') {
				return null;
			}
			target.hash = '';

			// No User-Agent: a browser sends its own, and not every one lets it be replaced.
			const headers = {};
			if (contentType) {
				headers['Content-Type'] = UTF8ToString(contentType);
			}
			if (contentEncoding) {
				headers['Content-Encoding'] = UTF8ToString(contentEncoding);
			}
			if (token) {
				headers['Authorization'] = 'Bearer ' + UTF8ToString(token);
			}
			return { method: UTF8ToString(method), url: target.href, headers };
		},

		respond(httpStatus, bytes, reply, replyCap, status, replyLen) {
			const len = Math.min(bytes.length, replyCap);
			HEAPU8.set(bytes.subarray(0, len), reply);
			HEAP32[status >> 2] = httpStatus;
			HEAPU32[replyLen >> 2] = len;
			return cwHttp.RESPONDED;
		},

		async fetch(method, url, contentType, contentEncoding, token, body, bodyLen, reply, replyCap, timeoutMs, status, replyLen) {
			const req = cwHttp.prepare(method, url, contentType, contentEncoding, token);
			if (!req) {
				return cwHttp.INVALID_URL;
			}
			const init = {
				method: req.method,
				headers: req.headers,
				redirect: 'manual',
				credentials: 'omit',
				cache: 'no-store',
				signal: AbortSignal.timeout(timeoutMs),
			};
			if (bodyLen > 0) {
				// A copy: the request outlives this call's view of memory.
				init.body = HEAPU8.slice(body, body + bodyLen);
			}

			let response;
			let bytes;
			try {
				response = await fetch(req.url, init);
				bytes = response.type === 'opaqueredirect'
					? new Uint8Array(0)
					: new Uint8Array(await response.arrayBuffer());
			} catch (e) {
				return cwHttp.FAILED;
			}
			// Memory may have grown while the request was in flight; the views are current again here.
			const httpStatus = response.type === 'opaqueredirect' ? cwHttp.REDIRECT : response.status;
			return cwHttp.respond(httpStatus, bytes, reply, replyCap, status, replyLen);
		},

		// The same request, synchronously, for an instance that cannot suspend.
		//
		// A synchronous request follows redirects by itself and cannot be told
		// not to. One is only noticed afterwards, by the address the reply came
		// from, and by then the browser has asked the other address too.
		send(method, url, contentType, contentEncoding, token, body, bodyLen, reply, replyCap, timeoutMs, status, replyLen) {
			if (typeof XMLHttpRequest === 'undefined') {
				return cwHttp.UNSUPPORTED;
			}
			const req = cwHttp.prepare(method, url, contentType, contentEncoding, token);
			if (!req) {
				return cwHttp.INVALID_URL;
			}
			const x = new XMLHttpRequest();
			try {
				x.open(req.method, req.url, false);
				for (const [name, value] of Object.entries(req.headers)) {
					x.setRequestHeader(name, value);
				}
				// Bytes kept apart as characters: a page may not ask a synchronous
				// request for a binary reply.
				x.overrideMimeType('text/plain; charset=x-user-defined');
				// Nor may a page set a timeout on one.
				if (typeof WorkerGlobalScope !== 'undefined') {
					x.timeout = timeoutMs;
				}
				x.send(bodyLen > 0 ? HEAPU8.slice(body, body + bodyLen) : null);
			} catch (e) {
				return cwHttp.FAILED;
			}
			if (x.status === 0) {
				return cwHttp.FAILED;
			}
			if (x.responseURL !== req.url) {
				return cwHttp.respond(cwHttp.REDIRECT, new Uint8Array(0), reply, replyCap, status, replyLen);
			}
			const bytes = Uint8Array.from(x.responseText, (c) => c.charCodeAt(0) & 0xff);
			return cwHttp.respond(x.status, bytes, reply, replyCap, status, replyLen);
		},
	},

	cw_http_web_fetch__deps: ['$cwHttp'],
	cw_http_web_fetch: (method, url, contentType, contentEncoding, token, body, bodyLen, reply, replyCap, timeoutMs, status, replyLen) =>
		cwHttp.send(method, url, contentType, contentEncoding, token, body, bodyLen, reply, replyCap, timeoutMs, status, replyLen),
});
