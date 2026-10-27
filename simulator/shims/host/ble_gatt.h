// PC/SDL 模拟器桩：NimBLE host 的最小子集（AppleMediaClient 静态引用这些类型和函数）。
// 模拟器上永远不会真正连接 Apple 设备：所有 gattc 调用返回 ENOTCONN。
#pragma once
#include <stdint.h>
#include <stddef.h>

#define BLE_HS_EDONE 12
#define BLE_HS_ENOTCONN 2
#define BLE_HS_ATT_ERR(status) (status)

#define BLE_GATT_CHR_PROP_BROADCAST 0x01
#define BLE_GATT_CHR_PROP_READ 0x02
#define BLE_GATT_CHR_PROP_WRITE_WITHOUT_RSP 0x04
#define BLE_GATT_CHR_PROP_WRITE 0x08
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_INDICATE 0x20

#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_NOTIFY_RX 6
#define BLE_GAP_EVENT_NOTIFY_TX 7
#define BLE_GAP_EVENT_MTU 8

struct ble_uuid {
  uint8_t type;
};
struct ble_uuid16_t {
  ble_uuid u;
  uint16_t value;
};
struct ble_uuid32_t {
  ble_uuid u;
  uint32_t value;
};
struct ble_uuid128_t {
  ble_uuid u;
  uint8_t value[16];
};
union ble_uuid_any_t {
  ble_uuid u;
  ble_uuid16_t u16;
  ble_uuid32_t u32;
  ble_uuid128_t u128;
};

struct os_mbuf;
struct ble_gatt_error {
  int status;
  uint16_t att_handle;
};
struct ble_gatt_svc {
  ble_uuid_any_t uuid;
  uint16_t start_handle;
  uint16_t end_handle;
};
struct ble_gatt_chr {
  ble_uuid_any_t uuid;
  uint8_t properties;
  uint16_t def_handle;
  uint16_t val_handle;
};
struct ble_gatt_dsc {
  ble_uuid_any_t uuid;
  uint16_t handle;
};
struct ble_gatt_attr {
  os_mbuf *om;
  uint16_t handle;
  uint16_t offset;
};

struct ble_gap_event {
  int type;
  union {
    struct {
      int status;
      uint16_t conn_handle;
    } connect;
    struct {
      int reason;
      uint16_t conn_handle;
    } disconnect;
    struct {
      uint16_t conn_handle;
      uint16_t attr_handle;
      os_mbuf *om;
    } notify_rx;
  };
};

static inline int ble_att_mtu(uint16_t connHandle) { (void)connHandle; return 0; }
static inline int ble_uuid_from_str(ble_uuid_any_t *out, const char *str) { (void)out; (void)str; return -1; }
static inline int ble_uuid_cmp(const ble_uuid *a, const ble_uuid *b) { (void)a; (void)b; return 1; }
static inline uint16_t ble_uuid_u16(const ble_uuid *u) { (void)u; return 0; }
#define OS_MBUF_PKTLEN(om) ((size_t)((om) ? 0 : 0))
static inline int os_mbuf_copydata(const os_mbuf *om, uint16_t off, uint16_t len, void *dst) {
  (void)om; (void)off; (void)len; (void)dst; return -1;
}
static inline int ble_gattc_disc_all_svcs(uint16_t conn, int (*cb)(uint16_t, const ble_gatt_error *, const ble_gatt_svc *, void *), void *arg) {
  (void)conn; (void)cb; (void)arg; return BLE_HS_ENOTCONN;
}
static inline int ble_gattc_disc_all_chrs(uint16_t conn, uint16_t start, uint16_t end, int (*cb)(uint16_t, const ble_gatt_error *, const ble_gatt_chr *, void *), void *arg) {
  (void)conn; (void)start; (void)end; (void)cb; (void)arg; return BLE_HS_ENOTCONN;
}
static inline int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end, int (*cb)(uint16_t, const ble_gatt_error *, uint16_t, const ble_gatt_dsc *, void *), void *arg) {
  (void)conn; (void)start; (void)end; (void)cb; (void)arg; return BLE_HS_ENOTCONN;
}
static inline int ble_gattc_write_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len, int (*cb)(uint16_t, const ble_gatt_error *, ble_gatt_attr *, void *), void *arg) {
  (void)conn; (void)handle; (void)data; (void)len; (void)cb; (void)arg; return BLE_HS_ENOTCONN;
}
static inline int ble_gattc_read_long(uint16_t conn, uint16_t handle, uint16_t offset, int (*cb)(uint16_t, const ble_gatt_error *, ble_gatt_attr *, void *), void *arg) {
  (void)conn; (void)handle; (void)offset; (void)cb; (void)arg; return BLE_HS_ENOTCONN;
}
