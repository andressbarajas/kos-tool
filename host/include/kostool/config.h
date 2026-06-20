/* host/include/kostool/config.h */
#ifndef KOSTOOL_CONFIG_H
#define KOSTOOL_CONFIG_H

struct kostool_context;

/* Load kos-tool.cfg.  On Linux/BSD it is read from $XDG_CONFIG_HOME (or
 * ~/.config) first, then from the directory holding the kos-tool binary; on
 * macOS/Windows only the binary's directory is used.  Creates a default
 * config file if none exists (under ~/.config on Linux/BSD). */
void config_load(struct kostool_context *ctx);

#endif /* KOSTOOL_CONFIG_H */
