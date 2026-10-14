#include "ble.hpp"

#include <cstring>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

namespace halo::board::ble {
namespace {

constexpr const char* kTag = "ble";

// 78450000-6bb6-46e1-be3c-499c2950b6ac, with 0001 to 0006 for the six
// characteristics. NimBLE wants the bytes least significant first, which is the
// reverse of how the UUID is written down.
#define HALO_UUID(a, b)                                                                  \
    BLE_UUID128_INIT(0xac, 0xb6, 0x50, 0x29, 0x9c, 0x49, 0x3c, 0xbe, 0xe1, 0x46, 0xb6,   \
                     0x6b, (b), (a), 0x45, 0x78)

const ble_uuid128_t kSvcUuid = HALO_UUID(0x00, 0x00);
const ble_uuid128_t kAudioUuid = HALO_UUID(0x00, 0x01);
const ble_uuid128_t kAudioAckUuid = HALO_UUID(0x00, 0x02);
const ble_uuid128_t kEventsUuid = HALO_UUID(0x00, 0x03);
const ble_uuid128_t kScreenUuid = HALO_UUID(0x00, 0x04);
const ble_uuid128_t kWallpaperUuid = HALO_UUID(0x00, 0x05);
const ble_uuid128_t kSettingsUuid = HALO_UUID(0x00, 0x06);

Handlers g_handlers;
uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_audio_val_handle = 0;
uint16_t g_events_val_handle = 0;
bool g_events_subscribed = false;
uint8_t g_own_addr_type = 0;
uint8_t g_msg_id = 0;
const char* g_name = "HaloAI";

void advertise();

// Copies an incoming write into a flat buffer. mbufs can be chained, so a long
// Screen write is not necessarily contiguous.
int flatten(const os_mbuf* om, char* out, size_t out_size, size_t* out_len) {
    const uint16_t len = OS_MBUF_PKTLEN(om);
    if (len > out_size) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t copied = 0;
    const int rc = ble_hs_mbuf_to_flat(om, out, static_cast<uint16_t>(out_size), &copied);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    *out_len = copied;
    return 0;
}

// Screen messages carry pre-wrapped screen_lines, so they are the largest thing
// the phone sends. 1 KB is generous for 12 lines of 48 characters.
char g_rx[1024];

int on_write(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void* arg) {
    size_t len = 0;
    const int rc = flatten(ctxt->om, g_rx, sizeof(g_rx), &len);
    if (rc != 0) return rc;
    auto which = reinterpret_cast<uintptr_t>(arg);
    switch (which) {
        case 2:
            if (g_handlers.on_audio_ack) g_handlers.on_audio_ack(g_rx, len);
            break;
        case 4:
            if (g_handlers.on_screen) g_handlers.on_screen(g_rx, len);
            break;
        case 5:
            if (g_handlers.on_wallpaper)
                g_handlers.on_wallpaper(reinterpret_cast<const uint8_t*>(g_rx), len);
            break;
        case 6:
            if (g_handlers.on_settings) g_handlers.on_settings(g_rx, len);
            break;
        default:
            break;
    }
    return 0;
}

int access_noop(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*) { return 0; }

const ble_gatt_chr_def kChars[] = {
    {.uuid = &kAudioUuid.u,
     .access_cb = access_noop,
     .arg = reinterpret_cast<void*>(1),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_NOTIFY,
     .min_key_size = 0,
     .val_handle = &g_audio_val_handle,
     .cpfd = nullptr},
    {.uuid = &kAudioAckUuid.u,
     .access_cb = on_write,
     .arg = reinterpret_cast<void*>(2),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
     .min_key_size = 0,
     .val_handle = nullptr,
     .cpfd = nullptr},
    {.uuid = &kEventsUuid.u,
     .access_cb = access_noop,
     .arg = reinterpret_cast<void*>(3),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_NOTIFY,
     .min_key_size = 0,
     .val_handle = &g_events_val_handle,
     .cpfd = nullptr},
    {.uuid = &kScreenUuid.u,
     .access_cb = on_write,
     .arg = reinterpret_cast<void*>(4),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_WRITE,
     .min_key_size = 0,
     .val_handle = nullptr,
     .cpfd = nullptr},
    {.uuid = &kWallpaperUuid.u,
     .access_cb = on_write,
     .arg = reinterpret_cast<void*>(5),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
     .min_key_size = 0,
     .val_handle = nullptr,
     .cpfd = nullptr},
    {.uuid = &kSettingsUuid.u,
     .access_cb = on_write,
     .arg = reinterpret_cast<void*>(6),
     .descriptors = nullptr,
     .flags = BLE_GATT_CHR_F_WRITE,
     .min_key_size = 0,
     .val_handle = nullptr,
     .cpfd = nullptr},
    {0},
};

const ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &kSvcUuid.u,
     .includes = nullptr,
     .characteristics = kChars},
    {0},
};

int on_gap_event(ble_gap_event* event, void*) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_conn = event->connect.conn_handle;
                ESP_LOGI(kTag, "connected, handle %u", g_conn);
                // The central initiates the MTU exchange, and this build is
                // peripheral only, so there is nothing to do here. Our preferred
                // MTU is CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU (256); the phone asks
                // for 517 and the smaller wins. A 176 byte chunk needs 179, and
                // below that the link fails loudly rather than streaming badly.
            } else {
                ESP_LOGW(kTag, "connect failed, status %d", event->connect.status);
                advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kTag, "disconnected, reason %d", event->disconnect.reason);
            g_conn = BLE_HS_CONN_HANDLE_NONE;
            g_events_subscribed = false;
            if (g_handlers.on_disconnect) g_handlers.on_disconnect();
            advertise();
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(kTag, "MTU now %d", event->mtu.value);
            if (event->mtu.value < 179)
                ESP_LOGE(kTag, "MTU %d is below the 179 an audio chunk needs: "
                               "the link will fail loudly rather than stream badly",
                         event->mtu.value);
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            // hello goes out when Events notifications are turned on, not when
            // the link comes up. A notification sent before the phone subscribes
            // is dropped by the stack and nobody ever sees it.
            if (event->subscribe.attr_handle == g_events_val_handle) {
                g_events_subscribed = event->subscribe.cur_notify != 0;
                ESP_LOGI(kTag, "events subscribed=%d", g_events_subscribed ? 1 : 0);
                if (g_events_subscribed && g_handlers.on_events_subscribed)
                    g_handlers.on_events_subscribed();
            }
            break;

        default:
            break;
    }
    return 0;
}

void advertise() {
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(g_name);
    fields.name_len = static_cast<uint8_t>(strlen(g_name));
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) ESP_LOGE(kTag, "adv_set_fields rc=%d", rc);

    // The service UUID goes in the scan response: a 128 bit UUID plus the name
    // does not fit in the 31 byte advertising packet.
    ble_hs_adv_fields rsp{};
    rsp.uuids128 = const_cast<ble_uuid128_t*>(&kSvcUuid);
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) ESP_LOGE(kTag, "adv_rsp_set_fields rc=%d", rc);

    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, on_gap_event,
                           nullptr);
    if (rc != 0) ESP_LOGE(kTag, "adv_start rc=%d", rc);
    else ESP_LOGI(kTag, "advertising as %s", g_name);
}

void on_sync() {
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &g_own_addr_type) != 0) {
        ESP_LOGE(kTag, "no usable address");
        return;
    }
    advertise();
}

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

bool connected() { return g_conn != BLE_HS_CONN_HANDLE_NONE; }

uint16_t mtu() {
    return connected() ? ble_att_mtu(g_conn) : 0;
}

bool notify_audio(const uint8_t* data, size_t len) {
    if (!connected()) return false;
    os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
    if (om == nullptr) return false;  // out of buffers: the link is backed up
    return ble_gattc_notify_custom(g_conn, g_audio_val_handle, om) == 0;
}

bool notify_event(const char* json, size_t len) {
    if (!connected() || !g_events_subscribed) return false;
    // Every JSON write carries the 2 byte prefix, even when it fits in one
    // packet, so there is a single code path on both sides.
    const uint16_t att = ble_att_mtu(g_conn);
    const size_t room = (att > 5 ? att - 3 : 20) - 2;
    const uint8_t id = g_msg_id++;
    size_t sent = 0;
    uint8_t frag = 0;
    uint8_t packet[256];
    while (sent < len) {
        const size_t take = (len - sent) < room ? (len - sent) : room;
        const bool last = (sent + take) >= len;
        packet[0] = id;
        packet[1] = static_cast<uint8_t>((last ? 0x80 : 0) | (frag & 0x7F));
        memcpy(packet + 2, json + sent, take);
        os_mbuf* om = ble_hs_mbuf_from_flat(packet, static_cast<uint16_t>(take + 2));
        if (om == nullptr) return false;
        if (ble_gattc_notify_custom(g_conn, g_events_val_handle, om) != 0) return false;
        sent += take;
        ++frag;
    }
    return true;
}

bool start(const char* device_name, const Handlers& handlers) {
    g_handlers = handlers;
    g_name = device_name;

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = [](int reason) { ESP_LOGE(kTag, "host reset, reason %d", reason); };

    ble_svc_gap_init();
    ble_gatts_count_cfg(kServices);
    ble_gatts_add_svcs(kServices);
    ble_svc_gap_device_name_set(device_name);

    nimble_port_freertos_init(host_task);
    return true;
}

}  // namespace halo::board::ble
