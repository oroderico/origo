/* libwally-core's build configuration for both Origo builds, in place of the
 * header its autoconf build would generate. The amalgamated sources read only
 * the one macro below. */
#ifndef LIBWALLYCORE_CONFIG_H
#define LIBWALLYCORE_CONFIG_H

/* The ESP32's Xtensa core faults on unaligned 32-bit loads, so ccan's hashes
 * must copy unaligned input rather than cast it. */
#define HAVE_UNALIGNED_ACCESS 0

#include "ccan_config.h"

#endif
