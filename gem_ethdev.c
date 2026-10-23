#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#include <rte_bus_vdev.h>
#include <rte_cycles.h>
#include <rte_ethdev_driver.h>
#include <rte_ethdev_vdev.h>
#include <rte_malloc.h>        

#include "gem_hw.h"
#include "gem_ethdev.h"
#include "gem_rxtx.h"
#include "gem_log.h"

int gem_logtype;
RTE_LOG_REGISTER(gem_logtype, pmd.net.gem, NOTICE);

/* ========== Device Lifecycle Ops ========== */
static int
gem_dev_configure(struct rte_eth_dev *dev __rte_unused)
{
    return 0;
}

static int
gem_dev_start(struct rte_eth_dev *dev)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    struct gem_rx_queue *rxq;
    struct gem_tx_queue *txq;
    uint16_t i;

    for (i = 0; i < dev->data->nb_rx_queues; i++) {
        rxq = adapter->rx_queues[i];

        PMD_LOG(INFO,
            "Configuring RXQ[%u] ring virt=%p iova=0x%lx",
            i,
            rxq->desc_ring,
            rxq->desc_ring_iova);

        gem_io_wmb();

        // gem_write32(
        //     rxq->mmio_base, 
        //     GEM_UPPER_RXQBASE, 
        //     (uint32_t)(rxq->desc_ring_iova >> 32)
        // );

        gem_write32(
            rxq->mmio_base,
            GEM_RXQBASE,
            (uint32_t)(rxq->desc_ring_iova)
        );

        PMD_LOG(INFO,
            "RXQ[%u] HW base readback: hi=0x%08x lo=0x%08x",
            i,
            gem_read32(rxq->mmio_base, GEM_UPPER_RXQBASE),
            gem_read32(rxq->mmio_base, GEM_RXQBASE)
        );

        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    }

/* Allocate physically contiguous memory for Dummy Queue 0 */
    const struct rte_memzone *mz_dummy = rte_memzone_reserve_aligned(
        "dummy_tx_q0",
        2 * sizeof(struct gem_bd),
        rte_socket_id(),
        RTE_MEMZONE_IOVA_CONTIG,
        RTE_CACHE_LINE_SIZE
    );

    if (mz_dummy) {
        struct gem_bd *dummy_ring = (struct gem_bd *)mz_dummy->addr;
        
        /* * Initialize the first descriptor: 
         * - addr_lo = 0
         * - Bit 31 (USED) = 1: Indicates software ownership; DMA stops reading.
         * - Bit 30 (WRAP) = 1: Marks the end of the ring; DMA wraps to the beginning.
         */
        dummy_ring[0].addr_lo = 0;
        dummy_ring[0].status = GEM_TX_USED_BIT | GEM_TX_WRAP_BIT;
        
        /* REQUIRED: Flush data from CPU cache to physical RAM */
        gem_cache_clean(dummy_ring, 2 * sizeof(struct gem_bd));
        gem_io_wmb();

        /* Write the physical address to the MAC Queue 0 register */
        gem_write32(adapter->mmio_base, GEM_TXQBASE_1, (uint32_t)mz_dummy->iova);
        
        PMD_LOG(INFO, "Configured Dummy TX Queue 0 at IOVA: 0x%08lx", mz_dummy->iova);
    } else {
        PMD_LOG(ERR, "Failed to allocate memory for Dummy TX Queue 0");
    }

    for (i = 0; i < dev->data->nb_tx_queues; i++){
        txq = adapter->tx_queues[i];

        PMD_LOG(INFO,
        "Configuring TXQ[%u] ring virt=%p iova=0x%lx",
        i,
        txq->desc_ring,
        txq->desc_ring_iova);

        gem_io_wmb();

        // gem_write32(
        //     txq->mmio_base, 
        //     GEM_UPPER_TXQBASE, 
        //     (uint32_t)(txq->desc_ring_iova >> 32)
        // );
        
        gem_write32(
            txq->mmio_base,
            GEM_TXQBASE,
            (uint32_t)txq->desc_ring_iova
        );

        PMD_LOG(INFO,
            "TXQ[%u] HW base readback: hi=0x%08x lo=0x%08x",
            i,
            gem_read32(txq->mmio_base, GEM_UPPER_TXQBASE),
            gem_read32(txq->mmio_base, GEM_TXQBASE)
        );

        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    }

    volatile void *base = adapter->mmio_base;
    uint32_t dmacfg =
        GEM_DMACFG_AMBA_BURST_LEN_16 |       /* f. INCR16 */
        GEM_DMACFG_RX_PBUF_SIZE_32KB |       /* b. RX PBUF 32KB */
        GEM_DMACFG_TX_PBUF_SIZE_32KB |       /* c. TX PBUF 32KB */
        GEM_DMACFG_RX_BUF_SIZE_1600  |       /* a. Set to 1600 bytes (0x19) */
        GEM_DMACFG_TX_PBUF_TCP_EN    |       /* d. TCP/IP checksum offload TX */
        GEM_DMACFG_ENDIAN_SWAP_PACKET;       /* e. Little endian (0) */

    PMD_LOG(INFO,
        "Writing DMACFG = 0x%08x",
        dmacfg);

    gem_write32_ordered(base, GEM_DMACFG, dmacfg);

    PMD_LOG(INFO,
        "DMACFG readback = 0x%08x",
        gem_read32(base, GEM_DMACFG));

    /* NETWORK CONFIGURATION */
    // uint32_t nwcfg =
    //     GEM_NWCFG_FULL_DUPLEX |                  /* a. Enable full duplex */
    //     GEM_NWCFG_GIGABIT_MODE_ENABLE |          /* b. Enable gigabit mode */
    //     GEM_NWCFG_MULTICAST_HASH_EN |            /* c. Enable multicast frames */
    //     GEM_NWCFG_COPY_ALL_FRAME |               /* d. Enable promiscuous mode */
    //     GEM_NWCFG_RECV_CHECKSUM_OFFLOAD_ENABLE | /* e. Enable TCP/IP checksum offload */
    //     GEM_NWCFG_PAUSE_EN |                     /* f. Enable pause frames */
    //     GEM_NWCFG_MDC_DIV_48;                    /* g. Set the MDC clock divisor */

    // nwcfg &= ~GEM_NWCFG_NO_BROADCAST;

    /* NETWORK CONFIGURATION (matching the Linux macb driver: 0x012E044A) */
    uint32_t nwcfg =
        GEM_NWCFG_FULL_DUPLEX |                  /* Bit 1 */
        GEM_NWCFG_JUMBO_FRAME_ENABLE |           /* Bit 3 */
        GEM_NWCFG_MULTICAST_HASH_EN |            /* Bit 6 */
        GEM_NWCFG_GIGABIT_MODE_ENABLE |          /* Bit 10 */
        GEM_NWCFG_FCS_REMOVE |                   /* Bit 17: Remove the 4-byte CRC */
        GEM_NWCFG_MDC_DIV_48 |                   /* Bits [18:20] */
        GEM_NWCFG_DBUS_WIDTH_64 |                /* Bit 21: Bus 64-bit */
        GEM_NWCFG_RECV_CHECKSUM_OFFLOAD_ENABLE;  /* Bit 24 */

    /* * Linux disables promiscuous mode by default. To capture
     * all packets regardless of MAC address during DPDK testing, keep this line:
     */
    nwcfg |= GEM_NWCFG_COPY_ALL_FRAME; 

    /* Ensure broadcast frames are not blocked (Bit 5 = 0) */
    nwcfg &= ~GEM_NWCFG_NO_BROADCAST;

    PMD_LOG(INFO,
        "Writing NWCFG = 0x%08x",
        nwcfg);
    gem_write32_ordered(base, GEM_NWCFG, nwcfg);

    PMD_LOG(INFO,
        "NWCFG readback = 0x%08x",
        gem_read32(base, GEM_NWCFG));

    /* IF LOOPBACK = 1: SKIP PHY INITIALIZATION COMPLETELY */
    // PMD_LOG(INFO, "Initializing PHY to generate MAC Clocks...");
    // if (gem_phy_init(adapter) < 0) {
    //     /* If initialization fails (e.g., no cable), allow it when loopback is enabled. */
    //     if (adapter->loopback == 1) {
    //         PMD_LOG(INFO, "PHY Init failed (no cable?), but Loopback is ON. Forcing start...");
    //     } else {
    //         PMD_LOG(ERR, "PHY Discovery/Init failed. Stopping.");
    //         return -EIO;
    //     }
    // } else {
    //     PMD_LOG(INFO, "PHY Initialized successfully.");
    // }

    PMD_LOG(INFO, "Initializing PHY to generate MAC Clocks...");
    
    /* INITIALIZE PHY: STOP IMMEDIATELY ON FAILURE, WITHOUT EXCEPTIONS */
    if (gem_phy_init(adapter) < 0) {
        PMD_LOG(ERR, "PHY Discovery/Init failed. Stopping.");
        return -EIO;
    } else {
        PMD_LOG(INFO, "PHY Initialized successfully.");
    }

    if (gem_hw_start(adapter) < 0)
        return -EIO;
    
    dev->data->dev_link.link_status = ETH_LINK_UP;
    
    PMD_LOG(INFO, "Device started");
    return 0;
}

static int
gem_dev_stop(struct rte_eth_dev *dev)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    uint16_t i;
    
    gem_hw_stop(adapter);
    
    dev->data->dev_link.link_status = ETH_LINK_DOWN;
    
    for (i = 0; i < dev->data->nb_rx_queues; i++)
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
    for (i = 0; i < dev->data->nb_tx_queues; i++)
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
    
    PMD_LOG(INFO, "Device stopped");
    return 0;
}

static int
gem_dev_close(struct rte_eth_dev *dev)
{
    PMD_LOG(INFO, "Closing device");
    
    if (rte_eal_process_type() != RTE_PROC_PRIMARY)
        return 0;
    
    dev->data->mac_addrs = NULL;
    return 0;
}

static int
gem_dev_info_get(struct rte_eth_dev *dev __rte_unused,
                    struct rte_eth_dev_info *dev_info)
{
    dev_info->max_rx_queues = GEM_MAX_RX_QUEUES;
    dev_info->max_tx_queues = GEM_MAX_TX_QUEUES;
    dev_info->min_rx_bufsize = 64;
    dev_info->max_rx_pktlen = GEM_RX_BUF_SIZE;
    dev_info->max_mac_addrs = 1;
    
    return 0;
}

static int
gem_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    uint16_t i;
    
    /* FIX: Add NULL checks to prevent crashes if queue not set up */
    for (i = 0; i < dev->data->nb_rx_queues; i++) {
        struct gem_rx_queue *rxq = adapter->rx_queues[i];
        if (rxq) {
            stats->ipackets += rxq->rx_pkts;
            stats->ibytes += rxq->rx_bytes;
        }
    }
    
    for (i = 0; i < dev->data->nb_tx_queues; i++) {
        struct gem_tx_queue *txq = adapter->tx_queues[i];
        if (txq) {
            stats->opackets += txq->tx_pkts;
            stats->obytes += txq->tx_bytes;
        }
    }
    
    return 0;
}

static int
gem_stats_reset(struct rte_eth_dev *dev)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    uint16_t i;
    
    for (i = 0; i < GEM_MAX_RX_QUEUES; i++) {
        if (adapter->rx_queues[i]) {
            adapter->rx_queues[i]->rx_pkts = 0;
            adapter->rx_queues[i]->rx_bytes = 0;
        }
    }
    
    for (i = 0; i < GEM_MAX_TX_QUEUES; i++) {
        if (adapter->tx_queues[i]) {
            adapter->tx_queues[i]->tx_pkts = 0;
            adapter->tx_queues[i]->tx_bytes = 0;
        }
    }
    
    return 0;
}

static int
gem_link_update(struct rte_eth_dev *dev, int wait_to_complete __rte_unused)
{
    struct gem_adapter *adapter = dev->data->dev_private;
    struct rte_eth_link link;
    volatile void *base = adapter->mmio_base;
    uint16_t bmsr;
    
    memset(&link, 0, sizeof(link));

    // if (adapter->loopback == 1) {
    //     link.link_status = ETH_LINK_UP;
    //     link.link_duplex = ETH_LINK_FULL_DUPLEX;
    //     link.link_speed = ETH_SPEED_NUM_1G;
    //     return rte_eth_linkstatus_set(dev, &link);
    // }

    if (adapter->phy_addr == 0xFF) {
        link.link_status = ETH_LINK_DOWN;
        return rte_eth_linkstatus_set(dev, &link);
    }

    /* Read the actual status register from the PHY */
    bmsr = gem_mdio_read(base, adapter->phy_addr, PHY_REG_BMSR);

    PMD_LOG(DEBUG, "BMSR = 0x%04x", bmsr);
    PMD_LOG(DEBUG, "Link bit = 0x%04x", bmsr & PHY_BMSR_LINK_UP);

    if (bmsr & PHY_BMSR_LINK_UP) {
        link.link_status = ETH_LINK_UP;
        link.link_duplex = ETH_LINK_FULL_DUPLEX;
        link.link_speed = ETH_SPEED_NUM_1G; 
    } else {
        link.link_status = ETH_LINK_DOWN;
        link.link_duplex = ETH_LINK_HALF_DUPLEX;
        link.link_speed = ETH_SPEED_NUM_NONE;
    }

    return rte_eth_linkstatus_set(dev, &link);
}

/* ========== eth_dev_ops Structure ========== */

static const struct eth_dev_ops gem_eth_dev_ops = {
    .dev_configure        = gem_dev_configure,
    .dev_start            = gem_dev_start,
    .dev_stop             = gem_dev_stop,
    .dev_close            = gem_dev_close,
    .dev_infos_get        = gem_dev_info_get,
    .rx_queue_setup       = gem_rx_queue_setup,
    .tx_queue_setup       = gem_tx_queue_setup,
    .rx_queue_release     = gem_rx_queue_release,
    .tx_queue_release     = gem_tx_queue_release,
    .link_update          = gem_link_update,
    .stats_get            = gem_stats_get,
    .stats_reset          = gem_stats_reset,
};

/* ========== uio-platform Device Probe ========== */
static int
gem_dev_create(struct rte_vdev_device *vdev, int config_phy, int loopback)
{
    struct rte_eth_dev *eth_dev;
    struct gem_adapter *adapter;

    int uio_idx;
    int fd;

    void *mmio_base;
    size_t mmio_size;

    PMD_LOG(INFO, "Creating GEM device (UIO)");

    uio_idx = find_uio_device("ethernet");

    if (uio_idx < 0) {
        PMD_LOG(ERR, "UIO device not found");
        return -ENODEV;
    }

    fd = gem_open_uio(uio_idx);

    if (fd < 0) {
        PMD_LOG(ERR, "Failed to open UIO");
        return -errno;
    }

    mmio_size = gem_get_map_size(uio_idx);

    mmio_base = gem_map_mmio(fd, mmio_size);
    if (!mmio_base) {
        PMD_LOG(ERR,"Failed to mmap MMIO");
        close(fd);
        return -ENOMEM;
    }

    eth_dev = rte_eth_vdev_allocate(vdev, sizeof(struct gem_adapter));

    if (!eth_dev) {
        munmap(mmio_base, mmio_size);
        close(fd);
        return -ENOMEM;
    }

    adapter = eth_dev->data->dev_private;
    adapter->mmio_base = mmio_base;
    adapter->mmio_size = mmio_size;
    adapter->uio_fd = fd;
    adapter->loopback = loopback;
    /* ---> STORE IN THE STRUCT <--- */
    adapter->config_phy = config_phy;
    adapter->phy_addr = 0xFF; /* Default value when no PHY has been found */

    rte_eth_random_addr(adapter->mac_addr.addr_bytes);
    adapter->mac_addr.addr_bytes[0] = 0x02;
    eth_dev->data->mac_addrs = &adapter->mac_addr;

    uint32_t mac_bottom = (adapter->mac_addr.addr_bytes[3] << 24) |
                          (adapter->mac_addr.addr_bytes[2] << 16) |
                          (adapter->mac_addr.addr_bytes[1] << 8)  |
                          (adapter->mac_addr.addr_bytes[0]);
    uint32_t mac_top    = (adapter->mac_addr.addr_bytes[5] << 8)  |
                          (adapter->mac_addr.addr_bytes[4]);
    
    gem_write32(adapter->mmio_base, GEM_SPEC_ADD1_BOTTOM, mac_bottom);
    gem_write32(adapter->mmio_base, GEM_SPEC_ADD1_TOP, mac_top);

    adapter = eth_dev->data->dev_private;
    adapter->config_phy = config_phy; /* Store in the struct */

    eth_dev->data->nb_rx_queues = 0;
    eth_dev->data->nb_tx_queues = 0;
    eth_dev->data->dev_link.link_speed = ETH_SPEED_NUM_1G;
    eth_dev->data->dev_link.link_duplex = ETH_LINK_FULL_DUPLEX;
    eth_dev->data->dev_link.link_status = ETH_LINK_DOWN;
    eth_dev->data->dev_link.link_autoneg = ETH_LINK_FIXED;


    eth_dev->dev_ops = &gem_eth_dev_ops;
    eth_dev->rx_pkt_burst = gem_recv_pkts;
    eth_dev->tx_pkt_burst = gem_xmit_pkts;

    gem_hw_reset(adapter);

    rte_eth_dev_probing_finish(eth_dev);
    PMD_LOG(INFO, "GEM device created successfully");
    return 0;
}

static int
gem_probe(struct rte_vdev_device *vdev)
{
    const char *name;
    const char *args;            /* Declare args */
    struct rte_kvargs *kvlist;   /* Declare kvlist */
    int config_phy = 0;          /* Declare and initialize config_phy */
    int loopback = 0;   /* Loopback disabled by default */
    int ret;

    name = rte_vdev_device_name(vdev);
    args = rte_vdev_device_args(vdev); /* Get the argument string after the comma */

    PMD_LOG(INFO, "Probing gem PMD (UIO): %s", name);

    /* --- START ARGUMENT PARSING --- */
    if (args != NULL) {
        kvlist = rte_kvargs_parse(args, gem_valid_args);
        if (kvlist == NULL) {
            PMD_LOG(ERR, "Failed to parse arguments");
            return -EINVAL;
        }

        /* Check whether the user provided configPhy */
        if (rte_kvargs_count(kvlist, GEM_CONFIG_PHY_ARG) == 1) {
            ret = rte_kvargs_process(kvlist, GEM_CONFIG_PHY_ARG,
                                     &gem_parse_int_arg, &config_phy);
            if (ret < 0) {
                PMD_LOG(ERR, "Failed to parse %s", GEM_CONFIG_PHY_ARG);
                rte_kvargs_free(kvlist);
                return -EINVAL;
            }
        }

        if (rte_kvargs_count(kvlist, GEM_LOOPBACK_ARG) == 1) {
            ret = rte_kvargs_process(kvlist, GEM_LOOPBACK_ARG, 
                                    &gem_parse_int_arg, &loopback);
            if (ret < 0) {
                PMD_LOG(ERR, "Failed to parse %s", GEM_LOOPBACK_ARG);
                rte_kvargs_free(kvlist);
                return -EINVAL;
            }
        }

        rte_kvargs_free(kvlist);
    }
    ret = gem_dev_create(vdev, config_phy, loopback); 

    return ret;
}

static int
gem_remove(struct rte_vdev_device *vdev)
{
    struct rte_eth_dev *eth_dev;
    struct gem_adapter *adapter;

    eth_dev =
        rte_eth_dev_allocated(
            rte_vdev_device_name(vdev));

    if (!eth_dev)
        return 0;

    adapter = eth_dev->data->dev_private;

    if (adapter->mmio_base)
        munmap(
            (void *)adapter->mmio_base,
            adapter->mmio_size);

    if (adapter->uio_fd >= 0)
        close(adapter->uio_fd);

    gem_dev_close(eth_dev);

    rte_eth_dev_release_port(eth_dev);

    PMD_LOG(INFO,
        "gem device removed");

    return 0;
}


static struct rte_vdev_driver gem_drv = {
    .probe  = gem_probe,
    .remove = gem_remove,
};

RTE_PMD_REGISTER_VDEV(net_gem, gem_drv);
RTE_PMD_REGISTER_PARAM_STRING(net_gem, "configPhy=<0|1> loopback=<0|1>");