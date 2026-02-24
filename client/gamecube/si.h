/* client/gamecube/si.h */
#ifndef KOSLOAD_GC_SI_H
#define KOSLOAD_GC_SI_H

/* Poll controller on given port (0-3).
 * Returns non-zero if any button is pressed. */
int si_poll_controller(int channel);

#endif /* KOSLOAD_GC_SI_H */
