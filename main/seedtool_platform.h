#ifndef SEEDTOOL_PLATFORM_H_
#define SEEDTOOL_PLATFORM_H_

#include <stddef.h>
#include <stdint.h>

typedef enum { KEY_LEFT, KEY_RIGHT, KEY_TIMEOUT } seedtool_key_t;

void seedtool_platform_init(void);
uint64_t seedtool_platform_milliseconds(void);
seedtool_key_t seedtool_platform_wait_key(uint32_t timeout_ms);
void seedtool_platform_random(uint8_t* output, size_t output_len);
_Noreturn void seedtool_platform_restart(void);

#endif
