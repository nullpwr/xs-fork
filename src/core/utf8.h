#ifndef UTF8_H
#define UTF8_H

/* Decode one codepoint from UTF-8 bytes. Returns bytes consumed (1-4) or -1 on error. */
int utf8_decode(const char *s, int len, int *codepoint);

/* Encode one codepoint to UTF-8 bytes. Returns bytes written (1-4). */
int utf8_encode(int codepoint, char *buf);

/* Count codepoints in a UTF-8 string */
int utf8_strlen(const char *s, int byte_len);

/* Get byte offset of the nth codepoint */
int utf8_offset(const char *s, int byte_len, int n);

/* Validate that a string is valid UTF-8. Returns 1 if valid, 0 if not. */
int utf8_validate(const char *s, int byte_len);

/* Iterate codepoints: get next codepoint and advance pointer. Returns codepoint or -1. */
int utf8_next(const char **p, const char *end);

/* Unicode-aware case conversion */
int utf8_toupper(int codepoint);
int utf8_tolower(int codepoint);

/* Case folding for case-insensitive comparison */
int utf8_casefold(int codepoint);

/* Unicode category checks */
int utf8_is_letter(int codepoint);
int utf8_is_digit(int codepoint);
int utf8_is_whitespace(int codepoint);
int utf8_is_combining(int codepoint);

/* Convert entire string to upper/lower/casefold (returns new allocated string, caller must free) */
char *utf8_str_upper(const char *s, int byte_len, int *out_len);
char *utf8_str_lower(const char *s, int byte_len, int *out_len);
char *utf8_str_casefold(const char *s, int byte_len, int *out_len);

/* Returns 1 if all bytes are < 128 */
int utf8_is_ascii(const char *s, int byte_len);

/* Grapheme cluster segmentation */
typedef struct {
    const char *data;
    int byte_len;
    int byte_pos;
} GraphemeIter;

void grapheme_iter_init(GraphemeIter *it, const char *s, int byte_len);

/* Advance to next grapheme cluster. Sets *start and *len to the cluster bytes.
   Returns 1 if a cluster was found, 0 at end. */
int grapheme_iter_next(GraphemeIter *it, const char **start, int *len);

/* Count grapheme clusters in a string */
int utf8_grapheme_count(const char *s, int byte_len);

/* NFC normalization (returns new allocated string, caller must free) */
char *utf8_normalize_nfc(const char *s, int byte_len, int *out_len);

#endif /* UTF8_H */
