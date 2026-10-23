#ifndef _GEM_ETHDEV_H_
#define _GEM_ETHDEV_H_

#include <stddef.h>

#include <rte_ethdev.h>
#include <rte_kvargs.h>

#include "gem_log.h"
#include "gem_rxtx.h"
#include "gem_hw.h"

struct gem_adapter {
    volatile void *mmio_base;   /* mapped register base */
    size_t mmio_size;           /* size of MMIO region */
    int uio_fd;                 /* /dev/uioX fd */
    struct gem_rx_queue *rx_queues[GEM_MAX_RX_QUEUES];
    struct gem_tx_queue *tx_queues[GEM_MAX_TX_QUEUES];
    struct rte_ether_addr mac_addr;
    int config_phy; /* 1 = Cấu hình PHY, 0 = Bỏ qua */
    uint8_t phy_addr;   /* Lưu địa chỉ chip PHY dò được */
    int loopback; /* <--- THÊM MỚI: 1 = Bật Loopback, 0 = Tắt */
};

#define GEM_CONFIG_PHY_ARG "configPhy"
#define GEM_LOOPBACK_ARG   "loopback"   /* <--- THÊM MỚI */

/* Danh sách các tham số hợp lệ mà driver của bạn chấp nhận */
static const char * const gem_valid_args[] = {
    GEM_CONFIG_PHY_ARG,
    GEM_LOOPBACK_ARG,
    NULL
};

/* Hàm callback để chuyển đổi chuỗi giá trị (ví dụ "1") thành số nguyên (int) */
static int
gem_parse_int_arg(const char *key __rte_unused,
                  const char *value, void *extra_args)
{
    int *i = (int *)extra_args;
    char *end;

    if (value == NULL || extra_args == NULL)
        return -EINVAL;

    *i = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0')
        return -EINVAL;

    return 0;
}

static uint16_t 
gem_mdio_read(volatile void *base, uint8_t phy_addr, uint8_t reg_addr) 
{
    uint32_t val;
    while (!(gem_read32(base, GEM_NWSR) & GEM_NWSR_MDIOIDLE));
    val = (PHY_CMD_CLAUSE_22 << 30) | (PHY_CMD_READ << 28) | 
          ((phy_addr & 0x1F) << 23) | ((reg_addr & 0x1F) << 18) | 
          (0x2 << 16);
    gem_write32(base, GEM_PHYMNT, val);
    while (!(gem_read32(base, GEM_NWSR) & GEM_NWSR_MDIOIDLE));
    return gem_read32(base, GEM_PHYMNT) & 0xFFFF;
}

static void 
gem_mdio_write(volatile void *base, uint8_t phy_addr, uint8_t reg_addr, uint16_t data) 
{
    uint32_t val;
    while (!(gem_read32(base, GEM_NWSR) & GEM_NWSR_MDIOIDLE));
    val = (PHY_CMD_CLAUSE_22 << 30) | (PHY_CMD_WRITE << 28) | 
          ((phy_addr & 0x1F) << 23) | ((reg_addr & 0x1F) << 18) | 
          (0x2 << 16) | (data & 0xFFFF);
    gem_write32(base, GEM_PHYMNT, val);
    while (!(gem_read32(base, GEM_NWSR) & GEM_NWSR_MDIOIDLE));
}

static int 
gem_phy_init(struct gem_adapter *adapter)
{
    volatile void *base = adapter->mmio_base;
    uint8_t phy_addr = 0xFF;
    uint16_t phy_id, bmsr;
    int i, timeout;

    PMD_LOG(INFO, "Starting PHY Initialization...");

    /* 1. Kích hoạt giao tiếp MDIO và giảm xung Clock MDC */
    uint32_t nwctrl = gem_read32(base, GEM_NWCTRL);
    gem_write32_ordered(base, GEM_NWCTRL, nwctrl | GEM_NWCTRL_MAN_PORT_EN);
    
    uint32_t nwcfg = gem_read32(base, GEM_NWCFG);
    gem_write32_ordered(base, GEM_NWCFG, nwcfg | GEM_NWCFG_MDC_DIV_48);

    /* 2. Dò tìm địa chỉ chip PHY */
    for (i = 0; i < 32; i++) {
        phy_id = gem_mdio_read(base, i, PHY_REG_ID1);
        if (phy_id != 0xFFFF && phy_id != 0x0000) {
            phy_addr = i;
            adapter->phy_addr = phy_addr;
            PMD_LOG(INFO, "!!!Found PHY device at address %d (ID: 0x%04x)", phy_addr, phy_id);
            break;
        }
    }

    if (phy_addr == 0xFF) {
        PMD_LOG(ERR, "No PHY detected on MDIO bus!");
        return -EIO;
    }

    /* KIỂM TRA CỜ: Nếu configPhy = 0, dừng lại ở đây, không reset PHY */
    if (adapter->config_phy == 0) {
        PMD_LOG(INFO, "configPhy=0: Skipped PHY Reset/Auto-Neg. Relying on existing state.");
        return 0;
    }

    /* 3. Reset PHY và khởi động Auto-Negotiation */
    // PMD_LOG(INFO, "Resetting PHY and requesting Auto-Negotiation...");
    // gem_mdio_write(base, phy_addr, PHY_REG_BMCR, 
    //                PHY_BMCR_RESET | PHY_BMCR_AN_EN | PHY_BMCR_AN_RESTART);

    /* 3. ĐÁNH THỨC PHY (Không Reset để giữ nguyên RGMII Delay của Linux) */
    PMD_LOG(INFO, "Waking up PHY and requesting Auto-Negotiation...");
    
    /* Đọc thanh ghi cấu hình hiện tại của PHY (BMCR) */
    uint16_t bmcr = gem_mdio_read(base, phy_addr, PHY_REG_BMCR);
    
    /* Xóa bit số 11 (Power Down) để đánh thức PHY dậy */
    bmcr &= ~(1 << 11); 
    
    /* Bật lại Auto-Negotiation để đàm phán tốc độ với PC */
    bmcr |= PHY_BMCR_AN_EN | PHY_BMCR_AN_RESTART;
    
    /* Ghi đè cấu hình mới xuống PHY */
    gem_mdio_write(base, phy_addr, PHY_REG_BMCR, bmcr);

    /* 4. Chờ PHY Reset hoàn tất */
    timeout = 1000;
    while (timeout > 0) {
        if (!(gem_mdio_read(base, phy_addr, PHY_REG_BMCR) & PHY_BMCR_RESET)) break;
        rte_delay_ms(1);
        timeout--;
    }

    /* 5. Đợi Link UP và Đàm phán hoàn tất */
    PMD_LOG(INFO, "Waiting for Link to come UP...");
    timeout = 5000;
    while (timeout > 0) {
        bmsr = gem_mdio_read(base, phy_addr, PHY_REG_BMSR);
        if ((bmsr & PHY_BMSR_LINK_UP) && (bmsr & PHY_BMSR_AN_DONE)) {
            PMD_LOG(INFO, "PHY Link is UP! Auto-Negotiation completed.");
            break;
        }
        rte_delay_ms(1);
        timeout--;
    }

    if (timeout == 0) {
        PMD_LOG(WARNING, "PHY Link timeout! Check Ethernet cable.");
        return -EIO;
    }

    /* 6. Đồng bộ tốc độ với MAC */
    uint16_t phy_status = gem_mdio_read(base, phy_addr, PHY_TI_STATUS_REG);
    nwcfg = gem_read32(base, GEM_NWCFG);
    
    nwcfg &= ~GEM_NWCFG_GIGABIT_MODE_ENABLE; 
    nwcfg &= ~(1 << 0); /* Tắt cờ 100Mbps */

    uint8_t speed = (phy_status >> 14) & 0x3;
    if (speed == 0x2) {
        PMD_LOG(INFO, "PHY negotiated: 1000 Mbps Full-Duplex");
        nwcfg |= GEM_NWCFG_GIGABIT_MODE_ENABLE;
    } else if (speed == 0x1) {
        PMD_LOG(INFO, "PHY negotiated: 100 Mbps Full-Duplex");
        nwcfg |= (1 << 0);
    } else {
        PMD_LOG(INFO, "PHY negotiated: 10 Mbps Full-Duplex");
    }

    gem_write32_ordered(base, GEM_NWCFG, nwcfg);
    return 0;
}

#endif