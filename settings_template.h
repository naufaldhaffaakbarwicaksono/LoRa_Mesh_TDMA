/*****************************************************************************************************
  LoRa Mesh Network Settings for SX1262 - Ra01S LIBRARY VERSION
  Migrated from SX1262_TimingControl to Ra01S (SX126x class) library
  
  All timing recalculated based on measured SX1262 operation durations
  
  ⚠️ IMPORTANT: Copy this file to settings.h and configure for your setup
*******************************************************************************************************/
#ifndef SETTINGS_H
#define SETTINGS_H

// Include Ra01S library for constants
#include "Ra01S.h"

// ============= DEBUG MODE =============
//#define VERBOSE  // Uncomment for verbose serial output

// Enable detailed timing logs from radio library
#define ENABLE_RADIO_DEBUG 0  // 1 = verbose radio timing, 0 = quiet

// ============= NODE CONFIGURATION =============
#define DEVICE_ID 1              // ⚠️ CHANGE THIS: Unique ID for each node (1-255)
#define IS_REFERENCE 0           // 1 for reference node, 0 for regular node
#define FIX_SLOT 0               // 1 to use fixed slot, 0 for auto-assign
#define SLOT_DEVICE 0            // Slot number if FIX_SLOT = 1

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
#define TX_OUTPUT_POWER 4

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
const uint8_t Nslot = 13;

// ============= TIMING ANALYSIS & MEASURED VALUES =============
//
// MEASURED OPERATION DURATIONS (SX1262 on ESP32):
// ──────────────────────────────────────────────────────────────
// writeBuffer(48 bytes):      400-600 μs (avg: 500 μs)
// setTx():                    200-500 μs (avg: 350 μs)
// LoRa air time (48B SF7):    98,000 μs (theoretical)
// TX callback + cleanup:      50-200 μs (avg: 100 μs)
// setRx():                    200-500 μs (avg: 350 μs)
// RX callback + processing:   100-300 μs (avg: 200 μs)
// ──────────────────────────────────────────────────────────────
//
// TOTAL TX CYCLE BREAKDOWN:
// ──────────────────────────────────────────────────────────────
// 1. Pre-TX setup:            850 μs (writeBuffer + setTx)
// 2. LoRa transmission:       98,000 μs (on-air)
// 3. Post-TX callback:        100 μs
// 4. Guard time:              5,000 μs (safety margin)
// 5. Mode switching:          500 μs (TX→Sleep→RX)
// ──────────────────────────────────────────────────────────────
// SUBTOTAL:                   104,450 μs (104.45 ms)
// With 20% safety margin:     125,340 μs (125.34 ms)

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
#define MAX_NEIGHBOURS_IN_PACKET 4  // Reduced from 6 to make room for tracking

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

#define MAX_NEIGHBOURS 10
#define MAX_INACTIVE_CYCLES 20
#define PROBABILITY_INITIATOR 100

// ============= WIFI =============
// ⚠️ CONFIGURE THESE FOR YOUR NETWORK
#define ENABLE_WIFI 0                     // Set to 1 to enable WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"        // ⚠️ CHANGE THIS
#define WIFI_PASS "YOUR_WIFI_PASSWORD"    // ⚠️ CHANGE THIS
#define SERVER_IP "192.168.1.100"         // ⚠️ CHANGE THIS: Your server IP
#define SERVER_PORT 5000                  // ⚠️ CHANGE THIS if needed
#define WIFI_QUEUE_SIZE 32

// ============= DISPLAY =============
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

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
