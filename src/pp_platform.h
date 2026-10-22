#ifndef PP_PLATFORM_H
#define PP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic milliseconds (never goes backwards, arbitrary origin, > 0). */
int64_t pp_now_ms(void);
/* Wall clock milliseconds since the Unix epoch (UTC). */
int64_t pp_wall_ms(void);
void pp_sleep_ms(int ms);
uint32_t pp_rand32(void);
void pp_rand_bytes(uint8_t *out, size_t n);

#endif
