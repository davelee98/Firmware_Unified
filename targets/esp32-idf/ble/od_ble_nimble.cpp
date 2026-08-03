/* NimBLE C-API implementation of od_ble. See od_ble.h for the contract and the GATT layout,
 * which is a wire contract and must not drift. */

#include "od_ble.h"

#include <string.h>

/* OD_BLE_MAX_FRAME -- the declared GATT value length, enforced in od_gatt_access(). */
#include "opendisplay_protocol.h"

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
 * characteristic deliberately share this UUID -- see od_ble.h.
 *
 * Spelled ONCE, and checked against the canonical big-endian form at compile time.
 *
 * This was wrong in the first port: byte 3 read 0x9F instead of 0x5F, so the firmware
 * registered 00002446-0000-1000-8000-0080_9F_9B34FB. Nothing could detect it. The service
 * registered, reported a valid handle, advertised, and accepted connections -- every log line
 * looked healthy -- and clients simply got NotFoundError on getPrimaryService() because that
 * service genuinely was not present. A hand-transcribed byte-reversed constant has no
 * redundancy, so the assert below supplies it: the LE array reversed must equal the Bluetooth
 * Base UUID with 0x2446 in the 16-bit slot. */
/* The Bluetooth Base UUID 00000000-0000-1000-8000-00805F9B34FB, little-endian, with the
 * 16-bit slot left to the caller. This is a fixed, well-known constant -- it is the same in
 * every Bluetooth product ever shipped -- so it is written once and never edited.
 *
 * Splicing rather than transcribing is the point. The ONLY project-specific number below is
 * 0x2446, which is the number a reader can check against the wire contract; the byte-reversal
 * is done by the compiler, not by me. The reference firmware gets the same property for free
 * by passing a string to NimBLE-Arduino's parser (and NimBLE's own C API offers
 * ble_uuid_from_str() for that, at runtime cost). */
#define OD_BT_BASE_UUID_LE_TAIL  0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, \
                                 0x00, 0x10, 0x00, 0x00
#define OD_UUID128_FROM_16(u16)  OD_BT_BASE_UUID_LE_TAIL,                 \
                                 (uint8_t)((u16) & 0xFFu),                \
                                 (uint8_t)(((u16) >> 8) & 0xFFu),         \
                                 0x00, 0x00

/* The OpenDisplay GATT service/characteristic, 0x2446. NOT currently a named constant in
 * shared/protocol/opendisplay_protocol.h -- the header carries OD_LAN_TCP_PORT 2446u, which is
 * the same number for an unrelated reason (the TCP port), not this UUID. The BLE service UUID
 * is wire contract and belongs in the canonical header; adding it is blocked on the header
 * freeze, so it is flagged here rather than duplicated silently. */
#define OD_BLE_SERVICE_UUID16  0x2446u

#define OD_UUID_LE_BYTES  OD_UUID128_FROM_16(OD_BLE_SERVICE_UUID16)

static constexpr uint8_t od_uuid_le[16] = { OD_UUID_LE_BYTES };

/* 00002446-0000-1000-8000-00805F9B34FB, MSB first -- read straight off the wire contract. */
static constexpr uint8_t od_uuid_be[16] = {
    0x00, 0x00, 0x24, 0x46,  0x00, 0x00,  0x10, 0x00,
    0x80, 0x00,  0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB
};

static constexpr bool od_uuid_le_is_be_reversed()
{
    for (int i = 0; i < 16; i++) {
        if (od_uuid_le[i] != od_uuid_be[15 - i]) {
            return false;
        }
    }
    return true;
}
static_assert(od_uuid_le_is_be_reversed(),
              "the little-endian GATT UUID does not reverse to "
              "00002446-0000-1000-8000-00805F9B34FB -- clients will not find the service");

static const ble_uuid128_t od_svc_uuid = BLE_UUID128_INIT(OD_UUID_LE_BYTES);
static const ble_uuid128_t od_chr_uuid = BLE_UUID128_INIT(OD_UUID_LE_BYTES);

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
        /* Enforce the declared value length at the GATT layer, returning ATT 0x0D
         * (Invalid Attribute Value Length). The Arduino characteristic declared max_len =
         * OD_BLE_MAX_FRAME for exactly this reason -- "makes the GATT layer reject an
         * oversize write with ATT 0x0D instead of letting it reach onWrite() and be dropped
         * silently". ble_gatt_chr_def has no max_len field, so the check has to be here;
         * without it the port answered "success" to writes it then discarded. */
        if (OS_MBUF_PKTLEN(ctxt->om) > OD_BLE_MAX_FRAME) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        static uint8_t buf[OD_BLE_MAX_FRAME];
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

    /* ADVERTISEMENT = flags + NAME + MSD. This is the Arduino payload byte-for-byte:
     * updatemsdata() built setName + setFlags(0x06) + setManufacturerData and pushed the whole
     * record with setAdvertisementData(). It fits 31 bytes EXACTLY --
     *   flags 3 + name (2 + 8, "OD" + 6 hex chars) + MSD (2 + 16) = 31
     * -- which is why the name length is not free to grow. A longer device name overflows and
     * ble_gap_adv_set_fields() will reject the record; the fallback below is what catches that.
     *
     * An earlier version of this port put the name in the SCAN RESPONSE instead. That is not
     * what the fleet advertises: a passive scanner (and any host that filters on name without
     * issuing a scan request) sees an unnamed device. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    if (s_msd_len) {
        fields.mfg_data = s_msd;
        fields.mfg_data_len = s_msd_len;
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        /* Only reachable with a device name longer than the stock 8 characters. Shorten the
         * name rather than drop the MSD: the MSD is what host discovery keys on
         * (manufacturer id 9286) and it carries the live sensor payload. */
        ESP_LOGW(TAG, "adv record too large (rc=%d, name=%u B); advertising without the name",
                 rc, (unsigned)fields.name_len);
        fields.name = NULL;
        fields.name_len = 0;
        fields.name_is_complete = 0;
        rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
            return;
        }
    }

    /* SCAN RESPONSE = the 128-bit service UUID (18 B). The Arduino build had no scan response
     * at all and its advertisement had no room for the UUID either, so 0x2446 was not
     * discoverable by scanning on any shipped unit -- a client had to connect to find it.
     * Putting it here costs nothing on air (scan responses are only sent when requested) and
     * makes "filter by service UUID" work in a scanner. It is additive: no byte of the
     * advertisement above changes. */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.uuids128 = (ble_uuid128_t *)&od_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
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
