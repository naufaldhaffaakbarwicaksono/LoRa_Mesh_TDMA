/*****************************************************************************************************
  Configuration Manager - EEPROM Storage & Serial Commands
  
  Features:
  - Store/load WiFi credentials, Server IP, DEBUG_MODE to EEPROM
  - Serial commands for runtime configuration
  - TDMA enable/disable with data reset
  - Non-blocking serial processing (only during processing phase)
*******************************************************************************************************/
#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <EEPROM.h>
#include <Arduino.h>

// ============= EEPROM STRUCTURE =============
#define EEPROM_SIZE 256
#define EEPROM_MAGIC 0xCA5E  // Magic number to validate stored config

// EEPROM addresses
#define ADDR_MAGIC        0    // 2 bytes
#define ADDR_SSID         2    // 33 bytes (32 + null)
#define ADDR_PASS         35   // 65 bytes (64 + null)
#define ADDR_SERVER_IP    100  // 16 bytes
#define ADDR_DEBUG_MODE   116  // 1 byte
#define ADDR_NODE_ID      117  // 2 bytes (optional override)
#define ADDR_RSSI_MIN     120  // 2 bytes (int16_t)
#define ADDR_RSSI_GOOD    122  // 2 bytes (int16_t)
#define ADDR_TX_POWER     124  // 1 byte (int8_t, -9 to +22 dBm)
#define ADDR_CHECKSUM     126  // 1 byte

// Limits
#define MAX_SSID_LEN      32
#define MAX_PASS_LEN      64
#define MAX_IP_LEN        15

// ============= RUNTIME CONFIG STRUCTURE =============
// Default RSSI thresholds (can be overridden by EEPROM)
#define DEFAULT_RSSI_MIN    -115  // Minimum threshold to accept packets
#define DEFAULT_RSSI_GOOD   -100  // "Good quality" threshold for routing priority
#define DEFAULT_TX_POWER    -9    // Default TX power (dBm), SX1262 range: -9 to +22

struct RuntimeConfig {
  char ssid[MAX_SSID_LEN + 1];
  char password[MAX_PASS_LEN + 1];
  char serverIP[MAX_IP_LEN + 1];
  uint8_t debugMode;
  int16_t rssiMin;      // Minimum RSSI threshold (dBm)
  int16_t rssiGood;     // Good quality RSSI threshold (dBm)
  int8_t txPower;       // TX power (dBm), range: -9 to +22
  bool valid;
};

// ============= SERIAL COMMAND BUFFER =============
#define SERIAL_CMD_BUFFER_SIZE 128
static char serialCmdBuffer[SERIAL_CMD_BUFFER_SIZE];
static uint8_t serialCmdIndex = 0;
static bool serialCmdReady = false;

// ============= TDMA CONTROL =============
// Note: tdmaEnabled is declared as 'volatile bool' in main .ino file
// These are helper flags only
static bool tdmaResetRequested = false;

// ============= FUNCTION DECLARATIONS =============

// Calculate simple checksum
inline uint8_t calcChecksum(const RuntimeConfig& cfg) {
  uint8_t sum = 0;
  const uint8_t* ptr = (const uint8_t*)&cfg;
  for (size_t i = 0; i < sizeof(RuntimeConfig) - 1; i++) {
    sum ^= ptr[i];
  }
  return sum;
}

// Initialize EEPROM
inline void configInit() {
  EEPROM.begin(EEPROM_SIZE);
}

// Check if EEPROM has valid config
inline bool configIsValid() {
  uint16_t magic = EEPROM.read(ADDR_MAGIC) | (EEPROM.read(ADDR_MAGIC + 1) << 8);
  return (magic == EEPROM_MAGIC);
}

// Load config from EEPROM
inline RuntimeConfig configLoad() {
  RuntimeConfig cfg;
  cfg.valid = false;
  
  if (!configIsValid()) {
    return cfg;
  }
  
  // Read SSID
  for (int i = 0; i <= MAX_SSID_LEN; i++) {
    cfg.ssid[i] = EEPROM.read(ADDR_SSID + i);
  }
  cfg.ssid[MAX_SSID_LEN] = '\0';
  
  // Read Password
  for (int i = 0; i <= MAX_PASS_LEN; i++) {
    cfg.password[i] = EEPROM.read(ADDR_PASS + i);
  }
  cfg.password[MAX_PASS_LEN] = '\0';
  
  // Read Server IP
  for (int i = 0; i <= MAX_IP_LEN; i++) {
    cfg.serverIP[i] = EEPROM.read(ADDR_SERVER_IP + i);
  }
  cfg.serverIP[MAX_IP_LEN] = '\0';
  
  // Read Debug Mode
  cfg.debugMode = EEPROM.read(ADDR_DEBUG_MODE);
  if (cfg.debugMode > 2) cfg.debugMode = 0;  // Validate
  
  // Read RSSI thresholds
  cfg.rssiMin = (int16_t)(EEPROM.read(ADDR_RSSI_MIN) | (EEPROM.read(ADDR_RSSI_MIN + 1) << 8));
  cfg.rssiGood = (int16_t)(EEPROM.read(ADDR_RSSI_GOOD) | (EEPROM.read(ADDR_RSSI_GOOD + 1) << 8));
  
  // Validate RSSI values (use defaults if out of range)
  if (cfg.rssiMin < -130 || cfg.rssiMin > -50 || cfg.rssiMin == 0) {
    cfg.rssiMin = DEFAULT_RSSI_MIN;
  }
  if (cfg.rssiGood < -120 || cfg.rssiGood > -40 || cfg.rssiGood == 0) {
    cfg.rssiGood = DEFAULT_RSSI_GOOD;
  }
  
  // Read TX Power
  cfg.txPower = (int8_t)EEPROM.read(ADDR_TX_POWER);
  // Validate TX power (SX1262 range: -9 to +22 dBm)
  if (cfg.txPower < -9 || cfg.txPower > 22) {
    cfg.txPower = DEFAULT_TX_POWER;
  }
  
  cfg.valid = true;
  return cfg;
}

// Save config to EEPROM
inline void configSave(const RuntimeConfig& cfg) {
  // Write magic
  EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC & 0xFF);
  EEPROM.write(ADDR_MAGIC + 1, (EEPROM_MAGIC >> 8) & 0xFF);
  
  // Write SSID
  for (int i = 0; i <= MAX_SSID_LEN; i++) {
    EEPROM.write(ADDR_SSID + i, cfg.ssid[i]);
  }
  
  // Write Password
  for (int i = 0; i <= MAX_PASS_LEN; i++) {
    EEPROM.write(ADDR_PASS + i, cfg.password[i]);
  }
  
  // Write Server IP
  for (int i = 0; i <= MAX_IP_LEN; i++) {
    EEPROM.write(ADDR_SERVER_IP + i, cfg.serverIP[i]);
  }
  
  // Write Debug Mode
  EEPROM.write(ADDR_DEBUG_MODE, cfg.debugMode);
  
  // Write RSSI thresholds
  EEPROM.write(ADDR_RSSI_MIN, cfg.rssiMin & 0xFF);
  EEPROM.write(ADDR_RSSI_MIN + 1, (cfg.rssiMin >> 8) & 0xFF);
  EEPROM.write(ADDR_RSSI_GOOD, cfg.rssiGood & 0xFF);
  EEPROM.write(ADDR_RSSI_GOOD + 1, (cfg.rssiGood >> 8) & 0xFF);
  
  // Write TX Power
  EEPROM.write(ADDR_TX_POWER, (uint8_t)cfg.txPower);
  
  // Commit
  EEPROM.commit();
}

// Clear EEPROM config (use defaults from settings.h)
inline void configClear() {
  EEPROM.write(ADDR_MAGIC, 0);
  EEPROM.write(ADDR_MAGIC + 1, 0);
  EEPROM.commit();
}

// ============= SERIAL COMMAND PROCESSING =============
// Commands:
//   SET_SSID <ssid>       - Set WiFi SSID (saves & reboots)
//   SET_PASS <password>   - Set WiFi password (saves & reboots)  
//   SET_SERVER <ip>       - Set server IP (saves & reboots)
//   SET_MODE <0/1/2>      - Set debug mode (saves & reboots)
//   SAVE                  - Save current config and reboot
//   SHOW                  - Show current configuration
//   RESET_CONFIG          - Clear EEPROM, use defaults (reboots)
//   TDMA_ON               - Enable TDMA (no reboot, no save)
//   TDMA_OFF              - Disable TDMA and reset all data (no reboot, no save)
//   TDMA_STATUS           - Show TDMA status
//   HELP                  - Show available commands

// Non-blocking serial check - call this during processing phase ONLY
inline bool serialCheckForCommand() {
  // Quick check - if no data available, return immediately
  if (!Serial.available()) {
    return serialCmdReady;
  }
  
  // Read available characters (non-blocking, limit iterations)
  int maxRead = 10;  // Limit chars per call to avoid blocking
  while (Serial.available() && maxRead-- > 0) {
    char c = Serial.read();
    
    if (c == '\n' || c == '\r') {
      if (serialCmdIndex > 0) {
        serialCmdBuffer[serialCmdIndex] = '\0';
        serialCmdReady = true;
        return true;
      }
    } else if (serialCmdIndex < SERIAL_CMD_BUFFER_SIZE - 1) {
      serialCmdBuffer[serialCmdIndex++] = c;
    }
  }
  
  return serialCmdReady;
}

// Get the ready command (clears buffer after getting)
inline const char* serialGetCommand() {
  if (serialCmdReady) {
    serialCmdReady = false;
    serialCmdIndex = 0;
    return serialCmdBuffer;
  }
  return nullptr;
}

// Helper: trim leading/trailing spaces
inline char* trimString(char* str) {
  while (*str == ' ') str++;
  char* end = str + strlen(str) - 1;
  while (end > str && *end == ' ') *end-- = '\0';
  return str;
}

// ============= TDMA CONTROL HELPER =============
// tdmaEnabled is managed in main .ino file (volatile for ISR safety)
// This helper is for reset request flag only
inline bool isTdmaResetRequested() {
  bool result = tdmaResetRequested;
  tdmaResetRequested = false;
  return result;
}

inline void requestTdmaReset() {
  tdmaResetRequested = true;
}

#endif // CONFIG_MANAGER_H
