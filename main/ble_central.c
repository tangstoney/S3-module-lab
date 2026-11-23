#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "config.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#ifndef CONFIG_LAB_BLE_FOB_NAME
#define CONFIG_LAB_BLE_FOB_NAME "S3-FOB"
#endif
#ifndef CONFIG_LAB_BLE_FOB_PAIR_NAME
#define CONFIG_LAB_BLE_FOB_PAIR_NAME "S3-FOB-PAIR"
#endif
#ifndef CONFIG_LAB_BLE_SMOKE_NAME
#define CONFIG_LAB_BLE_SMOKE_NAME "s3-smoke"
#endif

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <sys/queue.h>
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_central.h"
#include "fob_host.h"

/* S3-FOB GATT: 6f0b0001-... service, 6f0b0002-... EVENT, 6f0b0003-... HOST.
 * NimBLE 128-bit UUIDs are stored little-endian (reverse of the text form). */
static const ble_uuid128_t s_fob_svc_uuid = BLE_UUID128_INIT(
    0x01, 0x0b, 0x0f, 0x0b, 0x0f, 0x0b, 0x1d, 0x9a,
    0x7c, 0x4f, 0x3a, 0x5e, 0x01, 0x00, 0x0b, 0x6f);
static const ble_uuid128_t s_fob_event_uuid = BLE_UUID128_INIT(
    0x01, 0x0b, 0x0f, 0x0b, 0x0f, 0x0b, 0x1d, 0x9a,
    0x7c, 0x4f, 0x3a, 0x5e, 0x02, 0x00, 0x0b, 0x6f);
static const ble_uuid128_t s_fob_host_uuid = BLE_UUID128_INIT(
    0x01, 0x0b, 0x0f, 0x0b, 0x0f, 0x0b, 0x1d, 0x9a,
    0x7c, 0x4f, 0x3a, 0x5e, 0x03, 0x00, 0x0b, 0x6f);

/* Declared by the NimBLE store component, as in Espressif's blecent example. */
void ble_store_config_init(void);

typedef enum {
    BLE_LINK_DOWN = 0,
    BLE_LINK_CONNECTING,
    BLE_LINK_SECURING,
    BLE_LINK_DISCOVERING,
    BLE_LINK_SUBSCRIBING,
    BLE_LINK_READY,
    BLE_LINK_BACKOFF,
} ble_link_state_t;

typedef struct {
    uint16_t cccd_handle;
    uint16_t val_handle;
    uint8_t value;
} ble_sub_t;

typedef struct {
    struct ble_target *dev;
    uint32_t gen;
} ble_sub_op_t;

typedef struct ble_target {
    uint8_t id;
    const char *name;
    ble_addr_t addr;
    bool addr_valid;
    ble_link_state_t state;
    uint16_t conn_handle;
    uint16_t mtu;
    uint32_t conn_gen;
    uint16_t host_val_handle;
    uint16_t event_val_handle;
    int64_t sec_try_us;
    bool sec_tried;
    bool auto_reconnect;
    uint8_t fail_count;
    int64_t next_try_us;
    int64_t deadline_us;
    int64_t sub_retry_us;
    ble_sub_t subs[LAB_BLE_MAX_SUBS];
    ble_sub_op_t sub_op;
    uint8_t sub_count, sub_next, sub_ok, sub_fail;
    bool sub_inflight;
    uint16_t svc_count, chr_count, dsc_count;
    uint32_t notify_count, notify_dropped;
    bool log_notify;
    uint16_t last_handle;
    uint8_t last_len;
    uint8_t last_data[16];
} ble_target_t;

typedef struct {
    uint8_t dev;
    uint8_t indication;
    uint16_t handle;
    uint16_t orig_len;
    uint16_t len;
    uint32_t gen;
    uint8_t data[LAB_BLE_NOTIFY_MAX];
} ble_evt_t;

typedef enum {
    BLE_CMD_SCAN,
    BLE_CMD_CONNECT,
    BLE_CMD_DISCONNECT,
    BLE_CMD_PAIR,
} ble_cmd_op_t;

typedef struct {
    ble_cmd_op_t op;
    uint8_t dev;
    bool on;
} ble_cmd_t;

static ble_target_t s_dev[BLE_DEV_COUNT] = {
    {
        .id = BLE_DEV_FOB, .name = CONFIG_LAB_BLE_FOB_NAME,
        .conn_handle = BLE_HS_CONN_HANDLE_NONE,
        .auto_reconnect = true, .log_notify = true,
    },
    {
        .id = BLE_DEV_SMOKE, .name = CONFIG_LAB_BLE_SMOKE_NAME,
        .conn_handle = BLE_HS_CONN_HANDLE_NONE,
        .auto_reconnect = true, .log_notify = true,
    },
};

static uint8_t s_own_addr_type;
static bool s_available;
static bool s_synced;
static bool s_scanning;
static bool s_scan_allowed = true;
static bool s_fob_bonded;
static bool s_pairing_mode;
static int64_t s_pairing_deadline_us;
static ble_addr_t s_bonds[4];
static int s_bond_count;
static ble_target_t *s_connecting;
static QueueHandle_t s_evt_queue;
static struct ble_npl_callout s_maint;
static struct ble_npl_event s_cmd_evt;
static ble_cmd_t s_cmd;
static atomic_bool s_cmd_pending;

static void ble_start_scan(void);
static void ble_sub_next(ble_target_t *dev);
static void ble_maint_cb(struct ble_npl_event *ev);
static void ble_cmd_evt_cb(struct ble_npl_event *ev);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_disc_cb(const struct peer *peer, int status, void *arg);
static int ble_sub_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr, void *arg);
static void ble_on_notify(ble_target_t *dev, uint16_t handle,
                          const uint8_t *data, size_t len);

static const char *ble_state_name(ble_link_state_t state)
{
    switch (state) {
    case BLE_LINK_CONNECTING: return "连接中";
    case BLE_LINK_SECURING: return "加密中";
    case BLE_LINK_DISCOVERING: return "发现服务";
    case BLE_LINK_SUBSCRIBING: return "订阅中";
    case BLE_LINK_READY: return "已连接";
    case BLE_LINK_BACKOFF: return "重试等待";
    default: return "未连接";
    }
}

static uint32_t ble_backoff_ms(uint8_t fails)
{
    uint32_t ms = LAB_BLE_BACKOFF_MIN_MS;
    for (uint8_t i = 1; i < fails && ms < LAB_BLE_BACKOFF_MAX_MS; ++i)
        ms <<= 1;
    return ms > LAB_BLE_BACKOFF_MAX_MS ? LAB_BLE_BACKOFF_MAX_MS : ms;
}

static ble_target_t *ble_dev_by_conn(uint16_t conn_handle)
{
    for (int i = 0; i < BLE_DEV_COUNT; ++i)
        if (s_dev[i].conn_handle == conn_handle) return &s_dev[i];
    return NULL;
}

static ble_target_t *ble_dev_from_arg(void *arg, uint16_t conn_handle)
{
    for (int i = 0; i < BLE_DEV_COUNT; ++i)
        if (arg == &s_dev[i]) return &s_dev[i];
    return ble_dev_by_conn(conn_handle);
}

static bool ble_needs_scan(int64_t now)
{
    for (int i = 0; i < BLE_DEV_COUNT; ++i) {
        ble_target_t *dev = &s_dev[i];
        if (!dev->auto_reconnect) continue;
        if (dev->id == BLE_DEV_FOB && !s_pairing_mode && !s_fob_bonded)
            continue;
        if (dev->state == BLE_LINK_DOWN) return true;
        if (dev->state == BLE_LINK_BACKOFF && now >= dev->next_try_us) return true;
    }
    return false;
}

static bool ble_bond_has_addr(const ble_addr_t *addr)
{
    for (int i = 0; i < s_bond_count; ++i)
        if (ble_addr_cmp(&s_bonds[i], addr) == 0) return true;
    return false;
}

static void ble_refresh_bonds(void)
{
    s_bond_count = 0;
    if (ble_store_util_bonded_peers(s_bonds, &s_bond_count, 4) != 0)
        s_bond_count = 0;
    s_fob_bonded = s_bond_count > 0;
}

static void ble_start_scan(void)
{
    if (!s_available || !s_synced || !s_scan_allowed || s_connecting) return;
    if (ble_gap_disc_active()) {
        s_scanning = true;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!ble_needs_scan(now)) {
        if (s_scanning) {
            (void)ble_gap_disc_cancel();
            s_scanning = false;
        }
        return;
    }

    struct ble_gap_disc_params params = {0};
#ifdef CONFIG_LAB_BLE_SCAN_ACTIVE
    params.passive = 0;     /* Kconfig default y; n removes the symbol. */
#else
    params.passive = 1;
#endif
    params.filter_duplicates = 0;
    params.filter_policy = 0;
    params.limited = 0;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                          ble_gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_scanning = true;
    } else {
        s_scanning = false;
        printf("[ble] 扫描启动失败 rc=%d\n", rc);
    }
}

static void ble_maint_cb(struct ble_npl_event *ev)
{
    (void)ev;
    (void)ble_npl_callout_reset(&s_maint,
                                ble_npl_time_ms_to_ticks32(LAB_BLE_MAINT_MS));
    if (!s_available || !s_synced) return;

    int64_t now = esp_timer_get_time();
    if (s_scanning && !ble_gap_disc_active()) s_scanning = false;

    if (s_pairing_mode && now >= s_pairing_deadline_us) {
        s_pairing_mode = false;
        s_pairing_deadline_us = 0;
        printf("[ble] 配对窗口超时，退出配对模式\n");
        ble_target_t *fob = &s_dev[BLE_DEV_FOB];
        if (fob->state == BLE_LINK_SECURING ||
            fob->state == BLE_LINK_DISCOVERING ||
            fob->state == BLE_LINK_SUBSCRIBING) {
            fob->state = fob->auto_reconnect ? BLE_LINK_BACKOFF
                                             : BLE_LINK_DOWN;
            fob->next_try_us = now + LAB_BLE_BACKOFF_MIN_MS * 1000LL;
            if (fob->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                (void)ble_gap_terminate(fob->conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
        }
    }

    for (int i = 0; i < BLE_DEV_COUNT; ++i) {
        ble_target_t *dev = &s_dev[i];
        if (dev->state == BLE_LINK_SECURING) {
            if (dev->deadline_us && now >= dev->deadline_us) {
                dev->deadline_us = 0;
                printf("[ble] %s 加密/配对超时，断开重试\n", dev->name);
                if (dev->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                    (void)ble_gap_terminate(dev->conn_handle,
                                            BLE_ERR_REM_USER_CONN_TERM);
            } else if (!dev->sec_tried && now >= dev->sec_try_us) {
                dev->sec_tried = true;
                int rc = ble_gap_security_initiate(dev->conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY)
                    printf("[ble] %s 发起加密失败 rc=%d\n", dev->name, rc);
            }
        } else if ((dev->state == BLE_LINK_DISCOVERING ||
                    dev->state == BLE_LINK_SUBSCRIBING) &&
                   dev->deadline_us && now >= dev->deadline_us) {
            dev->deadline_us = 0;
            dev->sub_retry_us = INT64_MAX;
            printf("[ble] %s 服务发现/订阅超时，断开重试\n", dev->name);
            if (dev->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                (void)ble_gap_terminate(dev->conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
        } else if (dev->state == BLE_LINK_SUBSCRIBING && !dev->sub_inflight &&
                   now >= dev->sub_retry_us) {
            ble_sub_next(dev);
        }
    }

    if (s_connecting) return;
    if (ble_needs_scan(now)) {
        if (!ble_gap_disc_active()) ble_start_scan();
    } else if (s_scanning) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }
}

static void ble_sub_next(ble_target_t *dev)
{
    if (!dev || dev->state != BLE_LINK_SUBSCRIBING || dev->sub_inflight)
        return;

    for (;;) {
        if (dev->sub_next >= dev->sub_count) {
            dev->state = BLE_LINK_READY;
            dev->deadline_us = 0;
            dev->fail_count = 0;
            if (dev->id == BLE_DEV_FOB) {
                if (!s_fob_bonded) {
                    printf("[ble] S3-FOB未完成绑定，断开重试\n");
                    dev->state = BLE_LINK_BACKOFF;
                    dev->next_try_us = esp_timer_get_time() + 1000000LL;
                    if (dev->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                        (void)ble_gap_terminate(dev->conn_handle,
                                                BLE_ERR_AUTH_FAIL);
                    return;
                }
                if (s_pairing_mode) {
                    s_pairing_mode = false;
                    s_pairing_deadline_us = 0;
                    puts("[ble] S3-FOB配对成功");
                }
                fob_host_on_link_ready(dev->conn_handle,
                                       dev->host_val_handle);
            }
            printf("[ble] %s 就绪：订阅成功 %u/%u\n", dev->name,
                   (unsigned)dev->sub_ok, (unsigned)dev->sub_count);
            return;
        }

        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(dev->conn_handle, &desc) != 0) {
            printf("[ble] %s 订阅时连接已失效\n", dev->name);
            dev->state = BLE_LINK_BACKOFF;
            dev->deadline_us = 0;
            dev->sub_retry_us = INT64_MAX;
            return;
        }

        ble_sub_t *sub = &dev->subs[dev->sub_next];
        uint8_t value[2] = { sub->value, 0 };
        dev->sub_op.dev = dev;
        dev->sub_op.gen = dev->conn_gen;
        int rc = ble_gattc_write_flat(dev->conn_handle, sub->cccd_handle,
                                      value, sizeof(value), ble_sub_cb,
                                      &dev->sub_op);
        if (rc == 0) {
            dev->sub_inflight = true;
            return;
        }
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
            dev->sub_retry_us = esp_timer_get_time() + 100000;
            return;
        }
        printf("[ble] %s 订阅CCCD 0x%04X 失败 rc=%d\n", dev->name,
               (unsigned)sub->cccd_handle, rc);
        ++dev->sub_fail;
        ++dev->sub_next;
    }
}

static int ble_sub_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    ble_sub_op_t *op = arg;
    if (!op || !op->dev || !error) return 0;
    ble_target_t *dev = op->dev;
    if (op->gen != dev->conn_gen) return 0;

    /* NimBLE fails pending GATT procs before dispatching DISCONNECT. Do not
     * advance the subscription state or start another write on that link. */
    if (error->status == BLE_HS_ENOTCONN ||
        error->status == BLE_HS_ETIMEOUT)
        return 0;
    if (dev->state != BLE_LINK_SUBSCRIBING || dev->conn_handle != conn_handle)
        return 0;

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return 0;

    dev->sub_inflight = false;
    if (error->status == 0) {
        ++dev->sub_ok;
    } else {
        ++dev->sub_fail;
        printf("[ble] %s CCCD写入失败 status=%d\n", dev->name, error->status);
    }
    ++dev->sub_next;
    /* Defer the next write to the host event loop so a queued DISCONNECT
     * is handled first and the state check above stops the sequence. */
    dev->sub_retry_us = esp_timer_get_time() + 20000;
    return 0;
}

static void ble_disc_cb(const struct peer *peer, int status, void *arg)
{
    ble_target_t *dev = arg;
    if (!dev || !peer) return;
    if (dev->state != BLE_LINK_DISCOVERING ||
        dev->conn_handle != peer->conn_handle)
        return;

    if (status != 0) {
        printf("[ble] %s 服务发现失败 status=%d\n", dev->name, status);
        (void)ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    if (dev->id == BLE_DEV_FOB) {
        const struct peer_svc *svc =
            peer_svc_find_uuid(peer, &s_fob_svc_uuid.u);
        const struct peer_chr *evt =
            peer_chr_find_uuid(peer, &s_fob_svc_uuid.u, &s_fob_event_uuid.u);
        const struct peer_chr *host =
            peer_chr_find_uuid(peer, &s_fob_svc_uuid.u, &s_fob_host_uuid.u);
        const struct peer_dsc *cccd =
            peer_dsc_find_uuid(peer, &s_fob_svc_uuid.u, &s_fob_event_uuid.u,
                               BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
        if (!svc || !evt || !host || !cccd) {
            printf("[ble] S3-FOB服务/特征不完整 svc=%d evt=%d host=%d cccd=%d\n",
                   svc != NULL, evt != NULL, host != NULL, cccd != NULL);
            (void)ble_gap_terminate(peer->conn_handle,
                                    BLE_ERR_REM_USER_CONN_TERM);
            return;
        }

        dev->svc_count = 1;
        dev->chr_count = 2;
        dev->dsc_count = 1;
        dev->event_val_handle = evt->chr.val_handle;
        dev->host_val_handle = host->chr.val_handle;
        dev->sub_count = 1;
        dev->sub_next = 0;
        dev->sub_ok = 0;
        dev->sub_fail = 0;
        dev->sub_inflight = false;
        dev->sub_retry_us = 0;
        dev->subs[0].cccd_handle = cccd->dsc.handle;
        dev->subs[0].val_handle = evt->chr.val_handle;
        dev->subs[0].value = 0x01;
        dev->state = BLE_LINK_SUBSCRIBING;
        dev->deadline_us = esp_timer_get_time() +
                           LAB_BLE_SUB_TIMEOUT_MS * 1000LL;
        printf("[ble] S3-FOB服务发现完成，订阅EVENT\n");
        ble_sub_next(dev);
        return;
    }

    dev->svc_count = 0;
    dev->chr_count = 0;
    dev->dsc_count = 0;
    dev->sub_count = 0;
    dev->sub_next = 0;
    dev->sub_ok = 0;
    dev->sub_fail = 0;
    dev->sub_inflight = false;
    dev->sub_retry_us = 0;
    unsigned truncated = 0;

    struct peer_svc *svc;
    SLIST_FOREACH(svc, &peer->svcs, next) {
        ++dev->svc_count;
        struct peer_chr *chr;
        SLIST_FOREACH(chr, &svc->chrs, next) {
            ++dev->chr_count;
            struct peer_dsc *dsc;
            SLIST_FOREACH(dsc, &chr->dscs, next) ++dev->dsc_count;

            if (!(chr->chr.properties &
                  (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)))
                continue;

            SLIST_FOREACH(dsc, &chr->dscs, next) {
                if (ble_uuid_u16(&dsc->dsc.uuid.u) !=
                    BLE_GATT_DSC_CLT_CFG_UUID16)
                    continue;
                if (dev->sub_count < LAB_BLE_MAX_SUBS) {
                    uint8_t value = 0;
                    if (chr->chr.properties & BLE_GATT_CHR_PROP_NOTIFY)
                        value |= 0x01;
                    if (chr->chr.properties & BLE_GATT_CHR_PROP_INDICATE)
                        value |= 0x02;
                    dev->subs[dev->sub_count].cccd_handle = dsc->dsc.handle;
                    dev->subs[dev->sub_count].val_handle = chr->chr.val_handle;
                    dev->subs[dev->sub_count].value = value;
                    ++dev->sub_count;
                } else {
                    ++truncated;
                }
                break;
            }
        }
    }

    printf("[ble] %s 发现 svc=%u chr=%u dsc=%u，订阅%u个%s\n", dev->name,
           (unsigned)dev->svc_count, (unsigned)dev->chr_count,
           (unsigned)dev->dsc_count, (unsigned)dev->sub_count,
           truncated ? "（有截断）" : "");

    if (dev->sub_count == 0) {
        dev->state = BLE_LINK_READY;
        dev->deadline_us = 0;
        dev->fail_count = 0;
        puts("[ble] 该设备没有notify/indicate特征，仅保持连接");
        return;
    }
    dev->state = BLE_LINK_SUBSCRIBING;
    dev->deadline_us = esp_timer_get_time() + LAB_BLE_SUB_TIMEOUT_MS * 1000LL;
    ble_sub_next(dev);
}

static void ble_on_notify(ble_target_t *dev, uint16_t handle,
                          const uint8_t *data, size_t len)
{
    if (dev->id == BLE_DEV_FOB) {
        if (handle != dev->event_val_handle) {
            printf("[ble] S3-FOB未知通知handle=0x%04X\n", (unsigned)handle);
            return;
        }
        if (dev->state != BLE_LINK_SUBSCRIBING &&
            dev->state != BLE_LINK_READY)
            return;
        if (!dev->host_val_handle) {
            puts("[ble] S3-FOB未找到HOST特征，忽略通知");
            return;
        }
        /* First notify can race the CCCD write completion; the ready call is
         * idempotent, so it only sends STATE once. */
        fob_host_on_link_ready(dev->conn_handle, dev->host_val_handle);
        fob_host_on_event(dev->conn_handle, data, len);
        return;
    }
#if CONFIG_LAB_BLE_SMOKE_HOOK
    /* Bench placeholder only: byte != 0 means smoke alarm. */
    if (dev->id == BLE_DEV_SMOKE && len >= 1)
        alarm_set_smoke_alarm(data[0] != 0);
#endif
    (void)handle;
    (void)data;
    (void)len;
}

static void ble_start_discovery(ble_target_t *dev, const char *why)
{
    dev->state = BLE_LINK_DISCOVERING;
    dev->deadline_us = esp_timer_get_time() +
                       LAB_BLE_DISC_TIMEOUT_MS * 1000LL;
    printf("\n[ble] %s %s，发现服务...\n", dev->name, why);
    int rc = peer_add(dev->conn_handle);
    if (rc != 0) {
        printf("[ble] %s peer_add失败 rc=%d\n", dev->name, rc);
        (void)ble_gap_terminate(dev->conn_handle,
                                BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    rc = peer_disc_all(dev->conn_handle, ble_disc_cb, dev);
    if (rc != 0) {
        printf("[ble] %s 服务发现启动失败 rc=%d\n", dev->name, rc);
        (void)ble_gap_terminate(dev->conn_handle,
                                BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ble_start_scan();   /* Look for the other device while discovering. */
}

static void ble_evt_task(void *arg)
{
    (void)arg;
    ble_evt_t ev;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.dev >= BLE_DEV_COUNT) continue;
        ble_target_t *dev = &s_dev[ev.dev];
        if (ev.gen != dev->conn_gen) continue;  /* stale notify */
        dev->last_handle = ev.handle;
        dev->last_len = ev.len > sizeof(dev->last_data)
                        ? sizeof(dev->last_data) : (uint8_t)ev.len;
        memcpy(dev->last_data, ev.data, dev->last_len);

        if (dev->log_notify) {
            printf("\n[ble] %s %s handle=0x%04X len=%u%s\n", dev->name,
                   ev.indication ? "INDICATION" : "NOTIFY",
                   (unsigned)ev.handle, (unsigned)ev.orig_len,
                   ev.orig_len > ev.len ? "（截断）" : "");
            lab_dump(dev->name, ev.data, ev.len);
        }
        ble_on_notify(dev, ev.handle, ev.data, ev.len);
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
            event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
            return 0;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0)
            return 0;
        if (!fields.name || !fields.name_len) return 0;

        int64_t now = esp_timer_get_time();
        for (int i = 0; i < BLE_DEV_COUNT; ++i) {
            ble_target_t *dev = &s_dev[i];
            if (!dev->auto_reconnect) continue;
            if (dev->state != BLE_LINK_DOWN && dev->state != BLE_LINK_BACKOFF)
                continue;
            if (dev->state == BLE_LINK_BACKOFF && now < dev->next_try_us)
                continue;
            size_t name_len;
            const char *want = dev->name;
            if (dev->id == BLE_DEV_FOB) {
                if (s_pairing_mode) {
                    want = CONFIG_LAB_BLE_FOB_PAIR_NAME;
                } else if (s_fob_bonded &&
                           ble_bond_has_addr(&event->disc.addr)) {
                    want = CONFIG_LAB_BLE_FOB_NAME;
                } else {
                    continue;
                }
            }
            name_len = strlen(want);
            if (fields.name_len != name_len ||
                memcmp(fields.name, want, name_len) != 0)
                continue;
            if (s_connecting) return 0;

            dev->addr = event->disc.addr;
            dev->addr_valid = true;
            dev->state = BLE_LINK_CONNECTING;
            s_connecting = dev;
            printf("\n[ble] 发现 %s %s，%s...\n", dev->name,
                   addr_str(dev->addr.val),
                   (dev->id == BLE_DEV_FOB && s_pairing_mode)
                   ? "配对广播，连接中" : "连接中");
            (void)ble_gap_disc_cancel();
            s_scanning = false;
            int rc = ble_gap_connect(s_own_addr_type, &dev->addr,
                                     LAB_BLE_CONNECT_TIMEOUT_MS, NULL,
                                     ble_gap_event, dev);
            if (rc != 0) {
                printf("[ble] %s 连接请求失败 rc=%d\n", dev->name, rc);
                ++dev->fail_count;
                dev->next_try_us = now +
                    ble_backoff_ms(dev->fail_count) * 1000LL;
                dev->state = dev->auto_reconnect ? BLE_LINK_BACKOFF
                                                 : BLE_LINK_DOWN;
                s_connecting = NULL;
                ble_start_scan();
            }
            return 0;
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT: {
        ble_target_t *dev = ble_dev_from_arg(arg, event->connect.conn_handle);
        if (!dev) {
            printf("\n[ble] 连接事件无法匹配设备，status=%d\n",
                   event->connect.status);
            if (event->connect.status == 0)
                (void)ble_gap_terminate(event->connect.conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (s_connecting == dev) s_connecting = NULL;
        if (event->connect.status != 0) {
            printf("\n[ble] %s 连接失败 status=%d\n", dev->name,
                   event->connect.status);
            ++dev->fail_count;
            dev->next_try_us = esp_timer_get_time() +
                ble_backoff_ms(dev->fail_count) * 1000LL;
            dev->state = dev->auto_reconnect ? BLE_LINK_BACKOFF
                                             : BLE_LINK_DOWN;
            ble_start_scan();
            return 0;
        }

        if (!dev->auto_reconnect) {
            printf("\n[ble] %s 连接建立时已被要求断开\n", dev->name);
            dev->state = BLE_LINK_DOWN;
            (void)ble_gap_terminate(event->connect.conn_handle,
                                    BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ++dev->conn_gen;
        dev->conn_handle = event->connect.conn_handle;
        dev->mtu = 0;
        dev->sub_retry_us = 0;
        dev->host_val_handle = 0;
        dev->event_val_handle = 0;
        dev->sec_tried = false;
        dev->sec_try_us = 0;

        if (dev->id == BLE_DEV_FOB) {
            dev->state = BLE_LINK_SECURING;
            dev->sec_try_us = esp_timer_get_time() +
                              LAB_BLE_SEC_FALLBACK_MS * 1000LL;
            dev->deadline_us = esp_timer_get_time() +
                               LAB_BLE_SEC_TIMEOUT_MS * 1000LL;
            printf("\n[ble] S3-FOB已连接 conn=%u，等待加密/配对...\n",
                   (unsigned)dev->conn_handle);
            ble_start_scan();
            return 0;
        }

        ble_start_discovery(dev, "已连接");
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        ble_target_t *dev = ble_dev_from_arg(arg,
            event->disconnect.conn.conn_handle);
        if (!dev) return 0;
        bool was_ready = dev->state == BLE_LINK_READY;
        printf("\n[ble] %s 断开 reason=%d\n", dev->name,
               event->disconnect.reason);
        if (dev->id == BLE_DEV_FOB && !s_pairing_mode &&
            event->disconnect.reason == BLE_ERR_AUTH_FAIL) {
            /* The fob rejected our stale bond; remove it and require KEY3. */
            (void)ble_store_util_delete_peer(
                &event->disconnect.conn.peer_id_addr);
            ble_refresh_bonds();
            puts("[ble] S3-FOB拒绝未绑定主机：请长按KEY3重新配对");
        }
        (void)peer_delete(event->disconnect.conn.conn_handle);
        ++dev->conn_gen;
        dev->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        dev->mtu = 0;
        dev->deadline_us = 0;
        dev->sub_count = 0;
        dev->sub_next = 0;
        dev->sub_inflight = false;
        dev->sub_retry_us = 0;
        if (dev->id == BLE_DEV_FOB) {
            fob_host_on_link_down();
            dev->host_val_handle = 0;
            dev->event_val_handle = 0;
            dev->sec_tried = false;
        }
        if (s_connecting == dev) s_connecting = NULL;
        if (dev->id == BLE_DEV_FOB && s_pairing_mode) {
            /* Keep the pairing window open; the fob re-advertises PAIR. */
            dev->fail_count = 0;
            dev->next_try_us = esp_timer_get_time() + 1000000LL;
            dev->state = BLE_LINK_BACKOFF;
        } else if (dev->auto_reconnect) {
            if (dev->id == BLE_DEV_FOB && was_ready) {
                /* The fob intentionally disconnects after each command and
                 * pair-hold. Do not grow the backoff for that normal close. */
                dev->fail_count = 0;
                dev->next_try_us = esp_timer_get_time() + 300000LL;
            } else {
                ++dev->fail_count;
                dev->next_try_us = esp_timer_get_time() +
                    ble_backoff_ms(dev->fail_count) * 1000LL;
            }
            dev->state = BLE_LINK_BACKOFF;
        } else {
            dev->fail_count = 0;
            dev->next_try_us = 0;
            dev->state = BLE_LINK_DOWN;
        }
        ble_start_scan();
        return 0;
    }
    case BLE_GAP_EVENT_MTU: {
        ble_target_t *dev = ble_dev_from_arg(arg, event->mtu.conn_handle);
        if (dev) dev->mtu = event->mtu.value;
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        ble_target_t *dev = ble_dev_from_arg(arg,
                                             event->notify_rx.conn_handle);
        if (!dev) return 0;
        ++dev->notify_count;
        ble_evt_t ev = {
            .dev = dev->id,
            .indication = event->notify_rx.indication,
            .handle = event->notify_rx.attr_handle,
            .orig_len = OS_MBUF_PKTLEN(event->notify_rx.om),
            .gen = dev->conn_gen,
        };
        ev.len = ev.orig_len > LAB_BLE_NOTIFY_MAX
                 ? LAB_BLE_NOTIFY_MAX : ev.orig_len;
        (void)os_mbuf_copydata(event->notify_rx.om, 0, ev.len, ev.data);
        if (xQueueSend(s_evt_queue, &ev, 0) != pdTRUE)
            ++dev->notify_dropped;
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        ble_start_scan();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        ble_target_t *dev = ble_dev_from_arg(arg,
                                             event->enc_change.conn_handle);
        if (!dev) return 0;
        printf("\n[ble] %s 加密状态变化 status=%d\n",
               dev->name, event->enc_change.status);
        if (dev->id != BLE_DEV_FOB) return 0;

        if (event->enc_change.status != 0) {
            printf("[ble] S3-FOB加密/配对失败 status=%d\n",
                   event->enc_change.status);
            s_fob_bonded = false;
            ++dev->fail_count;
            dev->next_try_us = esp_timer_get_time() +
                ble_backoff_ms(dev->fail_count) * 1000LL;
            dev->state = dev->auto_reconnect ? BLE_LINK_BACKOFF
                                             : BLE_LINK_DOWN;
            dev->deadline_us = 0;
            if (!s_pairing_mode)
                puts("[ble] 绑定失效或遥控器已解绑：长按KEY3重新配对");
            (void)ble_gap_terminate(event->enc_change.conn_handle,
                                    BLE_ERR_AUTH_FAIL);
            ble_start_scan();
            return 0;
        }

        struct ble_gap_conn_desc desc;
        bool bonded = false;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
            bonded = desc.sec_state.bonded;
        if (bonded) ble_refresh_bonds();
        else s_fob_bonded = false;
        printf("[ble] S3-FOB加密完成%s\n",
               bonded ? "，已绑定" : "（未绑定，正常模式将不自动连接）");
        if (dev->state == BLE_LINK_SECURING)
            ble_start_discovery(dev, "加密完成");
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ble_target_t *dev = ble_dev_from_arg(arg,
            event->repeat_pairing.conn_handle);
        if (!dev || dev->id != BLE_DEV_FOB) return 0;
        if (!s_pairing_mode) {
            puts("[ble] 非配对模式收到重复配对请求，忽略");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void ble_on_reset(int reason)
{
    printf("\n[ble] NimBLE主机复位 reason=%d\n", reason);
    s_synced = false;
    s_scanning = false;
    s_connecting = NULL;
    fob_host_on_link_down();
    for (int i = 0; i < BLE_DEV_COUNT; ++i) {
        ble_target_t *dev = &s_dev[i];
        ++dev->conn_gen;
        dev->state = BLE_LINK_DOWN;
        dev->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        dev->mtu = 0;
        dev->deadline_us = 0;
        dev->sub_count = 0;
        dev->sub_next = 0;
        dev->sub_inflight = false;
        dev->sub_retry_us = 0;
        dev->host_val_handle = 0;
        dev->event_val_handle = 0;
        dev->sec_tried = false;
    }
}

static void ble_on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        puts("[ble] 没有可用BLE地址");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        puts("[ble] 地址类型推断失败");
        return;
    }
    s_synced = true;
    s_available = true;
    puts("[ble] 主机同步完成，开始按名称扫描");
    (void)ble_npl_callout_reset(&s_maint,
                                ble_npl_time_ms_to_ticks32(LAB_BLE_MAINT_MS));
    ble_start_scan();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int ble_fob_write_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error && error->status != 0)
        printf("[fob] HOST写失败 status=%d\n", error->status);
    return 0;
}

static void ble_fob_tx(uint16_t conn_handle, uint16_t attr_handle,
                       const uint8_t *data, size_t len)
{
    if (!data || !len || !attr_handle ||
        conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return;
    int rc = ble_gattc_write_flat(conn_handle, attr_handle, data,
                                  (uint16_t)len, ble_fob_write_cb, NULL);
    if (rc != 0)
        printf("[fob] HOST写请求失败 rc=%d\n", rc);
}

esp_err_t ble_central_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        printf("[ble] NVS初始化失败 %s\n", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        printf("[ble] nimble_port_init失败 %s\n", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID;

    int rc;
#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    rc = peer_init(BLE_DEV_COUNT, LAB_BLE_PEER_SVCS, LAB_BLE_PEER_SVCS,
                   LAB_BLE_PEER_CHRS, LAB_BLE_PEER_DSCS);
#else
    rc = peer_init(BLE_DEV_COUNT, LAB_BLE_PEER_SVCS,
                   LAB_BLE_PEER_CHRS, LAB_BLE_PEER_DSCS);
#endif
    if (rc != 0) {
        printf("[ble] peer_init失败 rc=%d\n", rc);
        return ESP_FAIL;
    }

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    if (ble_svc_gap_device_name_set("s3-hub") != 0)
        puts("[ble] 设置GAP名称失败");
#endif
    ble_store_config_init();

    fob_host_init();
    fob_host_set_tx(ble_fob_tx);
    ble_refresh_bonds();
    printf("[ble] 已保存绑定数=%d，FOB自动连接=%s\n", s_bond_count,
           s_fob_bonded ? "允许" : "需KEY3配对");

    s_evt_queue = xQueueCreate(LAB_BLE_EVT_QUEUE_DEPTH, sizeof(ble_evt_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ble_evt_task, "ble_evt", LAB_BLE_TASK_STACK_BYTES, NULL,
                    LAB_BLE_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ble_npl_event_init(&s_cmd_evt, ble_cmd_evt_cb, NULL);
    if (ble_npl_callout_init(&s_maint, nimble_port_get_dflt_eventq(),
                             ble_maint_cb, NULL) != 0) {
        puts("[ble] 维护定时器初始化失败");
        return ESP_FAIL;
    }

    s_available = true;
    nimble_port_freertos_init(ble_host_task);
    printf("[ble] NimBLE Central已启动：%s / %s\n",
           s_dev[0].name, s_dev[1].name);
    return ESP_OK;
}

bool ble_available(void)
{
    return s_available;
}

void ble_status(void)
{
    printf("BLE %s，同步=%s，扫描=%s，允许扫描=%s，配对模式=%s，FOB绑定=%s(%d)\n",
           s_available ? "已初始化" : "未初始化",
           s_synced ? "是" : "否", s_scanning ? "是" : "否",
           s_scan_allowed ? "是" : "否",
           s_pairing_mode ? "是" : "否", s_fob_bonded ? "是" : "否",
           s_bond_count);
    for (int i = 0; i < BLE_DEV_COUNT; ++i) {
        ble_target_t *dev = &s_dev[i];
        printf("  %s: %s", dev->name, ble_state_name(dev->state));
        if (dev->conn_handle != BLE_HS_CONN_HANDLE_NONE)
            printf(" conn=%u mtu=%u", (unsigned)dev->conn_handle,
                   (unsigned)dev->mtu);
        if (dev->id == BLE_DEV_FOB)
            printf(" evt=0x%04X host=0x%04X",
                   (unsigned)dev->event_val_handle,
                   (unsigned)dev->host_val_handle);
        if (dev->addr_valid)
            printf(" addr=%s", addr_str(dev->addr.val));
        printf(" svc=%u chr=%u dsc=%u sub=%u/%u ok=%u fail=%u\n",
               (unsigned)dev->svc_count, (unsigned)dev->chr_count,
               (unsigned)dev->dsc_count, (unsigned)dev->sub_count,
               (unsigned)dev->sub_next, (unsigned)dev->sub_ok,
               (unsigned)dev->sub_fail);
        printf("    notify=%u drop=%u auto=%s log=%s",
               (unsigned)dev->notify_count, (unsigned)dev->notify_dropped,
               dev->auto_reconnect ? "on" : "off",
               dev->log_notify ? "on" : "off");
        if (dev->last_len) {
            printf(" last=0x%04X:", (unsigned)dev->last_handle);
            for (uint8_t n = 0; n < dev->last_len; ++n)
                printf(" %02X", (unsigned)dev->last_data[n]);
        }
        putchar('\n');
    }
}

static bool ble_cmd_has_dev(uint8_t sel, int i)
{
    return sel == BLE_DEV_ALL || sel == (uint8_t)i;
}

static void ble_cmd_evt_cb(struct ble_npl_event *ev)
{
    (void)ev;
    ble_cmd_t cmd = s_cmd;
    atomic_store(&s_cmd_pending, false);
    if (!s_available) return;

    switch (cmd.op) {
    case BLE_CMD_SCAN:
        s_scan_allowed = cmd.on;
        if (!cmd.on) {
            if (s_scanning) (void)ble_gap_disc_cancel();
            s_scanning = false;
        } else {
            ble_start_scan();
        }
        break;
    case BLE_CMD_CONNECT:
        for (int i = 0; i < BLE_DEV_COUNT; ++i) {
            if (!ble_cmd_has_dev(cmd.dev, i)) continue;
            ble_target_t *dev = &s_dev[i];
            dev->auto_reconnect = true;
            dev->fail_count = 0;
            dev->next_try_us = 0;
            if (dev->state == BLE_LINK_BACKOFF) dev->state = BLE_LINK_DOWN;
            if (dev->id == BLE_DEV_FOB && !s_fob_bonded && !s_pairing_mode)
                puts("[ble] S3-FOB未绑定：先长按KEY3或执行ble pair进入配对");
        }
        s_scan_allowed = true;
        ble_start_scan();
        break;
    case BLE_CMD_DISCONNECT:
        for (int i = 0; i < BLE_DEV_COUNT; ++i) {
            if (!ble_cmd_has_dev(cmd.dev, i)) continue;
            ble_target_t *dev = &s_dev[i];
            dev->auto_reconnect = false;
            if (dev->state == BLE_LINK_CONNECTING)
                (void)ble_gap_conn_cancel();
            if (dev->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                (void)ble_gap_terminate(dev->conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
            if (dev->state == BLE_LINK_BACKOFF)
                dev->state = BLE_LINK_DOWN;
        }
        ble_start_scan();
        break;
    case BLE_CMD_PAIR: {
        ble_target_t *fob = &s_dev[BLE_DEV_FOB];
        s_pairing_mode = !s_pairing_mode;
        if (s_pairing_mode) {
            s_pairing_deadline_us = esp_timer_get_time() +
                                    LAB_BLE_PAIR_WINDOW_MS * 1000LL;
            s_scan_allowed = true;
            fob->auto_reconnect = true;
            fob->fail_count = 0;
            fob->next_try_us = 0;
            if (fob->state == BLE_LINK_CONNECTING)
                (void)ble_gap_conn_cancel();
            if (fob->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                (void)ble_gap_terminate(fob->conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
            fob->state = BLE_LINK_DOWN;
            puts("\n[ble] 配对模式开启120s：请长按遥控器PAIR 3秒");
            puts("[ble] 若遥控器已绑定，先长按遥控器PAIR 10秒解绑");
        } else {
            puts("\n[ble] 配对模式取消");
            if (fob->state == BLE_LINK_SECURING ||
                fob->state == BLE_LINK_DISCOVERING ||
                fob->state == BLE_LINK_SUBSCRIBING) {
                if (fob->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                    (void)ble_gap_terminate(fob->conn_handle,
                                            BLE_ERR_REM_USER_CONN_TERM);
                fob->state = fob->auto_reconnect ? BLE_LINK_BACKOFF
                                                 : BLE_LINK_DOWN;
                fob->next_try_us = esp_timer_get_time() + 1000000LL;
            }
        }
        ble_start_scan();
        break;
    }
    }
}

static esp_err_t ble_post_cmd(ble_cmd_op_t op, int dev, bool on)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_cmd_pending, &expected, true))
        return ESP_ERR_INVALID_STATE;
    s_cmd.op = op;
    s_cmd.dev = (uint8_t)dev;
    s_cmd.on = on;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_cmd_evt);
    return ESP_OK;
}

esp_err_t ble_scan_set(bool on)
{
    return ble_post_cmd(BLE_CMD_SCAN, BLE_DEV_ALL, on);
}

esp_err_t ble_connect_dev(int dev)
{
    if (dev != BLE_DEV_ALL && (dev < 0 || dev >= BLE_DEV_COUNT))
        return ESP_ERR_INVALID_ARG;
    return ble_post_cmd(BLE_CMD_CONNECT, dev, true);
}

esp_err_t ble_disconnect_dev(int dev)
{
    if (dev != BLE_DEV_ALL && (dev < 0 || dev >= BLE_DEV_COUNT))
        return ESP_ERR_INVALID_ARG;
    return ble_post_cmd(BLE_CMD_DISCONNECT, dev, false);
}

esp_err_t ble_log_set(int dev, bool on)
{
    if (dev == BLE_DEV_ALL) {
        for (int i = 0; i < BLE_DEV_COUNT; ++i) s_dev[i].log_notify = on;
        return ESP_OK;
    }
    if (dev < 0 || dev >= BLE_DEV_COUNT) return ESP_ERR_INVALID_ARG;
    s_dev[dev].log_notify = on;
    return ESP_OK;
}

esp_err_t ble_central_pairing_toggle(void)
{
    return ble_post_cmd(BLE_CMD_PAIR, BLE_DEV_FOB, true);
}

bool ble_central_pairing_active(void)
{
    return s_pairing_mode;
}

#else  /* CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED */

esp_err_t ble_central_init(void)
{
    puts("[ble] 未启用蓝牙：请在sdkconfig开启CONFIG_BT_ENABLED/CONFIG_BT_NIMBLE_ENABLED");
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_available(void)
{
    return false;
}

void ble_status(void)
{
    puts("BLE未启用（CONFIG_BT_ENABLED/CONFIG_BT_NIMBLE_ENABLED）");
}

esp_err_t ble_scan_set(bool on) { (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_connect_dev(int dev) { (void)dev; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_disconnect_dev(int dev) { (void)dev; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_log_set(int dev, bool on)
{
    (void)dev; (void)on;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_central_pairing_toggle(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_central_pairing_active(void)
{
    return false;
}

#endif
