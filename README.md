# LoRa Mesh Network Node - Ra01S Library Version

## 📋 Deskripsi Proyek

Sistem jaringan mesh LoRa menggunakan modul SX1262 (Ra01S) dengan protokol TDMA (Time Division Multiple Access) untuk komunikasi multi-hop antar node. Proyek ini dirancang untuk ESP32 dengan fitur-fitur:

- ✅ TDMA-based mesh networking dengan automatic slot assignment
- ✅ Collision avoidance melalui neighbor discovery
- ✅ Multi-hop message routing dengan message tracking
- ✅ OLED display (SSD1306) dengan navigasi rotary encoder
- ✅ Integrasi sensor (temperature, humidity, battery monitoring)
- ✅ Forward queue untuk relay pesan
- ✅ WiFi batch messaging ke server (opsional)

## 🔧 Hardware Requirements

### Komponen Utama
- **ESP32** DevKit atau compatible board
- **SX1262 LoRa Module** (Ra01S compatible)
- **OLED Display** SSD1306 128x64 (I2C)
- **Rotary Encoder** dengan push button
- **Sensor** (opsional):
  - AHT10 Temperature/Humidity sensor
  - INA219 Power monitor

### Pin Configuration

#### ESP32 to SX1262 (Ra01S)
```
ESP32 Pin    →    SX1262/Ra01S
──────────────────────────────
GPIO 5       →    NSS (Chip Select)
GPIO 18      →    SCK (SPI Clock)
GPIO 19      →    MISO
GPIO 23      →    MOSI
GPIO 4       →    RESET
GPIO 22      →    BUSY
GPIO 21      →    DIO1
GPIO 26      →    TXEN
GPIO 27      →    RXEN
```

#### I2C Peripherals
```
ESP32 Pin    →    Peripheral
──────────────────────────────
GPIO 16      →    SDA (OLED Display)
GPIO 17      →    SCL (OLED Display)
```

#### Rotary Encoder
```
ESP32 Pin    →    Encoder
──────────────────────────────
GPIO 33      →    CLK (A)
GPIO 32      →    DT (B)
GPIO 25      →    SW (Button)
```

## 📚 Library Dependencies

### Arduino Libraries yang Dibutuhkan

Install melalui Arduino Library Manager:

1. **Adafruit GFX Library** - Graphics core library
2. **Adafruit SSD1306** - OLED display driver
3. **Wire.h** - I2C communication (built-in)
4. **SPI.h** - SPI communication (built-in)

### Custom Libraries (Included)

- **Ra01S.h / Ra01S.cpp** - SX1262 LoRa module driver
  - Based on SX126x class
  - Polling mode (non-blocking)
  - Support untuk LoRa transmission/reception

## 🚀 Quick Start Guide

### 1. Persiapan Hardware

1. Sambungkan semua komponen sesuai pin configuration di atas
2. Pastikan power supply ESP32 stabil (min 500mA)
3. Cek koneksi SPI dan I2C dengan multimeter jika perlu

### 2. Konfigurasi Node

Edit file **`settings.h`** sebelum upload:

```cpp
// Node Configuration
#define DEVICE_ID 1          // ID unik untuk setiap node (1-255)
#define IS_REFERENCE 0       // 1 untuk reference node, 0 untuk node biasa
#define FIX_SLOT 0           // 1 untuk fixed slot, 0 untuk auto-assign
#define SLOT_DEVICE 0        // Slot number jika FIX_SLOT = 1
```

### 3. Konfigurasi LoRa

Parameter LoRa sudah di-set optimal untuk jarak menengah:

```cpp
#define RF_FREQUENCY 915000000UL     // 915 MHz (sesuaikan dengan region)
#define TX_OUTPUT_POWER 4            // 4 dBm (bisa dinaikkan max 22 dBm)
#define LORA_SPREADING_FACTOR 7      // SF7 (balance speed/range)
#define LORA_BANDWIDTH SX126X_LORA_BW_125_0   // 125 kHz
#define LORA_CODINGRATE SX126X_LORA_CR_4_5    // 4/5 error correction
```

### 4. Kompilasi dan Upload

1. Buka **node_ra01s.ino** di Arduino IDE
2. Pilih board: **ESP32 Dev Module**
3. Pilih port serial yang sesuai
4. Upload sketch

### 5. Monitoring

Buka Serial Monitor dengan baud rate **115200** untuk melihat:
- Neighbor discovery
- Slot assignment
- Message transmission/reception
- RSSI/SNR values
- Forward queue status

## 📖 Struktur File

```
node_ra01s/
├── node_ra01s.ino       # Main program file
├── Ra01S.h              # Ra01S/SX1262 library header
├── Ra01S.cpp            # Ra01S/SX1262 library implementation
├── settings.h           # Configuration file (EDIT THIS!)
└── README.md            # Dokumentasi (file ini)
```

## 🔄 Cara Kerja Sistem

### TDMA Slot Mechanism

Sistem menggunakan 13 slot waktu (configurable):

```
Slot 0: [Reference Node - Neighbor Broadcast]
Slot 1-12: [Regular Nodes - Data + Neighbor Info]
```

**Timing per Cycle:**
- Total cycle: ~2 detik
- TX duration: ~125 ms per slot
- RX duration: ~1.8 detik (listen to other slots)

### Packet Structure

#### Neighbor Broadcast Packet (Reference Node)
```
[Packet Type: 0x01] [Node ID] [Timestamp] [Tracking Hops]
```

#### Unified Data Packet (Regular Nodes)
```
[Packet Type: 0x03] [Node ID] [Message ID] [Hop Count] 
[Neighbor Count] [Neighbor IDs...] [Sensor Data] [Tracking Hops]
```

### Message Forwarding

Sistem menggunakan **FIFO queue** untuk relay pesan:

1. Node menerima pesan dari node lain
2. Pesan di-add ke forward queue
3. Pada slot TX berikutnya, pesan di-forward
4. Tracking hops mencatat jalur pesan

## 🎛️ OLED Display Menu

Navigasi menggunakan rotary encoder:

### Menu Pages:
1. **Home** - Status node, neighbor count, RSSI/SNR
2. **Neighbors** - Daftar neighbor dengan RSSI
3. **Messages** - Last received message
4. **Stats** - TX/RX packet count, success rate
5. **Config** - Node settings

**Kontrol:**
- Rotate: Navigasi menu/scroll
- Push Button: Select/Enter

## 🧪 Testing & Troubleshooting

### Test Sederhana

1. **Single Node Test:**
   ```
   Upload ke 1 node → Check serial output → 
   Verifikasi OLED display → Check sensor readings
   ```

2. **Two Node Test:**
   ```
   Upload ke 2 nodes (ID berbeda) →
   Tunggu neighbor discovery (5-10 detik) →
   Check "Neighbours" di OLED →
   Send message dari satu node
   ```

3. **Multi-hop Test:**
   ```
   Setup 3+ nodes dalam range berbeda →
   Node A ↔ Node B ↔ Node C
   (A tidak reach C directly) →
   Send dari A, verify received di C
   ```

### Common Issues

#### 1. LoRa Tidak Transmit
```
Solusi:
- Cek koneksi SPI (SCK, MISO, MOSI, NSS)
- Verify RESET dan BUSY pin
- Check TX_OUTPUT_POWER (jangan 0)
- Monitor serial untuk error messages
```

#### 2. OLED Display Blank
```
Solusi:
- Verify I2C address (biasanya 0x3C)
- Cek SDA/SCL connections
- Test dengan I2C scanner sketch
```

#### 3. Neighbor Discovery Gagal
```
Solusi:
- Pastikan RF_FREQUENCY sama semua node
- Check spreading factor dan bandwidth
- Verify antenna terhubung
- Increase TX_OUTPUT_POWER
```

#### 4. High Packet Loss
```
Solusi:
- Reduce TX_OUTPUT_POWER jika terlalu dekat
- Check interference (WiFi, microwave)
- Adjust TDMA timing (increase guard time)
- Verify clock sync antar nodes
```

## 📊 Performance Metrics

### Measured Timing (ESP32 + SX1262)

```
Operation              Duration
─────────────────────────────────
writeBuffer(48B)       500 μs
setTx()                350 μs
LoRa air time (SF7)    98 ms
TX callback            100 μs
setRx()                350 μs
RX callback            200 μs
```

### Network Capacity

- **Max nodes:** 13 (satu reference + 12 regular)
- **Max neighbors per node:** 10
- **Message queue depth:** 8 messages
- **Tracking hops:** 8 hops
- **Payload size:** 48 bytes per packet

## 🔐 Security Notes

⚠️ **Penting:** Sistem ini TIDAK menggunakan enkripsi.

Untuk production:
- Implement AES encryption pada payload
- Add message authentication (HMAC)
- Implement key exchange mechanism

## 🛠️ Advanced Configuration

### WiFi Server Integration

Enable WiFi batch messaging dalam `settings.h`:

```cpp
#define ENABLE_WIFI 1
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* serverURL = "http://your-server.com/api/messages";
```

### Timing Optimization

Adjust TDMA timing untuk aplikasi spesifik:

```cpp
// In settings.h
#define TX_GUARD_TIME_US 5000   // Default: 5ms
#define CYCLE_DURATION_US 2000000  // Default: 2 seconds
```

## 📝 Migration Notes (dari SX1262_TimingControl)

Proyek ini migrasi dari library SX1262_TimingControl ke Ra01S:

**Key Changes:**
- ✅ Polling mode (non-blocking) instead of callbacks
- ✅ No semaphores needed
- ✅ Simplified interrupt handling
- ✅ Direct RSSI/SNR reading via GetPacketStatus()

**Benefits:**
- Lebih stabil (no race conditions)
- Easier debugging
- Lower memory usage
- Better timing precision

## 👥 Credits & License

**Author:** [Your Name]  
**Version:** 1.0.0  
**Date:** December 2025

**Based on:**
- Ra01S Library (SX126x class)
- Adafruit Display Libraries
- SX1262_TimingControl (original research)

**License:** MIT License (atau sesuai kebutuhan)

## 📞 Support

Untuk pertanyaan atau issue:
1. Check troubleshooting section
2. Review serial debug output
3. Open issue di GitHub repository

---

**⚡ Happy Meshing! ⚡**
