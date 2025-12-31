/*****************************************************************************************************
  LoRa Mesh Network Node for SX1262 - Ra01S LIBRARY VERSION
  Migrated from SX1262_TimingControl to Ra01S (SX126x class) library
  
  Features:
  - TDMA-based mesh networking with automatic slot assignment
  - Collision avoidance through neighbor discovery
  - Multi-hop message routing
  - OLED display with rotary encoder navigation
  - Sensor integration (simulated temperature/humidity/battery)
  
  Hardware Requirements:
  - ESP32 with SX1262 LoRa module (Ra01S compatible)
  - SSD1306 OLED display (128x64)
  - Rotary encoder with push button
  - AHT10 temperature/humidity sensor (optional)
  - INA219 power monitor (optional)
  
  Configure your node in settings.h before uploading!
  
  MIGRATION NOTES (Ra01S Library):
  ════════════════════════════════════════════════════════════════════════════
  - Ra01S uses POLLING mode instead of callbacks/interrupts
  - radio.Receive() polls for incoming data (non-blocking)
  - radio.Send() can be synchronous (SX126x_TXMODE_SYNC) or async
  - Simplified code - no semaphores, no ISR callbacks needed
  - All timing still in microseconds for TDMA precision
*******************************************************************************************************/

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include "Ra01S.h"  // Ra01S library (SX126x class)
#include "settings.h"
#include "config_manager.h"  // EEPROM config & serial commands
#include <sys/time.h>

// WiFi untuk NTP time sync & remote monitoring
#if ENABLE_WIFI == 1
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <time.h>
  
  // WiFi UDP objects for remote monitoring
  WiFiUDP udpMonitor;  // Send events to monitoring server
  WiFiUDP udpCommand;  // Receive commands from control script
#endif

// Timer Interrupt
hw_timer_t * tdmaTimer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// ============= SERIAL DEBUG MACROS =============
// Conditional compilation - zero overhead when disabled
#if ENABLE_SERIAL_DEBUG == 1
  #define DEBUG_PRINT(fmt, ...)       Serial.printf(fmt, ##__VA_ARGS__)
  #define DEBUG_PRINTLN(str)          Serial.println(str)
  #define DEBUG_PRINT_LATENCY(...)    Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(fmt, ...)       // No-op (0µs overhead)
  #define DEBUG_PRINTLN(str)          // No-op
  #define DEBUG_PRINT_LATENCY(...)    // No-op
#endif

// Critical logs (always enabled, even in production)
#define LOG_ERROR(fmt, ...)           Serial.printf("[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)            Serial.printf("[INFO] " fmt, ##__VA_ARGS__)

// ============= HARDWARE OBJECTS =============
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Ra01S (SX126x) radio object
// Constructor: SX126x(spiSelect, reset, busy, txen, rxen)
SX126x radio(LORA_PIN_NSS, LORA_PIN_RESET, LORA_PIN_BUSY, LORA_TXEN, LORA_RXEN);

// ============= TIME MANAGEMENT =============
#if ENABLE_WIFI == 1
  bool timeSynced = false;
  
  // NTP sync reference
  int64_t ntpEpochAtSync = 0;   // NTP epoch time at sync (microseconds)
  uint64_t microsAtSync = 0;    // micros() value at NTP sync
  
  // Drift compensation (with overflow protection)
  int32_t driftPpm = 0;         // Measured clock drift (parts per million, limited to ±100)
  uint64_t lastDriftCheck = 0;  // Last drift check/re-sync time
#endif

// ============= MULTI-PAGE DISPLAY =============
uint8_t currentPage = DISPLAY_PAGE_INFO;
uint32_t lastDisplayUpdate = 0;
bool displayNeedsUpdate = true;

// FreeRTOS Task Handles & Synchronization
TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t dataLogTaskHandle = NULL;
TaskHandle_t wifiMonitorTaskHandle = NULL;
SemaphoreHandle_t displayMutex = NULL;
QueueHandle_t logQueue = NULL;

// WiFi Event Queue (for remote monitoring)
#if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
  #define WIFI_EVENT_QUEUE_SIZE 100  // Increased for PDR/Latency data
  
  struct WiFiEvent {
    char message[300];  // Formatted event message (increased for detailed stats)
  };
  
  QueueHandle_t wifiEventQueue = NULL;
  
  // WiFi Monitor PDR & Latency tracking (same as gateway mode)
  #define WIFI_ENABLE_PDR_TRACKING 1
  #define WIFI_ENABLE_LATENCY_CALC 1
#endif

// ============= DATA LOGGING STRUCTURES =============
// Data logging structure (used when DEBUG_MODE == DEBUG_MODE_GATEWAY_ONLY)
struct DataLogEntry {
  uint8_t logType;  // 0=PDR, 1=Latency, 2=Packet, 3=Sync
  uint32_t timestamp;
  uint16_t nodeId;
  uint16_t messageId;
  uint8_t hopCount;
  int64_t latencyUs;
  float pdr;
  int16_t rssi;
  int8_t snr;
  char extraData[32];
};

#define LOG_QUEUE_SIZE 50

// ============= RUNTIME CONFIGURATION (FROM EEPROM) =============
RuntimeConfig runtimeConfig;
char activeSSID[MAX_SSID_LEN + 1];
char activePassword[MAX_PASS_LEN + 1];
char activeServerIP[MAX_IP_LEN + 1];
uint8_t activeDebugMode;
bool configLoaded = false;

// ============= TDMA CONTROL =============
volatile bool tdmaEnabled = true;  // Control TDMA execution

// ============= TIMER INTERRUPT FLAGS =============
volatile bool tdmaSlotTick = false;
volatile uint32_t tdmaInterruptCount = 0;

// ============= ENCODER VARIABLES =============
volatile int32_t encoderRaw = 0;
volatile bool buttonPressed = false;
volatile uint32_t lastEncoderISR = 0;
volatile uint32_t lastButtonISR = 0;
const uint32_t ENCODER_DEBOUNCE_US = 500;
const uint32_t BUTTON_DEBOUNCE_MS = 150;

// ============= MESH NETWORK VARIABLES =============
// RSSI threshold: ignore neighbors below -115 dBm
#define RSSI_THRESHOLD_DBM -115

NeighbourInfo neighbours[MAX_NEIGHBOURS];
uint8_t neighbourIndices[MAX_NEIGHBOURS];
uint8_t neighbourCount = 0;
MyNodeInfo myInfo;

// TX/RX Buffers
uint8_t rxBuffer[RXBUFFER_SIZE];
uint8_t txBuffer[TXBUFFER_SIZE];
uint8_t rxPacketLength = 0;
uint8_t txPacketLength = 0;

// Slot availability tracking
bool slotAvailability[Nslot];

// Message handling - unified with neighbor broadcast
char sensorDataToSend[SENSOR_DATA_LENGTH + 1];
char sensorDataReceived[SENSOR_DATA_LENGTH + 1];
bool hasSensorDataToSend = false;

// FIFO Forwarding Queue
#define FORWARD_QUEUE_SIZE 8
struct ForwardMessage {
  uint16_t originalSender;
  uint16_t messageId;
  uint8_t hopCount;
  uint8_t dataLen;
  char data[SENSOR_DATA_LENGTH + 1];
  uint16_t tracking[MAX_TRACKING_HOPS];
  #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
    int64_t txTimestampUs;  // Original TX timestamp from sender
  #endif
};
ForwardMessage forwardQueue[FORWARD_QUEUE_SIZE];
uint8_t forwardQueueHead = 0;  // Index to write
uint8_t forwardQueueTail = 0;  // Index to read
uint8_t forwardQueueCount = 0;

// ============= MESSAGE TRACKING =============
uint16_t messageIdCounter = 0;
uint16_t ownMessageOrigSender = 0;
uint16_t ownMessageId = 0;

// ============= LATENCY TRACKING =============
// Enable for both GATEWAY_ONLY and WIFI_MONITOR modes
#if (ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1) || (ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR)
  #ifndef LATENCY_CACHE_SIZE
    #define LATENCY_CACHE_SIZE 20
  #endif
  
  struct LatencyRecord {
    uint16_t messageId;
    uint16_t origSender;
    int64_t txTimestampUs;
    int64_t rxTimestampUs;
    uint8_t hopCount;
    int64_t latencyUs;
  };
  LatencyRecord latencyRecords[LATENCY_CACHE_SIZE];
  uint8_t latencyRecordIndex = 0;  // Circular buffer index
  uint8_t latencyRecordCount = 0;  // Total records (capped at LATENCY_CACHE_SIZE)
  
  // TX timestamp cache for sent messages (circular buffer)
  struct TxTimestampCache {
    uint16_t messageId;
    int64_t timestampUs;
  };
  TxTimestampCache txTimestampCache[LATENCY_CACHE_SIZE];
  uint8_t txTimestampCacheIndex = 0;  // Circular buffer index
  uint8_t txTimestampCacheCount = 0;  // Current count
  
  // Statistics
  uint32_t totalLatencyCalculations = 0;
  int64_t totalLatencyUs = 0;
  int64_t minLatencyUs = INT64_MAX;
  int64_t maxLatencyUs = 0;
#endif

// ============= PDR (Packet Delivery Ratio) TRACKING =============
#define MAX_PDR_NODES 10
// Enable PDR for GATEWAY_ONLY and WIFI_MONITOR modes
#if DEBUG_MODE == DEBUG_MODE_GATEWAY_ONLY || DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
  #define ENABLE_PDR_TRACKING 1
#else
  #define ENABLE_PDR_TRACKING 0
#endif

#if ENABLE_PDR_TRACKING == 1
  struct PdrNodeStats {
    uint16_t nodeId;
    uint16_t lastSeqReceived;     // Last sequence number received
    uint16_t expectedCount;       // Expected packet count (based on sequence)
    uint16_t receivedCount;       // Actually received count
    uint16_t gapCount;            // Detected gaps (lost packets)
    float pdr;                    // Packet Delivery Ratio (%)
    uint32_t lastUpdateTime;      // Last packet received time
    bool initialized;             // First packet received flag
    
    // Latency statistics (for this specific node)
    uint32_t latencyCount;        // Number of latency measurements
    int64_t totalLatencyUs;       // Sum of all latencies
    int64_t minLatencyUs;         // Minimum latency
    int64_t maxLatencyUs;         // Maximum latency
    float avgLatencyMs;           // Average latency (milliseconds)
  };
  
  PdrNodeStats pdrStats[MAX_PDR_NODES];
  uint8_t pdrNodeCount = 0;
  
  // Overall network statistics
  uint32_t totalPacketsExpected = 0;
  uint32_t totalPacketsReceived = 0;
  uint32_t totalPacketsLost = 0;
  float networkPdr = 100.0;
#endif

// ============= WIFI BATCH BUFFER =============
#define WIFI_BATCH_SIZE 10
struct WifiMessage {
  uint16_t origSender;
  uint16_t messageId;
  char data[SENSOR_DATA_LENGTH + 1];
  uint8_t tracking[MAX_TRACKING_HOPS];
  uint8_t trackingLen;
};
WifiMessage wifiBatchBuffer[WIFI_BATCH_SIZE];
uint8_t wifiBatchCount = 0;

// ============= RX DATA =============
// Ra01S returns RSSI/SNR via GetPacketStatus()
int8_t rxRssi = 0;
int8_t rxSnr = 0;

// ============= TIMING VARIABLES =============
uint8_t loopCounter = 0;

// ============= AUTO-SEND MESSAGE CONFIG =============
#define AUTO_SEND_INTERVAL_CYCLES 6  // 6 cycles for 1 gateway + 5 sensor nodes
uint8_t autoSendCounter = 0;

// Cycle validation for sequential transmission
bool cycleValidated = false;
uint8_t cycleValidationCount = 0;
int8_t lastReceivedCycle = -1;
#define CYCLE_VALIDATION_THRESHOLD 3  // Changed from 5 to 3 for faster validation

// Simulated sensor data
float simTemperature = 25.0;
float simHumidity = 60.0;
uint8_t simBattery = 85;

// ============= WIFI BATCH FUNCTIONS =============
void sendWifiBatch() {
  #if ENABLE_WIFI == 1
    if (wifiBatchCount == 0) return;
    
    unsigned long sendStart = millis();
    Serial.printf("[Node %d] [WIFI] Sending batch of %d messages to server...\n", 
                  myInfo.id, wifiBatchCount);
    
    // TODO: Implement actual HTTP POST request here
    // Format: JSON array with all buffered messages
    // Example:
    // {
    //   "gateway_id": 1,
    //   "timestamp": 1234567890,
    //   "messages": [
    //     {"sender": 2, "msg_id": 546, "data": "T21H62", "tracking": [2]},
    //     {"sender": 3, "msg_id": 803, "data": "T23H66", "tracking": [3]}
    //   ]
    // }
    
    // For now, just print the batch
    Serial.printf("[Node %d] [WIFI] Batch payload:\n", myInfo.id);
    for (uint8_t i = 0; i < wifiBatchCount; i++) {
      Serial.printf("  [%d] MsgID:%d From:%d Data:%s Track:", 
                    i, wifiBatchBuffer[i].messageId, 
                    wifiBatchBuffer[i].origSender, 
                    wifiBatchBuffer[i].data);
      for (uint8_t j = 0; j < wifiBatchBuffer[i].trackingLen; j++) {
        Serial.printf("%d ", wifiBatchBuffer[i].tracking[j]);
      }
      Serial.printf("\n");
    }
    
    unsigned long sendDuration = millis() - sendStart;
    Serial.printf("[Node %d] [WIFI] Batch sent in %lu ms\n", myInfo.id, sendDuration);
    
    // Clear batch buffer
    wifiBatchCount = 0;
  #endif
}

// ============= STATISTICS =============
uint32_t txPacketCount = 0;
uint32_t rxPacketCount = 0;
int16_t lastRssi = 0;
int8_t lastSnr = 0;
char nodeStatus[12] = "INIT";

// ============= TIMING MEASUREMENT =============
uint32_t lastTxDuration_us = 0;
uint32_t lastRxDuration_us = 0;

// ============= FUNCTION PROTOTYPES =============
void IRAM_ATTR encoderISR();
void IRAM_ATTR buttonISR();

void initLoRa();
void initDisplay();
void initEncoder();

// Data logging functions (used when DEBUG_MODE != DEBUG_MODE_OFF)
void dataLogTask(void* parameter);
void logPacketData(uint16_t nodeId, uint16_t msgId, uint8_t hopCount, int64_t latencyUs, int16_t rssi, int8_t snr);
void initMyInfo();
void updateDisplay();
void displayTask(void* parameter);

void updateDisplay();
void updateNeighbourStatus();
void printStatusLine();

void transmitUnifiedPacket();
uint8_t processRxPacket();
uint16_t selectBestNextHop();
bool enqueueForward(ForwardMessage* msg);
bool dequeueForward(ForwardMessage* msg);

ResponderOutput responder(uint32_t timeoutMs);

// ============= PDR TRACKING FUNCTIONS =============
#if ENABLE_PDR_TRACKING == 1
void updatePdrStats(uint16_t nodeId, uint16_t messageId);
void updateNodeLatency(uint16_t nodeId, int64_t latencyUs);
#endif

// ============= TIME MANAGEMENT FUNCTIONS =============
#if ENABLE_WIFI == 1
// Get current time in microseconds since epoch with drift compensation
int64_t getCurrentTimeUs() {
  if (!timeSynced) return 0;
  
  // Calculate elapsed time since last NTP sync
  uint64_t currentMicros = micros();
  uint64_t elapsedUs = currentMicros - microsAtSync;
  
  #if ENABLE_DRIFT_COMPENSATION == 1
    // Apply drift compensation (safe calculation to prevent overflow)
    // driftCorrection = elapsed * (driftPpm / 1000000)
    // Split to prevent overflow: (elapsed / 1000) * driftPpm / 1000
    int64_t elapsedMs = elapsedUs / 1000;  // Convert to milliseconds first
    int64_t driftCorrectionUs = (elapsedMs * driftPpm) / 1000;
    
    return ntpEpochAtSync + elapsedUs + driftCorrectionUs;
  #else
    return ntpEpochAtSync + elapsedUs;
  #endif
}

// Periodic NTP re-sync with drift measurement (overflow-safe)
void updateDriftCompensation() {
  #if ENABLE_DRIFT_COMPENSATION == 1
    if (!timeSynced) return;
    
    uint64_t now = millis();
    if (now - lastDriftCheck < DRIFT_CHECK_INTERVAL_MS) return;
    
    // Get fresh NTP time
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
      int64_t ntpCurrentUs = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
      int64_t ourTimeUs = getCurrentTimeUs();
      
      // Calculate drift only over this interval (prevents unbounded growth)
      int64_t driftUs = ntpCurrentUs - ourTimeUs;
      uint64_t intervalUs = micros() - microsAtSync;
      
      if (intervalUs > 1000000) {  // At least 1 second elapsed
        // Calculate PPM: drift_ppm = (drift / interval) * 1000000
        // Use milliseconds to prevent overflow
        int64_t intervalMs = intervalUs / 1000;
        int64_t driftMs = driftUs / 1000;
        int32_t newDriftPpm = (int32_t)((driftMs * 1000000LL) / intervalMs);
        
        // Limit to reasonable range (saturation) to prevent overflow
        if (newDriftPpm > MAX_DRIFT_PPM) newDriftPpm = MAX_DRIFT_PPM;
        if (newDriftPpm < -MAX_DRIFT_PPM) newDriftPpm = -MAX_DRIFT_PPM;
        
        // Only update if change is significant (hysteresis)
        if (abs(newDriftPpm - driftPpm) > 2) {
          driftPpm = newDriftPpm;
        }
        
        Serial.printf("[TIME] Drift: %lld μs over %llu s, PPM: %ld\n", 
                      driftUs, intervalUs/1000000, driftPpm);
        
        // RE-SYNC: Reset reference point to prevent unbounded growth
        ntpEpochAtSync = ntpCurrentUs;
        microsAtSync = micros();
      }
      
      lastDriftCheck = now;
    }
  #endif
}

// Format timestamp for display
void formatTimestamp(int64_t timeUs, char* buffer, size_t bufSize) {
  if (!timeSynced || timeUs == 0) {
    snprintf(buffer, bufSize, "NO_SYNC");
    return;
  }
  
  int64_t timeSec = timeUs / 1000000LL;
  int64_t microPart = timeUs % 1000000LL;
  
  struct tm timeinfo;
  time_t timeSec_t = (time_t)timeSec;
  localtime_r(&timeSec_t, &timeinfo);
  
  snprintf(buffer, bufSize, "%02d:%02d:%02d.%06lld", 
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, microPart);
}
#endif

// ============= TIMER INTERRUPT ISR =============
void IRAM_ATTR onTdmaTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  tdmaSlotTick = true;
  tdmaInterruptCount++;
  portEXIT_CRITICAL_ISR(&timerMux);
}

// ============= PDR TRACKING IMPLEMENTATIONS =============
#if ENABLE_PDR_TRACKING == 1
void updatePdrStats(uint16_t nodeId, uint16_t messageId) {
  // Extract sequence number from messageId
  // Assuming messageId format: (nodeId << 8) | sequenceCounter
  uint16_t seqNum = messageId & 0xFF;
  
  // Find or create entry for this node
  int8_t nodeIndex = -1;
  for (uint8_t i = 0; i < pdrNodeCount; i++) {
    if (pdrStats[i].nodeId == nodeId) {
      nodeIndex = i;
      break;
    }
  }
  
  // Create new entry if not found
  if (nodeIndex == -1 && pdrNodeCount < MAX_PDR_NODES) {
    nodeIndex = pdrNodeCount;
    pdrStats[nodeIndex].nodeId = nodeId;
    pdrStats[nodeIndex].lastSeqReceived = 0;
    pdrStats[nodeIndex].expectedCount = 0;
    pdrStats[nodeIndex].receivedCount = 0;
    pdrStats[nodeIndex].gapCount = 0;
    pdrStats[nodeIndex].pdr = 100.0;
    pdrStats[nodeIndex].initialized = false;
    
    // Initialize latency fields
    pdrStats[nodeIndex].latencyCount = 0;
    pdrStats[nodeIndex].totalLatencyUs = 0;
    pdrStats[nodeIndex].minLatencyUs = INT64_MAX;
    pdrStats[nodeIndex].maxLatencyUs = 0;
    pdrStats[nodeIndex].avgLatencyMs = 0.0;
    
    pdrNodeCount++;
  }
  
  if (nodeIndex >= 0) {
    PdrNodeStats* stats = &pdrStats[nodeIndex];
    
    if (!stats->initialized) {
      // First packet from this node
      stats->lastSeqReceived = seqNum;
      stats->receivedCount = 1;
      stats->expectedCount = 1;
      stats->initialized = true;
      stats->pdr = 100.0;
    } else {
      // Calculate expected packets based on sequence jump
      uint16_t seqDiff;
      
      if (seqNum > stats->lastSeqReceived) {
        seqDiff = seqNum - stats->lastSeqReceived;
      } else {
        // Handle sequence number wrap-around (0-255)
        seqDiff = (256 - stats->lastSeqReceived) + seqNum;
      }
      
      // Update counts
      stats->receivedCount++;
      stats->expectedCount += seqDiff;
      
      // Detect gaps (lost packets)
      if (seqDiff > 1) {
        uint16_t lostPackets = seqDiff - 1;
        stats->gapCount += lostPackets;
        
        // Update global statistics
        totalPacketsLost += lostPackets;
        
        DEBUG_PRINT("[PDR] Node %d: Gap detected! Seq %d->%d, Lost %d packets\n",
                    nodeId, stats->lastSeqReceived, seqNum, lostPackets);
      }
      
      stats->lastSeqReceived = seqNum;
      
      // Calculate PDR
      if (stats->expectedCount > 0) {
        stats->pdr = (stats->receivedCount * 100.0) / stats->expectedCount;
      }
    }
    
    stats->lastUpdateTime = millis();
    
    // Update global statistics
    totalPacketsExpected = 0;
    totalPacketsReceived = 0;
    for (uint8_t i = 0; i < pdrNodeCount; i++) {
      totalPacketsExpected += pdrStats[i].expectedCount;
      totalPacketsReceived += pdrStats[i].receivedCount;
    }
    
    if (totalPacketsExpected > 0) {
      networkPdr = (totalPacketsReceived * 100.0) / totalPacketsExpected;
    }
    
    DEBUG_PRINT("[PDR] Node %d: Seq#%d RX:%d/%d (%.1f%%) Gaps:%d\n",
                nodeId, seqNum, stats->receivedCount, stats->expectedCount, 
                stats->pdr, stats->gapCount);
  }
}

// Update latency statistics for specific node
void updateNodeLatency(uint16_t nodeId, int64_t latencyUs) {
  // Find node in PDR stats
  for (uint8_t i = 0; i < pdrNodeCount; i++) {
    if (pdrStats[i].nodeId == nodeId) {
      pdrStats[i].latencyCount++;
      pdrStats[i].totalLatencyUs += latencyUs;
      
      if (latencyUs < pdrStats[i].minLatencyUs) {
        pdrStats[i].minLatencyUs = latencyUs;
      }
      if (latencyUs > pdrStats[i].maxLatencyUs) {
        pdrStats[i].maxLatencyUs = latencyUs;
      }
      
      // Calculate average in milliseconds
      pdrStats[i].avgLatencyMs = (pdrStats[i].totalLatencyUs / pdrStats[i].latencyCount) / 1000.0;
      
      DEBUG_PRINT("[STATS] Node %d: Latency %.1fms (min:%.1f avg:%.1f max:%.1f)\n",
                  nodeId, latencyUs/1000.0,
                  pdrStats[i].minLatencyUs/1000.0,
                  pdrStats[i].avgLatencyMs,
                  pdrStats[i].maxLatencyUs/1000.0);
      break;
    }
  }
}
#endif

// ============= ENCODER ISR =============
void IRAM_ATTR encoderISR() {
  uint32_t now = micros();
  if (now - lastEncoderISR < ENCODER_DEBOUNCE_US) return;
  lastEncoderISR = now;
  
  static uint8_t oldState = 0;
  static const int8_t transitions[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };
  
  uint8_t newState = 0;
  if (digitalRead(ENCODER_A)) newState |= 0x02;
  if (digitalRead(ENCODER_B)) newState |= 0x01;
  
  uint8_t idx = (oldState << 2) | newState;
  encoderRaw += transitions[idx];
  oldState = newState;
}

// ============= BUTTON ISR =============
void IRAM_ATTR buttonISR() {
  uint32_t now = millis();
  if (now - lastButtonISR < BUTTON_DEBOUNCE_MS) return;
  lastButtonISR = now;
  
  if (digitalRead(ENCODER_SW) == LOW) {
    buttonPressed = true;
    // Cycle through display pages
    currentPage = (currentPage + 1) % DISPLAY_PAGE_COUNT;
    displayNeedsUpdate = true;
  }
}

// ============= INITIALIZATION FUNCTIONS =============
void initEncoder() {
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  
  delay(10);
  
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW), buttonISR, FALLING);
  
  Serial.println("[ENCODER] Initialized with interrupts");
}

void initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    while(1);
  }
  
  // Create mutex for thread-safe display access
  displayMutex = xSemaphoreCreateMutex();
  if (displayMutex == NULL) {
    Serial.println("[DISPLAY] Failed to create mutex!");
    while(1);
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("LoRa Mesh Node");
  display.println("Ra01S Library");
  display.println("Initializing...");
  display.display();
}

void initLoRa() {
  Serial.println("Initializing SX1262 (Ra01S Library)...");
  
  // Initialize SPI with custom pins
  SPI.begin(LORA_PIN_SCLK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
  
  // Enable debug output (optional)
  radio.DebugPrint(true);
  
  // Initialize radio
  // begin(frequency, txPower, tcxoVoltage, useRegulatorLDO)
  // tcxoVoltage = 0.0 means no TCXO
  // useRegulatorLDO = false means use DC-DC regulator
  int16_t result = radio.begin(RF_FREQUENCY, TX_OUTPUT_POWER, 0.0, false);
  
  if (result != ERR_NONE) {
    Serial.printf("[ERROR] SX1262 init failed! Error: %d\n", result);
    display.println("LoRa: FAILED");
    display.display();
    while(1);
  }
  
  // Configure LoRa modulation parameters
  // LoRaConfig(sf, bw, cr, preambleLen, payloadLen, crcOn, invertIQ)
  // payloadLen = 0 for variable length (explicit header)
  // payloadLen > 0 for fixed length (implicit header)
  radio.LoRaConfig(
    LORA_SPREADING_FACTOR,   // SF7 = 7
    LORA_BANDWIDTH,          // BW125 = 0x04
    LORA_CODINGRATE,         // CR4/5 = 0x01
    LORA_PREAMBLE_LENGTH,    // 8 symbols
    FIXED_PACKET_LENGTH,     // Fixed 48 bytes (implicit header)
    true,                    // CRC on
    false                    // Standard IQ
  );
  
  // Disable debug after init
  radio.DebugPrint(false);
  
  Serial.println("SX1262 Ra01S: OK");
  display.println("LoRa: OK (Ra01S)");
  display.display();
  
  Serial.println("LoRa configured:");
  Serial.printf("  Frequency: %.1f MHz\n", RF_FREQUENCY / 1000000.0);
  Serial.printf("  TX Power: %d dBm\n", TX_OUTPUT_POWER);
  Serial.printf("  SF: %d\n", LORA_SPREADING_FACTOR);
  Serial.printf("  BW: %s\n", 
                (LORA_BANDWIDTH == SX126X_LORA_BW_125_0) ? "125kHz" :
                (LORA_BANDWIDTH == SX126X_LORA_BW_250_0) ? "250kHz" :
                (LORA_BANDWIDTH == SX126X_LORA_BW_500_0) ? "500kHz" : "Other");
  Serial.printf("  CR: 4/%d\n", LORA_CODINGRATE + 4);
  
  Serial.println("\nTiming Analysis:");
  Serial.printf("  Measured ToA: %lu μs (%.2f ms)\n", MEASURED_TOA_US, MEASURED_TOA_US / 1000.0);
  Serial.printf("  Effective ToA: %lu μs (%.2f ms) [+20%% margin]\n", 
                EFFECTIVE_TOA_US, EFFECTIVE_TOA_US / 1000.0);
  Serial.printf("  Slot duration: %lu μs (%.2f ms)\n", Tslot_us, Tslot_us / 1000.0);
  Serial.printf("  Safety margin: %.1fx\n", (float)Tslot_us / (float)EFFECTIVE_TOA_US);
  
  // Initialize Timer Interrupt for TDMA timing
  Serial.println("\nInitializing Timer Interrupt...");
  // Timer with 1 MHz frequency (ESP32 Arduino core 3.x API)
  tdmaTimer = timerBegin(1000000);  // 1 MHz = 1 microsecond resolution
  timerAttachInterrupt(tdmaTimer, &onTdmaTimer);
  // Set alarm to trigger every Tperiod_us (full TDMA cycle)
  timerAlarm(tdmaTimer, Tperiod_us, true, 0);  // autoreload=true, unlimited=0
  Serial.printf("  Timer period: %lu μs (%.2f ms)\n", Tperiod_us, Tperiod_us / 1000.0);
  Serial.printf("  Timer accuracy: ±%d μs\n", TIMER_ERROR_MARGIN_US);
}

void initMyInfo() {
  #if DEVICE_ID == 0
    myInfo.id = random(1, 65536);
  #else
    myInfo.id = DEVICE_ID;
  #endif
  
  myInfo.posX.number = random(-500, 500);
  myInfo.posY.number = random(-500, 500);
  myInfo.posZ.number = random(-500, 500);
  
  #if FIX_SLOT == 1
    myInfo.slotIndex = SLOT_DEVICE;
  #else
    myInfo.slotIndex = random(0, Nslot);
  #endif
  
  Serial.print("Node ID: "); Serial.println(myInfo.id);
  Serial.print("Slot Index: "); Serial.println(myInfo.slotIndex);
  Serial.print("Is Reference: "); Serial.println(IS_REFERENCE);
}

// ============= DISPLAY UPDATE (NON-BLOCKING) =============
void updateDisplay() {
  // Non-blocking update - check if enough time has passed
  uint32_t now = millis();
  if (!displayNeedsUpdate && (now - lastDisplayUpdate < DISPLAY_UPDATE_INTERVAL_MS)) {
    return;
  }
  
  lastDisplayUpdate = now;
  displayNeedsUpdate = false;
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  char hopChar = (myInfo.hoppingDistance == 0x7F) ? '?' : ('0' + myInfo.hoppingDistance);
  
  // ========== PAGE 0: NODE INFO & NEIGHBORS ==========
  if (currentPage == DISPLAY_PAGE_INFO) {
    // ROW 1: Node Info
    char line1[22];
    snprintf(line1, sizeof(line1), "ID:%04d H:%c S:%d %s", 
             myInfo.id, hopChar, myInfo.slotIndex, nodeStatus);
    display.println(line1);
    
    // Neighbor List (max 6 rows)
    uint8_t displayCount = 0;
    for (uint8_t i = 0; i < MAX_NEIGHBOURS && displayCount < 6; i++) {
      if (neighbours[i].id > 0) {
        char hopStr[3];
        if (neighbours[i].hoppingDistance == 0x7F) {
          strcpy(hopStr, "?");
        } else {
          snprintf(hopStr, sizeof(hopStr), "%d", neighbours[i].hoppingDistance);
        }
        
        char bidirFlag = neighbours[i].amIListedAsNeighbour ? '*' : ' ';
        char line[22];
        snprintf(line, sizeof(line), "%c%4d S%d H%s R%d", 
                 bidirFlag, neighbours[i].id, neighbours[i].slotIndex, hopStr, neighbours[i].rssi);
        display.println(line);
        displayCount++;
      }
    }
    
    if (displayCount == 0) {
      display.println("  (no neighbors)");
    }
    
    // Last row: Stats
    display.setCursor(0, 56);
    char statsLine[22];
    snprintf(statsLine, sizeof(statsLine), "TX:%lu RX:%lu N:%d P1/%d", 
             txPacketCount, rxPacketCount, neighbourCount, DISPLAY_PAGE_COUNT);
    display.println(statsLine);
  }
  
  // ========== PAGE 1: PER-NODE STATISTICS (GATEWAY ONLY) ==========
  #if ENABLE_PDR_TRACKING == 1
  else if (currentPage == DISPLAY_PAGE_PDR) {
    display.println("== NODE STATS ==");
    display.setTextSize(1);
    
    if (myInfo.hoppingDistance == 0) {
      // Gateway: Show per-node statistics (similar to neighbor list format)
      if (pdrNodeCount == 0) {
        display.setCursor(0, 20);
        display.println("No data yet");
      } else {
        // Show up to 6 nodes (to fit screen)
        uint8_t shown = 0;
        for (uint8_t i = 0; i < pdrNodeCount && shown < 6; i++) {
          char line[22];
          
          // Format similar to neighbor list: Node# Pkt PDR% Lat
          if (pdrStats[i].latencyCount > 0) {
            // Has latency data
            snprintf(line, sizeof(line), "%d P%d D%.0f%% L%.0f",
                     pdrStats[i].nodeId,
                     pdrStats[i].receivedCount,
                     pdrStats[i].pdr,
                     pdrStats[i].avgLatencyMs);
          } else {
            // No latency data yet
            snprintf(line, sizeof(line), "%d P%d D%.0f%%",
                     pdrStats[i].nodeId,
                     pdrStats[i].receivedCount,
                     pdrStats[i].pdr);
          }
          display.println(line);
          shown++;
        }
      }
    } else {
      // Non-gateway: Show own transmission stats
      display.setCursor(0, 12);
      display.println("Stats: GW only");
      display.printf("This Node: %d\n", myInfo.id);
      display.printf("Sent: %d pkts\n", messageIdCounter);
      display.println("View on gateway");
      display.println("for full stats");
    }
    
    display.setCursor(0, 56);
    if (myInfo.hoppingDistance == 0) {
      display.print("[GW Stats] P2/");
    } else {
      display.print("[Node Info] P2/");
    }
    display.print(DISPLAY_PAGE_COUNT);
  }
  #endif
  
  // ========== PAGE 2: WIFI INFO (WIFI_MONITOR MODE ONLY) ==========
  #if DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR && ENABLE_WIFI == 1
  else if (currentPage == DISPLAY_PAGE_WIFI) {
    display.println("== WIFI MONITOR ==");
    if (WiFi.status() == WL_CONNECTED) {
      display.println("Status: Connected");
      display.print("IP: ");
      display.println(WiFi.localIP().toString().c_str());
      display.print("SSID: ");
      display.println(activeSSID);
      display.print("Server: ");
      display.println(activeServerIP);
    } else {
      display.println("Status: Disconnected");
      display.println("Reconnecting...");
    }
    
    display.setCursor(0, 56);
    display.print("[WiFi Info] P3/");
    display.print(DISPLAY_PAGE_COUNT);
  }
  #endif
  
  display.display();
}

// ============= DATA LOGGING TASK (FreeRTOS) =============
#if ENABLE_DATA_LOG == 1
// Runs on Core 0, processes log queue and outputs structured CSV data
void dataLogTask(void* parameter) {
  Serial.println("[DATA_LOG_TASK] Started on Core 0");
  
  DataLogEntry logEntry;
  
  for(;;) {
    // Wait for log entry from queue (blocking, 100ms timeout)
    if (xQueueReceive(logQueue, &logEntry, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Output structured CSV format for easy parsing
      // Format: LOG_TYPE,TIMESTAMP,NODE_ID,MSG_ID,HOP_COUNT,LATENCY_US,PDR,RSSI,SNR,EXTRA
      
      Serial.printf("{NODE%d} DATA,%d,%lu,%d,%d,%d,%lld,%.2f,%d,%d,%s\n",
                    myInfo.id,
                    logEntry.logType,
                    logEntry.timestamp,
                    logEntry.nodeId,
                    logEntry.messageId,
                    logEntry.hopCount,
                    logEntry.latencyUs,
                    logEntry.pdr,
                    logEntry.rssi,
                    logEntry.snr,
                    logEntry.extraData);
    }
    
    // Yield to prevent watchdog issues
    yield();
  }
}
#endif

// Helper: Log packet data (PDR, latency, etc.)
void logPacketData(uint16_t nodeId, uint16_t msgId, uint8_t hopCount, int64_t latencyUs, int16_t rssi, int8_t snr) {
  #if DEBUG_MODE == DEBUG_MODE_GATEWAY_ONLY
    if (logQueue == NULL) return;
    
    DataLogEntry entry;
    entry.logType = (latencyUs >= 0) ? 1 : 0;  // 1=Latency, 0=PDR only
    entry.timestamp = millis();
    entry.nodeId = nodeId;
    entry.messageId = msgId;
    entry.hopCount = hopCount;
    entry.latencyUs = latencyUs;
    entry.pdr = 0.0;  // Will be calculated separately
    entry.rssi = rssi;
    entry.snr = snr;
    snprintf(entry.extraData, sizeof(entry.extraData), "GW:%d", myInfo.id);
    
    // Send to queue (non-blocking)
    xQueueSend(logQueue, &entry, 0);
  #endif
}

// Helper: Send WiFi event (non-blocking, queued)
void sendWifiEvent(const char* eventType, const char* details) {
  #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
    if (wifiEventQueue == NULL || WiFi.status() != WL_CONNECTED) return;
    
    WiFiEvent evt;
    int64_t timestamp = timeSynced ? getCurrentTimeUs() : (int64_t)micros();
    
    snprintf(evt.message, sizeof(evt.message), 
             "EVENT,%lld,%d,%s,%s",
             timestamp, myInfo.id, eventType, details);
    
    // Send to queue (non-blocking)
    xQueueSend(wifiEventQueue, &evt, 0);
  #endif
}

// Helper: Send PDR stats via WiFi (for WIFI_MONITOR mode)
void sendPdrStatsWifi() {
  #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR && ENABLE_PDR_TRACKING == 1
    if (wifiEventQueue == NULL || WiFi.status() != WL_CONNECTED) return;
    if (pdrNodeCount == 0) return;
    
    WiFiEvent evt;
    int64_t timestamp = timeSynced ? getCurrentTimeUs() : (int64_t)micros();
    
    // Send overall network PDR
    snprintf(evt.message, sizeof(evt.message), 
             "PDR_NETWORK,%lld,%d,TOTAL,Exp:%lu,Rx:%lu,Lost:%lu,PDR:%.2f%%",
             timestamp, myInfo.id, 
             totalPacketsExpected, totalPacketsReceived, totalPacketsLost, networkPdr);
    xQueueSend(wifiEventQueue, &evt, 0);
    
    // Send per-node PDR statistics
    for (uint8_t i = 0; i < pdrNodeCount; i++) {
      PdrNodeStats* stats = &pdrStats[i];
      
      snprintf(evt.message, sizeof(evt.message), 
               "PDR_NODE,%lld,%d,Node%d,Seq:%d,Exp:%d,Rx:%d,Gaps:%d,PDR:%.2f%%,LatCnt:%lu,LatAvg:%.1fms,LatMin:%.1fms,LatMax:%.1fms",
               timestamp, myInfo.id, stats->nodeId,
               stats->lastSeqReceived, stats->expectedCount, stats->receivedCount,
               stats->gapCount, stats->pdr,
               stats->latencyCount, stats->avgLatencyMs,
               stats->minLatencyUs / 1000.0, stats->maxLatencyUs / 1000.0);
      xQueueSend(wifiEventQueue, &evt, 0);
    }
  #endif
}

// Helper: Send latency data via WiFi (for WIFI_MONITOR mode)
void sendLatencyDataWifi(uint16_t nodeId, uint16_t msgId, uint8_t hopCount, int64_t latencyUs, int16_t rssi, int8_t snr) {
  #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
    if (wifiEventQueue == NULL || WiFi.status() != WL_CONNECTED) return;
    
    WiFiEvent evt;
    int64_t timestamp = timeSynced ? getCurrentTimeUs() : (int64_t)micros();
    
    snprintf(evt.message, sizeof(evt.message), 
             "LATENCY,%lld,%d,Node%d,MsgID:%d,Hop:%d,Lat:%.1fms,RSSI:%ddBm,SNR:%ddB",
             timestamp, myInfo.id, nodeId, msgId, hopCount,
             latencyUs / 1000.0, rssi, snr);
    
    xQueueSend(wifiEventQueue, &evt, 0);
  #endif
}

// ============= WIFI MONITOR TASK (FreeRTOS) =============
// Runs on Core 0, sends events via UDP to Python monitoring server
#if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
void wifiMonitorTask(void* parameter) {
  Serial.println("[WIFI_MONITOR] Task started on Core 0");
  
  // Wait for WiFi connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
  }
  
  // Start UDP for monitoring (send events)
  udpMonitor.begin(MONITOR_UDP_PORT);
  Serial.printf("[WIFI_MONITOR] UDP monitoring on port %d\n", MONITOR_UDP_PORT);
  
  // Start UDP for commands (receive)
  udpCommand.begin(COMMAND_UDP_PORT);
  Serial.printf("[WIFI_MONITOR] UDP command server on port %d\n", COMMAND_UDP_PORT);
  
  WiFiEvent evt;
  char packetBuffer[256];
  uint32_t lastPdrReportTime = 0;
  const uint32_t PDR_REPORT_INTERVAL_MS = 5000;  // Send PDR stats every 5 seconds
  
  for(;;) {
    // Send queued events to monitoring server (non-blocking batch send)
    int eventsSent = 0;
    while (eventsSent < 10 && xQueueReceive(wifiEventQueue, &evt, pdMS_TO_TICKS(1)) == pdTRUE) {
      udpMonitor.beginPacket(activeServerIP, MONITOR_UDP_PORT);
      udpMonitor.write((uint8_t*)evt.message, strlen(evt.message));
      udpMonitor.endPacket();
      eventsSent++;
    }
    
    // Periodic PDR statistics report (WiFi Monitor mode only)
    #if ENABLE_PDR_TRACKING == 1
      uint32_t now = millis();
      if (now - lastPdrReportTime > PDR_REPORT_INTERVAL_MS) {
        sendPdrStatsWifi();
        lastPdrReportTime = now;
      }
    #endif
    
    // Check for incoming commands
    int packetSize = udpCommand.parsePacket();
    if (packetSize > 0) {
      int len = udpCommand.read(packetBuffer, sizeof(packetBuffer) - 1);
      if (len > 0) {
        packetBuffer[len] = '\0';
        
        // Parse command: CMD,NODE_ID,COMMAND
        char* token = strtok(packetBuffer, ",");
        if (token && strcmp(token, "CMD") == 0) {
          token = strtok(NULL, ",");
          int targetNode = atoi(token);
          
          // Check if command is for this node
          if (targetNode == myInfo.id || targetNode == 0) {  // 0 = broadcast
            token = strtok(NULL, ",");
            if (token) {
              String cmd = String(token);
              cmd.trim();
              
              // Execute command
              if (cmd == "TDMA_STOP" || cmd == "STOP") {
                tdmaEnabled = false;
                resetTDMAState();
                Serial.printf("{NODE%d} [WIFI_CMD] TDMA STOPPED\\n", myInfo.id);
                sendWifiEvent("CMD_EXECUTED", "TDMA_STOP");
              }
              else if (cmd == "TDMA_START" || cmd == "START") {
                tdmaEnabled = true;
                Serial.printf("{NODE%d} [WIFI_CMD] TDMA STARTED\\n", myInfo.id);
                sendWifiEvent("CMD_EXECUTED", "TDMA_START");
              }
              else if (cmd == "STATUS") {
                char status[128];
                snprintf(status, sizeof(status), "ID:%d,Slot:%d,Hop:%d,Neighbors:%d,TDMA:%s",
                        myInfo.id, myInfo.slotIndex, myInfo.hoppingDistance, 
                        neighbourCount, tdmaEnabled ? "ON" : "OFF");
                sendWifiEvent("STATUS", status);
              }
              else if (cmd == "CYCLE_STATUS") {
                char cycleStatus[128];
                snprintf(cycleStatus, sizeof(cycleStatus), "Cycle:%d,Validated:%s,Progress:%d/%d,LastRx:%d",
                        myInfo.syncedCycle, cycleValidated ? "YES" : "NO", 
                        cycleValidationCount, CYCLE_VALIDATION_THRESHOLD, lastReceivedCycle);
                sendWifiEvent("CYCLE_VAL", cycleStatus);
              }
              else if (cmd == "PDR_STATS") {
                // Send immediate PDR stats report
                sendPdrStatsWifi();
              }
            }
          }
        }
      }
    }
    
    yield();
  }
}
#endif

// ============= DISPLAY TASK (FreeRTOS) =============
// Runs on Core 0, updates display at 5 Hz (200ms interval)
void displayTask(void* parameter) {
  Serial.println("[DISPLAY_TASK] Started on Core 0");
  
  const TickType_t xDelay = pdMS_TO_TICKS(200);  // 200ms = 5Hz update rate
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for(;;) {
    // Wait for next cycle
    vTaskDelayUntil(&xLastWakeTime, xDelay);
    
    // Check if display needs update
    if (displayNeedsUpdate || (millis() - lastDisplayUpdate > 500)) {
      // Take mutex before accessing display
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        updateDisplay();
        lastDisplayUpdate = millis();
        displayNeedsUpdate = false;
        
        // Release mutex
        xSemaphoreGive(displayMutex);
      } else {
        // Mutex timeout - skip this update
        Serial.println("[DISPLAY_TASK] Mutex timeout, skipping update");
      }
    }
    
    // Feed watchdog
    yield();
  }
}

void printStatusLine() {
  char hopChar = (myInfo.hoppingDistance == 0x7F) ? '?' : ('0' + myInfo.hoppingDistance);
  Serial.printf("[Node %d] [STATUS] ID:%d H:%c S:%d N:%d TX:%lu RX:%lu FwdQ:%d\n",
                myInfo.id, myInfo.id, hopChar, myInfo.slotIndex, neighbourCount,
                txPacketCount, rxPacketCount, forwardQueueCount);
}

// ============= FIFO QUEUE FUNCTIONS =============
bool enqueueForward(ForwardMessage* msg) {
  if (forwardQueueCount >= FORWARD_QUEUE_SIZE) {
    Serial.printf("[Node %d] [QUEUE] Forward queue full!\n", myInfo.id);
    return false;
  }
  
  memcpy(&forwardQueue[forwardQueueHead], msg, sizeof(ForwardMessage));
  forwardQueueHead = (forwardQueueHead + 1) % FORWARD_QUEUE_SIZE;
  forwardQueueCount++;
  
  Serial.printf("[Node %d] [QUEUE] Enqueued MsgID:%d count:%d\n", 
                myInfo.id, msg->messageId, forwardQueueCount);
  return true;
}

bool dequeueForward(ForwardMessage* msg) {
  if (forwardQueueCount == 0) {
    return false;
  }
  
  memcpy(msg, &forwardQueue[forwardQueueTail], sizeof(ForwardMessage));
  forwardQueueTail = (forwardQueueTail + 1) % FORWARD_QUEUE_SIZE;
  forwardQueueCount--;
  
  return true;
}

// ============= NEXT HOP SELECTION =============
uint16_t selectBestNextHop() {
  // Select best next hop from bidirectional neighbors
  // Priority: Good RSSI (> -100) > Low hop count > Best SNR
  
  uint16_t bestNodeId = 0;
  int16_t bestRssi = -200;
  int8_t bestSnr = -128;
  uint8_t bestHop = 0xFF;
  
  for (uint8_t i = 0; i < neighbourCount; i++) {
    uint8_t idx = neighbourIndices[i];
    
    // Filter 1: RSSI must be above RSSI_THRESHOLD_DBM (-115)
    if (neighbours[idx].rssi < RSSI_THRESHOLD_DBM) continue;
    
    // Filter 2: Must be bidirectional and have valid hop distance
    if (!neighbours[idx].amIListedAsNeighbour) continue;
    if (neighbours[idx].hoppingDistance >= myInfo.hoppingDistance) continue;
    if (neighbours[idx].hoppingDistance == 0x7F) continue;
    
    // Selection criteria:
    // 1. Prefer RSSI > MIN_RSSI_THRESHOLD (-100)
    // 2. Then prefer lower hop count
    // 3. Finally prefer better SNR as tie-breaker
    
    bool currentGoodRssi = (neighbours[idx].rssi > MIN_RSSI_THRESHOLD);
    bool bestGoodRssi = (bestRssi > MIN_RSSI_THRESHOLD);
    
    bool shouldSelect = false;
    
    if (bestNodeId == 0) {
      // First valid candidate
      shouldSelect = true;
    } else if (currentGoodRssi && !bestGoodRssi) {
      // Prefer good RSSI over bad RSSI regardless of hop
      shouldSelect = true;
    } else if (!currentGoodRssi && bestGoodRssi) {
      // Keep best with good RSSI
      shouldSelect = false;
    } else if (neighbours[idx].hoppingDistance < bestHop) {
      // Both same RSSI quality, prefer lower hop
      shouldSelect = true;
    } else if (neighbours[idx].hoppingDistance == bestHop) {
      // Same hop, prefer better RSSI
      if (neighbours[idx].rssi > bestRssi) {
        shouldSelect = true;
      } else if (neighbours[idx].rssi == bestRssi && neighbours[idx].snr > bestSnr) {
        // Same RSSI, prefer better SNR
        shouldSelect = true;
      }
    }
    
    if (shouldSelect) {
      bestNodeId = neighbours[idx].id;
      bestRssi = neighbours[idx].rssi;
      bestSnr = neighbours[idx].snr;
      bestHop = neighbours[idx].hoppingDistance;
    }
  }
  
  if (bestNodeId > 0) {
    Serial.printf("[Node %d] [ROUTE] Selected next hop: Node %d (hop:%d RSSI:%d SNR:%d)\n",
                  myInfo.id, bestNodeId, bestHop, bestRssi, bestSnr);
  }
  
  return bestNodeId;
}

// ============= TRANSMIT FUNCTION =============
void transmitUnifiedPacket() {
  memset(txBuffer, 0, FIXED_PACKET_LENGTH);
  
  // HEADER SECTION (12 bytes)
  txBuffer[0] = (uint8_t)((ADR_BROADCAST >> 8) & 0xFF);
  txBuffer[1] = (uint8_t)((ADR_BROADCAST) & 0xFF);
  txBuffer[2] = CMD_ID_AND_POS;
  txBuffer[3] = (uint8_t)((myInfo.id >> 8) & 0xFF);
  txBuffer[4] = (uint8_t)((myInfo.id) & 0xFF);
  txBuffer[5] = myInfo.slotIndex;
  txBuffer[6] = (myInfo.isLocalized << 7) | myInfo.hoppingDistance;
  
  uint8_t neighborsToSend = min((uint8_t)neighbourCount, (uint8_t)MAX_NEIGHBOURS_IN_PACKET);
  // Pack cycle (5 bits) and neighbor count (3 bits) into byte 7
  txBuffer[7] = (myInfo.syncedCycle << 3) | (neighborsToSend & 0x07);
  
  // Byte 8: Data mode (will be set below)
  // Bytes 9-10: Hop decision target ID (will be set below)
  // Byte 11: Stratum (bits 7-6) + reserved (bits 5-1) + TimeSyncFlag (bit 0)
  // Stratum encoding: 0=GATEWAY, 1=DIRECT, 2=INDIRECT, 3=LOCAL
  #if ENABLE_WIFI == 1
    txBuffer[11] = ((myInfo.syncStratum & 0x03) << 6) | (timeSynced ? 0x01 : 0x00);
  #else
    txBuffer[11] = ((myInfo.syncStratum & 0x03) << 6);
  #endif
  
  // NEIGHBOR SECTION (24 bytes: 12-35, max 6 neighbors)
  uint8_t byteIdx = 12;
  for (uint8_t i = 0; i < neighborsToSend; i++) {
    uint8_t idx = neighbourIndices[i];
    txBuffer[byteIdx] = (uint8_t)((neighbours[idx].id >> 8) & 0xFF);
    txBuffer[byteIdx + 1] = (uint8_t)((neighbours[idx].id) & 0xFF);
    txBuffer[byteIdx + 2] = neighbours[idx].slotIndex;
    txBuffer[byteIdx + 3] = (neighbours[idx].isLocalized << 7) | neighbours[idx].hoppingDistance;
    byteIdx += 4;
  }
  
  // DATA SECTION (20 bytes: 28-47)
  // Determine data mode and content
  uint8_t dataMode = DATA_MODE_NONE;
  uint16_t hopDecisionTarget = 0;
  uint16_t origSender = 0;
  uint16_t msgId = 0;
  uint8_t hopCount = 0;
  uint8_t dataLen = 0;
  char dataToSend[SENSOR_DATA_LENGTH + 1] = {0};
  uint16_t tracking[MAX_TRACKING_HOPS] = {0};
  #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
    int64_t embeddedTxTimestamp = 0;  // For forwarding
  #endif
  
  // Priority 1: Check forward queue (send one forwarded message per cycle)
  ForwardMessage fwdMsg;
  if (forwardQueueCount > 0 && myInfo.hoppingDistance != 0x7F && myInfo.hoppingDistance != 0) {
    if (dequeueForward(&fwdMsg)) {
      dataMode = DATA_MODE_FORWARD;
      origSender = fwdMsg.originalSender;
      msgId = fwdMsg.messageId;
      hopCount = fwdMsg.hopCount;
      dataLen = fwdMsg.dataLen;
      memcpy(dataToSend, fwdMsg.data, dataLen);
      memcpy(tracking, fwdMsg.tracking, sizeof(tracking));
      
      #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
        // Preserve original TX timestamp from forwarded message
        embeddedTxTimestamp = fwdMsg.txTimestampUs;
      #endif
      
      // Select next hop
      hopDecisionTarget = selectBestNextHop();
    }
  }
  
  // Priority 2: Own sensor data (only when queue is empty)
  if (dataMode == DATA_MODE_NONE && hasSensorDataToSend && 
      myInfo.hoppingDistance != 0x7F && myInfo.hoppingDistance != 0) {
    dataMode = DATA_MODE_OWN;
    origSender = ownMessageOrigSender;
    msgId = ownMessageId;
    hopCount = 1;
    dataLen = strlen(sensorDataToSend);
    if (dataLen > SENSOR_DATA_LENGTH) dataLen = SENSOR_DATA_LENGTH;
    memcpy(dataToSend, sensorDataToSend, dataLen);
    
    // Initialize tracking with own ID
    tracking[0] = myInfo.id;
    
    // Select next hop
    hopDecisionTarget = selectBestNextHop();
    
    // Store initial timestamp for latency tracking (will be embedded in packet)
    #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
      if (timeSynced) {
        int64_t txTimestampUs = getCurrentTimeUs();
        
        // Store in circular buffer for local tracking
        txTimestampCache[txTimestampCacheIndex].messageId = msgId;
        txTimestampCache[txTimestampCacheIndex].timestampUs = txTimestampUs;
        txTimestampCacheIndex = (txTimestampCacheIndex + 1) % LATENCY_CACHE_SIZE;
        if (txTimestampCacheCount < LATENCY_CACHE_SIZE) txTimestampCacheCount++;
        
        #if LATENCY_VERBOSE_LOG == 1
          char timeStr[32];
          formatTimestamp(txTimestampUs, timeStr, sizeof(timeStr));
          DEBUG_PRINT("[Node %d] [TX_TS] MsgID:%d T:%s\n", myInfo.id, msgId, timeStr);
        #endif
      }
    #endif
    
    hasSensorDataToSend = false;
  }
  
  // Set header bytes 8-10
  txBuffer[8] = dataMode;
  txBuffer[9] = (uint8_t)((hopDecisionTarget >> 8) & 0xFF);
  txBuffer[10] = (uint8_t)(hopDecisionTarget & 0xFF);
  
  if (dataMode != DATA_MODE_NONE) {
    // Data payload structure (28-47, 20 bytes):
    // 28-29: Original sender ID
    // 30-31: Message ID
    // 32: Hop count
    // 33: Data length
    // 34-35: Tracking[0]
    // 36-37: Tracking[1]
    // 38-39: Tracking[2]
    // 40-45: Sensor data (6 bytes)
    // 46-47: Reserved
    
    txBuffer[28] = (uint8_t)((origSender >> 8) & 0xFF);
    txBuffer[29] = (uint8_t)(origSender & 0xFF);
    txBuffer[30] = (uint8_t)((msgId >> 8) & 0xFF);
    txBuffer[31] = (uint8_t)(msgId & 0xFF);
    txBuffer[32] = hopCount;
    txBuffer[33] = dataLen;
    
    // Tracking
    for (uint8_t i = 0; i < MAX_TRACKING_HOPS; i++) {
      txBuffer[34 + i*2] = (uint8_t)((tracking[i] >> 8) & 0xFF);
      txBuffer[35 + i*2] = (uint8_t)(tracking[i] & 0xFF);
    }
    
    // Embed TX timestamp for DATA_MODE_OWN (bytes 40-47)
    #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
      if (timeSynced && dataMode == DATA_MODE_OWN) {
        // Get current timestamp when sending
        int64_t txTimestampUs = getCurrentTimeUs();
        
        // Embed timestamp in packet (8 bytes: 40-47)
        txBuffer[40] = (uint8_t)((txTimestampUs >> 56) & 0xFF);
        txBuffer[41] = (uint8_t)((txTimestampUs >> 48) & 0xFF);
        txBuffer[42] = (uint8_t)((txTimestampUs >> 40) & 0xFF);
        txBuffer[43] = (uint8_t)((txTimestampUs >> 32) & 0xFF);
        txBuffer[44] = (uint8_t)((txTimestampUs >> 24) & 0xFF);
        txBuffer[45] = (uint8_t)((txTimestampUs >> 16) & 0xFF);
        txBuffer[46] = (uint8_t)((txTimestampUs >> 8) & 0xFF);
        txBuffer[47] = (uint8_t)(txTimestampUs & 0xFF);
        
        #if LATENCY_VERBOSE_LOG == 1
          char timeStr[32];
          formatTimestamp(txTimestampUs, timeStr, sizeof(timeStr));
          DEBUG_PRINT("[Node %d] [TX_TS_EMBED] MsgID:%d T:%s\n", myInfo.id, msgId, timeStr);
        #endif
      } else if (dataMode == DATA_MODE_FORWARD && embeddedTxTimestamp > 0) {
        // Forward: preserve the original TX timestamp from sender
        txBuffer[40] = (uint8_t)((embeddedTxTimestamp >> 56) & 0xFF);
        txBuffer[41] = (uint8_t)((embeddedTxTimestamp >> 48) & 0xFF);
        txBuffer[42] = (uint8_t)((embeddedTxTimestamp >> 40) & 0xFF);
        txBuffer[43] = (uint8_t)((embeddedTxTimestamp >> 32) & 0xFF);
        txBuffer[44] = (uint8_t)((embeddedTxTimestamp >> 24) & 0xFF);
        txBuffer[45] = (uint8_t)((embeddedTxTimestamp >> 16) & 0xFF);
        txBuffer[46] = (uint8_t)((embeddedTxTimestamp >> 8) & 0xFF);
        txBuffer[47] = (uint8_t)(embeddedTxTimestamp & 0xFF);
        
        #if LATENCY_VERBOSE_LOG == 1
          DEBUG_PRINT("[Node %d] [FWD_TS] MsgID:%d preserving TX timestamp\n", myInfo.id, msgId);
        #endif
      }
    #endif
    
    // Stratum names for display
    const char* stratumNames[] = {"GW", "D1", "D2", "LC"};
    Serial.printf("[Node %d] [TX] slot:%d hop:%d cycle:%d nbr:%d stratum:%s(%d) | %s: MsgID:%d orig:%d hops:%d target:%d\n", 
                  myInfo.id, myInfo.slotIndex, myInfo.hoppingDistance, myInfo.syncedCycle, neighborsToSend,
                  stratumNames[myInfo.syncStratum], myInfo.syncStratum,
                  (dataMode == DATA_MODE_OWN) ? "OWN" : "FWD",
                  msgId, origSender, hopCount, hopDecisionTarget);
    
    strcpy(nodeStatus, (dataMode == DATA_MODE_FORWARD) ? "TX_FWD" : "TX_DATA");
  } else {
    const char* stratumNames[] = {"GW", "D1", "D2", "LC"};
    Serial.printf("[Node %d] [TX] slot:%d hop:%d cycle:%d nbr:%d stratum:%s(%d) | NO_DATA\n", 
                  myInfo.id, myInfo.slotIndex, myInfo.hoppingDistance, myInfo.syncedCycle, neighborsToSend,
                  stratumNames[myInfo.syncStratum], myInfo.syncStratum);
    strcpy(nodeStatus, "TX_ID");
  }
  
  // Ra01S: Send with synchronous mode (blocks until TX complete)
  uint32_t txStart = micros();
  bool txSuccess = radio.Send(txBuffer, FIXED_PACKET_LENGTH, SX126x_TXMODE_SYNC);
  lastTxDuration_us = micros() - txStart;
  
  if (txSuccess) {
    txPacketCount++;
  } else {
    Serial.printf("[Node %d] [RADIO_TX] FAILED!\n", myInfo.id);
  }
}

// Process received unified packet
uint8_t processRxPacket() {
  uint8_t selectedNeighbourIdx = 0;
  
  // PARSE HEADER (12 bytes)
  uint16_t senderId = (rxBuffer[3] << 8) | rxBuffer[4];
  uint8_t senderSlot = rxBuffer[5];
  uint8_t senderHop = rxBuffer[6] & 0x7F;
  bool senderLocalized = (rxBuffer[6] >> 7) & 0x01;
  uint8_t senderCycle = (rxBuffer[7] >> 3) & 0x1F;
  uint8_t numNeighborsInPacket = rxBuffer[7] & 0x07;
  uint8_t dataMode = rxBuffer[8];
  uint16_t hopDecisionTarget = (rxBuffer[9] << 8) | rxBuffer[10];
  
  // Parse byte 11: Stratum (bits 7-6) + TimeSyncFlag (bit 0)
  uint8_t senderStratum = (rxBuffer[11] >> 6) & 0x03;
  bool senderTimeSynced = rxBuffer[11] & 0x01;
  
  if (numNeighborsInPacket > MAX_NEIGHBOURS_IN_PACKET) {
    numNeighborsInPacket = MAX_NEIGHBOURS_IN_PACKET;
  }
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] [RX_PKT] from ID:%d slot:%d hop:%d cycle:%d nbr:%d RSSI:%d SNR:%d\n",
                  myInfo.id, senderId, senderSlot, senderHop, senderCycle, numNeighborsInPacket, rxRssi, rxSnr);
  #endif
  
  // RSSI FILTER: Ignore packets with RSSI below threshold
  if (rxRssi < RSSI_THRESHOLD_DBM) {
    #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
      // Quick non-blocking event (pre-formatted to minimize processing)
      char rssiEvent[96];
      snprintf(rssiEvent, sizeof(rssiEvent), 
              "From:N%d,RSSI:%ddBm,Threshold:%ddBm,Status:REJECTED",
              senderId, rxRssi, RSSI_THRESHOLD_DBM);
      sendWifiEvent("RSSI_LOW", rssiEvent);  // Non-blocking queue
    #endif
    
    #ifdef VERBOSE
      Serial.printf("[Node %d] [RX_REJECT] RSSI too low from Node %d: %d < %d dBm\n",
                    myInfo.id, senderId, rxRssi, RSSI_THRESHOLD_DBM);
    #endif
    
    return 255;  // Return invalid slot, ignore this packet completely
  }
  
  // UPDATE/ADD SENDER TO NEIGHBOR LIST
  bool foundSender = false;
  for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].id == senderId) {
      selectedNeighbourIdx = i;
      foundSender = true;
      break;
    }
  }
  
  bool isNewNeighbor = false;
  if (!foundSender) {
    for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
      if (neighbours[i].id == 0) {
        selectedNeighbourIdx = i;
        foundSender = true;
        neighbourCount++;
        isNewNeighbor = true;
        break;
      }
    }
  }
  
  if (foundSender) {
    neighbours[selectedNeighbourIdx].id = senderId;
    neighbours[selectedNeighbourIdx].slotIndex = senderSlot;
    neighbours[selectedNeighbourIdx].hoppingDistance = senderHop;
    neighbours[selectedNeighbourIdx].isLocalized = senderLocalized;
    
    // Update cycle and track sequence
    uint8_t prevCycle = neighbours[selectedNeighbourIdx].syncedCycle;
    neighbours[selectedNeighbourIdx].syncedCycle = senderCycle;
    
    // Add to cycle history buffer (3 cycles for faster sync validation)
    neighbours[selectedNeighbourIdx].cycleHistory[neighbours[selectedNeighbourIdx].cycleHistoryIdx] = senderCycle;
    neighbours[selectedNeighbourIdx].cycleHistoryIdx = (neighbours[selectedNeighbourIdx].cycleHistoryIdx + 1) % 3;
    
    // Validate if cycles are sequential (allowing wrap-around)
    bool sequential = true;
    uint8_t validCount = 0;
    for (uint8_t k = 0; k < 3; k++) {
      if (neighbours[selectedNeighbourIdx].cycleHistory[k] != 255) validCount++;
    }
    
    if (validCount >= 3) {  // Need all 3 cycles to validate sequence
      // Check consecutive pattern
      for (uint8_t k = 0; k < 2; k++) {
        uint8_t curr = neighbours[selectedNeighbourIdx].cycleHistory[k];
        uint8_t next = neighbours[selectedNeighbourIdx].cycleHistory[k + 1];
        if (curr != 255 && next != 255) {
          // Check if next is consecutive (with wrap-around at AUTO_SEND_INTERVAL_CYCLES)
          uint8_t expected = (curr + 1) % AUTO_SEND_INTERVAL_CYCLES;
          if (next != expected) {
            sequential = false;
            break;
          }
        }
      }
    } else {
      sequential = false;  // Not enough data
    }
    
    neighbours[selectedNeighbourIdx].cyclesSequential = sequential;
    
    // Track neighbor's stratum for sync decisions
    neighbours[selectedNeighbourIdx].syncStratum = senderStratum;
    
    neighbours[selectedNeighbourIdx].rssi = rxRssi;
    neighbours[selectedNeighbourIdx].snr = rxSnr;
    neighbours[selectedNeighbourIdx].activityCounter = 0;
    
    
    #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
      if (isNewNeighbor) {
        char eventDetails[96];
        snprintf(eventDetails, sizeof(eventDetails), 
                "NodeID:%d,RSSI:%ddBm,Slot:%d,Hop:%d",
                senderId, rxRssi, senderSlot, senderHop);
        sendWifiEvent("NEIGHBOR_ADDED", eventDetails);
      }
    #endif
    
    #if ENABLE_SYNC_LOG == 1
      // Log neighbor discovery/update
      char syncDetail[64];
      snprintf(syncDetail, sizeof(syncDetail), "NBR_UPD:Slot=%d,Hop=%d,Cycle=%d,RSSI=%d", 
               senderSlot, senderHop, senderCycle, rxRssi);
      logSyncEvent("RX_NEIGHBOR", senderId, syncDetail);
    #endif
    
    // PARSE SENDER'S NEIGHBORS
    neighbours[selectedNeighbourIdx].numberOfNeighbours = numNeighborsInPacket;
    neighbours[selectedNeighbourIdx].amIListedAsNeighbour = false;
    
    uint8_t byteIdx = 12;
    for (uint8_t i = 0; i < numNeighborsInPacket; i++) {
      uint16_t neighborId = (rxBuffer[byteIdx] << 8) | rxBuffer[byteIdx + 1];
      uint8_t neighborSlot = rxBuffer[byteIdx + 2];
      uint8_t neighborHopInfo = rxBuffer[byteIdx + 3];
      uint8_t neighborHop = neighborHopInfo & 0x7F;
      bool neighborLocalized = (neighborHopInfo >> 7) & 0x01;
      
      neighbours[selectedNeighbourIdx].neighboursId[i] = neighborId;
      neighbours[selectedNeighbourIdx].neighboursSlot[i] = neighborSlot;
      neighbours[selectedNeighbourIdx].neighboursHoppingDistance[i] = neighborHop;
      neighbours[selectedNeighbourIdx].neighboursIsLocalized[i] = neighborLocalized;
      
      if (neighborId == myInfo.id) {
        neighbours[selectedNeighbourIdx].amIListedAsNeighbour = true;
        neighbours[selectedNeighbourIdx].isBidirectional = true;
        
        #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
          char bidirDetail[96];
          snprintf(bidirDetail, sizeof(bidirDetail), 
                  "NodeID:%d,RSSI:%ddBm,Status:BIDIRECTIONAL",
                  senderId, rxRssi);
          sendWifiEvent("BIDIR_LINK", bidirDetail);
        #endif
        
        #if ENABLE_SYNC_LOG == 1
          char syncDetail[64];
          snprintf(syncDetail, sizeof(syncDetail), "BIDIR_CONFIRMED:From=%d", senderId);
          logSyncEvent("BIDIR_LINK", myInfo.id, syncDetail);
        #endif
      }
      
      byteIdx += 4;
    }
    
    // ============= HIERARCHICAL SYNC LOGIC =============
    // Sync only requires RECEIVING packet from gateway or better-synced node
    // No bidirectional requirement - sync is one-way (RX from upstream)
    // Once all nodes sync, reliable TDMA communication can begin
    #if IS_REFERENCE == 0
    {
      bool shouldSync = false;
      uint8_t newStratum = myInfo.syncStratum;
      
      // Priority 1: Direct sync from Gateway (senderId == 1)
      if (senderId == 1) {
        // Gateway packet received = can sync directly
        newStratum = STRATUM_DIRECT;
        shouldSync = true;
        
      }
      // Priority 2: Sync from node with better stratum
      else if (senderStratum < myInfo.syncStratum && senderStratum < STRATUM_LOCAL) {
        // Inherit stratum+1 from better-synced neighbor
        newStratum = senderStratum + STRATUM_INHERIT_DELTA;
        if (newStratum > STRATUM_INDIRECT) newStratum = STRATUM_INDIRECT;  // Cap at stratum 2
        shouldSync = true;
        
      }
      // Priority 3: Refresh sync counter if same or better stratum source heard
      else if (senderStratum <= myInfo.syncStratum && myInfo.syncSource == senderId) {
        // Refresh from same source
        myInfo.syncValidCounter = SYNC_VALID_CYCLES;
      }
      
      if (shouldSync) {
        uint8_t oldStratum = myInfo.syncStratum;
        myInfo.syncStratum = newStratum;
        myInfo.syncSource = senderId;
        myInfo.syncValidCounter = SYNC_VALID_CYCLES;
        myInfo.syncedWithGateway = (newStratum < STRATUM_LOCAL);
        
        Serial.printf("[Node %d] [STRATUM] Synced: stratum %d->%d from Node %d\n",
                      myInfo.id, oldStratum, newStratum, senderId);
      }
    }
    #endif
    // ============= END HIERARCHICAL SYNC LOGIC =============
    
    // HOP DISTANCE CALCULATION (Bellman-Ford)
    // Hop distance also only needs RX - routing path calculated from received info
    #if IS_REFERENCE == 0
    {
      if (neighbours[selectedNeighbourIdx].hoppingDistance != 0x7F &&
          neighbours[selectedNeighbourIdx].rssi >= RSSI_THRESHOLD_DBM) {
        uint8_t newHop = neighbours[selectedNeighbourIdx].hoppingDistance + 1;
        if (newHop < myInfo.hoppingDistance) {
          myInfo.hoppingDistance = newHop;
          Serial.printf("[Node %d] [HOP] Updated to %d via node %d (RSSI:%d)\n", 
                        myInfo.id, myInfo.hoppingDistance, senderId, neighbours[selectedNeighbourIdx].rssi);
        }
      } else if (neighbours[selectedNeighbourIdx].rssi < RSSI_THRESHOLD_DBM) {
        Serial.printf("[Node %d] [HOP_SKIP] Node %d - RSSI too low (%d)\n",
                      myInfo.id, senderId, neighbours[selectedNeighbourIdx].rssi);
      }
    }
      
      // CYCLE SYNCHRONIZATION: Sync from neighbors with lower hop distance (closer to gateway)
      // Only requires RX from upstream node - no bidirectional needed
      if (neighbours[selectedNeighbourIdx].hoppingDistance < myInfo.hoppingDistance &&
          neighbours[selectedNeighbourIdx].rssi >= RSSI_THRESHOLD_DBM) {
        // Cycle validation logic: check for sequential consistency
        if (!cycleValidated) {
          // Check if cycle is sequential (0->1->2->3->4)
          if (lastReceivedCycle == -1) {
            // First cycle received
            lastReceivedCycle = senderCycle;
            cycleValidationCount = 1;
            Serial.printf("[Node %d] [CYCLE_VAL] First cycle: %d\n", myInfo.id, senderCycle);
            
            #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
              char detail[64];
              snprintf(detail, sizeof(detail), "FirstCycle=%d,From=%d,RSSI=%d", senderCycle, senderId, neighbours[selectedNeighbourIdx].rssi);
              sendWifiEvent("CYCLE_VAL", detail);
            #endif
            
            #if ENABLE_SYNC_LOG == 1
              char detail[64];
              snprintf(detail, sizeof(detail), "FIRST_CYCLE=%d,FromNode=%d", senderCycle, senderId);
              logSyncEvent("CYCLE_INIT", myInfo.id, detail);
            #endif
          } else {
            // Check if next cycle is sequential
            int8_t expectedCycle = (lastReceivedCycle + 1) % AUTO_SEND_INTERVAL_CYCLES;
            if (senderCycle == expectedCycle) {
              cycleValidationCount++;
              lastReceivedCycle = senderCycle;
              Serial.printf("[Node %d] [CYCLE_VAL] Sequential OK: %d (%d/%d)\n", 
                          myInfo.id, senderCycle, cycleValidationCount, CYCLE_VALIDATION_THRESHOLD);
              
              #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
                char detail[64];
                snprintf(detail, sizeof(detail), "Cycle=%d,Progress=%d/%d,From=%d,RSSI=%d", 
                         senderCycle, cycleValidationCount, CYCLE_VALIDATION_THRESHOLD, senderId, neighbours[selectedNeighbourIdx].rssi);
                sendWifiEvent("CYCLE_VAL", detail);
              #endif
              
              if (cycleValidationCount >= CYCLE_VALIDATION_THRESHOLD) {
                cycleValidated = true;
                Serial.printf("[Node %d] [CYCLE_VAL] ✓ Validation complete! Ready for sequential TX\n", myInfo.id);
                
                #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
                  char vDetail[64];
                  snprintf(vDetail, sizeof(vDetail), "VALIDATED! Cycle=%d,AutoSend=READY", senderCycle);
                  sendWifiEvent("CYCLE_VAL", vDetail);
                #endif
              }
            } else {
              // Not sequential, reset validation
              Serial.printf("[Node %d] [CYCLE_VAL] ✗ Not sequential: got %d, expected %d. Resetting...\n", 
                          myInfo.id, senderCycle, expectedCycle);
              
              #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
                char detail[64];
                snprintf(detail, sizeof(detail), "RESET! Got=%d,Exp=%d,From=%d,RSSI=%d", 
                         senderCycle, expectedCycle, senderId, neighbours[selectedNeighbourIdx].rssi);
                sendWifiEvent("CYCLE_VAL", detail);
              #endif
              
              lastReceivedCycle = senderCycle;
              cycleValidationCount = 1;
            }
          }
        }
        
        // Always update current cycle
        if (myInfo.syncedCycle != senderCycle) {
          myInfo.syncedCycle = senderCycle;
          Serial.printf("[Node %d] [CYCLE_SYNC] Aligned to cycle %d from node %d (hop %d)\n", 
                        myInfo.id, myInfo.syncedCycle, senderId, neighbours[selectedNeighbourIdx].hoppingDistance);
          
          #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
            char detail[64];
            snprintf(detail, sizeof(detail), "Cycle=%d,From=%d,Hop=%d,RSSI=%d", 
                     senderCycle, senderId, neighbours[selectedNeighbourIdx].hoppingDistance, neighbours[selectedNeighbourIdx].rssi);
            sendWifiEvent("CYCLE_SYNC", detail);
          #endif
        }
      }
    #endif
    
    // PARSE DATA SECTION (if present)
    if (dataMode == DATA_MODE_OWN || dataMode == DATA_MODE_FORWARD) {
      uint16_t origSender = (rxBuffer[28] << 8) | rxBuffer[29];
      uint16_t msgId = (rxBuffer[30] << 8) | rxBuffer[31];
      uint8_t hopCount = rxBuffer[32];
      uint8_t dataLen = rxBuffer[33];
      if (dataLen > SENSOR_DATA_LENGTH) dataLen = SENSOR_DATA_LENGTH;
      
      // Parse tracking
      uint16_t tracking[MAX_TRACKING_HOPS];
      for (uint8_t i = 0; i < MAX_TRACKING_HOPS; i++) {
        tracking[i] = (rxBuffer[34 + i*2] << 8) | rxBuffer[35 + i*2];
      }
      
      // Parse data
      memset(sensorDataReceived, 0, sizeof(sensorDataReceived));
      for (uint8_t i = 0; i < dataLen; i++) {
        sensorDataReceived[i] = (char)rxBuffer[40 + i];
      }
      sensorDataReceived[dataLen] = '\0';
      
      Serial.printf("[Node %d] [RX_DATA] %s MsgID:%d orig:%d hops:%d target:%d data:%s\n",
                    myInfo.id, 
                    (dataMode == DATA_MODE_OWN) ? "OWN" : "FWD",
                    msgId, origSender, hopCount, hopDecisionTarget, sensorDataReceived);
      
      // GATEWAY BEHAVIOR (hop = 0)
      if (myInfo.hoppingDistance == 0) {
        // Skip loopback packets (gateway's own transmissions)
        if (origSender == myInfo.id) {
          Serial.printf("[Node %d] [LOOPBACK] Ignoring own packet MsgID:%d\n", myInfo.id, msgId);
          return 0; // Exit early
        }
        
        #if ENABLE_PDR_TRACKING == 1
          // Update PDR statistics for this sender
          updatePdrStats(origSender, msgId);
          
          // Send PDR update via WiFi (WIFI_MONITOR mode)
          #if DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR && ENABLE_WIFI == 1
            // Send individual packet reception event
            if (wifiEventQueue != NULL && WiFi.status() == WL_CONNECTED) {
              WiFiEvent evt;
              int64_t timestamp = timeSynced ? getCurrentTimeUs() : (int64_t)micros();
              
              // Find this node's PDR stats
              for (uint8_t i = 0; i < pdrNodeCount; i++) {
                if (pdrStats[i].nodeId == origSender) {
                  snprintf(evt.message, sizeof(evt.message), 
                           "PKT_RX,%lld,%d,From:%d,MsgID:%d,Seq:%d,PDR:%.1f%%",
                           timestamp, myInfo.id, origSender, msgId, 
                           pdrStats[i].lastSeqReceived, pdrStats[i].pdr);
                  xQueueSend(wifiEventQueue, &evt, 0);
                  break;
                }
              }
            }
          #endif
        #endif
        
        #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
          // Calculate end-to-end latency if time synced
          if (timeSynced) {
            int64_t rxTimestampUs = getCurrentTimeUs();
            int64_t latencyUs = -1;
            int64_t txTimestampUs = 0;
            
            // Extract embedded TX timestamp from packet (bytes 40-47)
            txTimestampUs = ((int64_t)rxBuffer[40] << 56) |
                           ((int64_t)rxBuffer[41] << 48) |
                           ((int64_t)rxBuffer[42] << 40) |
                           ((int64_t)rxBuffer[43] << 32) |
                           ((int64_t)rxBuffer[44] << 24) |
                           ((int64_t)rxBuffer[45] << 16) |
                           ((int64_t)rxBuffer[46] << 8) |
                           ((int64_t)rxBuffer[47]);
            
            // Validate timestamp (should be reasonable - within last hour)
            int64_t currentTime = getCurrentTimeUs();
            int64_t timeDiff = currentTime - txTimestampUs;
            
            if (txTimestampUs > 0 && timeDiff > 0 && timeDiff < 3600000000LL) {
              latencyUs = timeDiff;
            } else {
              #if LATENCY_VERBOSE_LOG == 1
                DEBUG_PRINT("[GW %d] [LATENCY] Invalid TX timestamp: %lld (diff: %lld)\n", 
                           myInfo.id, txTimestampUs, timeDiff);
              #endif
            }
            
            // Calculate and log latency
            if (latencyUs >= 0) {
              double latencyMs = latencyUs / 1000.0;
              
              // Update statistics
              totalLatencyCalculations++;
              totalLatencyUs += latencyUs;
              if (latencyUs < minLatencyUs) minLatencyUs = latencyUs;
              if (latencyUs > maxLatencyUs) maxLatencyUs = latencyUs;
              
              // Store in circular buffer
              latencyRecords[latencyRecordIndex].messageId = msgId;
              latencyRecords[latencyRecordIndex].origSender = origSender;
              latencyRecords[latencyRecordIndex].txTimestampUs = txTimestampUs;
              latencyRecords[latencyRecordIndex].rxTimestampUs = rxTimestampUs;
              latencyRecords[latencyRecordIndex].hopCount = hopCount;
              latencyRecords[latencyRecordIndex].latencyUs = latencyUs;
              latencyRecordIndex = (latencyRecordIndex + 1) % LATENCY_CACHE_SIZE;
              if (latencyRecordCount < LATENCY_CACHE_SIZE) latencyRecordCount++;
              
              // Update per-node latency statistics
              #if ENABLE_PDR_TRACKING == 1
                updateNodeLatency(origSender, latencyUs);
              #endif
              
              #if LATENCY_VERBOSE_LOG == 1
                // Full logging (can add overhead)
                char rxTimeStr[32], txTimeStr[32];
                formatTimestamp(rxTimestampUs, rxTimeStr, sizeof(rxTimeStr));
                formatTimestamp(txTimestampUs, txTimeStr, sizeof(txTimeStr));
                DEBUG_PRINT("[GW %d] [RX_TS] %s\n", myInfo.id, rxTimeStr);
                DEBUG_PRINT_LATENCY("[GW %d] [LATENCY] MsgID:%d E2E:%.3fms (%dhop) TX:%s\n", 
                              myInfo.id, msgId, latencyMs, hopCount, txTimeStr);
              #else
                // Minimal logging (reduce overhead)
                DEBUG_PRINT_LATENCY("[GW] LAT:%d %.1fms %dh\n", msgId, latencyMs, hopCount);
              #endif
              
              // Log to data collection queue (only in GATEWAY_ONLY mode)
              #if DEBUG_MODE == DEBUG_MODE_GATEWAY_ONLY
                logPacketData(origSender, msgId, hopCount, latencyUs, rxRssi, rxSnr);
              #endif
              
              // Send latency data via WiFi (WIFI_MONITOR mode)
              #if DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
                sendLatencyDataWifi(origSender, msgId, hopCount, latencyUs, rxRssi, rxSnr);
              #endif
              
              // WiFi event: Gateway received with routing info
              #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
                char routePath[64] = "";
                for (uint8_t i = 0; i < hopCount && i < MAX_TRACKING_HOPS; i++) {
                  if (tracking[i] > 0) {
                    char nodeStr[8];
                    snprintf(nodeStr, sizeof(nodeStr), "%d%s", tracking[i], (i < hopCount-1) ? ">" : "");
                    strcat(routePath, nodeStr);
                  }
                }
                char details[256];
                int64_t timestamp = timeSynced ? getCurrentTimeUs() : (int64_t)micros();
                snprintf(details, sizeof(details), 
                         "Msg:%d,From:%d,Hops:%d,Route:[%s>GW],Lat:%.1fms,RSSI:%d,TS:%lld",
                         msgId, origSender, hopCount, routePath, latencyMs, rxRssi, timestamp);
                sendWifiEvent("GW_RX_DATA", details);
              #endif
            } else if (origSender != myInfo.id) {
              // Only log if it's not from gateway itself (reduce spam)
              #if LATENCY_VERBOSE_LOG == 1
                DEBUG_PRINT("[GW %d] [LATENCY] MsgID:%d - TX timestamp N/A (remote)\n", myInfo.id, msgId);
              #endif
            }
          }
        #endif
        
        DEBUG_PRINT("[GW] RX:%d from:%d %s\n", msgId, origSender, sensorDataReceived);
        
        #if ENABLE_WIFI == 1
          // Add to WiFi batch buffer for later sending
          if (wifiBatchCount < WIFI_BATCH_SIZE) {
            wifiBatchBuffer[wifiBatchCount].origSender = origSender;
            wifiBatchBuffer[wifiBatchCount].messageId = msgId;
            strncpy(wifiBatchBuffer[wifiBatchCount].data, sensorDataReceived, SENSOR_DATA_LENGTH);
            wifiBatchBuffer[wifiBatchCount].data[SENSOR_DATA_LENGTH] = '\0';
            wifiBatchBuffer[wifiBatchCount].trackingLen = (hopCount < MAX_TRACKING_HOPS) ? hopCount : MAX_TRACKING_HOPS;
            for (uint8_t i = 0; i < wifiBatchBuffer[wifiBatchCount].trackingLen; i++) {
              wifiBatchBuffer[wifiBatchCount].tracking[i] = tracking[i];
            }
            wifiBatchCount++;
            Serial.printf("[Node %d] [WIFI] Buffered message (batch: %d/%d)\n", 
                          myInfo.id, wifiBatchCount, WIFI_BATCH_SIZE);
          } else {
            Serial.printf("[Node %d] [WIFI] Buffer full, message dropped!\n", myInfo.id);
          }
        #endif
        
      } else {
        // NON-GATEWAY BEHAVIOR
        // Check if this packet is intended for me
        bool isForMe = (hopDecisionTarget == myInfo.id);
        
        if (!isForMe) {
          // Not for me - only use for sync/neighbor discovery
          Serial.printf("[Node %d] [RX_DATA] Packet target:%d (not for me), ignoring data\n",
                        myInfo.id, hopDecisionTarget);
        } else {
          // Packet is for me - enqueue for forwarding
          Serial.printf("[Node %d] [RX_DATA] Packet is for me, enqueueing\n", myInfo.id);
          
          ForwardMessage fwdMsg;
          fwdMsg.originalSender = origSender;
          fwdMsg.messageId = msgId;
          fwdMsg.hopCount = hopCount + 1;
          fwdMsg.dataLen = dataLen;
          memcpy(fwdMsg.data, sensorDataReceived, dataLen);
          
          // Update tracking: add my ID to the path
          memcpy(fwdMsg.tracking, tracking, sizeof(tracking));
          if (hopCount < MAX_TRACKING_HOPS) {
            fwdMsg.tracking[hopCount] = myInfo.id;
          }
          
          // Preserve embedded TX timestamp for forwarding
          #if ENABLE_WIFI == 1 && ENABLE_LATENCY_CALC == 1
            // Extract timestamp from bytes 40-47 of received packet
            fwdMsg.txTimestampUs = ((int64_t)rxBuffer[40] << 56) |
                                  ((int64_t)rxBuffer[41] << 48) |
                                  ((int64_t)rxBuffer[42] << 40) |
                                  ((int64_t)rxBuffer[43] << 32) |
                                  ((int64_t)rxBuffer[44] << 24) |
                                  ((int64_t)rxBuffer[45] << 16) |
                                  ((int64_t)rxBuffer[46] << 8) |
                                  ((int64_t)rxBuffer[47]);
          #endif
          
          enqueueForward(&fwdMsg);
          
          // WiFi event: Forwarding packet
          #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
            char details[128];
            snprintf(details, sizeof(details), "Msg:%d,From:%d,Hop:%d,NextHop:TBD",
                     msgId, origSender, hopCount + 1);
            sendWifiEvent("FORWARD_ENQUEUE", details);
          #endif
        }
      }
    }
  } else {
    Serial.println("[RX_WARN] Neighbor list full, cannot add sender!");
  }
  
  return senderSlot;
}

// ============= NEIGHBOR STATUS UPDATE =============
// ============= RE-CALCULATE HOP COUNT =============
// Re-calculate hop distance using Bellman-Ford algorithm
// Called every cycle in processing phase
void recalculateHopCount() {
  #if IS_REFERENCE == 0  // Only non-gateway nodes
    uint8_t oldHop = myInfo.hoppingDistance;
    uint8_t minHop = 0x7F;  // Start with max value
    
    // Find minimum hop distance from valid neighbors
    for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
      if (neighbours[i].id != 0 && 
          neighbours[i].hoppingDistance != 0x7F &&
          neighbours[i].rssi >= RSSI_THRESHOLD_DBM) {  // Filter by RSSI
        
        uint8_t candidateHop = neighbours[i].hoppingDistance + 1;
        if (candidateHop < minHop) {
          minHop = candidateHop;
        }
      }
    }
    
    // Update hop count if changed
    if (minHop != myInfo.hoppingDistance && minHop != 0x7F) {
      myInfo.hoppingDistance = minHop;
      Serial.printf("[Node %d] [HOP_RECALC] %d -> %d\n", myInfo.id, oldHop, minHop);
      
      #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
        char detail[64];
        snprintf(detail, sizeof(detail), "Old=%d,New=%d", oldHop, minHop);
        sendWifiEvent("HOP_CHANGE", detail);
      #endif
    }
  #endif
}

void updateNeighbourStatus() {
  
  for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].id != 0) {
      neighbours[i].activityCounter++;
      
      // Remove if inactive OR RSSI too low
      if (neighbours[i].activityCounter >= MAX_INACTIVE_CYCLES) {
        Serial.printf("[Node %d] [TIMEOUT] Removing inactive neighbor %d\n", myInfo.id, neighbours[i].id);
        
        #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
          char eventDetails[128];
          snprintf(eventDetails, sizeof(eventDetails), 
                  "NodeID:%d,Reason:INACTIVE,Duration:%dms,RSSI:%ddBm,WasBidir:%s",
                  neighbours[i].id, neighbours[i].activityCounter * CYCLE_DURATION_MS, 
                  neighbours[i].rssi, neighbours[i].isBidirectional ? "YES" : "NO");
          sendWifiEvent("NEIGHBOR_REMOVED", eventDetails);
        #endif
        
        memset(&neighbours[i], 0, sizeof(NeighbourInfo));
        neighbourCount = max(0, (int)neighbourCount - 1);
      } else if (neighbours[i].rssi < RSSI_THRESHOLD_DBM) {
        Serial.printf("[Node %d] [RSSI_LOW] Removing neighbor %d (RSSI:%d < %d)\n", 
                      myInfo.id, neighbours[i].id, neighbours[i].rssi, RSSI_THRESHOLD_DBM);
        
        #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
          char eventDetails[96];
          snprintf(eventDetails, sizeof(eventDetails), 
                  "NodeID:%d,Reason:RSSI_LOW,RSSI:%ddBm,Threshold:%ddBm",
                  neighbours[i].id, neighbours[i].rssi, RSSI_THRESHOLD_DBM);
          sendWifiEvent("NEIGHBOR_REMOVED", eventDetails);
        #endif
        
        memset(&neighbours[i], 0, sizeof(NeighbourInfo));
        neighbourCount = max(0, (int)neighbourCount - 1);
      }
    }
    yield();
  }
  
  // Rebuild neighbor indices
  neighbourCount = 0;
  for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].id != 0) {
      neighbourIndices[neighbourCount] = i;
      neighbourCount++;
    }
  }
  
  
  // Sort by hop distance
  for (uint8_t i = 0; i < neighbourCount - 1; i++) {
    for (uint8_t j = 0; j < neighbourCount - i - 1; j++) {
      uint8_t idx1 = neighbourIndices[j];
      uint8_t idx2 = neighbourIndices[j + 1];
      if (neighbours[idx1].hoppingDistance > neighbours[idx2].hoppingDistance) {
        uint8_t temp = neighbourIndices[j];
        neighbourIndices[j] = neighbourIndices[j + 1];
        neighbourIndices[j + 1] = temp;
      }
    }
  }
}

// ============= RESPONDER FUNCTION (RX with timeout) =============
// Ra01S version: Uses polling instead of callbacks
ResponderOutput responder(uint32_t timeoutMs) {
  ResponderOutput output;
  output.senderSlot = 255;
  output.adjustTiming = false;
  
  uint32_t rxStart = millis();
  uint32_t rxStartUs = micros();
  lastRxDuration_us = 0;
  
  // Ra01S: radio.Receive() polls for data (non-blocking)
  // It checks IRQ status and returns received bytes if available
  while (millis() - rxStart < timeoutMs) {
    // Poll for received data
    uint8_t rxLen = radio.Receive(rxBuffer, FIXED_PACKET_LENGTH);
    
    if (rxLen > 0) {
      lastRxDuration_us = micros() - rxStartUs;
      rxPacketLength = rxLen;
      
      // Get RSSI and SNR
      radio.GetPacketStatus(&rxRssi, &rxSnr);
      lastRssi = rxRssi;
      lastSnr = rxSnr;
      rxPacketCount++;
      
      // Parse packet
      uint16_t addr = (rxBuffer[0] << 8) | rxBuffer[1];
      uint8_t cmd = rxBuffer[2];
      
      #ifdef VERBOSE
        Serial.printf("[Node %d] [RX] Addr=%d Cmd=%d RSSI=%d SNR=%d\n", 
                      myInfo.id, addr, cmd, rxRssi, rxSnr);
      #endif
      
      if (addr == ADR_BROADCAST || addr == myInfo.id) {
        if (cmd == CMD_ID_AND_POS) {
          uint8_t senderSlot = processRxPacket();
          
          if (senderSlot != 255) {
            output.senderSlot = senderSlot;
            output.adjustTiming = true;
            
          }
          
          strcpy(nodeStatus, "RX_PKT");
          return output;
        }
      }
    }
    
    // Small delay to prevent busy-waiting, yield for watchdog
    delay(1);
    yield();
  }
  
  // Timeout
  strcpy(nodeStatus, "RX_TOUT");
  return output;
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== LoRa Mesh Node (Ra01S Library) ===");
  
  // ============= LOAD CONFIG FROM EEPROM =============
  configInit();  // Initialize EEPROM
  
  runtimeConfig = configLoad();
  if (runtimeConfig.valid) {
    // Use EEPROM config
    strncpy(activeSSID, runtimeConfig.ssid, MAX_SSID_LEN);
    strncpy(activePassword, runtimeConfig.password, MAX_PASS_LEN);
    strncpy(activeServerIP, runtimeConfig.serverIP, MAX_IP_LEN);
    activeDebugMode = runtimeConfig.debugMode;
    configLoaded = true;
    Serial.println("[CONFIG] Loaded from EEPROM");
  } else {
    // Use defaults from settings.h
    strncpy(activeSSID, WIFI_SSID, MAX_SSID_LEN);
    strncpy(activePassword, WIFI_PASS, MAX_PASS_LEN);
    strncpy(activeServerIP, SERVER_IP, MAX_IP_LEN);
    activeDebugMode = DEBUG_MODE;
    
    // Initialize runtime config with defaults
    strncpy(runtimeConfig.ssid, WIFI_SSID, MAX_SSID_LEN);
    strncpy(runtimeConfig.password, WIFI_PASS, MAX_PASS_LEN);
    strncpy(runtimeConfig.serverIP, SERVER_IP, MAX_IP_LEN);
    runtimeConfig.debugMode = DEBUG_MODE;
    
    configLoaded = false;
    Serial.println("[CONFIG] Using defaults from settings.h");
  }
  
  Serial.printf("[CONFIG] SSID: %s\n", activeSSID);
  Serial.printf("[CONFIG] Server: %s\n", activeServerIP);
  Serial.printf("[CONFIG] Mode: %d\n", activeDebugMode);
  Serial.println("[CONFIG] Type 'HELP' for serial commands");
  
  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("I2C initialized - SDA:%d SCL:%d\n", I2C_SDA, I2C_SCL);
  
  // Initialize components
  initEncoder();
  initDisplay();
  initLoRa();
  initMyInfo();
  
  // Initialize WiFi and NTP time sync with microsecond precision
  #if ENABLE_WIFI == 1
    Serial.printf("[Node %d] [WIFI] Connecting to %s...\n", myInfo.id, activeSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(activeSSID, activePassword);
    
    int wifiRetry = 0;
    while (WiFi.status() != WL_CONNECTED && wifiRetry < 100) {
      delay(100);
      wifiRetry++;
      if (wifiRetry % 10 == 0) Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[Node %d] [WIFI] Connected! IP: %s\n", myInfo.id, WiFi.localIP().toString().c_str());
      
      // Configure NTP with multiple servers for redundancy
      #if ENABLE_NTP_SYNC == 1
        configTime(TIMEZONE_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
        Serial.printf("[Node %d] [TIME] Starting NTP sync (microsecond precision)...\n", myInfo.id);
        
        int ntpRetry = 0;
        struct timeval tv;
        while (ntpRetry < 50) {
          if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 100000) {
            break;
          }
          delay(100);
          ntpRetry++;
        }
        
        if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 100000) {
          // Capture exact time with microsecond precision
          microsAtSync = micros();
          ntpEpochAtSync = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
          timeSynced = true;
          
          struct tm timeinfo;
          localtime_r(&tv.tv_sec, &timeinfo);
          Serial.printf("[Node %d] [TIME] ✓ Synced with microsecond precision\n", myInfo.id);
          Serial.printf("[Node %d] [TIME] %02d:%02d:%02d.%06ld\n", 
                        myInfo.id, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, tv.tv_usec);
          Serial.printf("[Node %d] [TIME] Epoch: %lld μs\n", myInfo.id, ntpEpochAtSync);
          Serial.printf("[Node %d] [TIME] Sync micros: %llu\n", myInfo.id, microsAtSync);
          
          #if ENABLE_DRIFT_COMPENSATION == 1
            lastDriftCheck = millis();
            Serial.printf("[Node %d] [TIME] Drift compensation enabled (±%d ppm max)\n", 
                          myInfo.id, MAX_DRIFT_PPM);
          #endif
        } else {
          Serial.printf("[Node %d] [TIME] NTP sync timeout\n", myInfo.id);
        }
      #endif
    } else {
      Serial.printf("\n[Node %d] [WIFI] Connection failed\n", myInfo.id);
    }
  #endif
  
  display.println("\nSystem Ready!");
  display.println("(Ra01S Lib)");
  display.display();
  delay(2000);
  
  // Create Display Task on Core 0 (non-critical UI updates)
  xTaskCreatePinnedToCore(
    displayTask,           // Task function
    "DisplayTask",         // Name
    4096,                  // Stack size (bytes)
    NULL,                  // Parameter
    1,                     // Priority (1 = low, 25 = max)
    &displayTaskHandle,    // Task handle
    0                      // Core 0 (Arduino loop runs on Core 1)
  );
  
  if (displayTaskHandle == NULL) {
    Serial.println("[SETUP] Failed to create display task!");
  } else {
    Serial.println("[SETUP] Display task created on Core 0");
  }
  
  // Create Data Logging Task on Core 0 (for PySerial data collection)
  // Only create when DEBUG_MODE == DEBUG_MODE_GATEWAY_ONLY
  #if DEBUG_MODE == DEBUG_MODE_GATEWAY_ONLY
    logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(DataLogEntry));
    if (logQueue == NULL) {
      Serial.println("[SETUP] Failed to create log queue!");
    } else {
      xTaskCreatePinnedToCore(
        dataLogTask,         // Task function
        "DataLogTask",       // Name
        4096,                // Stack size
        NULL,                // Parameter
        2,                   // Priority (slightly higher than display)
        &dataLogTaskHandle,  // Task handle
        0                    // Core 0
      );
      
      if (dataLogTaskHandle == NULL) {
        Serial.println("[SETUP] Failed to create data log task!");
      } else {
        Serial.println("[SETUP] Data logging task created on Core 0");
        Serial.println("[SETUP] Format: {NODEX} DATA,TYPE,TIME,NODE,MSG,HOP,LAT_US,PDR,RSSI,SNR,EXTRA");
      }
    }
  #endif
  
  // Create WiFi Monitor Task on Core 0 (for remote relay node monitoring)
  #if ENABLE_WIFI == 1 && DEBUG_MODE == DEBUG_MODE_WIFI_MONITOR
    wifiEventQueue = xQueueCreate(WIFI_EVENT_QUEUE_SIZE, sizeof(WiFiEvent));
    if (wifiEventQueue == NULL) {
      Serial.println("[SETUP] Failed to create WiFi event queue!");
    } else {
      xTaskCreatePinnedToCore(
        wifiMonitorTask,         // Task function
        "WiFiMonitorTask",       // Name
        8192,                    // Stack size (larger for WiFi/UDP)
        NULL,                    // Parameter
        2,                       // Priority
        &wifiMonitorTaskHandle,  // Task handle
        0                        // Core 0
      );
      
      if (wifiMonitorTaskHandle == NULL) {
        Serial.println("[SETUP] Failed to create WiFi monitor task!");
      } else {
        Serial.println("[SETUP] WiFi monitor task created on Core 0");
        Serial.printf("[SETUP] Monitoring server: %s:%d\n", activeServerIP, MONITOR_UDP_PORT);
        Serial.printf("[SETUP] Command server: UDP port %d\n", COMMAND_UDP_PORT);
      }
    }
  #endif
  
  Serial.println("=== System Ready ===");
  Serial.println("Starting mesh network...\n");
  
  strcpy(nodeStatus, "READY");
}

// ============= TDMA STATE RESET FUNCTION =============
void resetTDMAState() {
  // Clear all neighbors
  for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
    neighbours[i].id = 0;
    neighbours[i].slotIndex = 0;
    neighbours[i].hoppingDistance = 0x7F;
    neighbours[i].isLocalized = false;
    neighbours[i].syncedCycle = 0;
    neighbours[i].rssi = 0;
    neighbours[i].snr = 0;
    neighbours[i].activityCounter = 0;
    neighbours[i].amIListedAsNeighbour = false;
    neighbours[i].isBidirectional = false;
    
    // Clear cycle history (3 cycles for sync validation)
    for (uint8_t j = 0; j < 3; j++) {
      neighbours[i].cycleHistory[j] = 255;
    }
    neighbours[i].cycleHistoryIdx = 0;
    neighbours[i].cyclesSequential = false;
  }
  
  neighbourCount = 0;
  
  // Reset hop distance (except for gateway)
  #if IS_REFERENCE == 1
    myInfo.hoppingDistance = 0x00;  // Gateway stays at hop 0
  #else
    myInfo.hoppingDistance = 0x7F;  // Nodes reset to unknown
  #endif
  
  // Reset cycle validation
  cycleValidated = false;
  cycleValidationCount = 0;
  lastReceivedCycle = -1;
  autoSendCounter = 0;
  
  // Reset visualization sync (not real protocol)
  
  // NOTE: NTP time is PRESERVED (no reboot!)
  // timeSynced, ntpEpochAtSync, microsAtSync unchanged
  
  Serial.printf("{NODE%d} [RESET] All TDMA state cleared (neighbors=%d, hop=%d)\n", 
                myInfo.id, neighbourCount, myInfo.hoppingDistance);
}

// ============= SERIAL COMMAND HANDLER =============
// Commands processed during processing phase to avoid TDMA interference
// EEPROM Commands (require reboot):
//   SET_SSID <ssid>       - Set WiFi SSID
//   SET_PASS <password>   - Set WiFi password  
//   SET_SERVER <ip>       - Set server IP
//   SET_MODE <0/1/2>      - Set debug mode
//   SAVE                  - Save current config and reboot
//   RESET_CONFIG          - Clear EEPROM, use defaults (reboots)
//   SHOW                  - Show current configuration
// TDMA Commands (no reboot, no save):
//   TDMA_ON               - Enable TDMA
//   TDMA_OFF              - Disable TDMA and reset all data
//   STATUS                - Show current status
//   HELP                  - Show available commands

void checkSerialCommands() {
  // Quick non-blocking check - limit processing time
  static char cmdBuffer[128];
  static uint8_t cmdIndex = 0;
  
  int maxRead = 5;  // Limit chars per call to avoid blocking TDMA
  while (Serial.available() && maxRead-- > 0) {
    char c = Serial.read();
    
    if (c == '\n' || c == '\r') {
      if (cmdIndex > 0) {
        cmdBuffer[cmdIndex] = '\0';
        
        // Parse command
        String command = String(cmdBuffer);
        command.trim();
        
        int spaceIndex = command.indexOf(' ');
        String cmd = (spaceIndex > 0) ? command.substring(0, spaceIndex) : command;
        String param = (spaceIndex > 0) ? command.substring(spaceIndex + 1) : "";
        param.trim();
        
        cmd.toUpperCase();
        
        // ============= TDMA CONTROL COMMANDS (NO REBOOT) =============
        if (cmd == "STOP" || cmd == "TDMA_OFF") {
          tdmaEnabled = false;
          resetTDMAState();
          
          // Reset statistics
          txPacketCount = 0;
          rxPacketCount = 0;
          messageIdCounter = 0;
          forwardQueueCount = 0;
          forwardQueueHead = 0;
          forwardQueueTail = 0;
          
          #if ENABLE_PDR_TRACKING == 1
            for (uint8_t i = 0; i < MAX_PDR_NODES; i++) {
              pdrStats[i] = {0};
            }
            pdrNodeCount = 0;
            totalPacketsExpected = 0;
            totalPacketsReceived = 0;
            totalPacketsLost = 0;
            networkPdr = 100.0;
          #endif
          
          Serial.printf("{NODE%d} [CMD] ⏸️  TDMA STOPPED - All data reset\n", myInfo.id);
        }
        else if (cmd == "START" || cmd == "TDMA_ON") {
          uint32_t delayMs = 0;
          if (param.length() > 0) {
            delayMs = param.toInt();
          }
          
          if (delayMs > 0) {
            Serial.printf("{NODE%d} [CMD] ▶️  TDMA START (delay=%lums)\n", myInfo.id, delayMs);
            delay(delayMs);
          }
          
          tdmaEnabled = true;
          Serial.printf("{NODE%d} [CMD] ✓ TDMA STARTED\n", myInfo.id);
        }
        else if (cmd == "STATUS") {
          Serial.printf("{NODE%d} [STATUS] ID:%d Slot:%d Hop:%d Cycle:%d Neighbors:%d TDMA:%s\n",
                        myInfo.id, myInfo.id, myInfo.slotIndex, myInfo.hoppingDistance, 
                        myInfo.syncedCycle, neighbourCount, tdmaEnabled ? "ON" : "OFF");
          Serial.printf("{NODE%d} [STATUS] TX:%lu RX:%lu FwdQ:%d\n",
                        myInfo.id, txPacketCount, rxPacketCount, forwardQueueCount);
        }
        else if (cmd == "PING") {
          Serial.printf("{NODE%d} [PONG]\n", myInfo.id);
        }
        
        // ============= CONFIGURATION COMMANDS (EEPROM, MAY REBOOT) =============
        else if (cmd == "SET_SSID") {
          if (param.length() > 0 && param.length() <= MAX_SSID_LEN) {
            strncpy(runtimeConfig.ssid, param.c_str(), MAX_SSID_LEN);
            runtimeConfig.ssid[MAX_SSID_LEN] = '\0';
            Serial.printf("{NODE%d} [CONFIG] SSID set to: %s (use SAVE to apply)\n", myInfo.id, runtimeConfig.ssid);
          } else {
            Serial.printf("{NODE%d} [ERROR] Invalid SSID (1-%d chars)\n", myInfo.id, MAX_SSID_LEN);
          }
        }
        else if (cmd == "SET_PASS") {
          if (param.length() <= MAX_PASS_LEN) {
            strncpy(runtimeConfig.password, param.c_str(), MAX_PASS_LEN);
            runtimeConfig.password[MAX_PASS_LEN] = '\0';
            Serial.printf("{NODE%d} [CONFIG] Password set (use SAVE to apply)\n", myInfo.id);
          } else {
            Serial.printf("{NODE%d} [ERROR] Password too long (max %d chars)\n", myInfo.id, MAX_PASS_LEN);
          }
        }
        else if (cmd == "SET_SERVER") {
          if (param.length() > 0 && param.length() <= MAX_IP_LEN) {
            strncpy(runtimeConfig.serverIP, param.c_str(), MAX_IP_LEN);
            runtimeConfig.serverIP[MAX_IP_LEN] = '\0';
            Serial.printf("{NODE%d} [CONFIG] Server IP set to: %s (use SAVE to apply)\n", myInfo.id, runtimeConfig.serverIP);
          } else {
            Serial.printf("{NODE%d} [ERROR] Invalid IP (max %d chars)\n", myInfo.id, MAX_IP_LEN);
          }
        }
        else if (cmd == "SET_MODE") {
          int mode = param.toInt();
          if (mode >= 0 && mode <= 2) {
            runtimeConfig.debugMode = (uint8_t)mode;
            const char* modeNames[] = {"OFF", "GATEWAY_ONLY", "WIFI_MONITOR"};
            Serial.printf("{NODE%d} [CONFIG] Debug mode set to: %d (%s) (use SAVE to apply)\n", 
                          myInfo.id, mode, modeNames[mode]);
          } else {
            Serial.printf("{NODE%d} [ERROR] Invalid mode (0=OFF, 1=GATEWAY, 2=WIFI)\n", myInfo.id);
          }
        }
        else if (cmd == "SAVE") {
          Serial.printf("{NODE%d} [CONFIG] Saving to EEPROM...\n", myInfo.id);
          configSave(runtimeConfig);
          Serial.printf("{NODE%d} [CONFIG] ✓ Saved! Rebooting in 2 seconds...\n", myInfo.id);
          delay(2000);
          ESP.restart();
        }
        else if (cmd == "SHOW") {
          Serial.printf("\n{NODE%d} [CONFIG] === Current Configuration ===\n", myInfo.id);
          Serial.printf("{NODE%d} [CONFIG] SSID: %s\n", myInfo.id, activeSSID);
          Serial.printf("{NODE%d} [CONFIG] Password: %s\n", myInfo.id, activePassword[0] ? "****" : "(empty)");
          Serial.printf("{NODE%d} [CONFIG] Server IP: %s\n", myInfo.id, activeServerIP);
          Serial.printf("{NODE%d} [CONFIG] Debug Mode: %d\n", myInfo.id, activeDebugMode);
          Serial.printf("{NODE%d} [CONFIG] Source: %s\n", myInfo.id, configLoaded ? "EEPROM" : "defaults (settings.h)");
          Serial.printf("{NODE%d} [CONFIG] === Pending Changes ===\n", myInfo.id);
          Serial.printf("{NODE%d} [CONFIG] SSID: %s%s\n", myInfo.id, runtimeConfig.ssid, 
                        strcmp(runtimeConfig.ssid, activeSSID) ? " *" : "");
          Serial.printf("{NODE%d} [CONFIG] Server IP: %s%s\n", myInfo.id, runtimeConfig.serverIP,
                        strcmp(runtimeConfig.serverIP, activeServerIP) ? " *" : "");
          Serial.printf("{NODE%d} [CONFIG] Debug Mode: %d%s\n", myInfo.id, runtimeConfig.debugMode,
                        runtimeConfig.debugMode != activeDebugMode ? " *" : "");
          Serial.printf("{NODE%d} [CONFIG] (* = changed, use SAVE to apply)\n\n", myInfo.id);
        }
        else if (cmd == "RESET_CONFIG") {
          Serial.printf("{NODE%d} [CONFIG] Clearing EEPROM...\n", myInfo.id);
          configClear();
          Serial.printf("{NODE%d} [CONFIG] ✓ EEPROM cleared! Will use defaults. Rebooting...\n", myInfo.id);
          delay(2000);
          ESP.restart();
        }
        else if (cmd == "HELP") {
          Serial.printf("\n{NODE%d} === Serial Commands ===\n", myInfo.id);
          Serial.printf("TDMA Control (no reboot):\n");
          Serial.printf("  TDMA_ON / START [delay_ms]  - Enable TDMA\n");
          Serial.printf("  TDMA_OFF / STOP             - Disable TDMA & reset data\n");
          Serial.printf("  STATUS                      - Show current status\n");
          Serial.printf("\nConfiguration (saved to EEPROM):\n");
          Serial.printf("  SET_SSID <ssid>             - Set WiFi SSID\n");
          Serial.printf("  SET_PASS <password>         - Set WiFi password\n");
          Serial.printf("  SET_SERVER <ip>             - Set server IP\n");
          Serial.printf("  SET_MODE <0/1/2>            - Set debug mode\n");
          Serial.printf("  SHOW                        - Show configuration\n");
          Serial.printf("  SAVE                        - Save & reboot\n");
          Serial.printf("  RESET_CONFIG                - Clear EEPROM & reboot\n\n");
        }
        else if (cmd.length() > 0) {
          Serial.printf("{NODE%d} [ERROR] Unknown command: %s (type HELP)\n", myInfo.id, cmd.c_str());
        }
        
        cmdIndex = 0;
      }
    } else if (cmdIndex < sizeof(cmdBuffer) - 1) {
      cmdBuffer[cmdIndex++] = c;
    }
  }
}

// ============= MAIN LOOP =============
void loop() {
  // Check for serial commands (STOP, START, status, etc.)
  checkSerialCommands();
  
  ResponderOutput rxOutput;
  
  // BUTTON HANDLING
  noInterrupts();
  bool btnPressed = buttonPressed;
  buttonPressed = false;
  interrupts();
  
  // GATEWAY: Increment synchronized cycle counter
  #if IS_REFERENCE == 1
    autoSendCounter++;
    if (autoSendCounter >= AUTO_SEND_INTERVAL_CYCLES) {
      autoSendCounter = 0;
    }
    myInfo.syncedCycle = autoSendCounter;
  #endif
  
  // ============= STRATUM TIMEOUT CHECK (5-CYCLE DEGRADATION) =============
  // Non-gateway nodes: Decrement sync validity counter each cycle
  // When counter reaches 0, degrade stratum to LOCAL (not synced)
  #if IS_REFERENCE == 0
  {
    static uint8_t lastCycleForStratum = 255;
    
    // Only process once per cycle change (using myInfo.syncedCycle)
    if (lastCycleForStratum != myInfo.syncedCycle) {
      lastCycleForStratum = myInfo.syncedCycle;
      
      // Decrement sync validity counter
      if (myInfo.syncValidCounter > 0) {
        myInfo.syncValidCounter--;
        
        // Check for stratum degradation
        if (myInfo.syncValidCounter == 0 && myInfo.syncStratum < STRATUM_LOCAL) {
          uint8_t oldStratum = myInfo.syncStratum;
          myInfo.syncStratum = STRATUM_LOCAL;
          myInfo.syncedWithGateway = false;
          myInfo.syncSource = 0;
          
          Serial.printf("[Node %d] [STRATUM] TIMEOUT: stratum %d->%d (no sync refresh for %d cycles)\n",
                        myInfo.id, oldStratum, STRATUM_LOCAL, SYNC_VALID_CYCLES);
          
        }
      }
    }
  }
  #endif
  // ============= END STRATUM TIMEOUT CHECK =============
  
  // AUTO-SEND SENSOR DATA (sequential transmission based on cycle)
  bool canAutoSend = (myInfo.hoppingDistance != 0 && 
                      myInfo.hoppingDistance != 0x7F && 
                      !hasSensorDataToSend &&
                      cycleValidated);  // Only send after cycle validation
  
  if (canAutoSend) {
    // Sequential transmission: Node sends when (cycle == nodeId - 1)
    // Cycle 0 -> Node 1, Cycle 1 -> Node 2, Cycle 2 -> Node 3, etc.
    uint8_t myTurnCycle = (myInfo.id - 1) % AUTO_SEND_INTERVAL_CYCLES;
    
    if (myInfo.syncedCycle == myTurnCycle) {
      bool hasNextHop = false;
      for (uint8_t i = 0; i < neighbourCount; i++) {
        uint8_t idx = neighbourIndices[i];
        if (neighbours[idx].hoppingDistance < myInfo.hoppingDistance &&
            neighbours[idx].amIListedAsNeighbour) {
          hasNextHop = true;
          break;
        }
      }
      
      if (hasNextHop) {
        simTemperature = 25.0 + (random(-50, 50) / 10.0);
        simHumidity = 60.0 + (random(-100, 100) / 10.0);
        int newBattery = (int)simBattery - (int)random(0, 2);
        simBattery = (uint8_t)constrain(newBattery, 0, 100);
        
        snprintf(sensorDataToSend, sizeof(sensorDataToSend), "T%.0fH%d", 
                 simTemperature, simBattery);
        
        messageIdCounter++;
        ownMessageOrigSender = myInfo.id;
        ownMessageId = (myInfo.id << 8) | (messageIdCounter & 0xFF);
        
        hasSensorDataToSend = true;
        
        Serial.printf("[Node %d] [AUTO_SEND_SEQ] 🔄 My turn! Cycle:%d (ID:%d) MsgID:%u T:%.1f B:%d%% data:%s\n", 
                      myInfo.id, myInfo.syncedCycle, myInfo.id, ownMessageId, simTemperature, simBattery, sensorDataToSend);
      }
    }
  } else if (!cycleValidated && myInfo.hoppingDistance != 0 && myInfo.hoppingDistance != 0x7F) {
    // Still validating cycles
    Serial.printf("[Node %d] [AUTO_SEND_SEQ] Waiting for cycle validation (%d/%d)\n", 
                  myInfo.id, cycleValidationCount, CYCLE_VALIDATION_THRESHOLD);
  }
  
  // BUTTON HANDLER (Manual Send)
  if (btnPressed) {
    if (myInfo.hoppingDistance != 0 && myInfo.hoppingDistance != 0x7F) {
      simTemperature = 25.0 + (random(-50, 50) / 10.0);
      
      snprintf(sensorDataToSend, sizeof(sensorDataToSend), "T%.0fH%d", 
               simTemperature, simBattery);
      
      messageIdCounter++;
      ownMessageOrigSender = myInfo.id;
      ownMessageId = (myInfo.id << 8) | (messageIdCounter & 0xFF);
      
      hasSensorDataToSend = true;
      
      Serial.printf("[Node %d] [BTN_MSG] Cycle:%d MsgID:%u data:%s\n", 
                    myInfo.id, myInfo.syncedCycle, ownMessageId, sensorDataToSend);
    } else if (myInfo.hoppingDistance == 0) {
      Serial.println("[BUTTON] Gateway cannot originate messages");
    } else {
      Serial.println("[BUTTON] No route to gateway yet");
    }
  }
  
  // ========== MESH NETWORK TDMA CYCLE ==========
  // Check if TDMA is enabled (can be paused via serial command)
  if (!tdmaEnabled) {
    // TDMA stopped - simulate node failure
    // Node won't transmit, neighbors will timeout and remove from routing table
    delay(100);  // Prevent busy loop
    return;  // Skip entire TDMA cycle
  }
  
  loopCounter++;
  unsigned long cycleStart = micros();
  
  if (loopCounter % 10 == 0) {
    printStatusLine();
  }
  
  // ========== PROCESSING PHASE ==========
  unsigned long procStart = micros();
  
  
  // Update drift compensation periodically
  #if ENABLE_WIFI == 1
    updateDriftCompensation();
  #endif
  
  // Re-calculate hop count every cycle (Bellman-Ford with RSSI filter)
  recalculateHopCount();
  
  // Update neighbor timeout and rebuild indices
  updateNeighbourStatus();
  
  // Display update now handled by separate task on Core 0
  // Just set the flag when data changes
  displayNeedsUpdate = true;
  
  // Send WiFi batch if gateway and has buffered messages
  #if ENABLE_WIFI == 1
    if (myInfo.hoppingDistance == 0 && wifiBatchCount > 0) {
      sendWifiBatch();
    }
  #endif
  
  uint32_t yieldCounter = 0;
  while ((micros() - procStart) < Tprocessing_us) {
    delayMicroseconds(100);
    yieldCounter++;
    if (yieldCounter >= 10) {
      yield();
      yieldCounter = 0;
    }
  }
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] Processing phase done: %lu μs\n", myInfo.id, micros() - cycleStart);
  #endif
  
  
  // ========== RX PHASE 1: Listen BEFORE my TX slot ==========
  unsigned long rxPhase1Start = micros();
  long Tduration_us = (long)myInfo.slotIndex * Tslot_us;
  long Tremaining_us = Tduration_us;
  
  
  while (Tremaining_us > 0) {
    uint32_t timeout_ms = calcTimeoutMs(Tremaining_us);
    if (timeout_ms == 0) break;
    
    
    yield();
    
    rxOutput = responder(timeout_ms);
    
    // TIMING SYNCHRONIZATION (LoRaQuake algorithm)
    if (rxOutput.adjustTiming && rxOutput.senderSlot != 255) {
      uint32_t syncStart = micros();
      int slotsRemaining;
      const char* syncCase;
      
      if (myInfo.slotIndex > rxOutput.senderSlot) {
        // Case 1: mySlot > senderSlot
        slotsRemaining = modulo(myInfo.slotIndex - rxOutput.senderSlot - 1, Nslot);
        Tremaining_us = (long)slotsRemaining * Tslot_us + slotOffset_us;
        syncCase = "CASE1_NORMAL";
      } else {
        // Case 2: mySlot <= senderSlot (wrap-around)
        slotsRemaining = modulo(myInfo.slotIndex - rxOutput.senderSlot - 1, Nslot);
        Tremaining_us = (long)slotsRemaining * Tslot_us + slotOffset_us + Tprocessing_us;
        syncCase = "CASE2_WRAPAROUND";
      }
      
      uint32_t syncDuration = micros() - syncStart;
      
    } else {
      Tremaining_us = Tduration_us - (long)(micros() - rxPhase1Start);
    }
  }
  
  uint32_t rxPhase1Duration = micros() - rxPhase1Start;
  
  
  // ========== TX PHASE ==========
  unsigned long txPhaseStart = micros();
  
  
  delayMicroseconds(TtxDelay_us);
  
  transmitUnifiedPacket();
  
  // Wait remaining slot time
  unsigned long txElapsed = micros() - txPhaseStart;
  if (txElapsed < Tslot_us) {
    delayMicroseconds(Tslot_us - txElapsed);
  }
  
  uint32_t txPhaseDuration = micros() - txPhaseStart;
  
  
  // ========== RX PHASE 2: Listen AFTER my TX slot ==========
  unsigned long rxPhase2Start = micros();
  Tduration_us = (long)(Nslot - myInfo.slotIndex - 1) * Tslot_us;
  Tremaining_us = Tduration_us;
  
  
  while (Tremaining_us > 0) {
    uint32_t timeout_ms = calcTimeoutMs(Tremaining_us);
    if (timeout_ms == 0) break;
    
    
    yield();
    
    rxOutput = responder(timeout_ms);
    
    // TIMING SYNCHRONIZATION (Phase 2)
    if (rxOutput.adjustTiming && rxOutput.senderSlot != 255) {
      int slotsRemaining = Nslot - rxOutput.senderSlot - 1;
      Tremaining_us = (long)slotsRemaining * Tslot_us + slotOffset_us;
      
    } else {
      Tremaining_us = Tduration_us - (long)(micros() - rxPhase2Start);
    }
  }
  
  
  // Ra01S: Put radio to sleep for power saving (optional)
  // No explicit sleep function in Ra01S, radio stays in RX mode
  
  #ifdef VERBOSE
    unsigned long totalCycleTime = micros() - cycleStart;
    Serial.printf("[Node %d] Cycle completed. Total: %lu μs\n", myInfo.id, totalCycleTime);
    Serial.printf("[Node %d] Neighbours: %d\n", myInfo.id, neighbourCount);
  #endif
  
  // Watchdog feed
  delay(1);
  yield();
}
