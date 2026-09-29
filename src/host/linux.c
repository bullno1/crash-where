/**
 * @file linux.c
 * Linux facts from `/proc`, `/sys`, and the C library.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "backend.h"
#include "cw.h"
#include "internal.h"

/**
 * First line of a file without its newline.
 *
 * @return `false` when the file is missing or the line is empty.
 */
static bool
read_line(const char* path, char* out, size_t cap) {
	FILE* f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	bool ok = fgets(out, (int)cap, f) != NULL;
	fclose(f);
	if (!ok) {
		return false;
	}
	out[strcspn(out, "\n")] = '\0';
	return out[0] != '\0';
}

/**
 * Value of `key` in a file of `key<sep> value` lines, such as
 * `/proc/meminfo` (`:`) or `/etc/os-release` (`=`). Quotes around an
 * os-release value are dropped.
 *
 * @return `false` when the key is absent or its value is empty.
 */
static bool
read_field(const char* path, const char* key, char sep, char* out, size_t cap) {
	FILE* f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	size_t key_len = strlen(key);
	bool found = false;
	char line[512];
	while (!found && fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, key_len) != 0) {
			continue;
		}
		const char* p = line + key_len;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (*p != sep) {
			continue;
		}
		++p;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		size_t len = strcspn(p, "\n");
		if (len >= 2 && p[0] == '"' && p[len - 1] == '"') {
			++p;
			len -= 2;
		}
		if (len >= cap) {
			len = cap - 1;
		}
		memcpy(out, p, len);
		out[len] = '\0';
		found = len > 0;
	}
	fclose(f);
	return found;
}

/** Numeric prefix of a field such as `12345 kB`. */
static bool
read_number(const char* path, const char* key, uint64_t* out) {
	char buf[64];
	if (!read_field(path, key, ':', buf, sizeof(buf))) {
		return false;
	}
	*out = strtoull(buf, NULL, 10);
	return true;
}

static const char*
sandbox_name(void) {
	if (getenv("PRESSURE_VESSEL_RUNTIME") != NULL || access("/run/pressure-vessel", F_OK) == 0) {
		return "pressure-vessel";
	}
	if (access("/.flatpak-info", F_OK) == 0) {
		return "flatpak";
	}
	if (getenv("SNAP") != NULL) {
		return "snap";
	}
	const char* runtime = getenv("STEAM_RUNTIME");
	if (runtime != NULL && runtime[0] != '\0' && strcmp(runtime, "0") != 0) {
		return "steam-runtime";
	}
	return NULL;
}

/**
 * The POSIX locale as a language tag: `en_US.UTF-8` becomes `en-US`.
 */
static void
put_locale(void) {
	static const char* const names[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
	const char* v = NULL;
	for (size_t i = 0; v == NULL && i < sizeof(names) / sizeof(names[0]); ++i) {
		const char* e = getenv(names[i]);
		if (e != NULL && e[0] != '\0') {
			v = e;
		}
	}
	if (v == NULL) {
		return;
	}
	char buf[32];
	size_t n = strcspn(v, ".@");
	if (n >= sizeof(buf)) {
		n = sizeof(buf) - 1;
	}
	for (size_t i = 0; i < n; ++i) {
		buf[i] = v[i] == '_' ? '-' : v[i];
	}
	buf[n] = '\0';
	cw_set_env("locale", buf);
}

static void
put_device(void) {
	char vendor[64];
	char product[64];
	bool have_vendor = read_line("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor));
	bool have_product = read_line("/sys/class/dmi/id/product_name", product, sizeof(product));
	if (!have_product) {
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
	cw_set_env("os", "linux");
	struct utsname u;
	if (uname(&u) == 0) {
		cw_set_env("os_version", u.release);
	}
	char buf[128];
	if (read_field("/etc/os-release", "ID", '=', buf, sizeof(buf))) {
		cw_set_env("os_distro", buf);
	}
#if defined(__x86_64__)
	cw_set_env("arch", "x86_64");
#elif defined(__aarch64__)
	cw_set_env("arch", "aarch64");
#elif defined(__i386__)
	cw_set_env("arch", "x86");
#endif
	const char* sandbox = sandbox_name();
	if (sandbox != NULL) {
		cw_set_env("sandbox", sandbox);
	}
	put_device();
	if (read_field("/proc/cpuinfo", "model name", ':', buf, sizeof(buf))) {
		cw_set_env("cpu", buf);
	}
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (cores > 0) {
		cw_host_set_u64("cpu_cores", (uint64_t)cores);
	}
	cw_host_cpu_features(buf, sizeof(buf));
	if (buf[0] != '\0') {
		cw_set_env("cpu_features", buf);
	}
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		cw_host_set_u64("ram_mb", ((uint64_t)si.totalram * si.mem_unit) >> 20);
	}
	put_locale();
}

/**
 * How likely DRM card `i` is the one a game renders on: the firmware's
 * boot display first, then any VGA-class device, then other display
 * controllers such as a headless integrated GPU. Negative when the card
 * has no PCI identity.
 */
static int
gpu_rank(int i) {
	char path[96];
	char buf[16];
	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/vendor", i);
	if (!read_line(path, buf, sizeof(buf))) {
		return -1;
	}
	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/boot_vga", i);
	if (read_line(path, buf, sizeof(buf)) && buf[0] == '1') {
		return 2;
	}
	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/class", i);
	return read_line(path, buf, sizeof(buf)) && strncmp(buf, "0x0300", 6) == 0 ? 1 : 0;
}

/** The most likely render device as `pci:<vendor>:<device>` and its kernel driver. */
static void
put_gpu(void) {
	int best = -1;
	int best_rank = -1;
	for (int i = 0; i < 16; ++i) {
		int rank = gpu_rank(i);
		if (rank > best_rank) {
			best = i;
			best_rank = rank;
		}
	}
	if (best < 0) {
		return;
	}

	char path[96];
	char vendor[16];
	char device[16];
	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/vendor", best);
	if (!read_line(path, vendor, sizeof(vendor))) {
		return;
	}
	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/device", best);
	if (!read_line(path, device, sizeof(device))) {
		return;
	}
	const char* v = strncmp(vendor, "0x", 2) == 0 ? vendor + 2 : vendor;
	const char* d = strncmp(device, "0x", 2) == 0 ? device + 2 : device;
	char gpu[40];
	snprintf(gpu, sizeof(gpu), "pci:%s:%s", v, d);
	cw_set_env("gpu", gpu);

	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/driver", best);
	char link[128];
	ssize_t n = readlink(path, link, sizeof(link) - 1);
	if (n > 0) {
		link[n] = '\0';
		const char* slash = strrchr(link, '/');
		const char* name = slash != NULL ? slash + 1 : link;
		char version_path[160];
		snprintf(version_path, sizeof(version_path), "/sys/module/%s/version", name);
		char version[64];
		char driver[224];
		if (read_line(version_path, version, sizeof(version))) {
			snprintf(driver, sizeof(driver), "%s %s", name, version);
		} else {
			snprintf(driver, sizeof(driver), "%s", name);
		}
		cw_set_env("gpu_driver", driver);
	}
}

/**
 * Whether a `/proc/<pid>/status` line is `key` and, if so, its number.
 */
static bool
is_field(const char* line, const char* key, uint64_t* out) {
	size_t key_len = strlen(key);
	if (strncmp(line, key, key_len) != 0) {
		return false;
	}
	*out = strtoull(line + key_len, NULL, 10);
	return true;
}

void
cw_host_backend_report(uint32_t pid) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%u/status", (unsigned)pid);
	FILE* f = fopen(path, "r");
	if (f != NULL) {
		char line[256];
		uint64_t value;
		while (fgets(line, sizeof(line), f) != NULL) {
			if (is_field(line, "VmRSS:", &value)) {
				cw_host_set_u64("rss_mb", value >> 10);
			} else if (is_field(line, "VmData:", &value)) {
				cw_host_set_u64("commit_mb", value >> 10);
			} else if (is_field(line, "Threads:", &value)) {
				cw_host_set_u64("threads", value);
			} else if (is_field(line, "TracerPid:", &value)) {
				/* The watcher itself traces the game while it writes a crash report. */
				if (value != 0 && value != (uint64_t)getpid()) {
					cw_set_env("debugger", "1");
				}
			}
		}
		fclose(f);
	}

	uint64_t n;
	if (read_number("/proc/meminfo", "MemAvailable", &n)) {
		cw_host_set_u64("free_mb", n >> 10);
	}

	if (!cw_env_has("gpu")) {
		put_gpu();
	}
}
