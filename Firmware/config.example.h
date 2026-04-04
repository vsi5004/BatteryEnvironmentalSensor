#pragma once

// =============================================================================
// WiFi
// =============================================================================
#define WIFI_SSID       "your-network-name"
#define WIFI_PASSWORD   "your-password"

// Static IP — eliminates DHCP round-trip on every wake (~1s saving).
// Set WIFI_STATIC_IP to 0 to fall back to DHCP.
#define WIFI_STATIC_IP  1
#define WIFI_IP         "192.168.1.x"
#define WIFI_GATEWAY    "192.168.1.1"
#define WIFI_SUBNET     "255.255.255.0"
#define WIFI_DNS        "192.168.1.1"

// WiFi transmit power. Reduce if device is close to the router.
// Options: WIFI_POWER_19_5dBm (max) down to WIFI_POWER_2dBm.
#define WIFI_TX_POWER   WIFI_POWER_11dBm

// =============================================================================
// MQTT / Home Assistant
// =============================================================================
#define MQTT_BROKER     "192.168.1.x"   // IP of your MQTT broker
#define MQTT_PORT       1883
#define MQTT_USER       ""              // leave empty if no auth
#define MQTT_PASS       ""
#define DEVICE_ID       "envsensor_01"  // must be unique per device

// =============================================================================
// Sensor tuning
// =============================================================================
#define SCD41_TEMP_OFFSET   4.0f    // °C — adjust after calibration

// ASC periods scaled for ~155s cycle vs. the Sensirion default of 5-min intervals.
// Scale factor: (5*60)/155 ≈ 1.94×. Values must be multiples of 4.
// Recalculate if you change SLEEP_DURATION_S significantly.
#define SCD41_ASC_INITIAL_PERIOD_H   88   // default 44h  × 1.94 → 88h
#define SCD41_ASC_STANDARD_PERIOD_H  304  // default 156h × 1.94 → 303h → 304h

// =============================================================================
// Thresholds
// =============================================================================
#define CO2_ALERT_PPM   2000

// =============================================================================
// Timing
// =============================================================================
#define SLEEP_DURATION_S            155     // seconds between wake cycles
#define DISPLAY_UPDATE_INTERVAL     300     // seconds between display refreshes
#define DISPLAY_FULL_REFRESH_EVERY   10     // full refresh every N updates (~50 min); clears ghosting
#define WIFI_TIMEOUT_MS           10000     // abort WiFi if not connected in this time
#define MQTT_TIMEOUT_MS            5000

// =============================================================================
// E-paper pin assignments (right-side Feather header)
// SPI bus (SCK=GPIO36, MOSI=GPIO35) is handled automatically by GxEPD2.
// =============================================================================
#define EPD_CS    10   // D10 / GPIO10
#define EPD_DC     9   // D9  / GPIO9
#define EPD_RES    6   // D6  / GPIO6
#define EPD_BUSY   5   // D5  / GPIO5

// =============================================================================
// Debug
// =============================================================================
#define DEBUG 1
