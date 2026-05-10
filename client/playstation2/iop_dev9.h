/* client/playstation2/iop_dev9.h
 *
 * EE-side front end for IOP DEV9 initialization and diagnostics.
 */

#ifndef IOP_DEV9_H
#define IOP_DEV9_H

typedef struct {
    int result;
    unsigned int rev;
    unsigned int power;
    unsigned int map;
    unsigned int presence;
} iop_dev9_diag_t;

/*
 * Initialize DEV9 by running a helper on the IOP.
 *
 * Returns 0 on success, negative on failure.  Additional diagnostics are
 * available via iop_dev9_last_diag().
 */
int iop_dev9_init(void);
const iop_dev9_diag_t *iop_dev9_last_diag(void);
const char *iop_dev9_phase1_status(void);

#endif /* IOP_DEV9_H */
