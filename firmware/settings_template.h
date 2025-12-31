/*****************************************************************************************************
  LoRa Mesh Network Settings for SX1262 - Ra01S LIBRARY VERSION  
  ⚠️ IMPORTANT: Copy this file to settings.h and configure for your setup
*******************************************************************************************************/
#ifndef SETTINGS_H
#define SETTINGS_H

// Include Ra01S library for constants
#include "Ra01S.h"

// ============= DEBUG MODE CONTROL (SINGLE POINT) =============
// Simplified debug system - only 3 modes:
//   0 = OFF        → Production (no serial output)
//   1 = GATEWAY    → Gateway DATA logs (for gateway_data_analysis.py)
//   2 = WIFI       → WiFi event monitoring (for wifi_monitor_control.py)
//                    Use this mode for PDR & Latency topology visualization

#define DEBUG_MODE_OFF 0            // No debug output (production)
#define DEBUG_MODE_GATEWAY_ONLY 1   // Gateway: DATA logs for PDR/Latency analysis
#define DEBUG_MODE_WIFI_MONITOR 2   // All nodes: Send events via WiFi (remote monitoring)

#define DEBUG_MODE DEBUG_MODE_OFF  // ← Change this (0/1/2)

// ============= NODE CONFIGURATION =============
#define DEVICE_ID 1              // ⚠️ CHANGE THIS: Unique ID for each node (1-255)
#define IS_REFERENCE 0           // 1 for reference node, 0 for regular node
#define FIX_SLOT 1               // 1 to use fixed slot, 0 for auto-assign
#define SLOT_DEVICE 1            // Slot number if FIX_SLOT = 1

// ============= HARDWARE PIN DEFINITIONS =============
#define I2C_SDA 16
#define I2C_SCL 17

// LoRa SX1262 pins
#define LORA_PIN_RESET 4
#define LORA_PIN_DIO_1 21
#define LORA_PIN_BUSY 22
#define LORA_PIN_NSS 5
#define LORA_PIN_SCLK 18
#define LORA_PIN_MISO 19
#define LORA_PIN_MOSI 23
#define LORA_TXEN 26
#define LORA_RXEN 27

// Encoder pins
#define ENCODER_SW 25
#define ENCODER_A 33
#define ENCODER_B 32

// ============= SX1262 LORA PARAMETERS =============
#define RF_FREQUENCY 915000000UL
#define TX_OUTPUT_POWER -9  // in dBm

// Ra01S library LoRa parameters
// Spreading Factor: 7 (SF7)
#define LORA_SPREADING_FACTOR 7

// Bandwidth: 125kHz (use Ra01S constant SX126X_LORA_BW_125_0 = 0x04)
#define LORA_BANDWIDTH SX126X_LORA_BW_125_0

// Coding Rate: 4/5 (use Ra01S constant SX126X_LORA_CR_4_5 = 0x01)
#define LORA_CODINGRATE SX126X_LORA_CR_4_5

// Preamble Length
#define LORA_PREAMBLE_LENGTH 8

// Legacy timeout values (ms)
#define RX_TIMEOUT_VALUE 3000
#define TX_TIMEOUT_VALUE 5000

// TDMA slots
const uint8_t Nslot = 8;

// Measured timing components (microseconds)
#define TX_PREPARE_TIME_US      850     // writeBuffer + setTx (measured)
#define TX_ONAIR_TIME_US        98000   // LoRa air time (theoretical)
#define TX_CALLBACK_TIME_US     100     // Callback processing
#define TX_GUARD_TIME_US        5000    // Channel clear safety
#define TX_MODE_SWITCH_US       500     // Mode change overhead

#define RX_SETUP_TIME_US        350     // setRx() duration
#define RX_CALLBACK_TIME_US     200     // RX done callback
#define RX_PROCESS_MAX_US       2000    // processRxPacket() worst case
#define RX_MODE_SWITCH_US       350     // Mode change

#define PROC_NEIGHBOR_US        1500    // updateNeighbourStatus()
#define PROC_DISPLAY_US         30000   // updateDisplay() worst case
#define PROC_MISC_US            500     // Misc calculations

// Total measured ToA
#define MEASURED_TOA_US         (TX_PREPARE_TIME_US + TX_ONAIR_TIME_US + \
                                 TX_CALLBACK_TIME_US + TX_GUARD_TIME_US + \
                                 TX_MODE_SWITCH_US)
                                 
// Safety margin (20% for clock drift and variations)
#define TOA_SAFETY_FACTOR       1.20f
#define EFFECTIVE_TOA_US        ((uint32_t)(MEASURED_TOA_US * TOA_SAFETY_FACTOR))

// For legacy compatibility
#define CALCULATED_TOA_MS       98
#define EFFECTIVE_TOA_MS        ((EFFECTIVE_TOA_US + 500) / 1000)

// ============= PACKET STRUCTURE =============
#define FIXED_PACKET_LENGTH 48
#define MAX_NEIGHBOURS_IN_PACKET 6  // Increased from 4 to 6 for better bi-directional detection

// Data modes
#define DATA_MODE_NONE    0
#define DATA_MODE_OWN     1
#define DATA_MODE_FORWARD 2

// RSSI threshold for routing decisions
#define MIN_RSSI_THRESHOLD -100  // Prefer nodes with RSSI > -100

// ============= TDMA TIMING PARAMETERS (MICROSECONDS) =============
const uint32_t Tslot_us = 500000UL;              // 500ms per slot
const uint32_t Tprocessing_us = 500000UL;        // 500ms processing phase (extended for WiFi batch sending)
const uint32_t Tpacket_us = EFFECTIVE_TOA_US;    // Effective packet time
const uint32_t TtxDelay_us = 5000UL;             // 5ms pre-TX delay
const uint32_t TrxDelay_us = 2000UL;             // 2ms pre-RX delay

const uint32_t Tperiod_us = (uint32_t)Nslot * Tslot_us;

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  CRITICAL: DO NOT MODIFY THIS FORMULA                                    ║
// ║  slotOffset verified identical to LoRaQuake implementation                ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
const uint32_t slotOffset_us = Tslot_us - Tpacket_us - TtxDelay_us - TrxDelay_us;

// Legacy millisecond values for compatibility
const uint32_t Tslot_ms = (Tslot_us + 500) / 1000;
const uint32_t Tperiod_ms = (Tperiod_us + 500) / 1000;
const uint32_t CYCLE_DURATION_MS = Tperiod_ms;  // For neighbor timeout calculation
const uint32_t Tprocessing_ms = (Tprocessing_us + 500) / 1000;
const uint32_t Tpacket_ms = (Tpacket_us + 500) / 1000;
const uint32_t TtxDelay_ms = (TtxDelay_us + 500) / 1000;
const uint32_t TrxDelay_ms = (TrxDelay_us + 500) / 1000;
const uint32_t slotOffset_ms = (slotOffset_us + 500) / 1000;

// ============= TIMING SYNCHRONIZATION =============
struct ResponderOutput {
  uint8_t senderSlot = 255;
  bool adjustTiming = false;
};

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  CRITICAL: DO NOT MODIFY - LoRaQuake verified modulo function            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
inline int modulo(int x, int y) {
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}

inline uint32_t calcTimeoutMs(int32_t remaining_us) {
  if (remaining_us <= 0) return 0;
  int32_t capped_us = (remaining_us > (int32_t)Tslot_us) ? (int32_t)Tslot_us : remaining_us;
  uint32_t timeout_ms = (capped_us + 500) / 1000;
  return (timeout_ms > 0) ? timeout_ms : 1;
}

// ============= NETWORK PARAMETERS =============
#define RXBUFFER_SIZE FIXED_PACKET_LENGTH
#define TXBUFFER_SIZE FIXED_PACKET_LENGTH

#define DATA_FLAG_HAS_DATA   0x01
#define DATA_FLAG_IS_FORWARD 0x02
#define SENSOR_DATA_LENGTH 6  // Reduced to fit tracking data
#define MAX_TRACKING_HOPS 3   // Maximum hops to track in packet

// Timestamp precision
#define TIMESTAMP_PRECISION_US 1         // Microsecond precision
#define TIMESTAMP_SIZE_BYTES 8           // 64-bit timestamp (microseconds since epoch)

#define MAX_NEIGHBOURS 10
#define MAX_INACTIVE_CYCLES 5
#define PROBABILITY_INITIATOR 100

// ============= WIFI =============
// ⚠️ CONFIGURE THESE FOR YOUR NETWORK
#define ENABLE_WIFI 0                     // Set to 1 to enable WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"        // ⚠️ CHANGE THIS
#define WIFI_PASS "YOUR_WIFI_PASSWORD"    // ⚠️ CHANGE THIS
#define SERVER_IP "192.168.1.100"         // ⚠️ CHANGE THIS: Your server IP
#define SERVER_PORT 5000                  // ⚠️ CHANGE THIS if needed
#define WIFI_QUEUE_SIZE 32

// WiFi monitoring & control (for remote relay node testing)
#define MONITOR_UDP_PORT 5001       // UDP port for event monitoring
#define COMMAND_UDP_PORT 5002       // UDP port for receiving commands

// ============= NTP TIME SYNC =============
#define ENABLE_NTP_SYNC 1                // Enable NTP time synchronization
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define NTP_SERVER_3 "time.google.com"
#define TIMEZONE_OFFSET_SEC (7 * 3600)   // UTC+7 (WIB)
#define DST_OFFSET_SEC 0                 // No daylight saving

// Latency measurement configuration
#define ENABLE_LATENCY_CALC 1            // Enable automatic latency calculation (gateway only)
#define LATENCY_VERBOSE_LOG 0            // 1=full logs, 0=minimal logs (reduce overhead)
#define LATENCY_CACHE_SIZE 20            // TX timestamp cache size (increase if many nodes)

// Timer Interrupt precision calibration (microseconds)
// ESP32 Timer error is typically ±1μs
#define TIMER_ERROR_MARGIN_US 1          // Timer interrupt accuracy

// Time drift compensation
#define ENABLE_DRIFT_COMPENSATION 0      // Disabled (WiFi reconnect interferes with TDMA)
#define DRIFT_CHECK_INTERVAL_MS 3600000  // Re-sync NTP every 1 hour (reset drift)
#define MAX_DRIFT_PPM 100                // Limit drift to ±100 ppm (overflow protection)

// ============= DISPLAY =============
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

// Multi-page display configuration
#define DISPLAY_PAGE_INFO 0              // Node info and neighbors
#define DISPLAY_PAGE_PDR 1               // PDR statistics (gateway only)
#define DISPLAY_PAGE_WIFI 2              // WiFi info (IP address) - only in WIFI_MONITOR mode

// Page count depends on DEBUG_MODE (WiFi page only in WIFI_MONITOR mode)
#if DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
  #define DISPLAY_PAGE_COUNT 3           // INFO, PDR, WIFI
#else
  #define DISPLAY_PAGE_COUNT 2           // INFO, PDR
#endif
#define DISPLAY_UPDATE_INTERVAL_MS 500   // Non-blocking update rate

// ============= HIERARCHICAL SYNC (STRATUM) =============
// Stratum levels for gateway-referenced synchronization
#define STRATUM_GATEWAY       0    // Gateway node (IS_REFERENCE=1) - authoritative time source
#define STRATUM_DIRECT        1    // Synced directly from gateway
#define STRATUM_INDIRECT      2    // Synced from node with stratum 1 (2-hop from gateway)
#define STRATUM_LOCAL         3    // Not synced to gateway (local time only)

// Sync validation parameters
#define SYNC_VALID_CYCLES     5    // Timeout: cycles without better sync before degradation
#define STRATUM_INHERIT_DELTA 1    // When syncing from node, inherit stratum+1

// ============= DATA STRUCTURES =============
typedef union {
  float number;
  uint8_t bytes[4];
} FloatUnion;

struct NeighbourInfo {
  uint16_t id = 0;
  uint8_t slotIndex = 0;
  bool isLocalized = false;
  uint8_t hoppingDistance = 0x7F;
  uint8_t syncedCycle = 0;  // Synchronized cycle number (0 to AUTO_SEND_INTERVAL_CYCLES-1)
  
  // Cycle sequence validation (for network sync detection)
  uint8_t cycleHistory[3] = {255, 255, 255};  // Last 3 cycles received
  uint8_t cycleHistoryIdx = 0;  // Circular buffer index
  bool cyclesSequential = false;  // True if last 3 cycles are consecutive
  
  // Hierarchical sync (stratum) tracking for neighbors
  uint8_t syncStratum = STRATUM_LOCAL;  // Neighbor's last reported stratum level
  
  FloatUnion posX, posY, posZ;
  
  uint8_t numberOfNeighbours = 0;
  uint16_t neighboursId[MAX_NEIGHBOURS];
  uint8_t neighboursSlot[MAX_NEIGHBOURS];
  uint8_t neighboursHoppingDistance[MAX_NEIGHBOURS];
  bool neighboursIsLocalized[MAX_NEIGHBOURS];
  bool amIListedAsNeighbour = false;
  
  int16_t rssi = 0;
  int8_t snr = 0;
  bool isDistanceMeasured = false;
  uint8_t activityCounter = 0;
  bool isBidirectional = false;  // Bidirectional link confirmed
};

struct MyNodeInfo {
  uint16_t id = 0;
  uint8_t slotIndex = 0;
  uint8_t isLocalized = IS_REFERENCE;
  #if IS_REFERENCE == 1
    uint8_t hoppingDistance = 0x00;
  #else
    uint8_t hoppingDistance = 0x7F;
  #endif
  uint8_t syncedCycle = 0;  // Synchronized cycle number (0 to AUTO_SEND_INTERVAL_CYCLES-1)
  FloatUnion posX, posY, posZ;
  
  // Hierarchical sync (stratum) fields
  #if IS_REFERENCE == 1
    uint8_t syncStratum = STRATUM_GATEWAY;  // Gateway is always stratum 0
  #else
    uint8_t syncStratum = STRATUM_LOCAL;    // Start as unsynced (stratum 3)
  #endif
  uint16_t syncSource = 0;                  // Node ID that provided sync (0=gateway/self, else neighbor)
  uint8_t syncValidCounter = 0;             // Countdown cycles until stratum degradation
  bool syncedWithGateway = false;           // True if syncStratum < STRATUM_LOCAL
};

const uint16_t ADR_BROADCAST = 0x0000;
const uint8_t CMD_ID_AND_POS = 0x00;
const uint8_t CMD_MESSAGE = 0x01;
const uint8_t CMD_SYNC_REQUEST = 0x02;
const uint8_t CMD_SYNC_RESPONSE = 0x03;

inline int mod(int x, int y) {
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}

#endif // SETTINGS_H
