# PMD Net GEM - DPDK Virtual Device Driver

**Contributor:** Pham Thi Phuong (Development Team Member)

A DPDK Poll Mode Driver (PMD) for the **Gigabit Ethernet MAC (GEM)** on **Xilinx Zynq UltraScale+ (ZCU102/ZCU111 board)** platforms. The driver operates in user space through Linux UIO (`uio_pdrv_genirq`) and supports packet transmission and reception using polling.

## Main Components

| File | Description |
|---|---|
| `gem_ethdev.c/.h` | Virtual device registration, device initialization, PHY configuration, and Ethernet device lifecycle management. |
| `gem_rxtx.c/.h` | RX/TX queue setup, descriptor ring and mbuf management, and DMA packet processing. |
| `gem_hw.c/.h` | MMIO mapping via UIO, register access, hardware reset, and cache synchronization. |
| `gem_log.h` | Logging and debugging support. |
| `meson.build` | DPDK build configuration. |
| `version.map` | Symbol export visibility control. |

## Features

- User-space GEM access through `/dev/uioX`.
- PHY communication via MDIO with auto-negotiation support.
- Packet transmission and reception using DPDK mbufs and DMA descriptor rings.
- Support for `configPhy`, `loopback`.

## Requirements

- Xilinx ZCU102 or ZCU111 board.
- DPDK build environment.