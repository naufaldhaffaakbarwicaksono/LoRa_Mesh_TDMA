#!/usr/bin/env python3
"""
Analyze and Visualize LoRa Mesh Topology from CSV Data
Automatically identifies network topology and generates visualization
"""

import csv
import sys
from collections import defaultdict
from typing import Dict, List, Tuple, Set
from topology_visualizer import TopologyVisualizer
import argparse

class TopologyAnalyzer:
    def __init__(self, csv_file: str):
        self.csv_file = csv_file
        self.nodes = set()
        self.gateway_id = None
        self.hop_counts = {}  # {node_id: hop_count}
        self.neighbor_links = defaultdict(set)  # {node_id: {neighbor_ids}}
        self.routing_links = defaultdict(lambda: defaultdict(int))  # {from_node: {to_node: packet_count}}
        self.rssi_values = defaultdict(dict)  # {node_id: {neighbor_id: rssi}}
        self.pdr_stats = {}  # {node_id: {pdr, tx, rx}}
        self.latency_stats = {}  # {node_id: {avg, min, max}}
        self.gw_rx_data = defaultdict(list)  # {node_id: [packet_data]}
        self.tx_counts = defaultdict(int)  # {node_id: tx_count} from AUTO_SEND events
        
    def parse_csv(self):
        """Parse CSV file and extract topology information"""
        print(f"[INFO] Parsing {self.csv_file}...")
        
        with open(self.csv_file, 'r') as f:
            reader = csv.DictReader(f)
            
            for row in reader:
                node_id = int(row['Node_ID'])
                event_type = row['Type']
                details = row['Details']
                
                self.nodes.add(node_id)
                
                # Parse NEIGHBOR_ADDED events
                if event_type == 'NEIGHBOR_ADDED':
                    self._parse_neighbor_added(node_id, details)
                
                # Parse BIDIR_LINK events (confirmed bidirectional links)
                elif event_type == 'BIDIR_LINK':
                    self._parse_bidir_link(node_id, details)
                
                # Parse HOP_CHANGE events (routing path changes)
                elif event_type == 'HOP_CHANGE':
                    self._parse_hop_change(node_id, details)
                
                # Parse PDR data (if available)
                elif event_type == 'PDR_NODE':
                    self._parse_pdr_node(node_id, details)
                
                # Parse LATENCY data
                elif event_type == 'LATENCY':
                    self._parse_latency(node_id, details)
                
                # Parse GW_RX_DATA (gateway receive events with routing)
                elif event_type == 'GW_RX_DATA':
                    self._parse_gw_rx_data(node_id, details)
                
                # Parse AUTO_SEND (TX events from sensor nodes)
                elif event_type == 'AUTO_SEND_SEQ' or 'AUTO_SEND' in event_type:
                    self._parse_auto_send(node_id, details)
        
        # Identify gateway (hop count 0)
        for node_id, hop in self.hop_counts.items():
            if hop == 0:
                self.gateway_id = node_id
                break
        
        # If no gateway found, assume node 1
        if self.gateway_id is None:
            self.gateway_id = 1
            self.hop_counts[1] = 0
        
        print(f"[INFO] Found {len(self.nodes)} nodes")
        print(f"[INFO] Gateway: Node {self.gateway_id}")
        print(f"[INFO] Routing links: {sum(len(v) for v in self.routing_links.values())}")
        
    def _parse_neighbor_added(self, node_id: int, details: str):
        """Parse NEIGHBOR_ADDED: NodeID:X,RSSI:YdBm,Slot:Z,Hop:H"""
        parts = details.split(',')
        neighbor_id = None
        rssi = None
        hop = None
        
        for part in parts:
            if 'NodeID:' in part:
                neighbor_id = int(part.split(':')[1])
            elif 'RSSI:' in part:
                rssi = int(part.split(':')[1].replace('dBm', ''))
            elif 'Hop:' in part:
                hop_str = part.split(':')[1]
                hop = int(hop_str) if hop_str != '127' else None
        
        if neighbor_id:
            self.nodes.add(neighbor_id)
            self.neighbor_links[node_id].add(neighbor_id)
            
            if rssi:
                self.rssi_values[node_id][neighbor_id] = rssi
            
            # Store neighbor's hop count
            if hop is not None:
                if hop == 0:
                    self.gateway_id = neighbor_id
                    self.hop_counts[neighbor_id] = 0
                # If this node sees a neighbor with known hop, infer own hop
                if node_id not in self.hop_counts or self.hop_counts[node_id] == 127:
                    if hop == 0:
                        # Direct neighbor of gateway = hop 1
                        self.hop_counts[node_id] = 1
    
    def _parse_bidir_link(self, node_id: int, details: str):
        """Parse BIDIR_LINK: NodeID:X,RSSI:YdBm,Status:BIDIRECTIONAL"""
        parts = details.split(',')
        neighbor_id = None
        
        for part in parts:
            if 'NodeID:' in part:
                neighbor_id = int(part.split(':')[1])
        
        if neighbor_id:
            # Bidirectional link confirmed
            self.neighbor_links[node_id].add(neighbor_id)
            self.neighbor_links[neighbor_id].add(node_id)
    
    def _parse_hop_change(self, node_id: int, details: str):
        """Parse HOP_CHANGE: Hop changed: X -> Y [via NodeZ]"""
        # Extract new hop count and routing path
        try:
            if '->' in details:
                parts = details.split('->')
                new_hop_str = parts[1].strip().split()[0]
                new_hop = int(new_hop_str)
                self.hop_counts[node_id] = new_hop
                
                # Extract routing via node
                if 'via Node' in details:
                    via_node_str = details.split('via Node')[1].strip().split()[0]
                    via_node = int(via_node_str)
                    self.routing_links[node_id].add(via_node)
                elif new_hop == 1:
                    # Direct to gateway
                    if self.gateway_id:
                        self.routing_links[node_id].add(self.gateway_id)
        except:
            pass
    
    def _parse_pdr_node(self, gateway_id: int, details: str):
        """Parse PDR_NODE: NodeX,Seq:N,Exp:N,Rx:N,Gaps:N,PDR:X%"""
        try:
            parts = details.split(',')
            node_str = parts[0].replace('Node', '')
            node_id = int(node_str)
            
            pdr_data = {}
            for part in parts[1:]:
                if ':' in part:
                    key, val = part.split(':', 1)
                    key = key.strip()
                    val = val.replace('%', '').strip()
                    if key in ['Exp', 'Rx']:
                        pdr_data[key] = int(val)
                    elif key == 'PDR':
                        pdr_data[key] = float(val)
            
            if node_id not in self.pdr_stats:
                self.pdr_stats[node_id] = pdr_data
        except:
            pass
    
    def _parse_latency(self, gateway_id: int, details: str):
        """Parse LATENCY: NodeX,MsgID:N,Hop:N,Lat:Xms"""
        try:
            parts = details.split(',')
            node_str = parts[0].replace('Node', '')
            node_id = int(node_str)
            
            for part in parts:
                if 'Lat:' in part:
                    lat_str = part.split(':')[1].replace('ms', '')
                    latency = float(lat_str)
                    
                    if node_id not in self.latency_stats:
                        self.latency_stats[node_id] = {'latencies': []}
                    self.latency_stats[node_id]['latencies'].append(latency)
        except:
            pass
    
    def _parse_gw_rx_data(self, gateway_id: int, details: str):
        """Parse GW_RX_DATA: Msg:X,From:Y,Hops:Z,Route:[A>B>GW],Lat:Xms,RSSI:Y"""
        try:
            parts = details.split(',')
            
            data = {}
            for part in parts:
                if 'From:' in part:
                    data['from'] = int(part.split(':')[1])
                elif 'Hops:' in part:
                    data['hops'] = int(part.split(':')[1])
                elif 'Route:' in part:
                    # Extract route like [6>GW] or [3>2>GW]
                    route_str = part.split(':')[1]
                    data['route'] = route_str
                elif 'Lat:' in part:
                    lat_str = part.split(':')[1].replace('ms', '')
                    data['latency'] = float(lat_str)
                elif 'RSSI:' in part:
                    data['rssi'] = int(part.split(':')[1])
            
            if 'from' in data:
                from_node = data['from']
                self.nodes.add(from_node)
                self.gw_rx_data[from_node].append(data)
                
                # Extract hop count from Hops field
                if 'hops' in data:
                    self.hop_counts[from_node] = data['hops']
                
                # Extract routing path from Route field
                if 'route' in data:
                    route = data['route'].replace('[', '').replace(']', '')
                    nodes_in_route = route.split('>')
                    
                    # Build routing links from route and count packets
                    for i in range(len(nodes_in_route) - 1):
                        try:
                            if nodes_in_route[i] != 'GW':
                                curr = int(nodes_in_route[i])
                                next_node = nodes_in_route[i + 1]
                                if next_node == 'GW':
                                    self.routing_links[curr][gateway_id] += 1
                                else:
                                    self.routing_links[curr][int(next_node)] += 1
                        except:
                            pass
                
                # Store latency
                if 'latency' in data:
                    if from_node not in self.latency_stats:
                        self.latency_stats[from_node] = {'latencies': []}
                    self.latency_stats[from_node]['latencies'].append(data['latency'])
        except Exception as e:
            pass
    
    def _parse_auto_send(self, node_id: int, details: str):
        """Parse AUTO_SEND_SEQ events to count TX"""
        # Count transmission events
        self.tx_counts[node_id] += 1
    
    def infer_routing_topology(self):
        """Infer routing paths from hop counts and neighbor information"""
        print("[INFO] Inferring routing topology...")
        
        # Set default hop count for nodes without explicit hop info
        for node_id in self.nodes:
            if node_id not in self.hop_counts:
                # If node has gateway as neighbor, it's hop 1
                if self.gateway_id and self.gateway_id in self.neighbor_links[node_id]:
                    self.hop_counts[node_id] = 1
                else:
                    self.hop_counts[node_id] = 1  # Default assumption
        
        # For nodes without explicit routing info, infer from hop counts
        for node_id in self.nodes:
            if node_id == self.gateway_id:
                continue
            
            node_hop = self.hop_counts.get(node_id, 1)
            
            # If no routing link yet, find best next hop
            if node_id not in self.routing_links or sum(self.routing_links[node_id].values()) == 0:
                # Find neighbors with lower hop count
                best_neighbor = None
                best_hop = 127
                best_rssi = -999
                
                for neighbor_id in self.neighbor_links[node_id]:
                    neighbor_hop = self.hop_counts.get(neighbor_id, 127)
                    
                    if neighbor_hop < node_hop:
                        # Prefer lower hop count, then better RSSI
                        rssi = self.rssi_values[node_id].get(neighbor_id, -999)
                        
                        if neighbor_hop < best_hop or (neighbor_hop == best_hop and rssi > best_rssi):
                            best_neighbor = neighbor_id
                            best_hop = neighbor_hop
                            best_rssi = rssi
                
                if best_neighbor:
                    self.routing_links[node_id][best_neighbor] = 1  # Inferred link
        
        print(f"[INFO] Routing topology inferred for all nodes")
    
    def calculate_statistics(self):
        """Calculate PDR and latency statistics from GW_RX_DATA"""
        # Calculate average latency
        for node_id, data in self.latency_stats.items():
            if 'latencies' in data and len(data['latencies']) > 0:
                lats = data['latencies']
                data['avg'] = sum(lats) / len(lats)
                data['min'] = min(lats)
                data['max'] = max(lats)
        
        # Calculate PDR from GW_RX_DATA
        for node_id, packets in self.gw_rx_data.items():
            rx_count = len(packets)
            tx_count = self.tx_counts.get(node_id, rx_count)  # Use TX count if available
            
            # If no TX count, estimate from RX (assume PDR ~100% for estimation)
            if tx_count == 0:
                tx_count = rx_count
            
            pdr = (rx_count / tx_count * 100) if tx_count > 0 else 0
            
            self.pdr_stats[node_id] = {
                'PDR': pdr,
                'Rx': rx_count,
                'Exp': tx_count
            }
    
    def identify_topology_type(self) -> str:
        """Identify topology type: star, linear, branching, or mixed"""
        # Count children per node
        children_count = defaultdict(int)
        for from_node, to_nodes in self.routing_links.items():
            for to_node in to_nodes:
                children_count[to_node] += 1
        
        # Star topology: all nodes (except gateway) are at hop 1 and connect directly to gateway
        non_gateway_nodes = [nid for nid in self.nodes if nid != self.gateway_id]
        if non_gateway_nodes:
            all_hop_1 = all(self.hop_counts.get(nid, 127) == 1 for nid in non_gateway_nodes)
            all_connect_to_gw = all(
                self.gateway_id in self.routing_links.get(nid, set()) 
                for nid in non_gateway_nodes
            )
            
            if all_hop_1 and all_connect_to_gw:
                return "Star"
        
        # Linear: all nodes have at most 1 child
        is_linear = all(count <= 1 for count in children_count.values())
        
        # Branching: some nodes have multiple children
        max_children = max(children_count.values()) if children_count else 0
        
        if is_linear:
            return "Linear (Chain)"
        elif max_children >= 2:
            return "Branching (Tree)" if max_children >= 3 else "Mixed"
        else:
            return "Mixed"
    
    def auto_position_nodes(self) -> Dict[int, Tuple[float, float]]:
        """Automatically position nodes based on hop count and routing"""
        import math
        positions = {}
        
        # Detect topology type to choose layout strategy
        topology_type = self.identify_topology_type()
        
        # Group nodes by hop count
        hop_groups = defaultdict(list)
        for node_id in self.nodes:
            hop = self.hop_counts.get(node_id, 1)
            hop_groups[hop].append(node_id)
        
        # Position gateway at left center for branching, center for star
        if self.gateway_id:
            if topology_type in ["Branching (Tree)", "Mixed"]:
                positions[self.gateway_id] = (-8, 0)  # Left side
            else:
                positions[self.gateway_id] = (0, 0)  # Center for star
        
        # Choose layout based on topology type
        if topology_type == "Star":
            # Circular layout for star topology
            hop_1_nodes = sorted(hop_groups.get(1, []))
            num_nodes = len(hop_1_nodes)
            radius = 6.5
            
            for i, node_id in enumerate(hop_1_nodes):
                angle = 2 * math.pi * i / num_nodes
                x = radius * math.cos(angle)
                y = radius * math.sin(angle)
                positions[node_id] = (x, y)
        
        elif topology_type in ["Branching (Tree)", "Mixed"]:
            # Hierarchical tree layout for branching topology
            # Position hop 1 nodes vertically on the right of gateway
            hop_1_nodes = sorted(hop_groups.get(1, []))
            if hop_1_nodes:
                y_spacing = 6.0 if len(hop_1_nodes) > 1 else 0
                y_start = -(len(hop_1_nodes) - 1) * y_spacing / 2
                
                for i, node_id in enumerate(hop_1_nodes):
                    x = 0  # Column for hop 1
                    y = y_start + i * y_spacing
                    positions[node_id] = (x, y)
            
            # Position hop 2+ nodes to the right of their parents
            for hop in range(2, max(hop_groups.keys()) + 1 if hop_groups else 2):
                nodes_at_hop = sorted(hop_groups.get(hop, []))
                
                for node_id in nodes_at_hop:
                    # Find parent node (node this one routes to)
                    parent_node = None
                    if node_id in self.routing_links:
                        # Get the node with most packets (primary route)
                        routes = self.routing_links[node_id]
                        if routes:
                            parent_node = max(routes.items(), key=lambda x: x[1])[0]
                    
                    if parent_node and parent_node in positions:
                        parent_x, parent_y = positions[parent_node]
                        
                        # Count how many children this parent has
                        siblings = [n for n in nodes_at_hop 
                                   if n in self.routing_links 
                                   and parent_node in self.routing_links[n]]
                        
                        sibling_index = siblings.index(node_id) if node_id in siblings else 0
                        num_siblings = len(siblings)
                        
                        # Position to the right and offset vertically
                        x = parent_x + 8  # Horizontal spacing
                        if num_siblings > 1:
                            y_offset = (sibling_index - (num_siblings - 1) / 2) * 5
                            y = parent_y + y_offset
                        else:
                            y = parent_y
                        
                        positions[node_id] = (x, y)
                    else:
                        # Fallback: position below previous hop nodes
                        x = (hop - 1) * 8
                        y = 0
                        positions[node_id] = (x, y)
        
        else:
            # Linear or other: use simple horizontal layout
            for hop in sorted(hop_groups.keys()):
                if hop == 0:
                    continue
                nodes_at_hop = sorted(hop_groups[hop])
                for i, node_id in enumerate(nodes_at_hop):
                    x = hop * 6
                    y = (i - (len(nodes_at_hop) - 1) / 2) * 4
                    positions[node_id] = (x, y)
        
        return positions
    
    def create_visualization(self, output_file: str = "topology_analyzed.png"):
        """Create topology visualization"""
        topology_type = self.identify_topology_type()
        title = f"LoRa Multihop Network - {topology_type}"
        
        viz = TopologyVisualizer(title)
        
        # Auto-position nodes
        positions = self.auto_position_nodes()
        
        # Calculate total RX at gateway (from GW_RX_DATA)
        total_rx = sum(len(packets) for packets in self.gw_rx_data.values())
        
        # Add nodes
        for node_id in sorted(self.nodes):
            hop = self.hop_counts.get(node_id, 1)
            pos = positions.get(node_id, (0, 0))
            
            if node_id == self.gateway_id:
                # Gateway node
                viz.add_node(node_id, pdr=None, avg_latency=None, 
                           position=pos, hop_count=0, rx_count=total_rx)
                viz.set_gateway(node_id)
            else:
                # Sensor node
                pdr = self.pdr_stats.get(node_id, {}).get('PDR', 0.0)
                tx = self.pdr_stats.get(node_id, {}).get('Exp', 0)
                rx = self.pdr_stats.get(node_id, {}).get('Rx', 0)
                avg_lat = self.latency_stats.get(node_id, {}).get('avg', 0.0)
                
                viz.add_node(node_id, pdr=pdr, avg_latency=avg_lat,
                           position=pos, hop_count=hop,
                           tx_count=tx, rx_at_gateway=rx)
        
        # Add routing links with packet counts
        for from_node, to_nodes in self.routing_links.items():
            total_packets = sum(to_nodes.values())
            for to_node, count in to_nodes.items():
                is_primary = (count >= total_packets / 2)  # >= 50% of packets
                viz.add_link(from_node, to_node, packets_sent=count, is_primary=is_primary)
        
        # Generate visualization
        viz.visualize(output_file)
        
        return viz
    
    def print_summary(self):
        """Print topology summary"""
        print("\n" + "="*70)
        print("TOPOLOGY ANALYSIS SUMMARY")
        print("="*70)
        
        print(f"\nTopology Type: {self.identify_topology_type()}")
        print(f"Gateway: Node {self.gateway_id}")
        print(f"Total Nodes: {len(self.nodes)}")
        
        print(f"\nHop Count Distribution:")
        hop_distribution = defaultdict(list)
        for node_id, hop in self.hop_counts.items():
            hop_distribution[hop].append(node_id)
        
        for hop in sorted(hop_distribution.keys()):
            nodes = sorted(hop_distribution[hop])
            print(f"  Hop {hop}: {nodes}")
        
        print(f"\nRouting Paths:")
        for from_node in sorted(self.routing_links.keys()):
            to_nodes = self.routing_links[from_node]
            if to_nodes:
                total_packets = sum(to_nodes.values())
                print(f"  Node {from_node}:")
                # Sort by packet count (descending)
                for to_node, count in sorted(to_nodes.items(), key=lambda x: x[1], reverse=True):
                    percentage = (count / total_packets * 100) if total_packets > 0 else 0
                    rssi = self.rssi_values.get(from_node, {}).get(to_node, None)
                    rssi_str = f" (RSSI: {rssi} dBm)" if rssi else ""
                    path_type = "PRIMARY" if percentage >= 50 else "ALTERNATE"
                    print(f"    → Node {to_node}: {count} packets ({percentage:.1f}%) [{path_type}]{rssi_str}")
        
        if self.pdr_stats:
            print(f"\nPDR Statistics (from GW_RX_DATA):")
            total_tx = 0
            total_rx = 0
            for node_id in sorted(self.pdr_stats.keys()):
                stats = self.pdr_stats[node_id]
                tx = stats.get('Exp', 0)
                rx = stats.get('Rx', 0)
                total_tx += tx
                total_rx += rx
                print(f"  Node {node_id}: PDR={stats.get('PDR', 0):.1f}% "
                      f"(RX={rx}/{tx})")
            print(f"  TOTAL: {total_rx}/{total_tx} packets received at gateway")
        
        if self.latency_stats:
            print(f"\nLatency Statistics (End-to-End):")
            for node_id in sorted(self.latency_stats.keys()):
                stats = self.latency_stats[node_id]
                if 'avg' in stats:
                    print(f"  Node {node_id}: Avg={stats['avg']:.1f}ms, "
                          f"Min={stats['min']:.1f}ms, Max={stats['max']:.1f}ms")
        
        # Show routing paths from GW_RX_DATA
        if self.gw_rx_data:
            print(f"\nGateway Received Data: {sum(len(p) for p in self.gw_rx_data.values())} total packets")
            print("  Packets per node:")
            for node_id in sorted(self.gw_rx_data.keys()):
                packets = self.gw_rx_data[node_id]
                # Get unique routes
                routes = set()
                for pkt in packets:
                    if 'route' in pkt:
                        routes.add(pkt['route'])
                route_str = ', '.join(sorted(routes)) if routes else 'N/A'
                print(f"    Node {node_id}: {len(packets)} packets (Routes: {route_str})")
        
        print("="*70 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description='Analyze and visualize LoRa mesh topology from CSV data',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  python3 analyze_topology_from_csv.py test_topology.csv
  python3 analyze_topology_from_csv.py wifi_events.csv -o my_topology.png
        '''
    )
    parser.add_argument('csv_file', help='Input CSV file with network events')
    parser.add_argument('-o', '--output', default='topology_analyzed.png',
                       help='Output PNG filename (default: topology_analyzed.png)')
    
    args = parser.parse_args()
    
    # Check if file exists
    try:
        with open(args.csv_file, 'r') as f:
            pass
    except FileNotFoundError:
        print(f"[ERROR] File not found: {args.csv_file}")
        sys.exit(1)
    
    print("="*70)
    print("LoRa Mesh Network Topology Analyzer")
    print("="*70)
    
    # Analyze topology
    analyzer = TopologyAnalyzer(args.csv_file)
    analyzer.parse_csv()
    analyzer.infer_routing_topology()
    analyzer.calculate_statistics()
    
    # Print summary
    analyzer.print_summary()
    
    # Create visualization
    print(f"[INFO] Generating topology visualization...")
    analyzer.create_visualization(args.output)
    
    print(f"\n[SUCCESS] Analysis complete!")
    print(f"[SUCCESS] Visualization saved to: {args.output}")
    print("="*70 + "\n")


if __name__ == "__main__":
    main()
