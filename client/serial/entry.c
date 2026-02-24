/* client/serial/entry.c */
/*
 * Serial client entry point for kosload.
 * Calls common_main with the platform's target_ops and the serial transport.
 */

#include <kosload/target.h>
#include <kosload/transport.h>

extern void common_main(const target_ops_t *tgt, const client_transport_ops_t *xport);

int main(void)
{
    common_main(target_get_ops(), &client_serial_transport_ops);
    return 0;
}
