#include <rte_malloc.h>        
#include <rte_memzone.h>       
#include <rte_ether.h>         
#include <rte_branch_prediction.h> 
#include <rte_ethdev_driver.h>

#include "gem_log.h"           
#include "gem_rxtx.h"
#include "gem_hw.h"
#include "gem_ethdev.h"


/* tx-queue-setup
alloc gem_txq: store state sw ring and pointer hw ring
alloc gem_tx_decs: init hw ring and store hw ring into gem_txq
alloc gem_tx_sw_ring: store into gem_txq
clean all status gem_txq
*/

int gem_tx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_id,
                      uint16_t nb_desc, unsigned int socket_id,
                      const struct rte_eth_txconf *tx_conf __rte_unused)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    struct gem_tx_queue *txq;
    const struct rte_memzone *mz;
    char mz_name[RTE_MEMZONE_NAMESIZE];
    uint32_t i;
    
    if (queue_id >= GEM_MAX_TX_QUEUES)
        return -EINVAL;
    
    txq = rte_zmalloc_socket("gem_txq", sizeof(*txq),
                             RTE_CACHE_LINE_SIZE, socket_id);
    if (!txq)
        return -ENOMEM;

    if (nb_desc == 0) nb_desc = GEM_NUM_TX_DESC;
    
    txq->nb_desc = nb_desc;
    txq->mmio_base = adapter->mmio_base;

    snprintf(mz_name, sizeof(mz_name), "gem_tx_desc_%u_%u",
             dev->data->port_id, queue_id);
    mz = rte_memzone_reserve_aligned(mz_name,
                                     txq->nb_desc * sizeof(struct gem_bd),
                                     socket_id,
                                     RTE_MEMZONE_IOVA_CONTIG,
                                     RTE_CACHE_LINE_SIZE);
    if (!mz) {
        rte_free(txq);
        return -ENOMEM;
    }

    PMD_LOG(INFO, "[TX SETUP] sizeof(struct gem_bd) = %lu bytes", sizeof(struct gem_bd));
    
    txq->desc_ring = (struct gem_bd *)mz->addr;
    txq->desc_ring_iova = mz->iova;
    memset(txq->desc_ring, 0, txq->nb_desc * sizeof(struct gem_bd)); 

    /* ==========================================================
     * DEBUG: VERIFY CONTIGUOUS PHYSICAL ADDRESSES OF THE TX RING
     * ========================================================== */
    PMD_LOG(INFO, "=== TX QUEUE %u: VERIFYING DESCRIPTOR CONTIGUITY ===", queue_id);
    for (i = 0; i < 5 && i < txq->nb_desc; i++) {
        void *virt_addr = &txq->desc_ring[i];
        rte_iova_t phys_addr = txq->desc_ring_iova + (i * sizeof(struct gem_bd));
        PMD_LOG(INFO, "  TX_BD[%u] | Virt = %p | Phys (IOVA) = 0x%08lx", i, virt_addr, phys_addr);
    }
    PMD_LOG(INFO, "=======================================================");

    txq->sw_ring = rte_zmalloc_socket("gem_tx_sw_ring",
                                      txq->nb_desc * sizeof(struct rte_mbuf *),
                                      RTE_CACHE_LINE_SIZE, socket_id);
    if (!txq->sw_ring) {
        rte_memzone_free(mz);
        rte_free(txq);
        return -ENOMEM;
    }

    for (i = 0; i < txq->nb_desc; i++) {
        int is_wrap = (i == (txq->nb_desc - 1));
        
        txq->desc_ring[i].addr_lo = 0;
        // txq->desc_ring[i].addr_hi = 0;
        // txq->desc_ring[i].reserved = 0;
        /* TX initialization: Set the USED bit to indicate software ownership */
        txq->desc_ring[i].status = GEM_TX_USED_BIT | (is_wrap ? GEM_TX_WRAP_BIT : 0);
    }

    gem_io_wmb();
    PMD_LOG(INFO, "[TX SETUP] BEFORE CLEAN: TX desc[0].status = 0x%08x", txq->desc_ring[0].status);

    /* Flush the initialized descriptor state from CPU cache to RAM */
    gem_cache_clean(txq->desc_ring, txq->nb_desc * sizeof(struct gem_bd));

    /* Invalidate cache to read back from RAM and verify the write */
    gem_cache_invalidate(txq->desc_ring, sizeof(struct gem_bd));

    PMD_LOG(INFO, "[TX SETUP] AFTER CLEAN: TX desc[0].status = 0x%08x", txq->desc_ring[0].status);

    txq->head = 0;
    txq->tail = 0;
    
    adapter->tx_queues[queue_id] = txq;
    dev->data->tx_queues[queue_id] = txq;
    
    PMD_LOG(INFO, "GEM TX queue %u setup completed. %u descriptors at Base IOVA: 0x%lx",
            queue_id, txq->nb_desc, txq->desc_ring_iova);
    
    return 0;
}


void gem_tx_queue_release(void *queue)
{
    struct gem_tx_queue *txq = (struct gem_tx_queue *)queue;
    uint16_t i;
    
    if (!txq)
        return;
    
    for (i = 0; i < txq->nb_desc; i++) {
        if (txq->sw_ring[i]) {
            rte_pktmbuf_free(txq->sw_ring[i]);
            txq->sw_ring[i] = NULL;
        }
    }
    
    rte_free(txq->sw_ring);    
    rte_free(txq);
}

/**
 * Transmit packets function for GEM (Gigabit Ethernet MAC)
 */
uint16_t
gem_xmit_pkts(void *tx_queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
    struct gem_tx_queue *txq = tx_queue;
    struct gem_bd *desc;
    struct rte_mbuf *mbuf;
    uint16_t nb_tx = 0;
    uint16_t head = txq->head; // Software cleanup position (clean pos)
    uint16_t tail = txq->tail; // Software enqueue position (write pos)
    uint16_t free_desc;
    uint32_t status;

    uint16_t used_desc = (tail - head + txq->nb_desc) % txq->nb_desc;
    if (used_desc >= 64) {
        /* Skip this iteration; the application will poll again without additional logging */
        return 0; 
    }

    PMD_LOG(DEBUG, "=== [TX BEGIN] Request to send %u packets ===", nb_pkts);
    PMD_LOG(DEBUG, "Current State: head = %u (SW clean pos), tail = %u (SW write pos)", head, tail);

    /* 1. CHECK MAC HARDWARE STATUS */
    uint32_t tx_status = gem_read32(txq->mmio_base, GEM_TXSTATUS);
    if (unlikely(tx_status != 0)) {
        // PMD_LOG(DEBUG, "[TX HW STATUS] TXSTATUS Register (0x14) = 0x%08x", tx_status); hi
        
        // if (tx_status & 0x01) PMD_LOG(DEBUG, " -> Bit 0 (Used Bit Read): HW tried to read a desc but USED bit was 1!");
        // if (tx_status & 0x02) PMD_LOG(DEBUG, " -> Bit 1 (Collision): Ethernet Collision detected.");
        // if (tx_status & 0x04) PMD_LOG(DEBUG, " -> Bit 2 (Retry Limit): MAC failed to transmit after retries.");
        // if (tx_status & 0x08) PMD_LOG(DEBUG, " -> Bit 3 (TX GO): TX is active.");
        // if (tx_status & 0x10) PMD_LOG(DEBUG, " -> Bit 4 (AMBA Error): DMA failed to read RAM.");
        // if (tx_status & 0x20) PMD_LOG(DEBUG, " -> Bit 5 (TX Complete): Frame transmitted successfully.");
        // if (tx_status & 0x40) PMD_LOG(DEBUG, " -> Bit 6 (TX Under-run): RAM too slow, MAC starved.");

        /* Write 1 to clear status flags and allow DMA to continue */
        gem_write32(txq->mmio_base, GEM_TXSTATUS, tx_status);
    }

    /* 2. RECLAIM LOOP: Release descriptors for completed transmissions */
    while (head != tail) {
        desc = &txq->desc_ring[head];
        
        /* Ensure the CPU reads the latest RAM data (on non-cache-coherent systems) */
        gem_cache_invalidate(desc, sizeof(struct gem_bd));
        gem_io_rmb();

        status = *(volatile uint32_t *)&desc->status;

        /* UG1087: Bit 31 (USED). 1 = transmission complete (SW owns it), 0 = HW is processing */
        if (!GEM_TX_IS_DONE(status)) {
            PMD_LOG(DEBUG, "[TX RECLAIM] Stop at head %u. Bit 31 is 0 (HW owns it).", head);
            break;
        }

        PMD_LOG(DEBUG, "[TX RECLAIM] Cleaned head %u. Status = 0x%08x", head, status);
        
        mbuf = txq->sw_ring[head];
        if (mbuf) {
            txq->tx_pkts++;
            txq->tx_bytes += mbuf->pkt_len;
            rte_pktmbuf_free(mbuf);
            txq->sw_ring[head] = NULL;
        }

        head++;
        if (head == txq->nb_desc)
            head = 0;
    }
    txq->head = head;

    /* 3. CALCULATE AVAILABLE DESCRIPTORS */
    free_desc = (head - tail - 1 + txq->nb_desc) % txq->nb_desc;
    
    PMD_LOG(DEBUG, "[TX CALC] free_desc = %u | head = %u | tail = %u", free_desc, head, tail);

    if (unlikely(free_desc == 0)) {
        PMD_LOG(WARNING, "[TX LIMIT] TX Ring is full. Returning 0.");
        return 0;
    }

    if (nb_pkts > free_desc)
        nb_pkts = free_desc;

    /* 4. TRANSMIT LOOP: Enqueue new packets into the ring */
    for (nb_tx = 0; nb_tx < nb_pkts; nb_tx++) {
        mbuf = tx_pkts[nb_tx];
        desc = &txq->desc_ring[tail];
        rte_iova_t pkt_iova = rte_mbuf_data_iova(mbuf);
        int is_wrap = (tail == (txq->nb_desc - 1));

        txq->sw_ring[tail] = mbuf;

        /* Set the 64-bit DMA address */
        // uint32_t addr_hi = (uint32_t)(pkt_iova >> 32);
        uint32_t addr_lo = (uint32_t)pkt_iova;
        
        // desc->addr_hi = addr_hi;
        desc->addr_lo = addr_lo;
        
        gem_io_wmb(); // Ensure the address is written before the status

        /* * Configure the status word:
         * Bits [13:0]: Length, Bit 15: EOF, Bit 30: Wrap, Bit 31: Used (0 for HW)
         */
        uint32_t new_status = GEM_TX_SET_CTRL(mbuf->data_len, 1, is_wrap);
        desc->status = new_status;

        PMD_LOG(DEBUG, "word 0 (add): %u, word 1 (status): %u", addr_lo, new_status);

        /* Flush cache so hardware can see the updated data */
        gem_cache_clean(rte_pktmbuf_mtod(mbuf, void *), mbuf->data_len);
        gem_cache_clean(desc, sizeof(struct gem_bd));

        tail++;
        if (tail == txq->nb_desc)
            tail = 0;
    }

    /* 5. TRIGGER HARDWARE TRANSMISSION (KICK DMA) */
    if (nb_tx > 0) {
        txq->tail = tail;
        gem_io_wmb();
        
        uint32_t ctrl_before = gem_read32(txq->mmio_base, GEM_NWCTRL);
        gem_write32_ordered(txq->mmio_base, GEM_NWCTRL, ctrl_before | GEM_NWCTRL_TX_START);
        
        // PMD_LOG(DEBUG, "[TX KICK] GEM_NWCTRL: Kicked TX_START bit. New tail = %u | NetworkCtrl = 0x%08x", tail, gem_read32(txq->mmio_base, GEM_NWCTRL));
    }

    PMD_LOG(DEBUG, "=== [TX END] Sent %u packets. head = %u, tail = %u ===", nb_tx, txq->head, txq->tail);

    return nb_tx;
}


int gem_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_id,
                      uint16_t nb_desc, unsigned int socket_id,
                      const struct rte_eth_rxconf *rx_conf __rte_unused,
                      struct rte_mempool *mb_pool)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    struct gem_rx_queue *rxq = NULL;
    const struct rte_memzone *mz;
    char mz_name[RTE_MEMZONE_NAMESIZE];
    uint32_t i;

    if (queue_id >= GEM_MAX_RX_QUEUES)
        return -EINVAL;

    /* Release the existing queue if setup is called again */
    if (dev->data->rx_queues[queue_id] != NULL) {
        struct gem_rx_queue *old_rxq = dev->data->rx_queues[queue_id];
        if (old_rxq->sw_ring) {
            for (i = 0; i < old_rxq->nb_desc; i++) {
                if (old_rxq->sw_ring[i])
                    rte_pktmbuf_free(old_rxq->sw_ring[i]);
            }
            rte_free(old_rxq->sw_ring);
        }
        rte_free(old_rxq);
        dev->data->rx_queues[queue_id] = NULL;
    }

    rxq = rte_zmalloc_socket("gem_rxq", sizeof(*rxq), RTE_CACHE_LINE_SIZE, socket_id);
    if (!rxq) return -ENOMEM;

    if (nb_desc == 0) nb_desc = GEM_NUM_RX_DESC;

    rxq->nb_desc = nb_desc;
    rxq->mb_pool = mb_pool;
    rxq->mmio_base = adapter->mmio_base;
    rxq->port_id = dev->data->port_id;

    snprintf(mz_name, sizeof(mz_name), "gem_rx_desc_%u_%u", dev->data->port_id, queue_id);

    mz = rte_memzone_reserve_aligned(mz_name, rxq->nb_desc * sizeof(struct gem_bd),
                                     socket_id, RTE_MEMZONE_IOVA_CONTIG, RTE_CACHE_LINE_SIZE);
    if (!mz) {
        rte_free(rxq);
        return -ENOMEM;
    }

    PMD_LOG(INFO, "[RX SETUP] sizeof(struct gem_bd) = %lu bytes", sizeof(struct gem_bd));

    rxq->desc_ring      = (struct gem_bd *)mz->addr;
    rxq->desc_ring_iova = mz->iova;
    memset(rxq->desc_ring, 0, rxq->nb_desc * sizeof(struct gem_bd));

    /* ==========================================================
     * DEBUG: VERIFY CONTIGUOUS PHYSICAL ADDRESSES OF THE RX RING
     * ========================================================== */
    PMD_LOG(INFO, "=== RX QUEUE %u: VERIFYING DESCRIPTOR CONTIGUITY ===", queue_id);
    for (i = 0; i < 5 && i < rxq->nb_desc; i++) {
        void *virt_addr = &rxq->desc_ring[i];
        rte_iova_t phys_addr = rxq->desc_ring_iova + (i * sizeof(struct gem_bd));
        PMD_LOG(INFO, "  RX_BD[%u] | Virt = %p | Phys (IOVA) = 0x%08lx", i, virt_addr, phys_addr);
    }
    PMD_LOG(INFO, "=======================================================");

    rxq->sw_ring = rte_zmalloc_socket("gem_rx_sw_ring", rxq->nb_desc * sizeof(struct rte_mbuf *),
                                      RTE_CACHE_LINE_SIZE, socket_id);
    if (!rxq->sw_ring) {
        rte_memzone_free(mz);
        rte_free(rxq);
        return -ENOMEM;
    }

    PMD_LOG(INFO, "=== RX QUEUE %u: INITIALIZING MBUFS & ADDR_LO ===", queue_id);
    for (i = 0; i < rxq->nb_desc; i++) {
        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(rxq->mb_pool);
        if (!mbuf) {
            PMD_LOG(ERR, "Failed to allocate mbuf for RX ring at index %u", i);
            while (i--) rte_pktmbuf_free(rxq->sw_ring[i]);
            rte_free(rxq->sw_ring);
            rte_memzone_free(mz);
            rte_free(rxq);
            return -ENOMEM;
        }

        mbuf->port = rxq->port_id;
        rxq->sw_ring[i] = mbuf;
        
        uint64_t dma_addr = rte_mbuf_data_iova(mbuf);
        int is_wrap = (i == (rxq->nb_desc - 1));
        
        // rxq->desc_ring[i].addr_hi = (uint32_t)(dma_addr >> 32);
        rxq->desc_ring[i].status = 0;
        // rxq->desc_ring[i].reserved = 0;
        
        /* Assign the mbuf address to addr_lo, clearing USED to hand ownership to HW */
        rxq->desc_ring[i].addr_lo = GEM_RX_SET_ADDR((uint32_t)dma_addr, is_wrap);

        /* Log the first five mbufs to verify the generated addr_lo values */
        if (i < 5) {
            PMD_LOG(INFO, "  RX_BD[%u] Config | Mbuf IOVA: 0x%08lx | Addr_lo Written: 0x%08x | Wrap: %d", 
                    i, dma_addr, rxq->desc_ring[i].addr_lo, is_wrap);
        }
    }
    PMD_LOG(INFO, "==================================================");

    gem_io_wmb();
    gem_cache_clean(rxq->desc_ring, rxq->nb_desc * sizeof(struct gem_bd));

    rxq->head = 0;
    rxq->tail = 0;

    adapter->rx_queues[queue_id] = rxq;
    dev->data->rx_queues[queue_id] = rxq;

    PMD_LOG(INFO, "GEM RX queue %u setup completed. %u descriptors at Base IOVA: 0x%lx",
            queue_id, rxq->nb_desc, rxq->desc_ring_iova);

    return 0;
}
void gem_rx_queue_release(void *queue)
{
    struct gem_rx_queue *rxq = (struct gem_rx_queue *)queue;
    uint16_t i;
    
    if (!rxq)
        return;
    
    for (i = 0; i < rxq->nb_desc; i++) {
        if (rxq->sw_ring[i]) {
            rte_pktmbuf_free(rxq->sw_ring[i]);
            rxq->sw_ring[i] = NULL;
        }
    }
    
    rte_free(rxq->sw_ring);    
    rte_free(rxq);
}


uint16_t gem_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
    struct gem_rx_queue *rxq = rx_queue;
    struct gem_bd *desc;
    struct rte_mbuf *mbuf, *new_mbuf;
    uint16_t nb_rx = 0;
    uint16_t head = rxq->head;

    /* Limit polling logs to avoid flooding the console when the ring is empty */
    static uint64_t poll_count = 0;
    if (poll_count++ % 100000 == 0) {
        // Log periodically to confirm polling without flooding the console
        // PMD_LOG(DEBUG, "Polling RX Ring... head = %u", head); 
    }

    gem_cache_invalidate(rxq->desc_ring, rxq->nb_desc * sizeof(struct gem_bd));
    gem_io_rmb();  

    while (nb_rx < nb_pkts) {
        desc = &rxq->desc_ring[head];

        uint32_t addr_lo = *(volatile uint32_t *)&desc->addr_lo;
        
        /* * UG1087 RX Word 0 (Address):
         * Bit 0 (USED): 0 = owned by software (HW has not written), 1 = HW has finished and returned ownership to SW.
         */
        if (!GEM_RX_IS_READY(addr_lo)) {
            // No new packet; the USED bit is still 0
            break; 
        }

        PMD_LOG(DEBUG, "=== [RX HIT] Received packet at head %u ===", head);
        PMD_LOG(DEBUG, "[RX ADDR] addr_lo = 0x%08x -> Bit 0 is 1 (HW gave it to SW)", addr_lo);
            
        gem_io_rmb();
        uint32_t status = desc->status;
        PMD_LOG(DEBUG, "[RX STATUS] RAW desc->status = 0x%08x", status);

        /* * UG1087 RX Word 1 (Status):
         * Bits [12:0]: Packet length
         * Bit 14: Start of Frame (SOF)
         * Bit 15: End of Frame (EOF)
         */
        int is_sof = GEM_RX_IS_SOF(status);
        int is_eof = GEM_RX_IS_EOF(status);
        uint32_t pkt_len = GEM_RX_GET_LEN(status);
        
        PMD_LOG(DEBUG, "  -> Length: %u bytes", pkt_len);
        PMD_LOG(DEBUG, "  -> SOF bit (14): %d | EOF bit (15): %d", is_sof ? 1 : 0, is_eof ? 1 : 0);

        if (unlikely(!is_sof || !is_eof)) {
            PMD_LOG(WARNING, "[RX DROP] Fragmented frame. Need both SOF=1 and EOF=1. Dropping.");
            new_mbuf = rxq->sw_ring[head];
            goto rearm_desc; 
        }

        mbuf = rxq->sw_ring[head];
        new_mbuf = rte_pktmbuf_alloc(rxq->mb_pool);
        
        if (unlikely(!new_mbuf)) {
            PMD_LOG(ERR, "[RX DROP] Memory pool empty! Reusing old mbuf.");
            new_mbuf = mbuf; 
        } else {
            mbuf->data_len = pkt_len;
            mbuf->pkt_len = pkt_len;
            mbuf->port = rxq->port_id;

            gem_cache_invalidate(rte_pktmbuf_mtod(mbuf, void *), pkt_len);

            rx_pkts[nb_rx++] = mbuf;
            rxq->rx_pkts++;
            rxq->rx_bytes += pkt_len;
            // PMD_LOG(INFO, "[RX SUCCESS] Packet extracted to DPDK format. IOVA: 0x%lx", rte_mbuf_data_iova(mbuf));
        }

rearm_desc:
        rxq->sw_ring[head] = new_mbuf;
        rte_iova_t dma_addr = rte_mbuf_data_iova(new_mbuf);
        int is_wrap = (head == (rxq->nb_desc - 1));

        /* Return the descriptor to hardware (clear USED bit) */
        // desc->addr_hi = (uint32_t)(dma_addr >> 32);
        desc->status = 0;
        // desc->reserved = 0;
        gem_io_wmb();
        
        /* SET_ADDR clears bit 0 (transfers ownership to HW) and sets WRAP if needed */
        uint32_t new_addr_lo = GEM_RX_SET_ADDR((uint32_t)dma_addr, is_wrap);
        desc->addr_lo = new_addr_lo;
        
        // PMD_LOG(INFO, "[RX REARM] head %u rearmed with IOVA = 0x%08x", 
        //         head, new_addr_lo);
        // PMD_LOG(INFO, "  -> Bit 0 is now 0 (HW owned). WRAP bit is %d", is_wrap);

        gem_cache_clean(desc, sizeof(struct gem_bd));
        
        head = (head + 1) % rxq->nb_desc;
    }
    
    // if (nb_rx > 0) {
    //     PMD_LOG(INFO, "=== [RX END] Processed %u packets. New head = %u ===", nb_rx, head);
    // }
    
    rxq->head = head;
    return nb_rx;
}