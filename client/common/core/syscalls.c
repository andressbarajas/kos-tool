/* client/common/core/syscalls.c */
/*
 * Shared client-side syscall helpers.
 *
 * Transport-specific syscall implementations (read, write, open, etc.)
 * and gethostinfo live in the transport layer files:
 *   - client/common/network/network_syscalls.c for network (UDP) syscalls
 *   - client/common/serial/serial_transport.c for serial (SCIF) syscalls
 *
 * gethostinfo is transport-specific because it references network globals
 * from the command and network stack layers. Putting it here would cause the
 * linker to pull commands.o into the serial build, cascading into the entire
 * network stack.
 *
 * This file is currently empty. It exists as a placeholder for any
 * future transport-independent syscall helpers.
 */
