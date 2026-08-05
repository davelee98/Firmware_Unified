/* NimBLE C-API implementation of od_ble. See od_ble.h for the contract, the layering split
 * against src/ble_transport_esp32.cpp, and the GATT layout -- which is a wire contract and
 * must not drift.
 *
 * There is no application state in this file. It knows about connections, not about sessions,
 * owners or transfers; everything it learns it reports through the od_ble_evt_* hooks and
 * then forgets. The one piece of bookkeeping it does keep, s_conn_count, exists only because
 * NimBLE's C API has no equivalent of NimBLEServer::getConnectedCount().
 */

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

static const char *TAG = "od_ble";

/* od_ble.h spells the sentinel without including a NimBLE header. Keep them the same value
 * rather than trusting a comment to stay true. */
static_assert(OD_BLE_CONN_NONE == BLE_HS_CONN_HANDLE_NONE,
              "OD_BLE_CONN_NONE must equal BLE_HS_CONN_HANDLE_NONE");

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
static uint8_t  s_own_addr_type  = 0;
static bool     s_addr_resolved  = false;   /* s_own_addr_type is meaningful only after sync */
static uint16_t s_preferred_mtu  = 0;
static bool     s_inited         = false;

/* The stack's peer count. Maintained here because NimBLE's C API offers no accessor for it;
 * written only from GAP events (host task), read from the loop task, hence atomic. It is a
 * COUNT, never a test for whether one particular link is up -- the transport's instance table
 * answers that per handle. */
static volatile uint8_t s_conn_count = 0;

static char    s_name[32]  = "OpenDisplay";
static uint8_t s_msd[32]   = {0};
static uint8_t s_msd_len   = 0;

/* Whether the application WANTS to be advertising, as distinct from whether the stack
 * currently is.
 *
 * Needed because the two events race in a way the caller cannot see. Host sync is
 * asynchronous -- there is no identity address, and therefore no possible advertisement,
 * until od_on_sync() runs some milliseconds after od_ble_init() returns -- while
 * BleTransport::begin() and startAdvertising() are consecutive statements on the loop task.
 * Without this flag, whichever happened second won: a startAdvertising() that landed before
 * sync did nothing at all and returned success, and the device stayed silent until something
 * else happened to restart it.
 *
 * It also keeps stop() honest. od_ble_advertise() is re-entered from BLE_GAP_EVENT_CONNECT
 * (failed attempt) and BLE_GAP_EVENT_ADV_COMPLETE, both on the host task, so a stop requested
 * from the loop task would otherwise be undone by the next stack event -- and the deep-sleep
 * path depends on stop meaning stop. */
static bool s_adv_wanted = false;

static void od_ble_advertise(void);

/* ------------------------------------------------------------------ GATT access */

static int od_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;

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
        /* Function-static rather than a stack array: this runs on the NimBLE host task, whose
         * stack is sized by CONFIG_BT_NIMBLE_TASK_STACK_SIZE and has no room to spare for a
         * 256-byte frame buffer. Safe because NimBLE serialises host callbacks -- there is
         * never a second access in flight -- and because od_ble_evt_write() is contractually
         * required to copy before returning. */
        static uint8_t buf[OD_BLE_MAX_FRAME];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        /* The handle travels with the frame. Without it the transport cannot tell an owner's
         * write from a gatecrasher's, and the non-owner filter -- which must run HERE, before
         * the bytes reach the RX ring, because during a ~16 s refresh no loop-side decision
         * runs at all -- would have nothing to decide on. */
        od_ble_evt_write(conn_handle, buf, len);
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
        /* OD-INSTRUMENTATION 2026-08-05 -- TEMPORARY, remove once the wake-connect defect is
         * understood. Deliberately ESP_LOGW and not od_log: od_log drops records emitted from
         * non-loop tasks under load (od_log.cpp's s_dropped path), and this callback runs on
         * the NimBLE host task during a busy wake -- so an od_log line here could go missing
         * and take the evidence with it. ESP_LOG writes straight to the port.
         *
         * WHAT THIS ANSWERS. On the first connection after a deep-sleep wake, the app's
         * connect hook demonstrably does not run: od_ble_evt_connect() allocates an epoch as
         * its first statement, for admitted and refused instances alike, yet the NEXT
         * connection logged e=1 -- the first epoch of the boot. The same connection produced
         * MTU and DISCONNECT events through THIS switch, so the callback was live. Those two
         * facts do not sit together, and this line separates them: if it prints, the event
         * arrived and the CONNECT case ran; if it does not, the event never reached us. */
        ESP_LOGW(TAG, "[instr] GAP CONNECT h=%u status=%d",
                 (unsigned)event->connect.conn_handle, event->connect.status);
        if (event->connect.status == 0) {
            if (s_conn_count < 0xFF) {
                __atomic_fetch_add(&s_conn_count, (uint8_t)1, __ATOMIC_RELEASE);
            }
            if (s_preferred_mtu) {
                ble_att_set_preferred_mtu(s_preferred_mtu);
            }
            od_ble_evt_connect(event->connect.conn_handle);
        } else {
            /* The connection attempt failed, so no link exists and no hook fires. Resuming
             * advertising is stack housekeeping, not policy -- without it the device goes
             * quiet after a failed connect and only a reboot brings it back.
             *
             * OD-INSTRUMENTATION: this branch was SILENT, which is why the defect took three
             * captures to corner -- it is also a caller of od_ble_advertise(), so it is one
             * candidate source of the "ble_gap_adv_start failed: 6" seen right after a wake. */
            ESP_LOGW(TAG, "[instr] GAP CONNECT FAILED status=%d -- no link hook, re-advertising",
                     event->connect.status);
            od_ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        /* OD-INSTRUMENTATION: pairs with the CONNECT line above so handles can be matched.
         * A DISCONNECT for a handle that never produced a CONNECT line is the signature of
         * the defect. */
        ESP_LOGW(TAG, "[instr] GAP DISCONNECT h=%u reason=0x%04X",
                 (unsigned)event->disconnect.conn.conn_handle,
                 (unsigned)event->disconnect.reason);
        const uint8_t n = __atomic_load_n(&s_conn_count, __ATOMIC_ACQUIRE);
        if (n > 0) {
            __atomic_store_n(&s_conn_count, (uint8_t)(n - 1), __ATOMIC_RELEASE);
        }
        /* Full width, unmodified. NimBLE's reason spans two ranges and truncation aliases
         * them -- see od_ble_evt_disconnect()'s declaration. */
        od_ble_evt_disconnect(event->disconnect.conn.conn_handle,
                              (uint16_t)event->disconnect.reason);
        /* Advertising is NOT restarted here. BleTransport::restartsAdvertisingOnDisconnect()
         * reports false for this target precisely so the application can hold the restart off
         * while an EPD refresh is mid-flight; re-arming it from the host task would take that
         * decision away and do it on the wrong task. */
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_chr_val_handle) {
            od_ble_evt_subscribe(event->subscribe.conn_handle,
                                 event->subscribe.cur_notify != 0);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        od_ble_evt_link_negotiated(event->mtu.conn_handle, "MTU exchange");
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        /* Fires whether or not the request was granted, and it is the only point at which the
         * granted PHY is knowable -- od_ble_request_fast_link() returns long before the
         * controller has finished. The hook re-reads both directions rather than trusting the
         * event's fields, so one log line describes the whole link. */
        od_ble_evt_link_negotiated(event->phy_updated.conn_handle, "PHY update");
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
    if (!s_adv_wanted) {
        return;
    }
    if (!s_addr_resolved) {
        /* Before sync there is no identity address to advertise from, and ble_gap_adv_start()
         * would fail with a log line on every call. od_on_sync() retries once it has one --
         * which is the whole reason the request is remembered rather than acted on. */
        return;
    }

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
    /* OD-INSTRUMENTATION: stamp EVERY start, with a sequence number, so the log shows whether
     * od_gap_event was the live callback before the connection in question formed -- the
     * "event arrived before we were listening" hypothesis. rc=6 is BLE_HS_ENOMEM and is
     * EXPECTED while a client holds the single connection slot
     * (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1): connectable undirected advertising needs a free
     * connection object. Logged, not silenced, because which CALLER produced it is the open
     * question. */
    {
        static uint32_t s_adv_seq = 0;
        ESP_LOGW(TAG, "[instr] adv_start #%u rc=%d%s", (unsigned)++s_adv_seq, rc,
                 rc == BLE_HS_ENOMEM ? " (ENOMEM: connection slot in use -- expected)" : "");
    }
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

bool od_ble_init(const char *device_name)
{
    if (s_inited) {
        return true;
    }
    if (device_name && *device_name) {
        strncpy(s_name, device_name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
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
        return false;
    }

    ble_svc_gap_device_name_set(s_name);
    if (s_preferred_mtu) {
        ble_att_set_preferred_mtu(s_preferred_mtu);
    }

    nimble_port_freertos_init(od_host_task);
    s_inited = true;
    ESP_LOGI(TAG, "NimBLE up; GATT service registered, val_handle=%u",
             (unsigned)s_chr_val_handle);
    return true;
}

bool od_ble_is_ready(void)
{
    return s_inited;
}

bool od_ble_notify_handle(uint16_t conn_handle, const uint8_t *data, uint16_t len)
{
    if (!s_inited || !data || len == 0 || conn_handle == OD_BLE_CONN_NONE) {
        return false;
    }
    if (s_chr_val_handle == 0) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;   /* mbuf exhaustion -- backpressure; caller retries from its queue */
    }
    /* Copies the payload into the mbuf above before this returns, so a concurrent client
     * WRITE_NR on this shared RX/TX characteristic cannot corrupt the outgoing frame.
     * ble_gatts_notify_custom() takes ownership of the mbuf on every path, success or not --
     * do not free it here. It fails when conn_handle is not connected or has not subscribed,
     * which is what makes a stale handle a false return rather than a misdelivery. */
    return ble_gatts_notify_custom(conn_handle, s_chr_val_handle, om) == 0;
}

uint8_t od_ble_connected_count(void)
{
    return __atomic_load_n(&s_conn_count, __ATOMIC_ACQUIRE);
}

bool od_ble_disconnect(uint16_t conn_handle)
{
    if (!s_inited || conn_handle == OD_BLE_CONN_NONE) {
        return false;
    }
    const int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    /* "Already gone" is success, matching NimBLE-Arduino's NimBLEServer::disconnect(): a
     * client that left between the caller's decision and this call is a benign race, not a
     * failure to ask. Anything else is a genuine failure and the caller logs it. */
    return rc == 0 || rc == BLE_HS_ENOTCONN || rc == BLE_HS_EALREADY;
}

uint16_t od_ble_conn_interval_units(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (!s_inited || conn_handle == OD_BLE_CONN_NONE) {
        return 0;
    }
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return 0;
    }
    return desc.conn_itvl;   /* 1.25 ms units */
}

void od_ble_request_fast_link(uint16_t conn_handle)
{
    if (!s_inited || conn_handle == OD_BLE_CONN_NONE) {
        return;
    }
    /* 2 Mbps both directions. The third argument is the CODED-PHY option and applies only
     * when the coded mask is set, so 0. The peer may decline and stay at 1M -- not an error,
     * and the grant (or refusal) surfaces via BLE_GAP_EVENT_PHY_UPDATE_COMPLETE. */
    int rc = ble_gap_set_prefered_le_phy(conn_handle,
                                         BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
    if (rc != 0) {
        ESP_LOGW(TAG, "2M PHY request rejected (rc=%d, staying at 1M)", rc);
    }
    /* 251-octet Link-Layer PDUs (max DLE). Unlike NimBLE-Arduino's setDataLen(handle, octets),
     * the C API takes the on-air time too and does not derive it: 251 payload octets plus the
     * 14 bytes of LL overhead at 8 us/byte on the 1M PHY is 2120 us. Passing the range
     * maximum instead would be rejected by controllers that validate the pair. */
    const uint16_t tx_time = (uint16_t)((251 + 14) * 8);   /* 2120 us, within 0x0148..0x4290 */
    rc = ble_gap_set_data_len(conn_handle, 251, tx_time);
    if (rc != 0) {
        ESP_LOGW(TAG, "DLE 251 request rejected (rc=%d)", rc);
    }
    ESP_LOGD(TAG, "Requested fast link: 2M PHY + 251-octet DLE");
}

void od_ble_link_params(uint16_t conn_handle, uint8_t *tx_phy_out, uint8_t *rx_phy_out,
                        uint16_t *att_mtu_out, uint16_t *interval_units_out)
{
    uint8_t tx_phy = 0;
    uint8_t rx_phy = 0;
    if (s_inited && conn_handle != OD_BLE_CONN_NONE) {
        if (ble_gap_read_le_phy(conn_handle, &tx_phy, &rx_phy) != 0) {
            tx_phy = 0;
            rx_phy = 0;
        }
    }
    if (tx_phy_out) *tx_phy_out = tx_phy;
    if (rx_phy_out) *rx_phy_out = rx_phy;
    if (att_mtu_out) {
        *att_mtu_out = (s_inited && conn_handle != OD_BLE_CONN_NONE)
                           ? ble_att_mtu(conn_handle) : 0;
    }
    if (interval_units_out) *interval_units_out = od_ble_conn_interval_units(conn_handle);
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
    /* Recorded BEFORE the attempt, so a call made before host sync is honoured by od_on_sync()
     * rather than lost. This is also the only way advertising ever starts. */
    s_adv_wanted = true;
    ble_gap_adv_stop();
    od_ble_advertise();
}

void od_ble_stop_advertising(void)
{
    s_adv_wanted = false;
    ble_gap_adv_stop();
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
    s_addr_resolved  = false;
    s_inited         = false;
    s_adv_wanted     = false;
    __atomic_store_n(&s_conn_count, (uint8_t)0, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "NimBLE host stopped and controller released");
}
