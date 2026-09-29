/**
 * @file windows.c
 * Windows facts from the registry, kernel32, ntdll, and DXGI.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "backend.h"
#include "cw.h"
#include "internal.h"

typedef LONG (WINAPI *rtl_get_version_t)(RTL_OSVERSIONINFOW* info);
typedef const char* (CDECL *wine_get_version_t)(void);
typedef void (CDECL *wine_get_host_version_t)(const char** sysname, const char** release);
typedef HRESULT (WINAPI *create_dxgi_factory1_t)(REFIID riid, void** factory);

/* Declared here so the library needs no dxguid. */
static const GUID iid_dxgi_factory1 = {
	0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 }
};
static const GUID iid_dxgi_device = {
	0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c }
};

static void
set_env_wide(const char* key, const wchar_t* value) {
	char buf[256];
	if (WideCharToMultiByte(CP_UTF8, 0, value, -1, buf, sizeof(buf), NULL, NULL) > 0) {
		cw_set_env(key, buf);
	}
}

/** A string value under `HKEY_LOCAL_MACHINE`, with leading blanks dropped. */
static bool
read_registry(const char* path, const char* name, char* out, DWORD cap) {
	if (RegGetValueA(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_SZ, NULL, out, &cap) != ERROR_SUCCESS) {
		return false;
	}
	size_t skip = strspn(out, " \t");
	if (skip > 0) {
		memmove(out, out + skip, strlen(out + skip) + 1);
	}
	return out[0] != '\0';
}

static void
put_os_version(void) {
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == NULL) {
		return;
	}
	rtl_get_version_t get_version = (rtl_get_version_t)GetProcAddress(ntdll, "RtlGetVersion");
	RTL_OSVERSIONINFOW v = { .dwOSVersionInfoSize = sizeof(v) };
	if (get_version != NULL && get_version(&v) == 0) {
		char buf[48];
		snprintf(buf, sizeof(buf), "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
		cw_set_env("os_version", buf);
	}

	/* Wine exports these from its ntdll; Windows has neither. */
	wine_get_version_t wine_version = (wine_get_version_t)GetProcAddress(ntdll, "wine_get_version");
	if (wine_version != NULL) {
		char buf[96];
		snprintf(buf, sizeof(buf), "wine %s", wine_version());
		cw_set_env("compat", buf);
	}
	wine_get_host_version_t host_version = (wine_get_host_version_t)GetProcAddress(ntdll, "wine_get_host_version");
	if (host_version != NULL) {
		const char* sysname = NULL;
		const char* release = NULL;
		host_version(&sysname, &release);
		if (sysname != NULL) {
			char buf[96];
			snprintf(buf, sizeof(buf), "%s %s", sysname, release != NULL ? release : "");
			cw_set_env("compat_host", buf);
		}
	}
}

static void
put_device(void) {
	static const char* const bios = "HARDWARE\\DESCRIPTION\\System\\BIOS";
	char vendor[64];
	char product[64];
	bool have_vendor = read_registry(bios, "SystemManufacturer", vendor, sizeof(vendor));
	if (!read_registry(bios, "SystemProductName", product, sizeof(product))) {
		return;
	}
	char device[160];
	if (have_vendor) {
		snprintf(device, sizeof(device), "%s %s", vendor, product);
	} else {
		snprintf(device, sizeof(device), "%s", product);
	}
	cw_set_env("device", device);
}

void
cw_host_backend_init(void) {
	cw_set_env("os", "windows");
	put_os_version();
#if defined(_M_X64)
	cw_set_env("arch", "x86_64");
#elif defined(_M_ARM64)
	cw_set_env("arch", "aarch64");
#elif defined(_M_IX86)
	cw_set_env("arch", "x86");
#endif
	put_device();
	char buf[128];
	if (read_registry("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", buf, sizeof(buf))) {
		cw_set_env("cpu", buf);
	}
	cw_host_set_u64("cpu_cores", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
	cw_host_cpu_features(buf, sizeof(buf));
	if (buf[0] != '\0') {
		cw_set_env("cpu_features", buf);
	}
	MEMORYSTATUSEX mem = { .dwLength = sizeof(mem) };
	if (GlobalMemoryStatusEx(&mem)) {
		cw_host_set_u64("ram_mb", mem.ullTotalPhys >> 20);
	}
	wchar_t locale[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
		set_env_wide("locale", locale);
	}
}

static void
put_threads(uint32_t pid) {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}
	PROCESSENTRY32W entry = { .dwSize = sizeof(entry) };
	for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
		if (entry.th32ProcessID == pid) {
			cw_host_set_u64("threads", entry.cntThreads);
			break;
		}
	}
	CloseHandle(snap);
}

/**
 * The first hardware adapter DXGI enumerates and its user-mode driver
 * version. DXGI is loaded here and released again, since only the
 * watcher ever runs this.
 */
static void
put_gpu(void) {
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	if (dxgi == NULL) {
		return;
	}
	create_dxgi_factory1_t create = (create_dxgi_factory1_t)GetProcAddress(dxgi, "CreateDXGIFactory1");
	IDXGIFactory1* factory = NULL;
	if (create != NULL && SUCCEEDED(create(&iid_dxgi_factory1, (void**)&factory))) {
		IDXGIAdapter1* adapter = NULL;
		for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; ++i) {
			DXGI_ADAPTER_DESC1 desc;
			bool hardware = SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))
				&& (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
			if (hardware) {
				set_env_wide("gpu", desc.Description);
				LARGE_INTEGER umd;
				if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(adapter, &iid_dxgi_device, &umd))) {
					char buf[48];
					snprintf(
						buf, sizeof(buf), "%u.%u.%u.%u",
						(unsigned)(umd.HighPart >> 16), (unsigned)(umd.HighPart & 0xffff),
						(unsigned)(umd.LowPart >> 16), (unsigned)(umd.LowPart & 0xffff)
					);
					cw_set_env("gpu_driver", buf);
				}
			}
			IDXGIAdapter1_Release(adapter);
			if (hardware) {
				break;
			}
		}
		IDXGIFactory1_Release(factory);
	}
	FreeLibrary(dxgi);
}

void
cw_host_backend_report(uint32_t pid) {
	HANDLE game = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (game != NULL) {
		PROCESS_MEMORY_COUNTERS_EX counters = { .cb = sizeof(counters) };
		if (GetProcessMemoryInfo(game, (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
			cw_host_set_u64("rss_mb", counters.WorkingSetSize >> 20);
			cw_host_set_u64("commit_mb", counters.PrivateUsage >> 20);
		}
		BOOL present = FALSE;
		if (CheckRemoteDebuggerPresent(game, &present) && present) {
			cw_set_env("debugger", "1");
		}
		CloseHandle(game);
	}
	put_threads(pid);
	MEMORYSTATUSEX mem = { .dwLength = sizeof(mem) };
	if (GlobalMemoryStatusEx(&mem)) {
		cw_host_set_u64("free_mb", mem.ullAvailPhys >> 20);
	}
	if (!cw_env_has("gpu")) {
		put_gpu();
	}
}
