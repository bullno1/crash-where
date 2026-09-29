/**
 * @file cw_host.h
 * Collector of standard host facts, built as its own library.
 */
#ifndef CW_HOST_H
#define CW_HOST_H

#include "cw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Collector of standard host facts.
 *
 * Plug it into both cw_config_t::collect_at_init and
 * cw_config_t::collect_at_report. At init it records the operating
 * system, sandbox and compatibility layer, device, CPU, memory, and
 * locale. At report time it records the memory use and thread count of
 * the game, whether a debugger holds it, free memory, and the GPU when
 * no `gpu` key was set. Every value is a machine fact; none names the
 * player. A custom collector may call it and then add its own keys.
 */
extern const cw_collector_t cw_collector_host;

#ifdef __cplusplus
}
#endif

#endif /* CW_HOST_H */
