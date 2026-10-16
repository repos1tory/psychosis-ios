#ifndef MDC_KEYMAP_H
#define MDC_KEYMAP_H

/* Map a printable ASCII character to a HID keyboard usage (page 0x07).
 * Returns 0 and fills *usage / *shift (1 when the left shift modifier must be
 * held) for typable characters; -1 for anything unmappable. */
int mdc_key_for_char(char c, unsigned short *usage, int *shift);

/* Convert UTF-8 to UTF-16LE. Invalid sequences become U+FFFD; surrogate
 * pairs are produced in little-endian order. Returns bytes written. */
unsigned long mdc_utf8_to_utf16le(const char *input, unsigned char *output, unsigned long output_cap);

#endif
