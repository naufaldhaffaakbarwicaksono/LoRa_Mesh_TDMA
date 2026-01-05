# LoRa Mesh TDMA

Jaringan mesh LoRa multi-hop dengan **self-healing** dan **auto-routing** berbasis TDMA (Time Division Multiple Access).

## ✨ Fitur Utama

| Fitur | Deskripsi |
|-------|-----------|
| **Multi-hop Mesh** | Data dapat diteruskan melalui beberapa node untuk mencapai gateway |
| **Auto-Routing** | Routing path otomatis berdasarkan hop distance dan kualitas sinyal (RSSI) |
| **Self-Healing** | Jaringan otomatis menyesuaikan rute jika ada node yang mati atau koneksi terputus |
| **TDMA** | Collision-free dengan pembagian slot waktu untuk setiap node |
| **Neighbor Discovery** | Node otomatis mendeteksi dan memelihara daftar tetangga aktif |

## 🔧 Hardware

- **MCU:** ESP32
- **LoRa:** SX1262 (Ra01S module)
- **Display:** OLED SSD1306 128x64
- **Sensor:** AHT10 (temperature/humidity), INA219 (battery)

## 📁 Struktur Project

```
LoRa_Mesh_TDMA/
├── firmware/          # Kode Arduino untuk node
│   ├── firmware.ino   # Main program
│   ├── settings.h     # Konfigurasi node (ID, slot, WiFi, dll)
│   └── Ra01S.*        # Library SX1262
├── server/            # Python server untuk monitoring
└── docs/              # Dokumentasi tambahan
```

## ⚡ Quick Start

### 1. Konfigurasi Node
Edit `firmware/settings.h`:
```cpp
#define DEVICE_ID 2        // ID unik node (1=gateway)
#define IS_REFERENCE 0     // 1=gateway, 0=sensor node
#define SLOT_DEVICE 2      // Slot TDMA
```

### 2. Upload Firmware
Upload `firmware.ino` ke setiap ESP32 dengan konfigurasi berbeda.

### 3. Jalankan
- Gateway otomatis menjadi referensi waktu (hop=0)
- Sensor node akan sync dan menentukan hop distance secara otomatis
- Data sensor dikirim melalui jalur optimal ke gateway

## 🔄 Cara Kerja

```
Sensor Node (hop=2) ──► Relay Node (hop=1) ──► Gateway (hop=0)
         │                     │                    │
    [Auto-routing]      [Store & Forward]     [Data Collection]
```

1. **Setiap node broadcast** informasi neighbor di slot TDMA masing-masing
2. **Hop distance dihitung** berdasarkan jarak ke gateway (Bellman-Ford)
3. **Data sensor dikirim** ke neighbor dengan hop lebih rendah
4. **Jika node mati**, tetangga akan menemukan rute alternatif (self-healing)

## 📝 Serial Commands

| Command | Fungsi |
|---------|--------|
| `STATUS` | Lihat status node |
| `SHOW_RSSI` | Lihat konfigurasi RSSI |
| `SET_TXPOWER <dBm>` | Set TX power (-9 s/d +22) |
| `HELP` | Daftar semua perintah |

## 📄 License

MIT License
