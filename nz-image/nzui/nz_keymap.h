/* nz_keymap.h - Cardputer Zero "Sym" layer.
 *
 * The vendor keyboard driver (tca8418_keypad_m5stack) has three key tables: base, Fn and
 * Sym.  A symbol such as '!' cannot be expressed as one plain key code, so in the Sym
 * table every symbol gets a private key code (borrowed from keypad / Japanese keys) and
 * userspace is expected to translate.  Pairs taken from the vendor's
 * tca8418_keypad_m5stack_keymap.map and the keymap1 table of cardputerzero-v5-overlay.dts
 * (m5stack-linux-dtoverlays 4f7aeee), and checked against the blue legends printed on the
 * unit: every pair below matches its legend.
 *
 * Several of these codes are ordinary punctuation on a PC keyboard (26 '[', 39 ';', ...).
 * A table loaded into the text console is global, so a USB keyboard attached to the
 * device gets the same substitutions there - as on the stock system.
 */
#ifndef NZ_KEYMAP_H
#define NZ_KEYMAP_H
static const struct { unsigned short code; char ch; } nz_sym_keys[] = {
	/* number row: 1 2 3 4 5 6 7 8 9 0 */
	{ 26, '!' }, { 27, '@' }, { 39, '#' }, { 40, '$' }, { 41, '%' },
	{ 43, '^' }, { 51, '&' }, { 52, '*' }, { 53, '(' }, { 94, ')' },
	/* Q W E R T Y U I O P */
	{ 55, '~' }, { 69, '`' }, { 70, '_' }, { 71, '-' }, { 72, '+' },
	{ 73, '=' }, { 74, '[' }, { 75, ']' }, { 76, '{' }, { 77, '}' },
	/* A S D . G H J K L   (F has no symbol) */
	{ 79, ';' }, { 80, ':' }, { 81, '\'' }, { 82, '"' }, { 83, '<' },
	{ 85, '>' }, { 86, '\\' }, { 89, '|' },
	/* . . . V B N M       (Z X C have no symbol) */
	{ 90, ',' }, { 91, '.' }, { 92, '/' }, { 93, '?' },
};
#define NZ_SYM_KEYS (sizeof(nz_sym_keys) / sizeof(nz_sym_keys[0]))

/* Character for a key code from the internal keyboard, 0 if the key types nothing.
 * `shift` is the state of KEY_LEFTSHIFT; it only affects letters. */
static inline int nz_key_char(int code, int shift)
{
	static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
	int c = 0;
	if (code >= 2 && code <= 10) return '1' + code - 2;       /* KEY_1 .. KEY_9 */
	if (code == 11) return '0';
	if (code == 57) return ' ';
	if (code >= 16 && code <= 25) c = row1[code - 16];
	else if (code >= 30 && code <= 38) c = row2[code - 30];
	else if (code >= 44 && code <= 50) c = row3[code - 44];
	if (c) return shift ? c - 32 : c;
	for (unsigned i = 0; i < NZ_SYM_KEYS; i++) if (nz_sym_keys[i].code == code) return nz_sym_keys[i].ch;
	return 0;
}
#endif
