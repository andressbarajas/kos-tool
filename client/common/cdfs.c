/* client/common/cdfs.c */
/*
 * Shared CDFS redirection management.
 *
 * Provides helpers for saving/restoring BIOS CDFS vectors and
 * enabling/disabling the CDFS redirect hook.
 *
 * The actual sector read implementation (cdfs_read_sectors) is
 * transport-specific and lives in the transport layer or
 * platform-specific cdfs_syscalls.c.
 */

#include <stdbool.h>
#include <kosload/target.h>

extern const target_ops_t *common_get_target(void);

static bool cdfs_saved = false;

void cdfs_init(void)
{
    const target_ops_t *target = common_get_target();
    if (!cdfs_saved) {
        target->cdfs_redir_save();
        cdfs_saved = true;
    }
    target->cdfs_redir_disable();
}

void cdfs_enable(void)
{
    const target_ops_t *target = common_get_target();
    target->cdfs_redir_enable();
}

void cdfs_disable(void)
{
    const target_ops_t *target = common_get_target();
    target->cdfs_redir_disable();
}
