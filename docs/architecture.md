# Architecture

kos-tool is organized around three boundaries:

- `target_ops_t`: console-specific client hardware behavior
- `client_transport_ops_t`: client-to-host communication behavior
- `transport_ops_t`: host-side communication behavior

These interfaces are the preferred extension points for new consoles and new
peripherals. Shared code should use them instead of branching on Dreamcast,
GameCube, serial, network, or a future transport name.

For concrete porting and smoke-test steps, see `docs/porting.md`.

## Client Layout

Client firmware is assembled from one console target and one client transport.

The console target owns hardware details:

- video setup and drawing
- timers and screensaver timing
- cache control
- program execution
- reboot
- RTC access
- CDFS redirection hooks
- console-specific memory detection

The target contract lives in:

- `client/include/kosload/target.h`

Current target implementations live in:

- `client/dreamcast/target.c`
- `client/gamecube/target.c`

The client transport owns communication with the host:

- loader command loop
- upload/download protocol handling
- syscall packet send
- program exit notification
- transport pause/resume around loaded program execution

The client transport contract lives in:

- `client/include/kosload/transport.h`

Current shared client transport implementations live in:

- `client/serial/serial_transport.c`
- `client/network/network_transport.c`

Network transport hardware is split one layer lower. Shared network code uses
the adapter contract in:

- `client/include/kosload/net_adapter.h`

Shared network stack declarations live in:

- `client/include/kosload/net_stack.h`

Shared packet helper declarations live in:

- `client/include/kosload/packet.h`

Shared DHCP protocol declarations live in:

- `client/include/kosload/dhcp.h`

Console-local `net.h` files remain thin wrappers so target drivers can keep
their existing local include paths while shared code includes the common
contract directly.

Console-local `packet.h` files intentionally remain wrappers that select the
target byte-order macros before including the shared packet helper
declarations. Packet helper implementations still live under each console tree
because checksum odd-byte handling and copy behavior differ by CPU.

Console-local `dhcp.h` files remain wrappers around the common DHCP packet
types, constants, state declarations, and public prototypes. DHCP behavior and
lease handling still live in each console's `dhcp.c` implementation.

Current console-specific adapter selection lives in:

- `client/dreamcast/net/adapter.c`
- `client/gamecube/net/adapter.c`

Individual Ethernet drivers stay under the console tree when they are hardware
specific, or under `client/common/drivers/` when the chip driver is shared by
multiple consoles.

Network drivers are layered as follows:

- `client/<console>/net/adapter.c` chooses the active adapter for that console
  and exposes it through `net_adapter_ops_t`
- console-owned integrated adapters and timing-sensitive hardware drivers stay
  under `client/<console>/net/`
- shared chip drivers, such as `client/common/drivers/w5500.c`, keep reusable
  register and socket logic outside any one console tree
- console bus bindings, such as `client/dreamcast/net/w5500_spi_dc.c` and
  `client/gamecube/net/w5500_spi_gc.c`, connect a shared chip driver to the
  target's bus, GPIO, EXI, SPI, or similar hardware path

Future adapters should follow the same split. If a chip can be reused across
multiple consoles, keep its chip-level driver in `client/common/drivers/` and
put only the console bus binding under `client/<console>/net/`. Adapter
probing, priority, and selection should remain console-specific.

The shared entrypoint is:

- `client/common/main.c`

Transport-specific entrypoints choose one target and one transport, then call
`common_main()`:

- `client/serial/entry.c`
- `client/network/entry.c`

## Host Layout

The host tool also uses a transport interface so high-level commands do not
need serial or network special cases.

The host transport contract lives in:

- `host/include/kostool/transport.h`

Current host transport implementations live in:

- `host/src/transport/serial.c`
- `host/src/transport/network.c`

High-level host features should call through `transport_ops_t` for data
transfer, command send/receive, execute, reset, RTC sync, GDB, console, CDFS,
and optional maintenance features.

## Protocol Layout

Shared wire constants, packet structures, ports, command IDs, adapter IDs, load
addresses, and low-level network packet structures live in:

- `include/kosload/protocol.h`

Keep protocol changes centralized there so host and client code cannot drift.
When adding optional behavior, prefer a capability bit or version-negotiated
command over target-specific assumptions in shared code.

## Adding A Console

A new console should normally add:

- a `client/<console>/target.c` implementing `target_ops_t`
- console-local video, timer, cache, reboot, exception, execute, and storage
  support
- build rules that select the target implementation
- linker scripts and memory constants for that console
- minimal shared-code changes, ideally limited to build wiring and protocol
  constants if needed

Shared client code should continue to call `target_ops_t`. If adding a console
requires a new callback, add it to the target interface only when the behavior
is genuinely common enough for other targets to understand.

## Adding A Transport Or Peripheral

A new host-visible transport, such as USB, should normally add both sides:

- a client implementation of `client_transport_ops_t`
- a host implementation of `transport_ops_t`
- capability bits for optional behavior
- protocol constants only when existing serial/network commands cannot express
  the feature
- tests or examples proving upload, download, execute, console output, program
  exit, and syscalls

If the peripheral is only hardware under an existing transport, keep it lower
in the tree. For example, a USB Ethernet adapter would usually be a network
adapter backend instead of a new top-level transport.

## Refactoring Rules

Reorganization should be incremental and behavior-preserving:

- prefer documenting an existing boundary before moving code
- extract duplicated helpers only after the duplicate behavior has stabilized
- keep console hardware code behind `target_ops_t`
- keep client wire behavior behind `client_transport_ops_t`
- keep host wire behavior behind `transport_ops_t`
- keep optional features capability-driven
- keep protocol definitions in `include/kosload/protocol.h`
- avoid renaming commands, changing packet layouts, or changing retry timing as
  part of organization-only patches

This lets new console or peripheral work build on the existing structure
without making Dreamcast, GameCube, serial, or network behavior more fragile.
