/* NimBLE C-API implementation of od_ble. See od_ble.h for the contract and the GATT layout,
 * which is a wire contract and must not drift. */

#include "od_ble.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Flags the imported code owns; callbacks here only ever set them. */
extern volatile bool bleRestartAdvertisingPending;
extern volatile bool esp32BleNotifySubscribed;
extern volatile bool bleDisconnectCleanupPending;
extern volatile bool msdUpdatePending;
extern uint8_t rebootFlag;

/* The command sink in communication.cpp -- the RX half of the characteristic. Declared here
 * rather than included, so this file depends on the app only through one symbol. */
extern void od_ble_on_write(const uint8_t *data, uint16_t len);

static const char *TAG = "od_ble";

/* 00002446-0000-1000-8000-00805F9B34FB, little-endian as NimBLE wants it. Service and
 * characteristic deliberately share this UUID -- see od_ble.h. */
static const ble_uuid128_t od_svc_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x9F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x46, 0x24, 0x00, 0x00);
static const ble_uuid128_t od_chr_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x9F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x46, 0x24, 0x00, 0x00);

static uint16_t s_chr_val_handle = 0;
static uint16_t s_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type  = 0;
static bool     s_addr_resolved  = false;   /* s_own_addr_type is meaningful only after sync */
static uint16_t s_preferred_mtu  = 0;
static bool     s_inited         = false;

static char    s_name[32]  = "OpenDisplay";
static uint8_t s_msd[32]   = {0};
static uint8_t s_msd_len   = 0;

static void od_ble_advertise(void);

/* ------------------------------------------------------------------ GATT access */

static int od_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        /* om_len is the flat length of the first mbuf; a long write can span several, so
         * flatten. The old NimBLE-Arduino path used NimBLEAttValue, which did this for us --
         * and note the reason its comment gave for not converting to String: pipe-write
         * frames begin with 0x00, so any C-string conversion truncates to length 0. Same trap
         * here: this payload is binary, never a string. */
        static uint8_t buf[512];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        od_ble_on_write(buf, len);
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* The characteristic is READ-able for discovery but carries no readable state; the
         * data path is notify-only. Returning an empty value matches what the Arduino
         * characteristic did with no value set. */
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_chr_def od_chrs[] = {
    {
        .uuid       = &od_chr_uuid.u,
        .access_cb  = od_gatt_access,
        .arg        = NULL,
        .descriptors = NULL,
        /* READ | WRITE | WRITE_NO_RSP | NOTIFY -- the exact property set the Arduino code
         * declared. NimBLE creates the 0x2902 CCCD automatically for NOTIFY, which is why
         * there is no explicit descriptor here (the old code relied on the same behaviour). */
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_chr_val_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def od_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &od_svc_uuid.u,
        .includes        = NULL,
        .characteristics = od_chrs,
    },
    { 0 }
};

/* ------------------------------------------------------------------ GAP events */

static int od_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "=== BLE CLIENT CONNECTED (ESP32) ===");
            rebootFlag = 0;
            esp32BleNotifySubscribed = false;
            /* Flag-only. updatemsdata() polls I2C and mutates the shared advertisement
             * vector that loop() also drives; running it on the host task corrupts the heap. */
            msdUpdatePending = true;
            if (s_preferred_mtu) {
                ble_att_set_preferred_mtu(s_preferred_mtu);
            }
        } else {
            od_ble_advertise();   /* connection failed -- resume advertising */
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "=== BLE CLIENT DISCONNECTED (ESP32) ===");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        esp32BleNotifySubscribed = false;
        /* Flag-only for the same reason: the teardown cuts the panel rail and touches SPI,
         * which races loop()'s streaming. serviceBleDisconnectCleanup() does it. */
        bleDisconnectCleanupPending  = true;
        bleRestartAdvertisingPending = true;
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_chr_val_handle) {
            esp32BleNotifySubscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "BLE notify subscription: %s",
                     esp32BleNotifySubscribed ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "ATT MTU negotiated: %u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        od_ble_advertise();
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ advertising */

static void od_ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* The 31-byte advertisement cannot hold both: flags(3) + 128-bit UUID(18) + 16-byte
     * MSD(18) = 39. The MSD wins -- it is what host discovery keys on (manufacturer id 9286)
     * and it carries the live sensor payload; the service UUID is discoverable after connect.
     *
     * Chosen up front rather than by letting ble_gap_adv_set_fields() fail and retrying: the
     * retry path fired on EVERY advertise, logging a warning each time for a condition that
     * is fixed and known at compile time. The UUID is only included when there is no MSD. */
    if (s_msd_len) {
        fields.mfg_data = s_msd;
        fields.mfg_data_len = s_msd_len;
    } else {
        fields.uuids128 = (ble_uuid128_t *)&od_svc_uuid;
        fields.num_uuids128 = 1;
        fields.uuids128_is_complete = 1;
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    /* Name goes in the scan response, where there is room for it. */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.name = (uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof adv);
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, od_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

/* ------------------------------------------------------------------ host lifecycle */

static void od_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    /* Published to the LAN mDNS TXT record via od_ble_get_identity_addr(); a host correlating
     * BLE with LAN matches on it, so log which kind it is. */
    s_addr_resolved = true;
    ESP_LOGI(TAG, "BLE identity address type: %s",
             (s_own_addr_type == BLE_OWN_ADDR_PUBLIC) ? "public" : "static-random");
    od_ble_advertise();
}

static void od_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
}

static void od_host_task(void *param)
{
    (void)param;
    nimble_port_run();            /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ public API */

void od_ble_init(const char *device_name)
{
    if (s_inited) {
        return;
    }
    if (device_name && *device_name) {
        strncpy(s_name, device_name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }

    ble_hs_cfg.sync_cb  = od_on_sync;
    ble_hs_cfg.reset_cb = od_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(od_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(od_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed: %d", rc);
        return;
    }

    ble_svc_gap_device_name_set(s_name);
    if (s_preferred_mtu) {
        ble_att_set_preferred_mtu(s_preferred_mtu);
    }

    nimble_port_freertos_init(od_host_task);
    s_inited = true;
    ESP_LOGI(TAG, "NimBLE up; GATT service registered, val_handle=%u",
             (unsigned)s_chr_val_handle);
}

bool od_ble_notify(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) {
        return false;
    }
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !esp32BleNotifySubscribed) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;   /* mbuf exhaustion -- caller retries from its response queue */
    }
    return ble_gatts_notify_custom(s_conn_handle, s_chr_val_handle, om) == 0;
}

uint8_t od_ble_connected_count(void)
{
    return (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) ? 0 : 1;
}

bool od_ble_notify_enabled(void)
{
    return od_ble_connected_count() > 0 && esp32BleNotifySubscribed;
}

void od_ble_set_manufacturer_data(const uint8_t *msd, uint8_t len)
{
    if (!msd || len == 0 || len > sizeof(s_msd)) {
        s_msd_len = 0;
        return;
    }
    memcpy(s_msd, msd, len);
    s_msd_len = len;
}

void od_ble_restart_advertising(void)
{
    if (!s_inited) {
        return;
    }
    ble_gap_adv_stop();
    od_ble_advertise();
}

void od_ble_stop_advertising(void)
{
    ble_gap_adv_stop();
}

void od_ble_clear_handles(void)
{
    s_chr_val_handle = 0;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    esp32BleNotifySubscribed = false;
}

void od_ble_set_preferred_mtu(uint16_t mtu)
{
    s_preferred_mtu = mtu;
    if (s_inited && mtu) {
        ble_att_set_preferred_mtu(mtu);
    }
}

bool od_ble_get_identity_addr(uint8_t addr_out[6], uint8_t *addr_type_out)
{
    if (!addr_out || !s_inited || !s_addr_resolved) {
        return false;
    }
    /* s_own_addr_type is BLE_OWN_ADDR_* as ble_gap_adv_start() consumes it; ble_hs_id_copy_addr
     * wants a BLE_ADDR_* identity type. The two low values coincide (PUBLIC=0, RANDOM=1) and
     * only the RPA variants differ, which this build never selects. */
    uint8_t id_type = (s_own_addr_type == BLE_OWN_ADDR_PUBLIC) ? BLE_ADDR_PUBLIC
                                                               : BLE_ADDR_RANDOM;
    if (ble_hs_id_copy_addr(id_type, addr_out, NULL) != 0) {
        return false;
    }
    if (addr_type_out) {
        *addr_type_out = id_type;
    }
    return true;
}

void od_ble_deinit(void)
{
    if (!s_inited) {
        return;
    }
    ble_gap_adv_stop();
    /* nimble_port_stop() unblocks nimble_port_run() in the host task; nimble_port_deinit()
     * then tears the host down AND disables/releases the controller, which is the half that
     * od_ble_stop_advertising() never did and that esp_restart() does not do for us. */
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    s_chr_val_handle = 0;
    s_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
    s_addr_resolved  = false;
    s_inited         = false;
    esp32BleNotifySubscribed = false;
    ESP_LOGI(TAG, "NimBLE host stopped and controller released");
}
