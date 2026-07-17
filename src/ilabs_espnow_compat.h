/*
 * ilabs_espnow_compat.h
 *
 * Minimal shims that let the arduino-esp32 ESP-NOW class API compile on an
 * RP2040/RP2350 host. The upstream ESP32_NOW.h pulls in ESP-IDF headers
 * (esp_now.h, esp_wifi_types.h, esp_mac.h, esp32-hal-log.h); none of those
 * exist on the Pico, so we reproduce just the types, constants and macros
 * the public API and the official examples actually reference.
 *
 * The radio itself lives on an ESP32-C6/C3 running the iLabs AT+EN ESP-NOW
 * interpreter; this library drives it over a Serial link (see ATLink).
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

/* ---- esp_now.h constants ------------------------------------------- */

#define ESP_NOW_ETH_ALEN            6      /* MAC address length            */
#define ESP_NOW_KEY_LEN             16     /* PMK / LMK length              */

#ifndef ESP_NOW_MAX_DATA_LEN
#define ESP_NOW_MAX_DATA_LEN        250    /* ESP-NOW v1 max frame payload  */
#endif

/*
 * Total peers the iLabs AT firmware allows (see AT+ENADDPEER, "max 20").
 * The upstream default on ESP32 is also 20.
 */
#ifndef ESP_NOW_MAX_TOTAL_PEER_NUM
#define ESP_NOW_MAX_TOTAL_PEER_NUM  20
#endif

/* ---- esp_wifi_types.h: interface + PHY rate ------------------------ */

typedef enum {
  WIFI_IF_STA = 0,
  WIFI_IF_AP  = 1,
} wifi_interface_t;

/*
 * Only the symbols the API/DEFAULT_ESPNOW_RATE_CONFIG reference are needed.
 * The AT bridge does not push a per-peer PHY rate in this release (the AT
 * firmware exposes rate globally via AT+ENRATE), so these are placeholders
 * kept for source compatibility of peer subclasses that touch setRate().
 */
typedef enum {
  WIFI_PHY_MODE_LR,
  WIFI_PHY_MODE_11B,
  WIFI_PHY_MODE_11G,
  WIFI_PHY_MODE_11A,
  WIFI_PHY_MODE_HT20,
  WIFI_PHY_MODE_HT40,
  WIFI_PHY_MODE_HE20,
} wifi_phy_mode_t;

typedef enum {
  WIFI_PHY_RATE_1M_L = 0x00,
} wifi_phy_rate_t;

typedef struct {
  wifi_phy_mode_t phymode;
  wifi_phy_rate_t rate;
  bool            ersu;
  bool            dcm;
} esp_now_rate_config_t;

/* ---- esp_now.h: receive info + send status ------------------------- */

/*
 * Mirrors esp_now_recv_info_t. rx_ctrl is a wifi_pkt_rx_ctrl_t* on the
 * ESP32; unused here (RSSI arrives via the AT +ENRECV field instead), so
 * it is a void* placeholder.
 */
typedef struct {
  uint8_t *src_addr;
  uint8_t *des_addr;
  void    *rx_ctrl;
} esp_now_recv_info_t;

typedef enum {
  ESP_NOW_SEND_SUCCESS = 0,
  ESP_NOW_SEND_FAIL    = 1,
} esp_now_send_status_t;

/* ---- esp_mac.h: MAC formatting helpers ----------------------------- */

#ifndef MACSTR
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

/* ---- esp32-hal-log.h: log_x macros --------------------------------- */

/*
 * Off by default. Define ILABS_ESPNOW_LOG=<Serial-like object> at build
 * time (e.g. -DILABS_ESPNOW_LOG=Serial) to route library/example logging
 * to a debug stream. The no-op form discards all arguments, so MAC2STR(...)
 * expansions inside log_x(...) calls still compile cleanly.
 */
#ifdef ILABS_ESPNOW_LOG
#define ILABS_ESPNOW_LOGF(tag, ...)      \
  do {                                   \
    ILABS_ESPNOW_LOG.printf("[" tag "] "); \
    ILABS_ESPNOW_LOG.printf(__VA_ARGS__); \
    ILABS_ESPNOW_LOG.println();          \
  } while (0)
#define log_e(...) ILABS_ESPNOW_LOGF("E", __VA_ARGS__)
#define log_w(...) ILABS_ESPNOW_LOGF("W", __VA_ARGS__)
#define log_i(...) ILABS_ESPNOW_LOGF("I", __VA_ARGS__)
#define log_d(...) ILABS_ESPNOW_LOGF("D", __VA_ARGS__)
#define log_v(...) ILABS_ESPNOW_LOGF("V", __VA_ARGS__)
#else
#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
#define log_v(...) ((void)0)
#endif
#define log_buf_v(b, l) ((void)0)
