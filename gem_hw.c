#include "gem_hw.h"
#include "gem_ethdev.h"
#include "gem_log.h"  

int find_uio_device(const char *target)
{
    struct dirent *entry;
    DIR *dir;

    dir = opendir(UIO_PATH);

    if (!dir)
        return -1;

    while ((entry = readdir(dir))) {

        if (strncmp(entry->d_name, "uio", 3))
            continue;

        char name_path[128];
        char name[64];

        snprintf(
            name_path,
            sizeof(name_path),
            "%s/%s/name",
            UIO_PATH,
            entry->d_name);

        FILE *f = fopen(name_path, "r");

        if (!f)
            continue;

        fgets(name, sizeof(name), f);

        fclose(f);

        if (strncmp(name, target, strlen(target)) == 0) {

            int idx = atoi(entry->d_name + 3);

            closedir(dir);

            return idx;
        }
    }

    closedir(dir);

    return -1;
}
//hi

size_t gem_get_map_size(int uio_idx)
{
    char path[128];
    unsigned long size;

    snprintf(
        path,
        sizeof(path),
        "/sys/class/uio/uio%d/maps/map0/size",
        uio_idx);

    FILE *f = fopen(path, "r");

    if (!f)
        return 0x1000;

    fscanf(f, "%lx", &size);

    fclose(f);

    return size;
}

int gem_open_uio(int uio_idx)
{
    char dev_path[64];

    snprintf(
        dev_path,
        sizeof(dev_path),
        "/dev/uio%d",
        uio_idx);

    return open(dev_path, O_RDWR | O_SYNC);
}

void * gem_map_mmio(int fd,
             size_t map_size)
{
    void *base;

    base = mmap(
        NULL,
        map_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);

    if (base == MAP_FAILED)
        return NULL;

    return base;
}

void gem_write32(volatile void *base, uint32_t reg, uint32_t val)
{
    rte_write32(val, (volatile void *)((uintptr_t)base + reg));
}

void gem_write32_ordered(volatile void *base, uint32_t reg, uint32_t val)
{
    rte_write32(val, (volatile void *)((uintptr_t)base + reg));
    gem_io_fence();
}


void gem_doorbell(volatile void *base, uint32_t reg, uint32_t val)
{
    gem_io_wmb();
    rte_write32(val, (volatile void *)((uintptr_t)base + reg));
}

uint32_t gem_read32(volatile void *base,uint32_t reg)
{
    return rte_read32((volatile void *)((uintptr_t)base + reg));
}

uint32_t gem_read32_acquire(volatile void *base, uint32_t reg)
{
    uint32_t val = rte_read32((volatile void *)((uintptr_t)base + reg));
    gem_io_rmb();
    return val;
}

// int gem_hw_reset(struct gem_adapter *adapter)
// {
//     volatile void *base = adapter->mmio_base;
//     gem_write32_ordered(base, GEM_NWCTRL, GEM_NWCTRL_CLEAR_STAT_REGS);

//     gem_write32_ordered(base, GEM_TXSTATUS, 0xFFFFFFFF);
//     gem_write32_ordered(base, GEM_RXSTATUS, 0xFFFFFFFF);

//     gem_write32_ordered(base, GEM_IDR, 0xFFFFFFFF);

//     gem_write32_ordered(base, GEM_RXQBASE, 0);
//     gem_write32_ordered(base, GEM_TXQBASE, 0);

//     gem_io_fence();
//     return 0;
// }

int gem_hw_reset(struct gem_adapter *adapter)
{
    volatile void *base = adapter->mmio_base;
    uint32_t  status;
    int timeout;

    PMD_LOG(INFO, "Hard resetting GEM hardware state (Cleaning up potential Zombie DMA)...");

    /* 1. KÉO PHANH KHẨN CẤP: Tắt RX và TX ngay lập tức */
    // ctrl = gem_read32(base, GEM_NWCTRL);
    // ctrl &= ~GEM_NWCTRL_RX_EN;
    // ctrl &= ~GEM_NWCTRL_TX_EN;
    gem_write32_ordered(base, GEM_NWCTRL, 0x0);

    /* 2. CHỜ HARDWARE DỪNG HẲN: Phải đợi nó xả hết công việc dở dang */
    timeout = 100000;
    do {
        status = gem_read32_acquire(base, GEM_NWSR);
        /* Nếu cả TX và RX đều đã báo rảnh (không còn bit ACT) thì thoát vòng lặp */
        if (!(status & GEM_NWSR_RX_ACT) && !(status & GEM_NWSR_TX_ACT))
            break;
        rte_delay_us_block(1);
    } while (--timeout);

    if (!timeout) {
        PMD_LOG(WARNING, "[RESET] Timeout waiting for GEM to idle. Status = 0x%08x", status);
    }

    /* 3. DỌN RÁC THANH GHI TRẠNG THÁI */
    gem_write32_ordered(base, GEM_NWCTRL, GEM_NWCTRL_CLEAR_STAT_REGS);
    gem_write32_ordered(base, GEM_TXSTATUS, 0xFF); /* Ghi 1 để xóa lỗi cũ */
    gem_write32_ordered(base, GEM_RXSTATUS, 0x0F);
    gem_write32_ordered(base, GEM_IDR, 0x7FFFEFF);      /* Tắt mọi ngắt */


    /* 4. XÓA SẠCH ĐỊA CHỈ RAM CŨ (Cực kỳ quan trọng cho 64-bit) */
    gem_write32_ordered(base, GEM_RXQBASE, 0);
    // gem_write32_ordered(base, GEM_UPPER_RXQBASE, 0);
    gem_write32_ordered(base, GEM_TXQBASE, 0);
    // gem_write32_ordered(base, GEM_UPPER_TXQBASE, 0);
    gem_write32_ordered(base, GEM_RXQBASE_1, 0);
    gem_write32_ordered(base, GEM_TXQBASE_1, 0);

    gem_io_fence();
    PMD_LOG(INFO, "GEM hardware is now clean and ready for new configuration.");
    return 0;
}

int gem_hw_start(struct gem_adapter *adapter)
{
    volatile void *base = adapter->mmio_base;
    uint32_t ctrl, status;

    ctrl = gem_read32(base, GEM_NWCTRL);
    ctrl |= GEM_NWCTRL_RX_EN | GEM_NWCTRL_TX_EN;
    /* Ghi bit Loopback xuống thanh ghi */
    if (adapter->loopback == 1) {
        ctrl |= GEM_NWCTRL_LOCAL_LOOPBACK;
    }
    gem_write32_ordered(base, GEM_NWCTRL, ctrl);

    gem_io_mb();
    status = gem_read32_acquire(base, GEM_NWSR);

    if (!(status & GEM_NWSR_RX_ACT)) {
        PMD_LOG(ERR,
            "RX not active (NWSR=0x%x)",
            status);
        return -1;
    }

    if (!(status & GEM_NWSR_TX_ACT)) {
        PMD_LOG(ERR,
            "TX not active (NWSR=0x%x)",
            status);
        return -1;
    }

    return 0;
}

// void gem_hw_stop(struct gem_adapter *adapter)
// {
//     volatile void *base = adapter->mmio_base;
//     uint32_t ctrl, status;
//     int timeout;

//     ctrl = gem_read32(base, GEM_NWCTRL);
//     ctrl &= ~GEM_NWCTRL_RX_EN;
//     ctrl &= ~GEM_NWCTRL_TX_EN;

//     gem_write32(base, GEM_NWCTRL, ctrl);
//     (void)gem_read32(base, GEM_NWCTRL);

//     gem_io_wmb();

//     timeout = 100000;
//     do {
//         status = gem_read32(base, GEM_NWSR);
//         if (!(status & GEM_NWSR_RX_ACT) &&
//             !(status & GEM_NWSR_TX_ACT))
//             break;
//         rte_delay_us_block(1);
//     } while (--timeout);

//     if (!timeout) {
//         PMD_LOG(
//             ERR,
//             "GEM stop timeout: status=0x%08x",
//             status
//         );
//     }
//     gem_write32(base, GEM_IDR, 0xFFFFFFFF);
//     gem_io_mb();
// }

void gem_hw_stop(struct gem_adapter *adapter)
{
    volatile void *base = adapter->mmio_base;
    // uint32_t ctrl, status;
    // int timeout;

    PMD_LOG(INFO, "Bypassing Hardware Stop. Registers remain untouched.");

    /* --- COMMENT TOÀN BỘ ĐOẠN NÀY LẠI ---
    ctrl = gem_read32(base, GEM_NWCTRL);
    ctrl &= ~GEM_NWCTRL_RX_EN;
    ctrl &= ~GEM_NWCTRL_TX_EN;

    gem_write32(base, GEM_NWCTRL, ctrl);
    (void)gem_read32(base, GEM_NWCTRL);

    gem_io_wmb();

    timeout = 100000;
    do {
        status = gem_read32(base, GEM_NWSR);
        if (!(status & GEM_NWSR_RX_ACT) &&
            !(status & GEM_NWSR_TX_ACT))
            break;
        rte_delay_us_block(1);
    } while (--timeout);

    if (!timeout) {
        PMD_LOG(ERR, "GEM stop timeout: status=0x%08x", status);
    }
    gem_write32(base, GEM_IDR, 0xFFFFFFFF);
    gem_io_mb();
    -------------------------------------- */
}
