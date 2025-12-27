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

// WiFi untuk NTP time sync
#if ENABLE_WIFI == 1
  #include <WiFi.h>
  #include <time.h>
#endif

// ============= HARDWARE OBJECTS =============
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Ra01S (SX126x) radio object
// Constructor: SX126x(spiSelect, reset, busy, txen, rxen)
SX126x radio(LORA_PIN_NSS, LORA_PIN_RESET, LORA_PIN_BUSY, LORA_TXEN, LORA_RXEN);

// Time sync for real-time debugging (WiFi only, using ESP32 built-in SNTP)
#if ENABLE_WIFI == 1
  bool timeSynced = false;
#endif

// ============= ENCODER VARIABLES =============
volatile int32_t encoderRaw = 0;
volatile bool buttonPressed = false;
volatile uint32_t lastEncoderISR = 0;
volatile uint32_t lastButtonISR = 0;
const uint32_t ENCODER_DEBOUNCE_US = 500;
const uint32_t BUTTON_DEBOUNCE_MS = 150;

// ============= MESH NETWORK VARIABLES =============
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
};
ForwardMessage forwardQueue[FORWARD_QUEUE_SIZE];
uint8_t forwardQueueHead = 0;  // Index to write
uint8_t forwardQueueTail = 0;  // Index to read
uint8_t forwardQueueCount = 0;

// ============= MESSAGE TRACKING =============
uint16_t messageIdCounter = 0;
uint16_t ownMessageOrigSender = 0;
uint16_t ownMessageId = 0;

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
#define AUTO_SEND_INTERVAL_CYCLES 5
uint8_t autoSendCounter = 0;

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
void initMyInfo();

void updateDisplay();
void updateNeighbourStatus();
void printStatusLine();

void transmitUnifiedPacket();
uint8_t processRxPacket();
uint16_t selectBestNextHop();
bool enqueueForward(ForwardMessage* msg);
bool dequeueForward(ForwardMessage* msg);

ResponderOutput responder(uint32_t timeoutMs);

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

// ============= DISPLAY UPDATE =============
void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  // ROW 1: Node Info
  char hopChar = (myInfo.hoppingDistance == 0x7F) ? '?' : ('0' + myInfo.hoppingDistance);
  char line1[22];
  snprintf(line1, sizeof(line1), "ID:%04d H:%c S:%d %s", 
           myInfo.id, 
           hopChar,
           myInfo.slotIndex,
           nodeStatus);
  display.println(line1);
  
  // ROW 2-3: Table Header
  display.println("ID  |Slt|Hop|RSSI");
  display.println("----+---+---+----");
  
  // ROW 4-8: Neighbor Table (max 5 rows)
  uint8_t displayCount = 0;
  for (uint8_t i = 0; i < MAX_NEIGHBOURS && displayCount < 5; i++) {
    if (neighbours[i].id > 0) {
      char hopStr[4];
      if (neighbours[i].hoppingDistance == 0x7F) {
        strcpy(hopStr, " ?");
      } else {
        snprintf(hopStr, sizeof(hopStr), "%2d", neighbours[i].hoppingDistance);
      }
      
      char line[22];
      snprintf(line, sizeof(line), "%4d|%3d|%s|%4d", 
               neighbours[i].id, 
               neighbours[i].slotIndex,
               hopStr,
               neighbours[i].rssi);
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
  snprintf(statsLine, sizeof(statsLine), "TX:%lu RX:%lu N:%d", 
           txPacketCount, rxPacketCount, neighbourCount);
  display.println(statsLine);
  
  display.display();
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
    
    // Must be bidirectional and have valid hop distance
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
  #ifdef VERBOSE
    Serial.println("TX: Unified packet");
  #endif
  
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
  // Byte 11: Reserved
  
  // NEIGHBOR SECTION (16 bytes: 12-27, max 4 neighbors)
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
    
    // Sensor data
    for (uint8_t i = 0; i < dataLen && i < SENSOR_DATA_LENGTH; i++) {
      txBuffer[40 + i] = (uint8_t)dataToSend[i];
    }
    
    Serial.printf("[Node %d] [TX] slot:%d hop:%d cycle:%d nbr:%d | %s: MsgID:%d orig:%d hops:%d target:%d\n", 
                  myInfo.id, myInfo.slotIndex, myInfo.hoppingDistance, myInfo.syncedCycle, neighborsToSend,
                  (dataMode == DATA_MODE_OWN) ? "OWN" : "FWD",
                  msgId, origSender, hopCount, hopDecisionTarget);
    
    strcpy(nodeStatus, (dataMode == DATA_MODE_FORWARD) ? "TX_FWD" : "TX_DATA");
  } else {
    Serial.printf("[Node %d] [TX] slot:%d hop:%d cycle:%d nbr:%d | NO_DATA\n", 
                  myInfo.id, myInfo.slotIndex, myInfo.hoppingDistance, myInfo.syncedCycle, neighborsToSend);
    strcpy(nodeStatus, "TX_ID");
  }
  
  // Ra01S: Send with synchronous mode (blocks until TX complete)
  uint32_t txStart = micros();
  bool txSuccess = radio.Send(txBuffer, FIXED_PACKET_LENGTH, SX126x_TXMODE_SYNC);
  lastTxDuration_us = micros() - txStart;
  
  if (txSuccess) {
    txPacketCount++;
    #if ENABLE_RADIO_DEBUG == 1
      Serial.printf("[Node %d] [RADIO_TX] Success! Duration: %lu μs\n", myInfo.id, lastTxDuration_us);
    #endif
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
  
  if (numNeighborsInPacket > MAX_NEIGHBOURS_IN_PACKET) {
    numNeighborsInPacket = MAX_NEIGHBOURS_IN_PACKET;
  }
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] [RX_PKT] from ID:%d slot:%d hop:%d cycle:%d nbr:%d RSSI:%d SNR:%d\n",
                  myInfo.id, senderId, senderSlot, senderHop, senderCycle, numNeighborsInPacket, rxRssi, rxSnr);
  #endif
  
  // UPDATE/ADD SENDER TO NEIGHBOR LIST
  bool foundSender = false;
  for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].id == senderId) {
      selectedNeighbourIdx = i;
      foundSender = true;
      break;
    }
  }
  
  if (!foundSender) {
    for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
      if (neighbours[i].id == 0) {
        selectedNeighbourIdx = i;
        foundSender = true;
        neighbourCount++;
        break;
      }
    }
  }
  
  if (foundSender) {
    neighbours[selectedNeighbourIdx].id = senderId;
    neighbours[selectedNeighbourIdx].slotIndex = senderSlot;
    neighbours[selectedNeighbourIdx].hoppingDistance = senderHop;
    neighbours[selectedNeighbourIdx].isLocalized = senderLocalized;
    neighbours[selectedNeighbourIdx].syncedCycle = senderCycle;
    neighbours[selectedNeighbourIdx].rssi = rxRssi;
    neighbours[selectedNeighbourIdx].snr = rxSnr;
    neighbours[selectedNeighbourIdx].activityCounter = 0;
    
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
      }
      
      byteIdx += 4;
    }
    
    // HOP DISTANCE CALCULATION (Bellman-Ford)
    #if IS_REFERENCE == 0
      if (neighbours[selectedNeighbourIdx].hoppingDistance != 0x7F) {
        uint8_t newHop = neighbours[selectedNeighbourIdx].hoppingDistance + 1;
        if (newHop < myInfo.hoppingDistance) {
          myInfo.hoppingDistance = newHop;
          Serial.printf("[Node %d] [HOP] Updated to %d via node %d\n", myInfo.id, myInfo.hoppingDistance, senderId);
        }
      }
      
      // CYCLE SYNCHRONIZATION: Sync from neighbors with lower hop distance (closer to gateway)
      if (neighbours[selectedNeighbourIdx].hoppingDistance < myInfo.hoppingDistance) {
        if (myInfo.syncedCycle != senderCycle) {
          myInfo.syncedCycle = senderCycle;
          Serial.printf("[Node %d] [CYCLE_SYNC] Aligned to cycle %d from node %d (hop %d)\n", 
                        myInfo.id, myInfo.syncedCycle, senderId, neighbours[selectedNeighbourIdx].hoppingDistance);
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
        Serial.printf("[Node %d] [GATEWAY] Received message %d from %d: %s (tracking: ",
                      myInfo.id, msgId, origSender, sensorDataReceived);
        for (uint8_t i = 0; i < hopCount && i < MAX_TRACKING_HOPS; i++) {
          if (tracking[i] > 0) Serial.printf("%d ", tracking[i]);
        }
        Serial.printf("--> GW)\n");
        
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
          
          enqueueForward(&fwdMsg);
        }
      }
    }
  } else {
    Serial.println("[RX_WARN] Neighbor list full, cannot add sender!");
  }
  
  return senderSlot;
}

// ============= NEIGHBOR STATUS UPDATE =============
void updateNeighbourStatus() {
  for (uint8_t i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].id != 0) {
      neighbours[i].activityCounter++;
      
      if (neighbours[i].activityCounter >= MAX_INACTIVE_CYCLES) {
        Serial.printf("[Node %d] [TIMEOUT] Removing inactive neighbor %d\n", myInfo.id, neighbours[i].id);
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
  lastRxDuration_us = 0;
  
  // Ra01S: radio.Receive() polls for data (non-blocking)
  // It checks IRQ status and returns received bytes if available
  while (millis() - rxStart < timeoutMs) {
    // Poll for received data
    uint8_t rxLen = radio.Receive(rxBuffer, FIXED_PACKET_LENGTH);
    
    if (rxLen > 0) {
      lastRxDuration_us = (millis() - rxStart) * 1000;
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
          output.senderSlot = processRxPacket();
          output.adjustTiming = true;
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
  
  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("I2C initialized - SDA:%d SCL:%d\n", I2C_SDA, I2C_SCL);
  
  // Initialize components
  initEncoder();
  initDisplay();
  initLoRa();
  initMyInfo();
  
  // Initialize WiFi and NTP time sync
  #if ENABLE_WIFI == 1
    Serial.printf("[Node %d] [WIFI] Connecting to %s...\n", myInfo.id, WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    int wifiRetry = 0;
    while (WiFi.status() != WL_CONNECTED && wifiRetry < 100) {
      delay(100);
      wifiRetry++;
      if (wifiRetry % 10 == 0) Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[Node %d] [WIFI] Connected! IP: %s\n", myInfo.id, WiFi.localIP().toString().c_str());
      
      configTime(7*3600, 0, "pool.ntp.org", "time.nist.gov");
      Serial.printf("[Node %d] [TIME] Starting NTP sync...\n", myInfo.id);
      
      int ntpRetry = 0;
      while (time(nullptr) < 100000 && ntpRetry < 50) {
        delay(100);
        ntpRetry++;
      }
      
      if (time(nullptr) > 100000) {
        timeSynced = true;
        struct tm timeinfo;
        getLocalTime(&timeinfo);
        Serial.printf("[Node %d] [TIME] Synced: %02d:%02d:%02d\n", 
                      myInfo.id, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      } else {
        Serial.printf("[Node %d] [TIME] NTP sync timeout\n", myInfo.id);
      }
    } else {
      Serial.printf("\n[Node %d] [WIFI] Connection failed\n", myInfo.id);
    }
  #endif
  
  display.println("\nSystem Ready!");
  display.println("(Ra01S Lib)");
  display.display();
  delay(2000);
  
  Serial.println("=== System Ready ===");
  Serial.println("Starting mesh network...\n");
  
  strcpy(nodeStatus, "READY");
}

// ============= MAIN LOOP =============
void loop() {
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
  
  // AUTO-SEND SENSOR DATA (using synchronized cycle)
  bool canAutoSend = (myInfo.hoppingDistance != 0 && 
                      myInfo.hoppingDistance != 0x7F && 
                      !hasSensorDataToSend);
  
  if (canAutoSend) {
    // Trigger send when synchronized cycle reaches the target (e.g., cycle 0)
    if (myInfo.syncedCycle == 0) {
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
        
        Serial.printf("[Node %d] [AUTO_MSG] Cycle:%d MsgID:%u T:%.1f B:%d%% data:%s\n", 
                      myInfo.id, myInfo.syncedCycle, ownMessageId, simTemperature, simBattery, sensorDataToSend);
      }
    }
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
  loopCounter++;
  unsigned long cycleStart = micros();
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] === Cycle %d ===\n", myInfo.id, loopCounter);
    Serial.printf("[Node %d] My slot: %d/%d\n", myInfo.id, myInfo.slotIndex, Nslot);
  #endif
  
  if (loopCounter % 10 == 0) {
    printStatusLine();
  }
  
  // ========== PROCESSING PHASE ==========
  unsigned long procStart = micros();
  updateNeighbourStatus();
  updateDisplay();
  
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
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] RX Phase 1: Initial duration = %ld μs\n", myInfo.id, Tduration_us);
  #endif
  
  while (Tremaining_us > 0) {
    uint32_t timeout_ms = calcTimeoutMs(Tremaining_us);
    if (timeout_ms == 0) break;
    
    yield();
    
    rxOutput = responder(timeout_ms);
    
    // TIMING SYNCHRONIZATION (LoRaQuake algorithm)
    if (rxOutput.adjustTiming && rxOutput.senderSlot != 255) {
      if (myInfo.slotIndex > rxOutput.senderSlot) {
        // Case 1: mySlot > senderSlot
        int slotsRemaining = modulo(myInfo.slotIndex - rxOutput.senderSlot - 1, Nslot);
        Tremaining_us = (long)slotsRemaining * Tslot_us + slotOffset_us;
        
        #ifdef VERBOSE
          Serial.printf("[Node %d] [SYNC-CASE1] slotsRemaining=%d Tremaining=%ld μs\n",
                        myInfo.id, slotsRemaining, Tremaining_us);
        #endif
      } else {
        // Case 2: mySlot <= senderSlot (wrap-around)
        int slotsRemaining = modulo(myInfo.slotIndex - rxOutput.senderSlot - 1, Nslot);
        Tremaining_us = (long)slotsRemaining * Tslot_us + slotOffset_us + Tprocessing_us;
        
        #ifdef VERBOSE
          Serial.printf("[Node %d] [SYNC-CASE2] slotsRemaining=%d Tremaining=%ld μs\n",
                        myInfo.id, slotsRemaining, Tremaining_us);
        #endif
      }
    } else {
      Tremaining_us = Tduration_us - (long)(micros() - rxPhase1Start);
    }
  }
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] RX Phase 1 done: %lu μs\n", myInfo.id, micros() - cycleStart);
  #endif
  
  // ========== TX PHASE ==========
  unsigned long txPhaseStart = micros();
  
  #ifdef VERBOSE
    Serial.println("TX Phase: Transmitting unified packet");
  #endif
  
  #if ENABLE_WIFI == 1
    if (timeSynced) {
      struct tm timeinfo;
      getLocalTime(&timeinfo);
      Serial.printf("[Node %d] [TX_START] %02d:%02d:%02d.%03d\n", 
                    myInfo.id, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, 
                    (int)((micros() % 1000000) / 1000));
    }
  #endif
  
  delayMicroseconds(TtxDelay_us);
  
  transmitUnifiedPacket();
  
  #if ENABLE_WIFI == 1
    if (timeSynced) {
      struct tm timeinfo;
      getLocalTime(&timeinfo);
      Serial.printf("[Node %d] [TX_DONE] %02d:%02d:%02d.%03d Duration: %lu μs\n", 
                    myInfo.id, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                    (int)((micros() % 1000000) / 1000), lastTxDuration_us);
    }
  #endif
  
  // Wait remaining slot time
  unsigned long txElapsed = micros() - txPhaseStart;
  if (txElapsed < Tslot_us) {
    delayMicroseconds(Tslot_us - txElapsed);
  }
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] TX Phase done: %lu μs\n", myInfo.id, micros() - cycleStart);
  #endif
  
  // ========== RX PHASE 2: Listen AFTER my TX slot ==========
  unsigned long rxPhase2Start = micros();
  Tduration_us = (long)(Nslot - myInfo.slotIndex - 1) * Tslot_us;
  Tremaining_us = Tduration_us;
  
  #ifdef VERBOSE
    Serial.printf("[Node %d] RX Phase 2: Initial duration = %ld μs\n", myInfo.id, Tduration_us);
  #endif
  
  while (Tremaining_us > 0) {
    uint32_t timeout_ms = calcTimeoutMs(Tremaining_us);
    if (timeout_ms == 0) break;
    
    yield();
    
    rxOutput = responder(timeout_ms);
    
    // TIMING SYNCHRONIZATION (Phase 2)
    if (rxOutput.adjustTiming && rxOutput.senderSlot != 255) {
      int slotsRemaining = Nslot - rxOutput.senderSlot - 1;
      Tremaining_us = (long)slotsRemaining * Tslot_us + slotOffset_us;
      
      #ifdef VERBOSE
        Serial.printf("[Node %d] [SYNC-PHASE2] slotsRemaining=%d Tremaining=%ld μs\n",
                      myInfo.id, slotsRemaining, Tremaining_us);
      #endif
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
