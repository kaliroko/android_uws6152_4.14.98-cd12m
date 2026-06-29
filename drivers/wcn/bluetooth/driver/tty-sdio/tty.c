/*
 * Spreadtrum mtty driver with TTY + native HCI support.
 * TTY: /dev/ttyBT0  (original)
 * HCI: /dev/hci0     (new, for Android standard stack)
 *
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 * Modified: HCI registration added, TTY preserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/tty.h>
#include <linux/vt_kern.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/compiler.h>           /* 添加此行，提供 __maybe_unused */
#ifdef CONFIG_OF
#include <linux/of_device.h>
#endif
#include <linux/compat.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include <misc/marlin_platform.h>
#include "tty.h"
#include "lpm.h"
#include "rfkill.h"
#include "dump.h"
#include "alignment/sitm.h"
#include "unisoc_bt_log.h"
#include <misc/wcn_bus.h>

#ifndef MTTY_STATE_OPEN
#define MTTY_STATE_OPEN  1
#endif
#ifndef MTTY_STATE_CLOSE
#define MTTY_STATE_CLOSE 0
#endif
#ifndef MTTY_DEV_MAX_NR
#define MTTY_DEV_MAX_NR  1
#endif

static struct semaphore sem_id;

struct rx_data {
    unsigned int channel;
    struct mbuf_t *head;
    struct mbuf_t *tail;
    unsigned int num;
    struct list_head entry;
};

struct mtty_device {
    struct mtty_init_data   *pdata;
    struct tty_port         *port;
    struct tty_struct       *tty;
    struct tty_driver       *driver;
    struct platform_device  *pdev;

    atomic_t state;
    atomic_t open_count;
    struct mutex rw_mutex;
    struct list_head rx_head;
    struct work_struct bt_rx_work;
    struct workqueue_struct *bt_rx_workqueue;

    struct hci_dev *hdev;
};

typedef struct {
    unsigned long vir;
    unsigned long phy;
    int size;
} dm_t;

extern int set_power_ret;
static struct device *dm_rx_t = NULL;
unsigned long dm_rx_phy[BT_PCIE_RX_MAX_NUM];
unsigned char *(dm_rx_ptr[BT_PCIE_RX_MAX_NUM]);

struct dma_buf {
    unsigned long vir;
    unsigned long phy;
    int size;
};

struct mchn_ops_t bt_pcie_rx_ops;
struct mchn_ops_t bt_pcie_tx_ops;
struct mchn_ops_t bt_sdio_rx_ops;
struct mchn_ops_t bt_sdio_tx_ops;

static struct mtty_device *mtty_dev;
static unsigned int que_task = 1;
static int que_sche = 1;
static bool is_user_debug = false;
bt_host_data_dump *data_dump = NULL;
extern void sdiohal_dump_aon_reg(void);
struct device *ttyBT_dev = NULL;

static bool is_dumped = false;
static int wcn_hw_type = 0;

/* ---------- sysfs ---------- */
static ssize_t chipid_show(struct device *dev,
       struct device_attribute *attr, char *buf)
{
    int i = 0, id;
    const char *id_str = NULL;
    id = wcn_get_chip_type();
    id_str = wcn_get_chip_name();
    dev_unisoc_bt_info(ttyBT_dev, "%s: chipid: %d, chipid_str: %s", __func__, id, id_str);
    i = scnprintf(buf, PAGE_SIZE, "%d/", id);
    strcat(buf, id_str);
    i += scnprintf(buf + i, PAGE_SIZE - i, "%s", buf + i);
    return i;
}

static ssize_t dumpmem_store(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t count)
{
    if (buf[0] == 2) {
        dev_unisoc_bt_info(ttyBT_dev, "Set is_user_debug true!\n");
        is_user_debug = true;
        return 0;
    }
    if (is_dumped == false) {
        dev_unisoc_bt_info(ttyBT_dev, "mtty BT start dump cp mem !\n");
        mdbg_assert_interface("BT command timeout assert !!!");
        bt_host_data_printf();
        if (data_dump != NULL) {
            vfree(data_dump);
            data_dump = NULL;
        }
    } else {
        dev_unisoc_bt_info(ttyBT_dev, "mtty BT has dumped cp mem, pls restart phone!\n");
    }
    is_dumped = true;
    return 0;
}

static DEVICE_ATTR_RO(chipid);
static DEVICE_ATTR_WO(dumpmem);
static struct attribute *bluetooth_attrs[] = {
    &dev_attr_chipid.attr,
    &dev_attr_dumpmem.attr,
    NULL,
};
static struct attribute_group bluetooth_group = {
    .name = NULL,
    .attrs = bluetooth_attrs,
};

/* ---------- DMA ---------- */
int mtty_dmalloc(struct device *priv, struct dma_buf *dm, int size)
{
    struct device *dev = priv;
    if (!dev) {
        dev_unisoc_bt_err(ttyBT_dev, "%s(NULL)\n", __func__);
        return -1;
    }
    if (dma_set_mask(dev, DMA_BIT_MASK(64))) {
        dev_unisoc_bt_info(ttyBT_dev, "dma_set_mask err\n");
        if (dma_set_coherent_mask(dev, DMA_BIT_MASK(64))) {
            dev_unisoc_bt_err(ttyBT_dev, "dma_set_coherent_mask err\n");
            return -1;
        }
    }
    dm->vir = (unsigned long)dma_alloc_coherent(dev, size,
                          (dma_addr_t *)(&(dm->phy)), GFP_DMA);
    if (dm->vir == 0) {
        dev_unisoc_bt_err(ttyBT_dev, "dma_alloc_coherent err\n");
        return -1;
    }
    dm->size = size;
    memset((unsigned char *)(dm->vir), 0x56, size);
    return 0;
}

int mtty_dma_buf_alloc(int chn, int size, int num)
{
    int ret, i;
    struct dma_buf temp = {0};
    struct mbuf_t *mbuf = NULL, *head = NULL, *tail = NULL;
    dm_rx_t = &mtty_dev->pdev->dev;
    if (!dm_rx_t) {
        dev_unisoc_bt_err(ttyBT_dev, "%s:PCIE device link error\n", __func__);
        return -1;
    }
    ret = sprdwcn_bus_list_alloc(chn, &head, &tail, &num);
    if (ret != 0) return -1;
    for (i = 0, mbuf = head; i < num; i++) {
        ret = mtty_dmalloc(dm_rx_t, &temp, size);
        if (ret != 0) return -1;
        mbuf->buf = (unsigned char *)(temp.vir);
        dm_rx_ptr[i] = mbuf->buf;
        mbuf->phy = (unsigned long)(temp.phy);
        dm_rx_phy[i] = mbuf->phy;
        mbuf->len = temp.size;
        memset(mbuf->buf, 0x0, mbuf->len);
        mbuf = mbuf->next;
    }
    ret = sprdwcn_bus_push_list(chn, head, tail, num);
    return ret;
}

int mtty_dma_buf_free(int num)
{
    int loop_count = 0;
    for (; loop_count < num; loop_count++) {
        if (!dm_rx_t) {
            dev_unisoc_bt_err(ttyBT_dev, "%s: dm_rx_t or dm_rx_ptr NULL\n", __func__);
        } else {
            dma_free_coherent(dm_rx_t, BT_PCIE_RX_DMA_SIZE,
                      (void *)dm_rx_ptr[loop_count], dm_rx_phy[loop_count]);
            dm_rx_ptr[loop_count] = NULL;
        }
    }
    return 0;
}

/* ---------- workqueue ---------- */
static void mtty_rx_work_queue(struct work_struct *work)
{
    int i, ret = 0;
    struct mtty_device *mtty;
    struct rx_data *rx = NULL;
    struct sk_buff *skb;

    que_task = que_task + 1;
    if (que_task > 65530) que_task = 0;
    que_sche = que_sche - 1;

    mtty = container_of(work, struct mtty_device, bt_rx_work);
    if (unlikely(!mtty)) {
        dev_unisoc_bt_err(ttyBT_dev, "mtty_rx_task mtty is NULL\n");
        return;
    }

    if (atomic_read(&mtty->state) == MTTY_STATE_OPEN) {
        do {
            mutex_lock(&mtty->rw_mutex);
            if (list_empty_careful(&mtty->rx_head)) {
                mutex_unlock(&mtty->rw_mutex);
                break;
            }
            rx = list_first_entry_or_null(&mtty->rx_head, struct rx_data, entry);
            if (!rx) {
                mutex_unlock(&mtty->rw_mutex);
                break;
            }
            list_del(&rx->entry);
            mutex_unlock(&mtty->rw_mutex);

            /* TTY */
            if (mtty->tty && mtty->port) {
                for (i = 0; i < rx->head->len; i++) {
                    ret = tty_insert_flip_char(mtty->port,
                                *(rx->head->buf + i), TTY_NORMAL);
                    if (ret != 1) {
                        i--;
                        continue;
                    } else {
                        tty_flip_buffer_push(mtty->port);
                    }
                }
            }

            /* HCI */
            if (mtty->hdev && test_bit(HCI_RUNNING, &mtty->hdev->flags)) {
                skb = bt_skb_alloc(rx->head->len, GFP_KERNEL);
                if (skb) {
                    skb_put_data(skb, rx->head->buf, rx->head->len);
                    hci_recv_frame(mtty->hdev, skb);
                }
            }

            kfree(rx->head->buf);
            kfree(rx);
        } while (1);
    }
}

/* ---------- SDIO/PCIE 接收回调 (加上 __maybe_unused) ---------- */
static __maybe_unused int mtty_sdio_rx_cb(int chn, struct mbuf_t *head,
                                          struct mbuf_t *tail, int num)
{
    int ret = 0, block_size;
    struct rx_data *rx;

    bt_wakeup_host();
    block_size = ((head->buf[2] & 0x7F) << 9) + (head->buf[1] << 1) + (head->buf[0] >> 7);

    if (atomic_read(&mtty_dev->state) == MTTY_STATE_CLOSE) {
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -1;
    }
    if (is_user_debug) {
        bt_host_data_save((unsigned char *)head->buf + BT_SDIO_HEAD_LEN, block_size, BT_DATA_IN);
    }

    if (mtty_dev == NULL) {
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -1;
    }

    if (!mtty_dev->hdev || !test_bit(HCI_RUNNING, &mtty_dev->hdev->flags)) {
        if (!work_pending(&mtty_dev->bt_rx_work)) {
            ret = tty_insert_flip_string(mtty_dev->port,
                            (unsigned char *)head->buf + BT_SDIO_HEAD_LEN,
                            block_size);
            if (ret)
                tty_flip_buffer_push(mtty_dev->port);
            if (ret == block_size) {
                sprdwcn_bus_push_list(chn, head, tail, num);
                return 0;
            }
        }
    }

    rx = kmalloc(sizeof(struct rx_data), GFP_KERNEL);
    if (rx == NULL) {
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -ENOMEM;
    }
    rx->head = head;
    rx->tail = tail;
    rx->channel = chn;
    rx->num = num;
    rx->head->len = (block_size) - ret;
    rx->head->buf = kmalloc(rx->head->len, GFP_KERNEL);
    if (rx->head->buf == NULL) {
        kfree(rx);
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -ENOMEM;
    }
    memcpy(rx->head->buf, (unsigned char *)head->buf + BT_SDIO_HEAD_LEN + ret, rx->head->len);
    sprdwcn_bus_push_list(chn, head, tail, num);
    mutex_lock(&mtty_dev->rw_mutex);
    list_add_tail(&rx->entry, &mtty_dev->rx_head);
    mutex_unlock(&mtty_dev->rw_mutex);
    if (!work_pending(&mtty_dev->bt_rx_work))
        queue_work(mtty_dev->bt_rx_workqueue, &mtty_dev->bt_rx_work);
    return 0;
}

static __maybe_unused int mtty_pcie_rx_cb(int chn, struct mbuf_t *head,
                                          struct mbuf_t *tail, int num)
{
    int ret = 0, len_send;
    struct rx_data *rx;

    bt_wakeup_host();
    len_send = head->len;

    if (atomic_read(&mtty_dev->state) == MTTY_STATE_CLOSE) {
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -1;
    }

    if (mtty_dev == NULL) {
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -1;
    }

    if (!mtty_dev->hdev || !test_bit(HCI_RUNNING, &mtty_dev->hdev->flags)) {
        if (!work_pending(&mtty_dev->bt_rx_work)) {
            ret = tty_insert_flip_string(mtty_dev->port,
                            (unsigned char *)head->buf + BT_PCIE_HEAD_LEN,
                            len_send);
            if (ret)
                tty_flip_buffer_push(mtty_dev->port);
            if (ret == len_send) {
                sprdwcn_bus_push_list(chn, head, tail, num);
                return 0;
            }
        }
    }

    rx = kmalloc(sizeof(struct rx_data), GFP_KERNEL);
    if (rx == NULL) {
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -ENOMEM;
    }
    rx->head = head;
    rx->tail = tail;
    rx->channel = chn;
    rx->num = num;
    rx->head->len = (len_send) - ret;
    rx->head->buf = kmalloc(rx->head->len, GFP_KERNEL);
    if (rx->head->buf == NULL) {
        kfree(rx);
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -ENOMEM;
    }
    memcpy(rx->head->buf, head->buf, rx->head->len);
    sprdwcn_bus_push_list(chn, head, tail, num);
    mutex_lock(&mtty_dev->rw_mutex);
    list_add_tail(&rx->entry, &mtty_dev->rx_head);
    mutex_unlock(&mtty_dev->rw_mutex);
    if (!work_pending(&mtty_dev->bt_rx_work))
        queue_work(mtty_dev->bt_rx_workqueue, &mtty_dev->bt_rx_work);
    return 0;
}

/* ---------- 发送回调 ---------- */
static __maybe_unused int mtty_sdio_tx_cb(int chn, struct mbuf_t *head,
                                          struct mbuf_t *tail, int num)
{
    int i;
    struct mbuf_t *pos = NULL;
    pos = head;
    for (i = 0; i < num; i++, pos = pos->next) {
        kfree(pos->buf);
        pos->buf = NULL;
    }
    if ((sprdwcn_bus_list_free(chn, head, tail, num)) == 0)
        up(&sem_id);
    else
        dev_unisoc_bt_err(ttyBT_dev, "%s sprdwcn_bus_list_free() fail\n", __func__);
    return 0;
}

static __maybe_unused int mtty_pcie_tx_cb(int chn, struct mbuf_t *head,
                                          struct mbuf_t *tail, int num)
{
    int i;
    struct mbuf_t *pos = NULL;
    pos = head;
    for (i = 0; i < num; i++, pos = pos->next) {
        struct device *dm = &mtty_dev->pdev->dev;
        dma_free_coherent(dm, pos->len, (void *)pos->buf, head->phy);
        pos->buf = NULL;
    }
    if ((sprdwcn_bus_list_free(chn, head, tail, num)) == 0)
        up(&sem_id);
    else
        dev_unisoc_bt_err(ttyBT_dev, "%s sprdwcn_bus_list_free() fail\n", __func__);
    return 0;
}

/* ---------- 硬件写入 ---------- */
static int mtty_sdio_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
    int num = 1, ret;
    struct mbuf_t *tx_head = NULL, *tx_tail = NULL;
    unsigned char *block = NULL;

    if (is_user_debug)
        bt_host_data_save(buf, count, BT_DATA_OUT);

    block = kmalloc(count + BT_SDIO_HEAD_LEN, GFP_KERNEL);
    if (!block) {
        dev_unisoc_bt_err(ttyBT_dev, "%s kmalloc failed\n", __func__);
        return -ENOMEM;
    }
    memset(block, 0, count + BT_SDIO_HEAD_LEN);
    memcpy(block + BT_SDIO_HEAD_LEN, buf, count);
    down(&sem_id);
    ret = sprdwcn_bus_list_alloc(BT_SDIO_TX_CHANNEL, &tx_head, &tx_tail, &num);
    if (ret) {
        up(&sem_id);
        kfree(block);
        return -ENOMEM;
    }
    tx_head->buf = block;
    tx_head->len = count;
    tx_head->next = NULL;
    ret = sprdwcn_bus_push_list(BT_SDIO_TX_CHANNEL, tx_head, tx_tail, num);
    if (ret) {
        kfree(tx_head->buf);
        tx_head->buf = NULL;
        sprdwcn_bus_list_free(BT_SDIO_TX_CHANNEL, tx_head, tx_tail, num);
        up(&sem_id);
        return -EBUSY;
    }
    return count;
}

static int mtty_pcie_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
    int num = 1, ret;
    struct mbuf_t *tx_head = NULL, *tx_tail = NULL;

    down(&sem_id);
    if (!sprdwcn_bus_list_alloc(BT_PCIE_TX_CHANNEL, &tx_head, &tx_tail, &num)) {
        struct device *dm = &mtty_dev->pdev->dev;
        if ((ret = dma_set_mask(dm, DMA_BIT_MASK(64)))) {
            dev_unisoc_bt_err(ttyBT_dev, "dma_set_mask err ret %d\n", ret);
            if ((ret = dma_set_coherent_mask(dm, DMA_BIT_MASK(64)))) {
                up(&sem_id);
                return -ENOMEM;
            }
        }
        tx_head->buf = (unsigned char *)dma_alloc_coherent(dm, count,
                                   (dma_addr_t *)(&(tx_head->phy)), GFP_DMA);
        if (!tx_head->buf) {
            up(&sem_id);
            return -ENOMEM;
        }
        memcpy(tx_head->buf, buf, count);
        tx_head->len = count;
        tx_head->next = NULL;
        ret = sprdwcn_bus_push_list(BT_PCIE_TX_CHANNEL, tx_head, tx_tail, num);
        if (ret) {
            dma_free_coherent(dm, count, (void *)tx_head->buf, tx_head->phy);
            tx_head->buf = NULL;
            sprdwcn_bus_list_free(BT_PCIE_TX_CHANNEL, tx_head, tx_tail, num);
            up(&sem_id);
            return -EBUSY;
        }
        return count;
    } else {
        up(&sem_id);
        return -ENOMEM;
    }
}

static int mtty_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
    if (wcn_hw_type == HW_TYPE_SDIO)
        return mtty_sdio_write(tty, buf, count);
    else if (wcn_hw_type == HW_TYPE_PCIE)
        return mtty_pcie_write(tty, buf, count);
    else
        return -ENOMEM;
}

static int sdio_data_transmit(uint8_t *data, size_t count)
{
    return mtty_write(NULL, data, count);
}

static int mtty_write_plus(struct tty_struct *tty, const unsigned char *buf, int count)
{
    return sitm_write(buf, count, sdio_data_transmit);
}

static void mtty_flush_chars(struct tty_struct *tty) { }
static int mtty_write_room(struct tty_struct *tty) { return INT_MAX; }

/* 前向声明 */
static int mtty_tty_open(struct tty_struct *tty, struct file *filp);
static void mtty_tty_close(struct tty_struct *tty, struct file *filp);

static const struct tty_operations mtty_ops = {
    .open  = mtty_tty_open,
    .close = mtty_tty_close,
    .write = mtty_write_plus,
    .flush_chars = mtty_flush_chars,
    .write_room  = mtty_write_room,
};

static struct tty_port *mtty_port_init(void)
{
    struct tty_port *port = kzalloc(sizeof(struct tty_port), GFP_KERNEL);
    if (port)
        tty_port_init(port);
    return port;
}

static int mtty_tty_driver_init(struct mtty_device *device)
{
    struct tty_driver *driver;
    int ret = 0;

    device->port = mtty_port_init();
    if (!device->port) return -ENOMEM;

    driver = alloc_tty_driver(MTTY_DEV_MAX_NR);
    if (!driver) return -ENOMEM;

    driver->owner = THIS_MODULE;
    driver->driver_name = device->pdata->name;
    driver->name = device->pdata->name;
    driver->major = 0;
    driver->type = TTY_DRIVER_TYPE_SYSTEM;
    driver->subtype = SYSTEM_TYPE_TTY;
    driver->init_termios = tty_std_termios;
    driver->driver_state = (void *)device;
    device->driver = driver;
    device->driver->flags = TTY_DRIVER_REAL_RAW;
    tty_set_operations(driver, &mtty_ops);
    tty_port_link_device(device->port, driver, 0);
    ret = tty_register_driver(driver);
    if (ret) {
        put_tty_driver(driver);
        tty_port_destroy(device->port);
    }
    return ret;
}

static void mtty_tty_driver_exit(struct mtty_device *device)
{
    struct tty_driver *driver = device->driver;
    tty_unregister_driver(driver);
    put_tty_driver(driver);
    tty_port_destroy(device->port);
}

static int mtty_parse_dt(struct mtty_init_data **init, struct device *dev)
{
#ifdef CONFIG_OF
    struct device_node *np = dev->of_node;
    struct mtty_init_data *pdata = NULL;
    int ret;
    pdata = kzalloc(sizeof(struct mtty_init_data), GFP_KERNEL);
    if (!pdata) return -ENOMEM;
    ret = of_property_read_string(np, "sprd,name", (const char **)&pdata->name);
    if (ret) {
        kfree(pdata);
        return ret;
    }
    *init = pdata;
    return 0;
#else
    return -ENODEV;
#endif
}

static inline void mtty_destroy_pdata(struct mtty_init_data **init)
{
#ifdef CONFIG_OF
    kfree(*init);
    *init = NULL;
#endif
}

/* ---------- TTY open/close ---------- */
static int mtty_tty_open(struct tty_struct *tty, struct file *filp)
{
    struct mtty_device *mtty = NULL;
    struct tty_driver *driver = NULL;

    data_dump = (bt_host_data_dump *)vmalloc(sizeof(bt_host_data_dump));
    if (data_dump)
        memset(data_dump, 0, sizeof(bt_host_data_dump));

    if (tty == NULL) return -ENOMEM;
    driver = tty->driver;
    mtty = (struct mtty_device *)driver->driver_state;
    if (mtty == NULL) return -ENOMEM;

    mtty->tty = tty;
    tty->driver_data = (void *)mtty;

    if (atomic_inc_return(&mtty->open_count) == 1) {
        sitm_ini();
        if (wcn_hw_type == HW_TYPE_PCIE) {
            sprdwcn_bus_chn_init(&bt_pcie_rx_ops);
            sprdwcn_bus_chn_init(&bt_pcie_tx_ops);
            mtty_dma_buf_alloc(BT_PCIE_RX_CHANNEL, BT_PCIE_RX_DMA_SIZE, BT_PCIE_RX_MAX_NUM);
        }
        atomic_set(&mtty->state, MTTY_STATE_OPEN);
    }
    return 0;
}

static void mtty_tty_close(struct tty_struct *tty, struct file *filp)
{
    struct mtty_device *mtty = NULL;

    if (tty == NULL) return;
    mtty = (struct mtty_device *)tty->driver_data;
    if (mtty == NULL) return;

    if (atomic_dec_return(&mtty->open_count) == 0) {
        atomic_set(&mtty->state, MTTY_STATE_CLOSE);
        sitm_cleanup();
        if (wcn_hw_type == HW_TYPE_PCIE) {
            mtty_dma_buf_free(BT_PCIE_RX_MAX_NUM);
            sprdwcn_bus_chn_deinit(&bt_pcie_rx_ops);
            sprdwcn_bus_chn_deinit(&bt_pcie_tx_ops);
        }
        if (data_dump != NULL) {
            vfree(data_dump);
            data_dump = NULL;
        }
    }
    mtty->tty = NULL;
}

/* ---------- HCI 回调 ---------- */
static int mtty_hci_open(struct hci_dev *hdev)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);

    if (atomic_inc_return(&mtty->open_count) == 1) {
        sitm_ini();
        if (wcn_hw_type == HW_TYPE_PCIE) {
            sprdwcn_bus_chn_init(&bt_pcie_rx_ops);
            sprdwcn_bus_chn_init(&bt_pcie_tx_ops);
            mtty_dma_buf_alloc(BT_PCIE_RX_CHANNEL, BT_PCIE_RX_DMA_SIZE, BT_PCIE_RX_MAX_NUM);
        }
        atomic_set(&mtty->state, MTTY_STATE_OPEN);
    }
    set_bit(HCI_RUNNING, &hdev->flags);
    return 0;
}

static int mtty_hci_close(struct hci_dev *hdev)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);

    clear_bit(HCI_RUNNING, &hdev->flags);

    if (atomic_dec_return(&mtty->open_count) == 0) {
        atomic_set(&mtty->state, MTTY_STATE_CLOSE);
        sitm_cleanup();
        if (wcn_hw_type == HW_TYPE_PCIE) {
            mtty_dma_buf_free(BT_PCIE_RX_MAX_NUM);
            sprdwcn_bus_chn_deinit(&bt_pcie_rx_ops);
            sprdwcn_bus_chn_deinit(&bt_pcie_tx_ops);
        }
    }
    return 0;
}

static int mtty_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
    int ret;
    ret = sitm_write(skb->data, skb->len, sdio_data_transmit);
    if (ret >= 0)
        kfree_skb(skb);
    return ret;
}

/* ---------- 复位通知 ---------- */
static int bluetooth_reset(struct notifier_block *this, unsigned long ev, void *ptr)
{
    unsigned char reset_buf[5] = {0x04, 0xff, 0x02, 0x57, 0xa5};
    struct sk_buff *skb;

    if (mtty_dev != NULL) {
        if (!work_pending(&mtty_dev->bt_rx_work) && mtty_dev->port) {
            tty_insert_flip_string(mtty_dev->port, reset_buf, sizeof(reset_buf));
            tty_flip_buffer_push(mtty_dev->port);
        }
        if (mtty_dev->hdev && test_bit(HCI_RUNNING, &mtty_dev->hdev->flags)) {
            skb = bt_skb_alloc(sizeof(reset_buf), GFP_ATOMIC);
            if (skb) {
                skb_put_data(skb, reset_buf, sizeof(reset_buf));
                hci_recv_frame(mtty_dev->hdev, skb);
            }
        }
    }
    return NOTIFY_DONE;
}

static struct notifier_block bluetooth_reset_block = {
    .notifier_call = bluetooth_reset,
};

/* ---------- probe / remove ---------- */
static int mtty_probe(struct platform_device *pdev)
{
    struct mtty_init_data *pdata = (struct mtty_init_data *)pdev->dev.platform_data;
    struct mtty_device *mtty;
    struct hci_dev *hdev;
    int rval = 0;

    if (pdev->dev.of_node && !pdata) {
        rval = mtty_parse_dt(&pdata, &pdev->dev);
        if (rval) return rval;
    }

    mtty = kzalloc(sizeof(struct mtty_device), GFP_KERNEL);
    ttyBT_dev = &pdev->dev;
    if (mtty == NULL) {
        mtty_destroy_pdata(&pdata);
        return -ENOMEM;
    }
    mtty->pdata = pdata;

    rval = mtty_tty_driver_init(mtty);
    if (rval) {
        kfree(mtty);
        mtty_destroy_pdata(&pdata);
        return rval;
    }

    platform_set_drvdata(pdev, mtty);
    atomic_set(&mtty->state, MTTY_STATE_CLOSE);
    atomic_set(&mtty->open_count, 0);
    mutex_init(&mtty->rw_mutex);
    INIT_LIST_HEAD(&mtty->rx_head);

    mtty->bt_rx_workqueue = create_singlethread_workqueue("SPRDBT_RX_QUEUE");
    if (!mtty->bt_rx_workqueue) {
        mtty_tty_driver_exit(mtty);
        kfree(mtty->port);
        kfree(mtty);
        mtty_destroy_pdata(&pdata);
        return -ENOMEM;
    }
    INIT_WORK(&mtty->bt_rx_work, mtty_rx_work_queue);
    mtty_dev = mtty;

    if (sysfs_create_group(&pdev->dev.kobj, &bluetooth_group))
        dev_unisoc_bt_err(ttyBT_dev, "%s failed to create attributes\n", __func__);

    rfkill_bluetooth_init(pdev);
    bluesleep_init();
    atomic_notifier_chain_register(&wcn_reset_notifier_list, &bluetooth_reset_block);

    wcn_hw_type = sprdwcn_bus_get_hwintf_type();
    if (wcn_hw_type == HW_TYPE_SDIO) {
        sprdwcn_bus_chn_init(&bt_sdio_rx_ops);
        sprdwcn_bus_chn_init(&bt_sdio_tx_ops);
    }

    sema_init(&sem_id, BT_TX_POOL_SIZE - 1);

    /* HCI 注册 */
    hdev = hci_alloc_dev();
    if (hdev) {
        mtty->hdev = hdev;
        hdev->bus = HCI_UART;
        hdev->open = mtty_hci_open;
        hdev->close = mtty_hci_close;
        hdev->send = mtty_hci_send_frame;
        hci_set_drvdata(hdev, mtty);
        SET_HCIDEV_DEV(hdev, &pdev->dev);
        if (hci_register_dev(hdev) < 0) {
            hci_free_dev(hdev);
            mtty->hdev = NULL;
        }
    }

    return 0;
}

static int mtty_remove(struct platform_device *pdev)
{
    struct mtty_device *mtty = platform_get_drvdata(pdev);

    if (mtty->hdev) {
        hci_unregister_dev(mtty->hdev);
        hci_free_dev(mtty->hdev);
    }
    mtty_tty_driver_exit(mtty);
    if (wcn_hw_type == HW_TYPE_SDIO) {
        sprdwcn_bus_chn_deinit(&bt_sdio_rx_ops);
        sprdwcn_bus_chn_deinit(&bt_sdio_tx_ops);
    }
    kfree(mtty->port);
    mtty_destroy_pdata(&mtty->pdata);
    flush_workqueue(mtty->bt_rx_workqueue);
    destroy_workqueue(mtty->bt_rx_workqueue);
    kfree(mtty);
    platform_set_drvdata(pdev, NULL);
    sysfs_remove_group(&pdev->dev.kobj, &bluetooth_group);
    bluesleep_exit();
    return 0;
}

static const struct of_device_id mtty_match_table[] = {
    { .compatible = "sprd,mtty", },
    { },
};

static struct platform_driver mtty_driver = {
    .driver = {
        .owner = THIS_MODULE,
        .name = "mtty",
        .of_match_table = mtty_match_table,
    },
    .probe = mtty_probe,
    .remove = mtty_remove,
};

module_platform_driver(mtty_driver);

MODULE_AUTHOR("Unisoc wcn bt");
MODULE_DESCRIPTION("Unisoc marlin tty driver with HCI support");
MODULE_LICENSE("GPL");