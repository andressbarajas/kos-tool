/* host/include/kostool/config.h */
#ifndef KOSTOOL_CONFIG_H
#define KOSTOOL_CONFIG_H

struct kostool_context;

/* Load kos-tool.cfg from the same directory as the kos-tool binary.
 * Creates a default config file if none exists. */
void config_load(struct kostool_context *ctx);

#endif /* KOSTOOL_CONFIG_H */
