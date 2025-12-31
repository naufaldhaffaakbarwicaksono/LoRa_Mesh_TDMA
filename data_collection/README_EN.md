# Data Collection & Analysis Tools

> **Language / Bahasa:** [🇮🇩 Bahasa Indonesia](README.md) | [🇬🇧 English](README_EN.md)

Tools for data collection, analysis, and visualization of LoRa Mesh TDMA network.

## 📋 Table of Contents

- [Overview](#overview)
- [Tools & Scripts](#tools--scripts)
- [Data Formats](#data-formats)
- [Dependencies Installation](#dependencies-installation)
- [Usage Guide](#usage-guide)
- [Example Outputs](#example-outputs)

---

## Overview

This folder contains tools for:
- **Remote Monitoring**: Real-time mesh network monitoring via WiFi UDP
- **Topology Visualization**: Network structure and inter-node connection visualization
- **Performance Analysis**: PDR (Packet Delivery Ratio), latency, and RSSI analysis
- **Data Collection**: Event log collection and storage for offline analysis

---

## Tools & Scripts

### 1. `wifi_monitor_control.py`

**Function:** UDP server for real-time monitoring and remote node control

**Features:**
- Receive events from all nodes via UDP port 5001
- Display real-time logs (TX, RX, NEIGHBOR, SYNC, FWD)
- Save events to CSV for offline analysis
- Send commands to nodes (start/stop data transmission)

**Usage:**

```bash
# Start monitoring server
python3 wifi_monitor_control.py --port 5001 --output wifi_events.csv

# Options:
# --port        : UDP port for receiving events (default: 5001)
# --output      : CSV output file (default: wifi_events.csv)
# --verbose     : Display detailed logs
```

**Event Log Format:**

```
[2025-12-31 14:30:01.123] Node 2 → TX_OWN: msgId=42, target=1, hop=1, RSSI=-65
[2025-12-31 14:30:01.250] Node 1 ← RX_DATA: msgId=42, from=2, latency=1250us
[2025-12-31 14:30:02.500] Node 3 → TX_FWD: msgId=42, target=1, hop=2
```

**Command Control:**

```bash
# Send command to specific node
echo "START_TX" | nc -u <node_ip> 5002  # Start data transmission
echo "STOP_TX" | nc -u <node_ip> 5002   # Stop data transmission
```

---

### 2. `topology_visualizer.py`

**Function:** Network topology visualization from WiFi events or manual CSV

**Features:**
- Generate topology graph with NetworkX
- Visualize RSSI strength (edge color)
- Display hop distance from gateway
- Export to PNG/PDF

**Usage:**

```bash
# From WiFi events log
python3 topology_visualizer.py --input wifi_events.csv --output topology.png

# From manual topology CSV
python3 topology_visualizer.py --input topology_star.csv --output star_topology.png --mode manual

# Options:
# --input       : Input CSV file
# --output      : Output image (PNG/PDF)
# --mode        : 'auto' (from events) or 'manual' (from topology CSV)
# --layout      : 'spring', 'circular', 'kamada_kawai' (default: spring)
# --show-rssi   : Display RSSI values on edges
# --show-hop    : Display hop distance on nodes
```

**Output:**
- Network graph with:
  - Gateway node (red color)
  - Regular nodes (blue color)
  - Edge thickness = signal strength
  - Node label = ID + hop distance

---

### 3. `analyze_topology_from_csv.py`

**Function:** In-depth topology and performance metrics analysis

**Features:**
- **Topology Analysis**: Node count, edge count, average degree
- **PDR Calculation**: Packet Delivery Ratio per node and network-wide
- **Latency Statistics**: Min/Max/Average latency per hop count
- **RSSI Distribution**: Signal strength histogram
- **Routing Analysis**: Path analysis and bottleneck detection

**Usage:**

```bash
# Analyze from WiFi events
python3 analyze_topology_from_csv.py --input wifi_events.csv --output analysis_report.txt

# Generate visualizations
python3 analyze_topology_from_csv.py --input wifi_events.csv --plot-all

# Options:
# --input       : WiFi events CSV file
# --output      : Text report output (default: analysis_report.txt)
# --plot-all    : Generate all plots (topology, PDR, latency, RSSI)
# --time-window : Time window for analysis (seconds, default: all)
```

**Output Files:**
- `analysis_report.txt` - Complete text report
- `topology.png` - Network topology graph
- `pdr_per_node.png` - PDR bar chart per node
- `latency_by_hops.png` - Latency box plot vs hop count
- `rssi_distribution.png` - RSSI values histogram

**Sample Report:**

```
=== TOPOLOGY ANALYSIS ===
Total Nodes: 6
Total Edges: 8
Average Degree: 2.67
Network Diameter: 3 hops

=== PDR ANALYSIS ===
Network-wide PDR: 94.3%
Node 1 (Gateway): 100.0% (120/120 packets)
Node 2: 96.7% (58/60 packets)
Node 3: 91.2% (52/57 packets)

=== LATENCY ANALYSIS ===
1-hop avg: 1.8ms (min: 1.2ms, max: 2.5ms)
2-hop avg: 5.3ms (min: 4.5ms, max: 6.2ms)
3-hop avg: 10.7ms (min: 9.1ms, max: 12.4ms)

=== RSSI ANALYSIS ===
Average RSSI: -72 dBm
Min RSSI: -95 dBm
Max RSSI: -58 dBm
```

---

### 4. `create_graphs.py`

**Function:** Generate various types of graphs for visual analysis

**Features:**
- Timeline plot (packet transmission over time)
- Latency heatmap (node-to-node)
- Routing path visualization
- TDMA slot utilization

**Usage:**

```bash
# Generate timeline plot
python3 create_graphs.py --type timeline --input wifi_events.csv --output timeline.png

# Generate latency heatmap
python3 create_graphs.py --type heatmap --input wifi_events.csv --output latency_heatmap.png

# Generate routing visualization
python3 create_graphs.py --type routing --input wifi_events.csv --output routing_paths.png

# Options:
# --type        : 'timeline', 'heatmap', 'routing', 'slot_usage'
# --input       : Input CSV file
# --output      : Output image file
# --start-time  : Start time for analysis (format: HH:MM:SS)
# --duration    : Analysis duration (seconds)
```

---

## Data Formats

### WiFi Events CSV Format

**Columns:**
```csv
timestamp,node_id,event_type,msg_id,sender,receiver,hop_count,rssi,snr,latency_us,data
2025-12-31 14:30:01.123,2,TX_OWN,42,2,1,1,-65,8,0,"T:25.3"
2025-12-31 14:30:01.250,1,RX_DATA,42,2,1,1,-65,8,1250,"T:25.3"
2025-12-31 14:30:02.500,3,TX_FWD,42,2,1,2,-72,6,0,"T:25.3"
```

**Event Types:**
- `TX_OWN` - Node transmits own data
- `TX_FWD` - Node forwards data from another node
- `TX_ID` - Node transmits neighbor info (heartbeat)
- `RX_DATA` - Node receives data packet
- `RX_PKT` - Node receives neighbor packet
- `NEIGHBOR_UPDATE` - Neighbor list changed
- `SYNC` - Time sync event

---

### Manual Topology CSV Format

**Columns:**
```csv
node_id,neighbor_id,rssi,snr,hop_distance,bidirectional
1,2,-65,8,1,1
1,3,-68,7,1,1
2,1,-65,8,0,1
2,4,-72,6,2,1
3,1,-68,7,0,1
3,5,-75,5,2,1
3,6,-78,4,2,1
```

**Sample Files:**
- `topology_star.csv` - Star topology (all nodes connect to gateway)
- `topology_branch.csv` - Branch topology (multi-hop tree structure)
- `wifi_events2.csv` - Sample WiFi event log for testing

**Sample Data - topology_star.csv:**
```csv
node_id,neighbor_id,rssi,snr,hop_distance,bidirectional
1,2,-65,8,1,1
1,3,-68,7,1,1
1,4,-72,6,1,1
1,5,-75,5,1,1
2,1,-65,8,0,1
3,1,-68,7,0,1
4,1,-72,6,0,1
5,1,-75,5,0,1
```

**Sample Data - topology_branch.csv:**
```csv
node_id,neighbor_id,rssi,snr,hop_distance,bidirectional
1,2,-65,8,1,1
1,3,-68,7,1,1
2,1,-65,8,0,1
2,4,-72,6,2,1
2,5,-75,5,2,1
3,1,-68,7,0,1
3,6,-78,4,2,1
```

---

## Dependencies Installation

**Requirements:**

```bash
# Python 3.7+
sudo apt update
sudo apt install python3 python3-pip

# Install Python packages
pip3 install numpy pandas matplotlib networkx scipy
```

**Package Details:**
- `numpy` - Numerical computing
- `pandas` - Data manipulation
- `matplotlib` - Plotting and visualization
- `networkx` - Graph analysis
- `scipy` - Scientific computing

**Optional:**

```bash
# For interactive visualization
pip3 install plotly dash

# For high-quality PDF export
sudo apt install texlive-latex-base texlive-fonts-recommended
```

---

## Usage Guide

### Complete Testing Workflow

**1. Setup Monitoring Server**

```bash
# Terminal 1: Start monitoring
cd /home/naufal/LoRa_Mesh_TDMA/data_collection
python3 wifi_monitor_control.py --port 5001 --output wifi_events_test1.csv --verbose
```

**2. Deploy & Run Mesh Network**

```bash
# Flash firmware to all nodes
# Set DEBUG_MODE = DEBUG_MODE_WIFI_MONITOR in settings.h
# Set WIFI_SSID, WIFI_PASS, SERVER_IP

# Power on nodes:
# - Node 1: Gateway (IS_REFERENCE=1, SLOT=1)
# - Node 2-6: Regular nodes (IS_REFERENCE=0, SLOT=2-6)
```

**3. Test Scenario**

```bash
# Wait until network synchronized (check logs)
# Gateway will print: [Node 1] Network synced: 5 neighbors

# Trigger data transmission from specific node
# Use rotary encoder or send command:
echo "START_TX" | nc -u 192.168.1.102 5002  # Node 2 starts sending data
```

**4. Monitor Real-time**

```bash
# Terminal will display:
[14:30:01.123] Node 2 → TX_OWN: msgId=42, target=1, hop=1
[14:30:01.250] Node 1 ← RX_DATA: msgId=42, from=2, latency=1250us
[14:30:02.500] Node 3 → TX_FWD: msgId=42, target=1, hop=2
```

**5. Analyze Data**

```bash
# Stop monitoring (Ctrl+C)

# Generate topology graph
python3 topology_visualizer.py --input wifi_events_test1.csv --output topology_test1.png

# Analyze performance
python3 analyze_topology_from_csv.py --input wifi_events_test1.csv --plot-all

# Generate timeline
python3 create_graphs.py --type timeline --input wifi_events_test1.csv --output timeline_test1.png
```

**6. Review Results**

```bash
# Open visualization results
xdg-open topology_test1.png
xdg-open pdr_per_node.png
xdg-open latency_by_hops.png

# Review text report
cat analysis_report.txt
```

---

## Example Outputs

### Network Topology

![Example Topology](../docs/images/example_topology_star.png)

**Star Topology:**
- Gateway at center (Node 1)
- 5 nodes connected directly
- PDR: 98.5%
- Avg latency: 1.8ms

### Latency Analysis

![Example Latency](../docs/images/example_latency.png)

**Latency Distribution:**
- 1-hop: 1.2-2.5ms (avg: 1.8ms)
- 2-hop: 4.5-6.2ms (avg: 5.3ms)
- 3-hop: 9.1-12.4ms (avg: 10.7ms)

### Routing Paths

![Example Routing](../docs/images/example_routing.png)

**Multi-hop Routing:**
- Node 6 → Node 5 → Node 3 → Gateway
- Automatic path selection
- Loop prevention active

---

## Troubleshooting

### Issue 1: WiFi Events Not Recorded

**Symptoms:**
```
Monitoring server running...
(no events incoming)
```

**Solutions:**
1. Check node WiFi connection:
   ```bash
   # Ping node
   ping 192.168.1.102  # Node IP
   ```
2. Verify SERVER_IP in settings.h:
   ```c
   #define SERVER_IP "192.168.1.100"  // Server computer IP
   ```
3. Check firewall:
   ```bash
   sudo ufw allow 5001/udp
   ```

### Issue 2: Python Import Error

**Symptoms:**
```
ImportError: No module named 'networkx'
```

**Solutions:**
```bash
# Install missing package
pip3 install networkx

# Or install all requirements
pip3 install numpy pandas matplotlib networkx scipy
```

### Issue 3: Plot Not Displayed

**Symptoms:**
```
Script finishes but no image output
```

**Solutions:**
```bash
# Ensure correct matplotlib backend
export MPLBACKEND=Agg

# Re-run script
python3 topology_visualizer.py --input wifi_events.csv --output topology.png
```

---

## Tips & Best Practices

### 1. Data Collection

- **Test Duration:** Minimum 5 minutes for reliable statistics
- **Sample Size:** Minimum 100 packets per node for PDR accuracy
- **Time Sync:** Ensure NTP sync active for accurate latency measurement

### 2. Topology Testing

- **Topology Variations:** Test at least 3 topologies (star, chain, mesh)
- **Distance Test:** Vary inter-node distance (5m, 10m, 20m)
- **Obstacle Test:** Test with/without obstacles (walls, furniture)

### 3. Performance Analysis

- **Baseline:** Establish baseline with ideal conditions first
- **Stress Test:** Increase packet rate to test collision handling
- **Failure Test:** Simulate node failure (power off) to test recovery

### 4. Visualization

- **Multiple Views:** Generate various plot types for complete insights
- **Export Quality:** Use `--dpi 300` for publication-quality images
- **Interactive:** Use plotly for interactive exploration

---

## File Structure

```
data_collection/
├── README.md                          # Documentation (Indonesian)
├── README_EN.md                       # Documentation (English)
├── wifi_monitor_control.py            # Real-time monitoring server
├── topology_visualizer.py             # Topology visualization
├── analyze_topology_from_csv.py       # Performance analysis
├── create_graphs.py                   # Graph generation
├── topology_star.csv                  # Sample: star topology
├── topology_branch.csv                # Sample: branch topology
├── wifi_events2.csv                   # Sample: WiFi event log
└── output/                            # Generated outputs
    ├── topology_test1.png
    ├── pdr_per_node.png
    ├── latency_by_hops.png
    └── analysis_report.txt
```

---

## Advanced Usage

### Custom Analysis Script

```python
import pandas as pd
import matplotlib.pyplot as plt

# Load WiFi events
df = pd.read_csv('wifi_events.csv')

# Filter TX events only
tx_events = df[df['event_type'].str.contains('TX')]

# Calculate per-node packet count
packets_per_node = tx_events.groupby('node_id').size()

# Plot
packets_per_node.plot(kind='bar')
plt.xlabel('Node ID')
plt.ylabel('Packet Count')
plt.title('TX Packets per Node')
plt.savefig('tx_packets.png')
```

### Real-time Dashboard

```python
# Use plotly Dash for live dashboard
import dash
from dash import dcc, html
from dash.dependencies import Input, Output
import pandas as pd

app = dash.Dash(__name__)

@app.callback(Output('graph', 'figure'), Input('interval', 'n_intervals'))
def update_graph(n):
    df = pd.read_csv('wifi_events.csv')
    # Update plot...
    return figure

app.run_server(debug=True)
```

---

## Contributing

To add new analysis scripts:

1. Fork repository
2. Add script to `data_collection/` folder
3. Update README with documentation
4. Submit pull request

---

## Author

**Naufal Dhaffa Akbar Wicaksono**  
Universitas Gadjah Mada  
© 2025

---

**Last Updated:** December 31, 2025
