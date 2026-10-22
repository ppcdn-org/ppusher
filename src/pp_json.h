#ifndef PP_JSON_H
#define PP_JSON_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal JSON reader: looks a key up in the top-level object of `json` and
 * returns the value. No DOM, no allocation; input is untrusted and never
 * recursed into deeper than a depth counter.
 */

/* Locate the raw value span of `key`. Returns 1 if found. */
int pp_json_find(const char *json, size_t n, const char *key, const char **v, size_t *vn);

/* String value (unescaped, NUL-terminated, truncated to outsz-1). 1 on success, 0 if absent / not a string. */
int pp_json_str(const char *json, size_t n, const char *key, char *out, size_t outsz);
/* Integer value (a JSON number; fractions are truncated). */
int pp_json_int(const char *json, size_t n, const char *key, int64_t *out);
int pp_json_uint(const char *json, size_t n, const char *key, uint64_t *out);
int pp_json_bool(const char *json, size_t n, const char *key, int *out);
/* Nested object span. */
int pp_json_obj(const char *json, size_t n, const char *key, const char **sub, size_t *subn);

/* Escape `s` as the body of a JSON string (no surrounding quotes). Returns length, or -1 if outsz is too small. */
int pp_json_escape(const char *s, char *out, size_t outsz);

#endif
