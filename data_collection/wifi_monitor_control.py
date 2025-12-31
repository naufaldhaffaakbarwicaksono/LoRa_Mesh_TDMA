#!/usr/bin/env python3
"""
WiFi Relay Node Monitor & Control
Remote monitoring and control for relay node failure testing

Usage:
  python3 wifi_monitor_control.py                    # Default output: wifi_events.csv
  python3 wifi_monitor_control.py -o test1.csv       # Custom output: test1.csv
  python3 wifi_monitor_control.py -o PDR_test_1      # Custom output: PDR_test_1.csv (auto-adds .csv)
"""

import socket
import threading
import time
from datetime import datetime
from collections import defaultdict
import sys
import argparse

# ============= CONFIGURATION =============
SERVER_IP = '0.0.0.0'  # Listen on all interfaces
MONITOR_UDP_PORT = 5001  # Receive events from nodes
COMMAND_UDP_PORT = 5002  # Send commands to nodes

# Node IP addresses (configure based on your WiFi network)
NODE_IPS = {
    1: '192.168.0.9',   # Gateway
    2: '192.168.0.11',  # Node 2
    3: '192.168.0.8',   # Node 3
    4: '192.168.0.10',  # Node 4
    5: '192.168.0.12',  # Node 5
    6: '192.168.0.13',  # Node 6
}

# Event display symbols
EVENT_ICONS = {
    'NEIGHBOR_UPDATE': '[N]',
    'NEIGHBOR_ADDED': '[N+]',
    'NEIGHBOR_REMOVED': '[N-]',
    'BIDIR_LINK': '[⟷]',
    'RSSI_LOW': '[L]',
    'HOP_CHANGE': '[H]',
    'FORWARD_ENQUEUE': '[F]',
    'GW_RX_DATA': '[G]',
    'CMD_EXECUTED': '[C]',
    'STATUS': '[S]',
    'CYCLE_SYNC': '[SYNC]',
    'CYCLE_VAL': '[VAL]',
    'LATENCY': '[⏱]',
    'PDR_NETWORK': '[PDR*]',
    'PDR_NODE': '[PDR]',
    'PKT_RX': '[RX]'
}
# =========================================

class WiFiMonitorControl:
    def __init__(self, output_file='wifi_events.csv'):
        self.events = defaultdict(list)
        self.lock = threading.Lock()
        self.running = False
        self.monitor_sock = None
        self.command_sock = None
        self.command_queue = []  # Queue for commands to send
        self.command_lock = threading.Lock()
        self.operation_number = 0  # Global operation counter
        self.operation_lock = threading.Lock()
        self.start_timestamp_us = None  # Reference timestamp from first event (node firmware time)
        
        # Custom output filename
        self.output_file = output_file
        if not self.output_file.endswith('.csv'):
            self.output_file += '.csv'
        
        self.stats = {
            'events_received': 0,
            'parse_errors': 0,
            'commands_sent': 0,
            'monitor_reconnects': 0,
            'command_errors': 0
        }
        
        # PDR & Latency tracking
        self.pdr_stats = {}  # {node_id: {sender_id: {pdr, expected, received, ...}}}
        self.latency_stats = {}  # {node_id: {sender_id: {latencies: [], avg, min, max}}}
        self.stats_lock = threading.Lock()
        
        # Display options
        self.show_absolute_time = False  # Toggle for displaying absolute NTP time instead of relative
        
    def parse_event(self, data):
        """Parse different event formats:
        - EVENT,TIMESTAMP_US,NODE_ID,TYPE,DETAILS
        - LATENCY,TIMESTAMP_US,NODE_ID,details...
        - PDR_NETWORK,TIMESTAMP_US,NODE_ID,details...
        - PDR_NODE,TIMESTAMP_US,NODE_ID,details...
        - PKT_RX,TIMESTAMP_US,NODE_ID,details...
        """
        try:
            msg = data.decode('utf-8').strip()
            parts = msg.split(',', 4)
            
            if len(parts) >= 4:
                msg_type = parts[0]
                
                # Handle all message types
                if msg_type in ['EVENT', 'LATENCY', 'PDR_NETWORK', 'PDR_NODE', 'PKT_RX']:
                    timestamp_us = int(parts[1])
                    node_id = int(parts[2])
                    
                    if msg_type == 'EVENT':
                        # Standard event: EVENT,TIMESTAMP,NODE,TYPE,DETAILS
                        if len(parts) >= 5:
                            event_type = parts[3]
                            details = parts[4]
                        else:
                            return None
                    else:
                        # Special data types (LATENCY, PDR_*, PKT_RX)
                        event_type = msg_type
                        details = ','.join(parts[3:]) if len(parts) > 3 else ''
                    
                    return {
                        'timestamp_us': timestamp_us,
                        'node_id': node_id,
                        'type': event_type,
                        'details': details,
                        'recv_time': datetime.now(),
                        'raw': msg
                    }
                else:
                    print(f"[PARSE_WARN] Unknown message type: {msg[:80]}")
                    self.stats['parse_errors'] += 1
            else:
                print(f"[PARSE_WARN] Invalid format: {msg[:80]}")
                self.stats['parse_errors'] += 1
        except ValueError as e:
            print(f"[PARSE_ERROR] Value error: {e}")
            self.stats['parse_errors'] += 1
        except Exception as e:
            print(f"[PARSE_ERROR] {e}: {data[:100]}")
            self.stats['parse_errors'] += 1
        return None
    
    def log_event(self, event):
        """Store event (thread-safe) and update PDR/Latency stats"""
        with self.lock:
            self.events[event['node_id']].append(event)
            self.stats['events_received'] += 1
        
        # Update PDR/Latency statistics
        self.update_statistics(event)
    
    def update_statistics(self, event):
        """Update PDR and latency statistics from events"""
        with self.stats_lock:
            node_id = event['node_id']
            event_type = event['type']
            details = event['details']
            
            # Parse LATENCY events
            if event_type == 'LATENCY':
                # Format: Node<X>,MsgID:<id>,Hop:<n>,Lat:<ms>ms,RSSI:<rssi>dBm,SNR:<snr>dB
                try:
                    parts = details.split(',')
                    sender_str = parts[0].replace('Node', '')
                    sender_id = int(sender_str)
                    
                    # Extract latency value
                    for part in parts:
                        if 'Lat:' in part:
                            lat_str = part.split(':')[1].replace('ms', '')
                            latency_ms = float(lat_str)
                            
                            # Store latency
                            if node_id not in self.latency_stats:
                                self.latency_stats[node_id] = {}
                            if sender_id not in self.latency_stats[node_id]:
                                self.latency_stats[node_id][sender_id] = {'latencies': []}
                            
                            self.latency_stats[node_id][sender_id]['latencies'].append(latency_ms)
                            
                            # Calculate statistics
                            lats = self.latency_stats[node_id][sender_id]['latencies']
                            self.latency_stats[node_id][sender_id]['avg'] = sum(lats) / len(lats)
                            self.latency_stats[node_id][sender_id]['min'] = min(lats)
                            self.latency_stats[node_id][sender_id]['max'] = max(lats)
                            self.latency_stats[node_id][sender_id]['count'] = len(lats)
                            break
                except Exception as e:
                    pass  # Ignore parse errors
            
            # Parse PDR_NODE events
            elif event_type == 'PDR_NODE':
                # Format: Node<X>,Seq:<n>,Exp:<n>,Rx:<n>,Gaps:<n>,PDR:<pct>%,...
                try:
                    parts = details.split(',')
                    sender_str = parts[0].replace('Node', '')
                    sender_id = int(sender_str)
                    
                    pdr_data = {}
                    for part in parts:
                        if ':' in part:
                            key, val = part.split(':', 1)
                            key = key.strip()
                            val = val.replace('%', '').strip()
                            if key in ['Seq', 'Exp', 'Rx', 'Gaps']:
                                pdr_data[key] = int(val)
                            elif key == 'PDR':
                                pdr_data[key] = float(val)
                            elif key in ['LatAvg', 'LatMin', 'LatMax']:
                                pdr_data[key] = float(val.replace('ms', ''))
                    
                    # Store PDR stats
                    if node_id not in self.pdr_stats:
                        self.pdr_stats[node_id] = {}
                    self.pdr_stats[node_id][sender_id] = pdr_data
                    
                except Exception as e:
                    pass  # Ignore parse errors
            
            # Parse PDR_NETWORK events
            elif event_type == 'PDR_NETWORK':
                # Format: TOTAL,Exp:<n>,Rx:<n>,Lost:<n>,PDR:<pct>%
                try:
                    pdr_data = {}
                    parts = details.split(',')
                    for part in parts:
                        if ':' in part:
                            key, val = part.split(':', 1)
                            key = key.strip()
                            val = val.replace('%', '').strip()
                            if key in ['Exp', 'Rx', 'Lost']:
                                pdr_data[key] = int(val)
                            elif key == 'PDR':
                                pdr_data[key] = float(val)
                    
                    # Store network-wide PDR
                    if node_id not in self.pdr_stats:
                        self.pdr_stats[node_id] = {}
                    self.pdr_stats[node_id]['NETWORK'] = pdr_data
                    
                except Exception as e:
                    pass  # Ignore parse errors
    
    
    def monitor_thread(self):
        """UDP monitoring server - receive events from nodes"""
        retry_count = 0
        max_retries = 5
        
        while self.running and retry_count < max_retries:
            try:
                # Create and configure monitor socket
                self.monitor_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.monitor_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                self.monitor_sock.bind((SERVER_IP, MONITOR_UDP_PORT))
                self.monitor_sock.settimeout(1.0)
                print(f"[MONITOR_SERVER] OK - Listening on 0.0.0.0:{MONITOR_UDP_PORT}")
                retry_count = 0  # Reset retry count on successful start
                
                # Main receive loop
                while self.running:
                    try:
                        data, addr = self.monitor_sock.recvfrom(2048)
                        event = self.parse_event(data)
                        if event:
                            self.log_event(event)
                            self.display_event(event)
                    except socket.timeout:
                        continue
                    except Exception as e:
                        if self.running:
                            print(f"[MONITOR_SERVER] Receive error: {e}")
                            break  # Break inner loop to reconnect
                
                # Close socket before reconnecting
                if self.monitor_sock:
                    self.monitor_sock.close()
                    
            except Exception as e:
                retry_count += 1
                print(f"[MONITOR_SERVER] ERROR - Failed to start (retry {retry_count}/{max_retries}): {e}")
                self.stats['monitor_reconnects'] += 1
                if self.running and retry_count < max_retries:
                    time.sleep(2)  # Wait before retry
        
        if self.monitor_sock:
            self.monitor_sock.close()
        print("[MONITOR_SERVER] Thread stopped")
    
    def display_event(self, event):
        """Display event with color coding, operation number, and relative timestamp"""
        icon = EVENT_ICONS.get(event['type'], '•')
        
        # Set reference timestamp from first event OR update if new timestamp is earlier
        # This handles NTP sync differences between nodes
        if self.start_timestamp_us is None:
            self.start_timestamp_us = event['timestamp_us']
            # Convert to readable absolute time
            from datetime import datetime
            abs_time = datetime.fromtimestamp(event['timestamp_us'] / 1_000_000.0)
            print(f"[INIT] Reference timestamp set to: {abs_time.strftime('%Y-%m-%d %H:%M:%S.%f')}")
        elif event['timestamp_us'] < self.start_timestamp_us:
            # Update reference if we receive an earlier timestamp (different node NTP sync)
            old_ref = self.start_timestamp_us
            self.start_timestamp_us = event['timestamp_us']
            from datetime import datetime
            abs_time = datetime.fromtimestamp(event['timestamp_us'] / 1_000_000.0)
            print(f"[INIT] Reference timestamp updated to: {abs_time.strftime('%Y-%m-%d %H:%M:%S.%f')} (earlier by {(old_ref - event['timestamp_us'])/1000:.1f}ms)")
        
        # Calculate relative seconds since reference (NTP-based, consistent)
        elapsed_sec = (event['timestamp_us'] - self.start_timestamp_us) / 1_000_000.0
        
        # Safety check - should never be negative now, but just in case
        if elapsed_sec < 0:
            elapsed_sec = 0.0
        
        # Format time display (absolute or relative)
        if self.show_absolute_time:
            from datetime import datetime
            abs_time = datetime.fromtimestamp(event['timestamp_us'] / 1_000_000.0)
            time_display = abs_time.strftime('%H:%M:%S.%f')[:-3]  # HH:MM:SS.mmm
        else:
            time_display = f"{elapsed_sec:7.1f}s"
        
        # Increment operation number for significant events
        with self.operation_lock:
            if event['type'] in ['NEIGHBOR_ADDED', 'NEIGHBOR_REMOVED', 'BIDIR_LINK', 
                                'HOP_CHANGE', 'CMD_EXECUTED', 'RSSI_LOW']:
                self.operation_number += 1
                op_num = self.operation_number
            else:
                op_num = None
        
        # Extract RSSI from details if available
        rssi_display = ""
        if 'RSSI:' in event['details']:
            try:
                rssi_start = event['details'].find('RSSI:') + 5
                rssi_end = event['details'].find('dBm', rssi_start)
                if rssi_end > rssi_start:
                    rssi = event['details'][rssi_start:rssi_end]
                    rssi_display = f" 📶{rssi}dBm"
            except:
                pass
        
        # Filter some verbose events
        if event['type'] in ['PDR_NODE', 'PKT_RX']:
            # Skip individual PDR_NODE and PKT_RX to reduce output
            return
        elif event['type'] == 'LATENCY':
            # Only display if latency is high or every Nth packet
            try:
                if 'Lat:' in event['details']:
                    lat_str = event['details'].split('Lat:')[1].split('ms')[0]
                    lat_ms = float(lat_str)
                    # Display if latency > 500ms or 1 in 10 packets
                    if lat_ms <= 500 and (self.stats['events_received'] % 10 != 0):
                        return
            except:
                pass
        
        # Standard event display
        if op_num:
            print(f"[{time_display}] [OP#{op_num:03d}] {icon} Node {event['node_id']} | "
                  f"{event['type']}: {event['details']}{rssi_display}")
        else:
            print(f"[{time_display}] {icon} Node {event['node_id']} | "
                  f"{event['type']}: {event['details']}{rssi_display}")
    
    def command_thread(self):
        """UDP command client - send commands to nodes"""
        print(f"[COMMAND_CLIENT] Started")
        
        while self.running:
            try:
                # Check command queue
                with self.command_lock:
                    if len(self.command_queue) > 0:
                        node_id, command = self.command_queue.pop(0)
                    else:
                        node_id, command = None, None
                
                if node_id is not None:
                    # Create socket if needed
                    if not self.command_sock:
                        self.command_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        self.command_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    
                    # Send command
                    message = f"CMD,{node_id},{command}"
                    
                    if node_id == 0:  # Broadcast
                        print(f"[COMMAND_CLIENT] BROADCAST - {command}")
                        for nid, ip in NODE_IPS.items():
                            try:
                                self.command_sock.sendto(message.encode(), (ip, COMMAND_UDP_PORT))
                                print(f"  -> Node {nid} ({ip})")
                            except Exception as e:
                                print(f"  ERROR - Node {nid}: {e}")
                                self.stats['command_errors'] += 1
                    else:
                        if node_id in NODE_IPS:
                            ip = NODE_IPS[node_id]
                            try:
                                self.command_sock.sendto(message.encode(), (ip, COMMAND_UDP_PORT))
                                print(f"[COMMAND_CLIENT] OK - Node {node_id} ({ip}): {command}")
                                self.stats['commands_sent'] += 1
                            except Exception as e:
                                print(f"[COMMAND_CLIENT] ERROR - {e}")
                                self.stats['command_errors'] += 1
                        else:
                            print(f"[COMMAND_CLIENT] ERROR - Unknown node ID: {node_id}")
                else:
                    # No command in queue, sleep briefly
                    time.sleep(0.1)
                    
            except Exception as e:
                print(f"[COMMAND_CLIENT] Error: {e}")
                self.stats['command_errors'] += 1
                time.sleep(1)
        
        if self.command_sock:
            self.command_sock.close()
        print("[COMMAND_CLIENT] Thread stopped")
    
    def send_command(self, node_id, command):
        """Queue command for sending (thread-safe)"""
        with self.command_lock:
            self.command_queue.append((node_id, command))
        return True
    
    def command_interface(self):
        """Interactive command interface"""
        print("\n" + "="*60)
        print("COMMAND INTERFACE")
        print("="*60)
        print("Commands:")
        print("  STOP <node>      - Stop TDMA (simulate failure)")
        print("  START <node>     - Start TDMA (resume)")
        print("  STATUS <node>    - Get node status")
        print("  REBOOT <node>    - Reboot node")
        print("  PING <node>      - Test WiFi connectivity")
        print("  TIME [abs|rel]   - Toggle time display (absolute NTP or relative)")
        print("  CYCLE <node>     - Check cycle validation status")
        print("  PDR <node>       - Request PDR stats from node")
        print("  BROADCAST <cmd>  - Send to all nodes")
        print("  STATS            - Show statistics")
        print("  PDR_STATS        - Show PDR & Latency statistics")
        print("  SAVE [file]      - Save events to CSV")
        print("  ANALYZE          - Analyze routing changes")
        print("  DEBUG            - Show cycle sync debug info")
        print("  EXPORT_TOPOLOGY  - Export data for topology visualizer")
        print("  QUIT             - Exit")
        print("="*60 + "\n")
        
        while self.running:
            try:
                cmd_input = input("CMD> ").strip()
                if not cmd_input:
                    continue
                
                parts = cmd_input.upper().split()
                cmd = parts[0]
                
                if cmd == 'QUIT':
                    print("[INFO] Stopping monitor...")
                    self.running = False
                    break
                    
                elif cmd in ['STOP', 'START', 'REBOOT', 'STATUS', 'PING', 'CYCLE', 'PDR']:
                    if len(parts) < 2:
                        print(f"[ERROR] Usage: {cmd} <node_id>")
                        continue
                    try:
                        node_id = int(parts[1])
                        if cmd in ['STOP', 'START']:
                            full_cmd = f"TDMA_{cmd}"
                        elif cmd == 'CYCLE':
                            full_cmd = "CYCLE_STATUS"
                        elif cmd == 'PDR':
                            full_cmd = "PDR_STATS"
                        else:
                            full_cmd = cmd
                        self.send_command(node_id, full_cmd)
                    except ValueError:
                        print("[ERROR] Invalid node ID")
                        
                elif cmd == 'BROADCAST':
                    if len(parts) < 2:
                        print("[ERROR] Usage: BROADCAST <command>")
                        continue
                    broadcast_cmd = ' '.join(parts[1:])
                    self.send_command(0, broadcast_cmd)
                    
                elif cmd == 'STATS':
                    self.show_stats()
                
                elif cmd == 'PDR_STATS':
                    self.show_pdr_latency_stats()
                    
                
                elif cmd == 'TIME':
                    if len(parts) > 1:
                        mode = parts[1].lower()
                        if mode in ['abs', 'absolute']:
                            self.show_absolute_time = True
                            print("[INFO] Display mode: ABSOLUTE NTP time")
                        elif mode in ['rel', 'relative']:
                            self.show_absolute_time = False
                            print("[INFO] Display mode: RELATIVE time")
                        else:
                            print("[ERROR] Unknown mode. Use: TIME [abs|rel]")
                    else:
                        # Toggle
                        self.show_absolute_time = not self.show_absolute_time
                        mode_str = "ABSOLUTE NTP" if self.show_absolute_time else "RELATIVE"
                        print(f"[INFO] Display mode toggled to: {mode_str}")
                elif cmd == 'SAVE':
                    filename = parts[1] if len(parts) > 1 else 'wifi_events.csv'
                    self.save_events(filename)
                    
                elif cmd == 'ANALYZE':
                    self.analyze_routing()
                    
                elif cmd == 'DEBUG':
                    self.show_cycle_debug()
                
                elif cmd == 'EXPORT_TOPOLOGY':
                    filename = parts[1] if len(parts) > 1 else 'topology_data.json'
                    self.export_topology_data(filename)
                    
                else:
                    print(f"[ERROR] Unknown command: {cmd}")
                    
            except (EOFError, KeyboardInterrupt):
                print("\n[INFO] Stopping monitor...")
                self.running = False
                break
            except Exception as e:
                print(f"[ERROR] {e}")
    
    def show_stats(self):
        """Display current statistics"""
        print("\n" + "="*60)
        print("STATISTICS")
        print("="*60)
        print(f"Events received:   {self.stats['events_received']}")
        print(f"Parse errors:      {self.stats['parse_errors']}")
        print(f"Commands sent:     {self.stats['commands_sent']}")
        print(f"Command errors:    {self.stats['command_errors']}")
        print(f"Monitor reconnects: {self.stats['monitor_reconnects']}")
        
        with self.lock:
            print(f"\nEvents per node:")
            for node_id in sorted(self.events.keys()):
                count = len(self.events[node_id])
                print(f"  Node {node_id}: {count} events")
        
        with self.command_lock:
            print(f"\nCommand queue: {len(self.command_queue)} pending")
        
        print("="*60 + "\n")
    
    def show_pdr_latency_stats(self):
        """Display PDR and Latency statistics"""
        print("\n" + "="*70)
        print(" PDR & LATENCY STATISTICS")
        print("="*70)
        
        with self.stats_lock:
            # Display PDR statistics
            if self.pdr_stats:
                print("\n📊 PACKET DELIVERY RATIO (PDR)")
                print("-" * 70)
                for node_id in sorted(self.pdr_stats.keys()):
                    print(f"\n🖥️  Node {node_id} (Gateway/Monitor):")
                    
                    # Network-wide stats
                    if 'NETWORK' in self.pdr_stats[node_id]:
                        net = self.pdr_stats[node_id]['NETWORK']
                        print(f"  📡 Network Total:")
                        print(f"     Expected: {net.get('Exp', 0)} | Received: {net.get('Rx', 0)} | "
                              f"Lost: {net.get('Lost', 0)} | PDR: {net.get('PDR', 0):.1f}%")
                    
                    # Per-node stats
                    print(f"  📋 Per-Node PDR:")
                    for sender_id in sorted(self.pdr_stats[node_id].keys()):
                        if sender_id == 'NETWORK':
                            continue
                        stats = self.pdr_stats[node_id][sender_id]
                        print(f"     From Node {sender_id}: {stats.get('PDR', 0):.1f}% "
                              f"(Rx:{stats.get('Rx', 0)}/{stats.get('Exp', 0)}, "
                              f"Gaps:{stats.get('Gaps', 0)}, Seq:{stats.get('Seq', 0)})")
                        
                        # Include latency if available
                        if 'LatAvg' in stats and stats.get('LatAvg', 0) > 0:
                            print(f"        └─ Latency: Avg={stats.get('LatAvg', 0):.1f}ms, "
                                  f"Min={stats.get('LatMin', 0):.1f}ms, Max={stats.get('LatMax', 0):.1f}ms")
            else:
                print("\n📊 No PDR data available yet")
            
            # Display Latency statistics
            if self.latency_stats:
                print("\n" + "-" * 70)
                print("⏱️  LATENCY MEASUREMENTS")
                print("-" * 70)
                for node_id in sorted(self.latency_stats.keys()):
                    print(f"\n🖥️  Node {node_id} (Gateway):")
                    for sender_id in sorted(self.latency_stats[node_id].keys()):
                        stats = self.latency_stats[node_id][sender_id]
                        print(f"  From Node {sender_id}:")
                        print(f"     Count: {stats.get('count', 0)} measurements")
                        print(f"     Avg: {stats.get('avg', 0):.2f} ms")
                        print(f"     Min: {stats.get('min', 0):.2f} ms")
                        print(f"     Max: {stats.get('max', 0):.2f} ms")
                        
                        # Show recent latencies (last 5)
                        recent = stats.get('latencies', [])[-5:]
                        if recent:
                            recent_str = ', '.join([f"{lat:.1f}ms" for lat in recent])
                            print(f"     Recent: {recent_str}")
            else:
                print("\n⏱️  No latency data available yet")
        
        print("\n" + "="*70 + "\n")
    
    def analyze_routing(self):
        """Analyze routing changes from events"""
        print("\n" + "="*60)
        print("ROUTING ANALYSIS")
        print("="*60)
        
        with self.lock:
            for node_id in sorted(self.events.keys()):
                events = self.events[node_id]
                
                # Hop changes
                hop_changes = [e for e in events if e['type'] == 'HOP_CHANGE']
                if hop_changes:
                    print(f"\n[HOP] Node {node_id} - Hop Changes: {len(hop_changes)}")
                    for e in hop_changes:
                        if self.start_timestamp_us:
                            elapsed = (e['timestamp_us'] - self.start_timestamp_us) / 1_000_000.0
                            print(f"  [{elapsed:7.1f}s] {e['details']}")
                        else:
                            print(f"  {e['details']}")
                
                # Gateway routing (only for gateway)
                if node_id == 1:
                    gw_rx = [e for e in events if e['type'] == 'GW_RX_DATA']
                    if gw_rx:
                        print(f"\n[GW] Gateway - Received Data: {len(gw_rx)} packets")
                        
                        # Extract routing paths
                        routes = defaultdict(int)
                        for e in gw_rx:
                            details = e['details']
                            if 'Route:' in details:
                                route = details.split('Route:')[1].split(',')[0].strip()
                                routes[route] += 1
                        
                        print("  Routing Paths:")
                        for route, count in sorted(routes.items(), key=lambda x: x[1], reverse=True):
                            print(f"    {route}: {count} packets")
        
        print("="*60 + "\n")
    
    def show_cycle_debug(self):
        """Show cycle synchronization debug information"""
        print("\n" + "="*60)
        print("CYCLE SYNC DEBUG")
        print("="*60)
        print("Note: CYCLE_VALIDATION_THRESHOLD = 3 sequential cycles")
        print("="*60)
        
        with self.lock:
            for node_id in sorted(self.events.keys()):
                events = self.events[node_id]
                
                # Cycle validation events
                cycle_val = [e for e in events if e['type'] == 'CYCLE_VAL']
                cycle_sync = [e for e in events if e['type'] == 'CYCLE_SYNC']
                
                if cycle_val or cycle_sync:
                    print(f"\n--- Node {node_id} ---")
                    
                    if cycle_val:
                        print(f"  Cycle Validations: {len(cycle_val)}")
                        for e in cycle_val[-5:]:  # Last 5
                            if self.start_timestamp_us:
                                elapsed = (e['timestamp_us'] - self.start_timestamp_us) / 1_000_000.0
                                print(f"    [{elapsed:7.1f}s] {e['details']}")
                            else:
                                print(f"    {e['details']}")
                    
                    if cycle_sync:
                        print(f"  Cycle Syncs: {len(cycle_sync)}")
                        for e in cycle_sync[-5:]:  # Last 5
                            if self.start_timestamp_us:
                                elapsed = (e['timestamp_us'] - self.start_timestamp_us) / 1_000_000.0
                                print(f"    [{elapsed:7.1f}s] {e['details']}")
                            else:
                                print(f"    {e['details']}")
        
        print("="*60 + "\n")
    
    def export_topology_data(self, filename='topology_data.json'):
        """Export PDR and Latency data for topology_visualizer.py"""
        import json
        
        topology_data = {
            'gateway_id': 1,
            'nodes': {},
            'links': [],
            'summary': {}
        }
        
        with self.stats_lock:
            # Extract per-node PDR and latency data
            for gateway_id in self.pdr_stats:
                if 'NETWORK' in self.pdr_stats[gateway_id]:
                    # Network summary
                    net = self.pdr_stats[gateway_id]['NETWORK']
                    topology_data['summary'] = {
                        'total_tx': net.get('Exp', 0),
                        'total_rx': net.get('Rx', 0),
                        'network_pdr': net.get('PDR', 0)
                    }
                
                # Per-node stats
                for sender_id in self.pdr_stats[gateway_id]:
                    if sender_id == 'NETWORK':
                        continue
                    
                    stats = self.pdr_stats[gateway_id][sender_id]
                    
                    # Get latency if available
                    avg_latency = stats.get('LatAvg', 0)
                    if avg_latency == 0 and gateway_id in self.latency_stats:
                        if sender_id in self.latency_stats[gateway_id]:
                            avg_latency = self.latency_stats[gateway_id][sender_id].get('avg', 0)
                    
                    topology_data['nodes'][sender_id] = {
                        'node_id': sender_id,
                        'tx_count': stats.get('Exp', 0),
                        'rx_at_gateway': stats.get('Rx', 0),
                        'pdr': stats.get('PDR', 0),
                        'avg_latency': avg_latency,
                        'min_latency': stats.get('LatMin', 0),
                        'max_latency': stats.get('LatMax', 0),
                        'gaps': stats.get('Gaps', 0),
                        'last_seq': stats.get('Seq', 0)
                    }
            
            # Extract routing links from HOP_CHANGE and NEIGHBOR events
            with self.lock:
                routing_info = {}  # {node_id: {'hop': hop_count, 'next_hop': next_node}}
                
                for node_id in self.events:
                    events = self.events[node_id]
                    
                    # Get last hop change for each node
                    hop_changes = [e for e in events if e['type'] == 'HOP_CHANGE']
                    if hop_changes:
                        last_hop = hop_changes[-1]
                        details = last_hop['details']
                        
                        # Parse "Hop changed: X -> Y via NodeZ" format
                        if 'via Node' in details:
                            try:
                                new_hop = int(details.split('->')[1].split('via')[0].strip())
                                next_node = int(details.split('via Node')[1].split()[0])
                                routing_info[node_id] = {
                                    'hop': new_hop,
                                    'next_hop': next_node
                                }
                            except:
                                pass
                        elif '->' in details:
                            try:
                                new_hop = int(details.split('->')[1].strip().split()[0])
                                routing_info[node_id] = {
                                    'hop': new_hop,
                                    'next_hop': 1 if new_hop == 1 else None  # Assume direct to gateway if hop=1
                                }
                            except:
                                pass
                    
                    # Get neighbor info
                    neighbor_events = [e for e in events if e['type'] == 'BIDIR_LINK']
                    for e in neighbor_events:
                        # Parse bidirectional link info
                        pass
                
                # Build links from routing info
                for node_id, info in routing_info.items():
                    if info.get('next_hop'):
                        topology_data['links'].append({
                            'from_node': node_id,
                            'to_node': info['next_hop']
                        })
                    
                    # Update node hop count
                    if node_id in topology_data['nodes']:
                        topology_data['nodes'][node_id]['hop_count'] = info.get('hop', 0)
        
        # Save to JSON
        try:
            with open(filename, 'w') as f:
                json.dump(topology_data, f, indent=2)
            print(f"\n[SAVED] Topology data to {filename}")
            print(f"[INFO] Nodes: {len(topology_data['nodes'])}")
            print(f"[INFO] Links: {len(topology_data['links'])}")
            
            # Also generate Python code for topology_visualizer.py
            py_filename = filename.replace('.json', '_viz.py')
            self.generate_topology_visualizer_code(topology_data, py_filename)
            
        except Exception as e:
            print(f"\n[ERROR] Failed to save: {e}")
    
    def generate_topology_visualizer_code(self, data, filename):
        """Generate Python code snippet for topology_visualizer.py"""
        code = '''#!/usr/bin/env python3
"""
Auto-generated topology data from WiFi Monitor
Run topology_visualizer.py to generate the visualization
"""
from topology_visualizer import TopologyVisualizer

def create_topology_from_data():
    viz = TopologyVisualizer("LoRa Mesh Network - Measured Data")
    
    # Gateway (Node 1)
    viz.add_node(1, pdr=None, avg_latency=None, position=(0, 3), hop_count=0, 
                 rx_count={total_rx})
    viz.set_gateway(1)
    
'''.format(total_rx=data['summary'].get('total_rx', 0))
        
        # Add nodes
        node_positions = {}
        hop_groups = {}  # Group nodes by hop count
        
        for node_id, info in data['nodes'].items():
            hop = info.get('hop_count', 1)
            if hop not in hop_groups:
                hop_groups[hop] = []
            hop_groups[hop].append(node_id)
        
        # Calculate positions based on hop count
        for hop in sorted(hop_groups.keys()):
            nodes = hop_groups[hop]
            x = hop * 3  # X based on hop count
            
            # Distribute nodes vertically
            if len(nodes) == 1:
                y_positions = [3]
            else:
                y_positions = [3 + (i - (len(nodes)-1)/2) * 2 for i in range(len(nodes))]
            
            for i, node_id in enumerate(sorted(nodes)):
                node_positions[node_id] = (x, y_positions[i])
        
        for node_id, info in sorted(data['nodes'].items()):
            pos = node_positions.get(node_id, (3, 3))
            code += f'''    # Node {node_id}
    viz.add_node({node_id}, pdr={info['pdr']:.1f}, avg_latency={info['avg_latency']:.1f}, 
                 position={pos}, hop_count={info.get('hop_count', 1)},
                 tx_count={info['tx_count']}, rx_at_gateway={info['rx_at_gateway']})
    
'''
        
        # Add links
        code += '''    # Routing links
'''
        for link in data['links']:
            code += f'''    viz.add_link({link['from_node']}, {link['to_node']})
'''
        
        code += '''
    return viz

if __name__ == "__main__":
    viz = create_topology_from_data()
    viz.visualize("topology_measured.png")
    print("Topology visualization saved to topology_measured.png")
'''
        
        try:
            with open(filename, 'w') as f:
                f.write(code)
            print(f"[SAVED] Visualizer code to {filename}")
        except Exception as e:
            print(f"[ERROR] Failed to save visualizer code: {e}")
    
    def save_events(self, filename=None):
        """Save all events to CSV with operation numbers and absolute timestamps"""
        # Use custom output file if no filename specified
        if filename is None:
            filename = self.output_file
        
        with self.lock:
            all_events = []
            for node_events in self.events.values():
                all_events.extend(node_events)
            all_events.sort(key=lambda e: e['timestamp_us'])
        
        if not all_events:
            print("[WARN] No events to save")
            return
        
        # Use reference timestamp from first received event (consistent with display)
        # Or use minimum timestamp if reference not set
        reference_timestamp = self.start_timestamp_us if self.start_timestamp_us else min(e['timestamp_us'] for e in all_events)
        
        try:
            with open(filename, 'w') as f:
                # CSV header - standard format (same as wifi_events2.csv)
                f.write("Operation,Relative_Time_S,Timestamp_US,Node_ID,Type,Details,Received_Time\n")
                op_counter = 0
                for e in all_events:
                    # Assign operation numbers to significant events
                    if e['type'] in ['NEIGHBOR_ADDED', 'NEIGHBOR_REMOVED', 'BIDIR_LINK', 
                                    'HOP_CHANGE', 'CMD_EXECUTED', 'RSSI_LOW']:
                        op_counter += 1
                        op_num = op_counter
                    else:
                        op_num = ''
                    
                    # Calculate relative time from reference timestamp (NTP-based, always >= 0)
                    relative_sec = (e['timestamp_us'] - reference_timestamp) / 1_000_000.0
                    
                    recv_time = e['recv_time'].strftime('%Y-%m-%d %H:%M:%S.%f')
                    details = e['details'].replace('"', '""')  # Escape quotes
                    f.write(f"{op_num},{relative_sec:.1f},{e['timestamp_us']},{e['node_id']},{e['type']},"
                           f"\"{details}\",{recv_time}\n")
            
            from datetime import datetime
            ref_time = datetime.fromtimestamp(reference_timestamp / 1_000_000.0)
            print(f"\n[SAVED] {len(all_events)} events to {filename}")
            print(f"[INFO] Total operations logged: {op_counter}")
            print(f"[INFO] Reference timestamp: {ref_time.strftime('%Y-%m-%d %H:%M:%S.%f')} ({reference_timestamp})")
            print(f"[INFO] Time range: 0.0s to {relative_sec:.1f}s ({relative_sec/60:.1f} minutes)")
        except Exception as e:
            print(f"\n[ERROR] Failed to save: {e}")
    
    def start(self):
        """Start monitoring system"""
        print("\n" + "="*60)
        print(" WiFi RELAY NODE MONITOR & CONTROL")
        print("="*60)
        print(f"Monitor UDP port: {MONITOR_UDP_PORT}")
        print(f"Command UDP port: {COMMAND_UDP_PORT}")
        print(f"Output file: {self.output_file}")
        print("\nConfigured nodes:")
        for node_id, ip in sorted(NODE_IPS.items()):
            print(f"  Node {node_id}: {ip}")
        print("="*60)
        
        self.running = True
        
        # Start monitor server thread
        monitor_t = threading.Thread(target=self.monitor_thread, daemon=True)
        monitor_t.start()
        print("[INIT] Monitor server thread started")
        
        # Start command client thread
        command_t = threading.Thread(target=self.command_thread, daemon=True)
        command_t.start()
        print("[INIT] Command client thread started")
        
        time.sleep(1)  # Wait for threads to initialize
        
        # Run command interface
        try:
            self.command_interface()
        finally:
            self.running = False
            print("[INIT] Waiting for threads to stop...")
            monitor_t.join(timeout=3)
            command_t.join(timeout=3)
            
            # Final summary
            print("\n" + "="*60)
            print("FINAL SUMMARY")
            print("="*60)
            self.show_stats()
            self.analyze_routing()
            self.save_events()
            print("\n[INFO] Monitor stopped")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='WiFi Relay Node Monitor & Control for LoRa Mesh PDR/Latency Testing',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  python3 wifi_monitor_control.py                      # Default output: wifi_events.csv
  python3 wifi_monitor_control.py -o pengetesan_PDR1   # Output: pengetesan_PDR1.csv
  python3 wifi_monitor_control.py -o linear_test.csv   # Output: linear_test.csv

Commands available during monitoring:
  SAVE [file]      - Save events to CSV (uses -o filename if not specified)
  PDR_STATS        - Show PDR & Latency statistics
  EXPORT_TOPOLOGY  - Export data for topology visualizer
  QUIT             - Exit and auto-save
        '''
    )
    parser.add_argument('-o', '--output', 
                        default='wifi_events.csv',
                        help='Output CSV filename (default: wifi_events.csv)')
    
    args = parser.parse_args()
    
    monitor = WiFiMonitorControl(output_file=args.output)
    monitor.start()
