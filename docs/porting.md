# Porting Checklist

Use this checklist when adding a new console, host transport, or network
adapter. Keep organization-only commits behavior-preserving; save protocol,
timing, and packet layout changes for separate commits.

## New Console

- Add `client/<console>/target.c` implementing `target_ops_t`.
- Add console-local video, timer, cache, reboot, exception, execute, RTC, and
  storage support as needed.
- Add linker scripts and memory constants for the console.
- Add build rules that select one target implementation and one client
  transport implementation.
- Keep shared client code calling `target_ops_t` instead of branching on the
  console name.
- Add a target callback only when the behavior is common enough for other
  consoles to understand.
- Keep console-specific packet byte order, DMA, cache, and timing behavior in
  the console tree until a diff proves a helper is safe to share.

## New Client Transport

- Add a client implementation of `client_transport_ops_t`.
- Add a matching host implementation of `transport_ops_t`.
- Add capability bits for optional behavior instead of assuming callbacks are
  supported by every transport.
- Keep command upload, download, execute, console, program exit, and syscall
  paths behind the transport interfaces.
- Reuse existing protocol commands when possible.
- Add protocol constants only when existing commands cannot express the new
  feature.

## New Network Adapter

- Decide whether the adapter is console-specific hardware or a reusable chip.
- Put reusable chip-level logic under `client/common/drivers/`.
- Put console bus bindings under `client/<console>/net/`.
- Keep adapter probing, priority, and selection in
  `client/<console>/net/adapter.c`.
- Keep FIFO pacing, retry timing, DMA, cache, and interrupt behavior
  console-local unless it has been proven identical across targets.
- Use `net_adapter_ops_t` so shared network code does not need to know which
  adapter is active.

## Required Smoke Tests

- Build the affected client firmware.
- Build the host tool.
- Upload and execute a small console output example.
- Run a syscall or file I/O example when the change touches console, syscall,
  or transport paths.
- Test reset, RTC sync, or speed changes when touching those optional host
  operations.
- Test DHCP and static IP when touching network adapter or DHCP code.
- Confirm GDB and CDFS behavior when touching console loops or syscall
  forwarding.

## Organization-Only Rules

- Do not rename protocol commands.
- Do not change packet layouts.
- Do not change retry timing, FIFO pacing, post-transfer delays, or recovery
  behavior.
- Do not merge endian-sensitive checksum or packet code unless the behavior is
  proven identical.
- Do not move files and change behavior in the same commit.
- Prefer small commits that document, wrap, or extract one stable boundary at a
  time.
- After the interfaces are clean, consider directory moves only when the move
  makes future diffs easier to review.
