#ifndef SEEDTOOL_PLATFORM_H_
#define SEEDTOOL_PLATFORM_H_

#include <stddef.h>
#include <stdint.h>

/* The board has two buttons and no select button: the left button moves to the
 * previous item, the right button to the next one, and both pressed together
 * stand in for the select button. */
/* KEY_REDRAW is never produced by the platform. The session-timeout wrapper
 * returns it after its warning screen has painted over whatever the caller was
 * showing, so that the caller repaints instead of acting on a stale screen. */
typedef enum { KEY_PREV, KEY_NEXT, KEY_SELECT, KEY_TIMEOUT, KEY_REDRAW } seedtool_key_t;

void seedtool_platform_init(void);
uint64_t seedtool_platform_milliseconds(void);
seedtool_key_t seedtool_platform_wait_key(uint32_t timeout_ms);
/* Abandons whatever the user is in the middle of doing, so that a screen just
 * painted only ever sees a gesture begun after it appeared. A press in progress
 * is dropped along with its release, since a release is what the device reports
 * as a press: without this, a button already going down when a screen is
 * replaced acts on the screen that took its place. */
void seedtool_platform_flush_keys(void);
void seedtool_platform_random(uint8_t* output, size_t output_len);
_Noreturn void seedtool_platform_restart(void);

#endif
