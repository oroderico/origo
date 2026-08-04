#ifndef SEEDTOOL_APP_H_
#define SEEDTOOL_APP_H_

/* Keyboard layouts: the key characters row by row, rows separated by '\n' and at
 * most ten keys per row. Exposed rather than kept private so the self-test can
 * prove they cover what they must — a rearranged layout that quietly dropped a
 * character would otherwise leave a passphrase impossible to retype. */
#define SEEDTOOL_WORD_LAYOUT "qwertyuiop\nasdfghjkl\nzxcvbnm\b"
#define SEEDTOOL_WORD_NUMBER_LAYOUT "1234567890\n\b\r"
#define SEEDTOOL_PASSPHRASE_PAGES 4
extern const char* const seedtool_passphrase_layouts[SEEDTOOL_PASSPHRASE_PAGES];

void seedtool_run(void);

#endif
