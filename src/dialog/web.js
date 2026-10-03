// Web backend of the dialog library, an Emscripten JS library.
//
// A Worker has no document, so the watcher posts the words to the page
// and waits for the pressed button behind a suspending import. The page
// draws a bare <dialog>, or whatever Module.cwShowDialog returns. An
// instance that cannot suspend gets the plain function at the bottom
// instead, which shows nothing.
addToLibrary({
	$cwDialog__deps: ['$cwWeb', '$UTF8ToString'],
	$cwDialog__postset: () => `
		cwWeb.suspending.push({ stub: _cw_dialog_web_show, run: cwDialog.show, watcher: true });
		cwWeb.page['dialog'] = cwDialog.onPage;`,
	$cwDialog: {
		DISMISSED: -1,
		UNSUPPORTED: -2,

		// Watcher: the dialog waiting for its answer, as { id, resolve }.
		waiting: null,
		last: 0,
		listening: false,

		// Watcher. Resolves to the pressed button's index.
		show(title, message, labels, n) {
			if (!cwDialog.listening) {
				cwDialog.listening = true;
				addEventListener('message', (e) => {
					const r = e.data['dialogResult'];
					if (r && cwDialog.waiting && r['id'] === cwDialog.waiting.id) {
						const { resolve } = cwDialog.waiting;
						cwDialog.waiting = null;
						resolve(r['button']);
					}
				});
			}
			const desc = {
				'id': ++cwDialog.last,
				'title': UTF8ToString(title),
				'message': UTF8ToString(message),
				'labels': [],
			};
			for (let i = 0; i < n; ++i) {
				desc['labels'].push(UTF8ToString(HEAPU32[(labels >> 2) + i]));
			}
			return new Promise((resolve) => {
				cwDialog.waiting = { id: desc['id'], resolve };
				postMessage({ 'dialog': desc });
			});
		},

		// Page: show it, then answer the watcher.
		async onPage(d) {
			let button = cwDialog.DISMISSED;
			try {
				const custom = Module['cwShowDialog'];
				button = custom
					? await custom(d['title'], d['message'], d['labels'])
					: await cwDialog.native(d);
			} catch (e) {
				err(`crash reporter: dialog failed: ${e}`);
			}
			cwWeb.post({ 'dialogResult': {
				'id': d['id'],
				'button': Number.isInteger(button) ? button : cwDialog.DISMISSED,
			} });
		},

		// A <dialog> with no style of its own, under class names the page
		// may address. Escape and the close event count as dismissal.
		native(d) {
			if (typeof document === 'undefined') {
				return cwDialog.DISMISSED;
			}
			// A game that holds the pointer would keep the player from the buttons.
			document.exitPointerLock?.();
			const dialog = document.createElement('dialog');
			dialog.className = 'cw-dialog';
			const title = document.createElement('h2');
			title.className = 'cw-dialog-title';
			title.textContent = d['title'];
			const message = document.createElement('p');
			message.className = 'cw-dialog-message';
			message.style.whiteSpace = 'pre-line';
			message.textContent = d['message'];
			const row = document.createElement('div');
			row.className = 'cw-dialog-buttons';
			dialog.append(title, message, row);
			let pressed = cwDialog.DISMISSED;
			d['labels'].forEach((label, i) => {
				const button = document.createElement('button');
				button.className = 'cw-dialog-button';
				button.textContent = label;
				button.autofocus = i === 0;
				button.addEventListener('click', () => {
					pressed = i;
					dialog.close();
				});
				row.append(button);
			});
			return new Promise((resolve) => {
				dialog.addEventListener('close', () => {
					dialog.remove();
					resolve(pressed);
				});
				document.body.append(dialog);
				dialog.showModal();
			});
		},
	},

	cw_dialog_web_show__deps: ['$cwDialog'],
	cw_dialog_web_show: (title, message, labels, n) => cwDialog.UNSUPPORTED,
});
