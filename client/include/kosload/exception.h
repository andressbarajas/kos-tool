/* client/include/kosload/exception.h */
#ifndef KOSLOAD_EXCEPTION_H
#define KOSLOAD_EXCEPTION_H

#include <stdint.h>

/* Exception handler interface */

/* Install exception handlers */
void exception_init(void);

/* Convert an exception code to a human-readable string */
const char *exception_code_to_string(uint32_t code);

#endif /* KOSLOAD_EXCEPTION_H */
