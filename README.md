# ELKS Enhanced


ELKS Enhanced is a maintained ELKS fork for Intel IA16 systems: 8086, 8088,
80186, 80188, 80286, NEC V20/V30 and close compatibles. It targets real XT/AT
class hardware, SBCs and FPGA systems, and emulator setups such as QEMU.

This tree keeps the ELKS kernel, driver model, filesystems and userspace
environment, but replaces the historical userspace networking stack with Adam
Dunkels' uIP. The goal is to keep the ELKS hardware and kernel boundaries
stable while moving protocol handling onto a smaller, better understood** TCP/IP
engine that fits 16-bit systems well.

* Better understood at my day job by other technicians coming from embedded/contiki

## Platform

- 16-bit x86 and IA16-compatible systems
- ELKS kernels and userspace images
- Real ISA hardware and QEMU test targets

## Networking Direction

The major change in this fork is the networking stack replacement:

- the old `ktcp` userspace stack is removed
- `/bin/uip` is the active network daemon
- ELKS keeps its kernel-side socket layer and hardware drivers
- the userspace bridge is unified as `/dev/netdev`
- TCP and UDP both run through the uIP-backed bridge
- uIP DHCP client and resolver support are available through build options

The integration was done to keep changes minimal on both sides:

- ELKS keeps its NIC drivers, kernel socket code and application model
- uIP stays close to its original structure and naming where possible
- the bridge layer is where ELKS and uIP are adapted to each other

## Why Replace The Old Stack

- uIP is much smaller and simpler than the old ELKS-specific networking stack
- the code path is easier to reason about on 8086-class memory budgets
- upstream uIP already provides TCP, UDP, DHCP and resolver building blocks
- serial links and ethernet links can be smoke-tested in QEMU with one daemon
- less ELKS-only network protocol code has to be maintained going forward

In practice, this gives ELKS a cleaner split between hardware access,
kernel/userspace socket ABI, and the protocol implementation itself.

The business requirement behind this change was straightforward: the deployed
PC/104 systems needed a network stack that was easier to reason about, easier
to maintain, and better aligned with the services expected on live industrial
networks. DHCP was required so systems could be commissioned and moved between
sites without static per-unit address work. Name services were required so
applications and support tooling could resolve hosts and infrastructure by
name instead of relying on fixed numeric addressing.

That combination made the old ELKS userspace networking stack the wrong fit for
the long-term deployment model. uIP provided a smaller and more comprehensible
base, while still giving enough protocol support to add DHCP, resolver-backed
name lookup, SLIP, CSLIP, and Ethernet operation in a way that fits the memory
and operational constraints of these systems. In our deployment work, the uIP
integration also scales to roughly 20 concurrent users on an ELKS machine,
compared with about 5 to 8 concurrent users on the standard ELKS `ktcp` stack.

## Building

Base ELKS build instructions remain in [BUILD.md](BUILD.md).

Typical workflow:

```sh
. ./env.sh
make menuconfig
make -j1 all
```

## Current Networking Scope

The uIP-backed networking work in this fork includes:

- ethernet via existing ELKS NIC drivers
- SLIP and CSLIP serial networking
- unified TCP and UDP userspace bridge
- DHCP client support
- resolver integration for ELKS networking tools
- QEMU smoke coverage for ethernet, SLIP and CSLIP paths

## Upstream ELKS

This fork starts from upstream ELKS and remains compatible with the general
ELKS build and image model. For the broader project history and upstream
documentation, see the ELKS documentation tree in `Documentation/`.
