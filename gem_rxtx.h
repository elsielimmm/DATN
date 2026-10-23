#ifndef _GEM_RXTX_H_
#define _GEM_RXTX_H_

#include <stdint.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

struct gem_bd {
    uint32_t addr_lo;  
    uint32_t status;   
    // uint32_t addr_hi;  
    // uint32_t reserved;
};

#define GEM_TX_USED_BIT         (1U << 31)  
#define GEM_TX_WRAP_BIT         (1U << 30)  
#define GEM_TX_LAST_BIT         (1U << 15)  
#define GEM_TX_LEN_MASK         0x3FFFU     
#define GEM_TX_IS_DONE(status)      ((status) & GEM_TX_USED_BIT)
#define GEM_TX_SET_CTRL(len, is_last, is_wrap) \
    ( ((len) & GEM_TX_LEN_MASK) | \
      ((is_last) ? GEM_TX_LAST_BIT : 0) | \
      ((is_wrap) ? GEM_TX_WRAP_BIT : 0) ) 
#define GEM_TX_MARK_USED(status)    ((status) |= GEM_TX_USED_BIT)

#define GEM_RX_USED_BIT         (1U << 0)   
#define GEM_RX_WRAP_BIT         (1U << 1)   
#define GEM_RX_ADDR_MASK        0xFFFFFFFCU
#define GEM_RX_IS_READY(addr_lo)    ((addr_lo) & GEM_RX_USED_BIT)
#define GEM_RX_SET_ADDR(phys_addr, is_wrap) \
    ( ((phys_addr) & GEM_RX_ADDR_MASK) | \
      ((is_wrap) ? GEM_RX_WRAP_BIT : 0) ) 
#define GEM_RX_EOF_BIT          (1U << 15)  
#define GEM_RX_SOF_BIT          (1U << 14)  
#define GEM_RX_LEN_MASK         0x1FFFU     
#define GEM_RX_GET_LEN(status)      ((status) & GEM_RX_LEN_MASK)
#define GEM_RX_IS_SOF(status)       ((status) & GEM_RX_SOF_BIT)
#define GEM_RX_IS_EOF(status)       ((status) & GEM_RX_EOF_BIT)

struct gem_tx_queue {
    struct gem_bd *desc_ring;
    rte_iova_t desc_ring_iova;

    struct rte_mbuf **sw_ring;

    uint16_t nb_desc;

    uint16_t head;
    uint16_t tail;

    uint64_t tx_pkts;
    uint64_t tx_bytes;

    volatile void *mmio_base;
};

struct gem_rx_queue {
    struct rte_mempool *mb_pool;

    struct gem_bd *desc_ring;
    rte_iova_t desc_ring_iova;

    struct rte_mbuf **sw_ring;

    uint16_t nb_desc;

    uint16_t head;
    uint16_t tail;

    uint16_t port_id;

    uint64_t rx_pkts;
    uint64_t rx_bytes;

    volatile void *mmio_base;
};

int gem_tx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_id,
                      uint16_t nb_desc, unsigned int socket_id,
                      const struct rte_eth_txconf *tx_conf __rte_unused);

void gem_tx_queue_release(void *txq);

uint16_t gem_xmit_pkts(void *tx_queue,
				              struct rte_mbuf **tx_pkts,
				              uint16_t nb_pkts);

int gem_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_id,
                      uint16_t nb_desc, unsigned int socket_id,
                      const struct rte_eth_rxconf *rx_conf __rte_unused,
                      struct rte_mempool *mb_pool);

void gem_rx_queue_release(void *rxq);


uint16_t gem_recv_pkts(void *rx_queue,
				              struct rte_mbuf **rx_pkts,
				              uint16_t nb_pkts);

#endif