/**
 * @file backend.h
 * What every cw_host backend implements and what they share.
 */
#ifndef CW_HOST_BACKEND_H
#define CW_HOST_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/**
 * Game side: record the facts known before any window or GPU exists.
 */
void
cw_host_backend_init(void);

/**
 * Watcher side: record the state of the game process and of the
 * machine at report time. The game is frozen or already gone.
 *
 * @param pid  The game process; 0 where the game has none.
 */
void
cw_host_backend_report(uint32_t pid);

/** cw_set_env() with a number. */
void
cw_host_set_u64(const char* key, uint64_t value);

/**
 * Space-separated ISA extensions the CPU and OS support, such as
 * `avx2`. Empty on architectures without a feature query.
 */
void
cw_host_cpu_features(char* out, size_t cap);

#endif /* CW_HOST_BACKEND_H */
