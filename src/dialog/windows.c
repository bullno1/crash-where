/**
 * @file windows.c
 * Windows backend: a task dialog.
 *
 * Task dialogs live in Common Controls version 6, which an executable
 * gets only by declaring it in its manifest. An MSVC build of a program
 * that links this file gets the declaration from the linker directive
 * below. Elsewhere the entry point is simply absent and no dialog is
 * shown.
 */
#include <stdlib.h>
#include <windows.h>
#include <commctrl.h>

#include "backend.h"
#include "internal.h"

#if defined(_MSC_VER)
#	pragma comment(linker, "\"/manifestdependency:type='win32' " \
		"name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
		"processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/* Custom button ids start past the standard ones such as IDCANCEL. */
#define FIRST_BUTTON_ID 100

typedef HRESULT (WINAPI *task_dialog_indirect_t)(
	const TASKDIALOGCONFIG* config,
	int* button, int* radio, BOOL* verified
);

/**
 * UTF-8 to a freshly allocated UTF-16 string.
 *
 * @return `NULL` when out of memory or the text is not UTF-8.
 */
static wchar_t*
to_wide(const char* text) {
	int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (len <= 0) {
		return NULL;
	}
	wchar_t* wide = malloc((size_t)len * sizeof(wchar_t));
	if (wide != NULL) {
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len);
	}
	return wide;
}

/**
 * The watcher has no window of its own to put the dialog in front of.
 * It may take the foreground since the game that had it started it.
 */
static HRESULT CALLBACK
on_event(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LONG_PTR user) {
	(void)wparam;
	(void)lparam;
	(void)user;
	if (msg == TDN_CREATED) {
		SetForegroundWindow(hwnd);
	}
	return S_OK;
}

int
cw_dialog_backend_show(
	const char* title, const char* message,
	const char* const* labels, int n
) {
	/* Resolved by name: an executable without the manifest dependency loads a version without it. */
	HMODULE comctl = LoadLibraryW(L"comctl32.dll");
	task_dialog_indirect_t task_dialog_indirect = comctl != NULL
		? (task_dialog_indirect_t)(void*)GetProcAddress(comctl, "TaskDialogIndirect")
		: NULL;
	if (task_dialog_indirect == NULL) {
		cw_log(CW_LOG_WARN, "task dialogs unavailable, Common Controls 6 is not in the manifest");
		return -1;
	}

	int pressed = -1;
	wchar_t* wide_title = to_wide(title);
	wchar_t* wide_message = to_wide(message);
	wchar_t* wide_labels[CW_DIALOG_BUTTONS] = { 0 };
	TASKDIALOG_BUTTON buttons[CW_DIALOG_BUTTONS];
	bool ok = wide_title != NULL && wide_message != NULL;
	for (int i = 0; i < n && ok; ++i) {
		wide_labels[i] = to_wide(labels[i]);
		ok = wide_labels[i] != NULL;
		buttons[i] = (TASKDIALOG_BUTTON){ .nButtonID = FIRST_BUTTON_ID + i, .pszButtonText = wide_labels[i] };
	}
	if (!ok) {
		cw_log(CW_LOG_ERROR, "dialog text could not be converted");
		goto cleanup;
	}

	/* Crisp text on a scaled display; a no-op when the manifest already says so. */
	SetProcessDPIAware();
	TASKDIALOGCONFIG config = {
		.cbSize = sizeof(config),
		.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT,
		.pszWindowTitle = wide_title,
		.pszContent = wide_message,
		.cButtons = (UINT)n,
		.pButtons = buttons,
		.nDefaultButton = FIRST_BUTTON_ID,
		.pfCallback = on_event,
	};
	int button = 0;
	HRESULT hr = task_dialog_indirect(&config, &button, NULL, NULL);
	if (FAILED(hr)) {
		cw_log(CW_LOG_WARN, "task dialog failed: 0x%08lx", (unsigned long)hr);
	} else if (button >= FIRST_BUTTON_ID && button < FIRST_BUTTON_ID + n) {
		pressed = button - FIRST_BUTTON_ID;
	}

cleanup:
	for (int i = 0; i < n; ++i) {
		free(wide_labels[i]);
	}
	free(wide_message);
	free(wide_title);
	return pressed;
}
