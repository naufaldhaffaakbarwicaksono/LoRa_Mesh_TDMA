# Data Collection Tools

Tools untuk monitoring dan analisis jaringan LoRa Mesh.

## 📁 Scripts

| Script | Fungsi |
|--------|--------|
| `wifi_monitor_control.py` | Real-time monitoring via UDP |
| `topology_visualizer.py` | Visualisasi topologi jaringan |
| `analyze_topology_from_csv.py` | Analisis PDR, latency, RSSI |
| `create_graphs.py` | Generate timeline & heatmap |

## ⚡ Quick Start

### 1. Install Dependencies
```bash
pip3 install numpy pandas matplotlib networkx
```

### 2. Jalankan Monitoring
```bash
python3 wifi_monitor_control.py --port 5001 --output events.csv
```

### 3. Analisis Data
```bash
# Visualisasi topologi
python3 topology_visualizer.py --input events.csv --output topology.png

# Analisis performance
python3 analyze_topology_from_csv.py --input events.csv --plot-all
```

## 📊 Sample Data

- `topology_star.csv` - Contoh topologi star
- `topology_branch.csv` - Contoh topologi multi-hop
- `wifi_events2.csv` - Sample event log

## 📝 Event Format

```csv
timestamp,node_id,event_type,msg_id,rssi,latency_us
2025-12-31 14:30:01,2,TX_OWN,42,-65,0
2025-12-31 14:30:01,1,RX_DATA,42,-65,1250
```
