#ifndef _GEM_HW_H_
#define _GEM_HW_H_

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <rte_common.h>
#include <rte_io.h>

#include <stddef.h>
#include <stdint.h>

#include "gem_log.h"           

#define gem_io_wmb()   rte_wmb()   /* before DMA start */
#define gem_io_rmb()   rte_rmb()   /* after status read */
#define gem_io_mb()    rte_mb()    /* full barrier */
#define gem_io_fence() __asm__ __volatile__("" ::: "memory")

#define CACHE_LINE_SIZE 64 /* ZynqMP Cortex-A53 has cache line là 64 bytes */

/* Push data CPU Cache to RAM (Used when read DMA) */
static inline void gem_cache_clean(volatile void *addr, size_t len) {
    // PMD_LOG(INFO, "gem_cache_clean");
    uintptr_t start = (uintptr_t)addr & ~(CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; start < end; start += CACHE_LINE_SIZE) {
        asm volatile("dc cvac, %0" : : "r" (start) : "memory");
    }
    asm volatile("dsb sy" ::: "memory"); /* ensure clear finished */
}

/* Cancel CPU Cache, data read from RAM (used after DMA written) */
static inline void gem_cache_invalidate(volatile void *addr, size_t len) {
    // PMD_LOG(DEBUG, "gem_cache_invalidate");
    uintptr_t start = (uintptr_t)addr & ~(CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; start < end; start += CACHE_LINE_SIZE) {
        /* Dùng civac (Clean and Invalidate) an toàn hơn ivac trong user-space Linux */
        asm volatile("dc civac, %0" : : "r" (start) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}


#define UIO_PATH "/sys/class/uio"
int find_uio_device(const char *target);
size_t gem_get_map_size(int uio_idx);
int gem_open_uio(int uio_idx);
void * gem_map_mmio(int fd, size_t map_size);

#define GEM_TX_RING_SIZE 512
#define GEM_TX_BUF_SIZE  2048

#define GEM_NWCTRL        0x0000
#define GEM_NWCTRL_LOCAL_LOOPBACK  (1 << 1) /* Bit 1: Bật Loopback nội bộ */
#define GEM_NWCTRL_RX_EN           (1 << 2)
#define GEM_NWCTRL_TX_EN           (1 << 3)
#define GEM_NWCTRL_CLEAR_STAT_REGS (1 << 5)  /* Xóa toàn bộ statistics */
#define GEM_NWCTRL_TX_START        (1 << 9)

#define GEM_NWCFG         0x0004
#define GEM_NWCFG_FULL_DUPLEX         (1 << 1)
#define GEM_NWCFG_COPY_ALL_FRAME      (1 << 4)
#define GEM_NWCFG_GIGABIT_MODE_ENABLE (1 << 10)
#define GEM_NWCFG_FCS_REMOVE          (1 << 17)
#define GEM_NWCFG_JUMBO_FRAME_ENABLE (1 << 3)
#define GEM_NWCFG_1536_RXEN          (1 << 8)
#define GEM_NWCFG_PAUSE_EN           (1 << 13) 
#define GEM_NWCFG_NO_BROADCAST        (1 << 5)   /* =1 disable broadcast */
#define GEM_NWCFG_MULTICAST_HASH_EN   (1 << 6)
#define GEM_NWCFG_DISABLE_COPY_OF_PAUSE_FRAME (1 << 23)
#define GEM_NWCFG_RECV_CHECKSUM_OFFLOAD_ENABLE (1 << 24)
#define GEM_NWCFG_DBUS_WIDTH_64    (1 << 21) /* Data Bus Width 64-bit */

#define GEM_NWSR          0x0008
#define GEM_NWSR_TX_ACT   (1 << 1)
#define GEM_NWSR_RX_ACT   (1 << 2)

#define GEM_DMACFG        0x0010
#define GEM_DMACFG_ADDR_64BIT         (1 << 30)     /* flag turn on bus AXI 64-bit */
#define GEM_TX_BD_EXTENDED_MODE_EN    (1 << 29)
#define GEM_RX_BD_EXTENDED_MODE_EN    (1 << 28)
#define GEM_DMACFG_AMBA_BURST_LEN_16  (0x10 << 0)   /* burst length INCR16 */
#define GEM_DMACFG_RX_PBUF_SIZE_32KB  (3 << 8)      /* RX Packet Buffer = 32KB */
#define GEM_DMACFG_TX_PBUF_SIZE_32KB  (1 << 10)     /* TX Packet Buffer = 32KB */
#define GEM_DMACFG_RX_BUF_SIZE_1600   (0x19 << 16)  /* RX Buffer Size = 1600 bytes */
#define GEM_DMACFG_RX_BUF_SIZE_1536   (0x18 << 16)
#define GEM_DMACFG_TX_PBUF_TCP_EN      (1 << 11)
#define GEM_DMACFG_ENDIAN_SWAP_PACKET   (0 << 7)
#define GEM_DMACFG_EXT_BD_EN          (1 << 28)     /* Extended Buffer Descriptor Mode (64-bit) */

#define GEM_TXSTATUS      0x0014
#define GEM_RXSTATUS      0x0020
#define GEM_IER           0x0024
#define GEM_IDR           0x0028
#define GEM_IMR           0x002C

#define GEM_RXQBASE       0x0018
#define GEM_TXQBASE       0x001C
#define GEM_UPPER_TXQBASE 0x04C8
#define GEM_UPPER_RXQBASE 0x04D4
#define GEM_RXQBASE_1       0x0480
#define GEM_TXQBASE_1       0x0440

#define GEM_SPEC_ADD1_BOTTOM  0x0088
#define GEM_SPEC_ADD1_TOP     0x008C

#define GEM_MAX_RX_QUEUES        1
#define GEM_MAX_TX_QUEUES        1
#define GEM_NUM_RX_DESC          1024
#define GEM_NUM_TX_DESC          1024
#define GEM_RX_BUF_SIZE          2048

void gem_write32(volatile void *base, uint32_t reg, uint32_t val);
void gem_write32_ordered(volatile void *base,uint32_t reg, uint32_t val);
void gem_doorbell(volatile void *base, uint32_t reg, uint32_t val);
uint32_t gem_read32(volatile void *base, uint32_t reg);
uint32_t gem_read32_acquire(volatile void *base, uint32_t reg);

/* --- bit for Network Control & Config --- */
#define GEM_NWCTRL_MAN_PORT_EN  (1 << 4)  /* Turn on MDIO of MAC */
#define GEM_NWCFG_MDC_DIV_48    (3 << 18) /* Devide Clock MDC to keep safe for PHY */

/* ============================================================================
 *  IEEE 802.3 PHY REGISTERS (Clause 22) & MDIO
 * ============================================================================
 */
#define GEM_PHYMNT      0x0034
#define GEM_NWSR_MDIOIDLE (1 << 2) /* Bit man_done MDIO is free */

#define PHY_CMD_WRITE   0x1
#define PHY_CMD_READ    0x2
#define PHY_CMD_CLAUSE_22 0x1

#define PHY_REG_BMCR            0x00 /* Basic Mode Control Register */
#define PHY_REG_BMSR            0x01 /* Basic Mode Status Register */
#define PHY_REG_ID1             0x02 /* PHY Identifier 1 */
#define PHY_TI_STATUS_REG       0x0011 /* Status of chip TI DP83867 */

/* bit control PHY */
#define PHY_BMCR_RESET          (1 << 15) /* Soft Reset chip PHY */
#define PHY_BMCR_AN_EN          (1 << 12) /* Bật Auto-Negotiation */
#define PHY_BMCR_AN_RESTART     (1 << 9)  /* req negotiation */
#define PHY_BMSR_LINK_UP        (1 << 2)  /* 1 = plug */
#define PHY_BMSR_AN_DONE        (1 << 5)  /* 1 = negotiation: done */

struct gem_adapter;

int gem_hw_reset(struct gem_adapter *adapter);
int gem_hw_start(struct gem_adapter *adapter);
void gem_hw_stop(struct gem_adapter *adapter);

#endif