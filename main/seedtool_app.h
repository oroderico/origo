#ifndef SEEDTOOL_APP_H_
#define SEEDTOOL_APP_H_

/* Keyboard layouts: the key characters row by row, rows separated by '\n' and at
 * most ten keys per row. Exposed rather than kept private so the self-test can
 * prove they cover what they must — a rearranged layout that quietly dropped a
 * character would otherwise leave a passphrase impossible to retype. */
#define SEEDTOOL_WORD_LAYOUT "qwertyuiop\nasdfghjkl\nzxcvbnm\b"
/* Accept before Backspace, not after: nearest_enabled (seedtool_app.c) walks
 * the keyboard as a circular ring, so whichever of the two sits closer to the
 * digit row is what a disabled key's cursor snaps to first. Backspace is
 * always enabled and would otherwise almost always win that race - it
 * follows the last digit directly - stranding the cursor one step short of
 * the accept key a completed number should usually land on. */
#define SEEDTOOL_WORD_NUMBER_LAYOUT "1234567890\n\r\b"
#define SEEDTOOL_PASSPHRASE_PAGES 4
extern const char* const seedtool_passphrase_layouts[SEEDTOOL_PASSPHRASE_PAGES];

void seedtool_run(void);

#endif
