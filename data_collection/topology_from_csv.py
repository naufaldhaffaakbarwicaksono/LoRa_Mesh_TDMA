#!/usr/bin/env python3
"""
Topology Visualizer from CSV Events
Generates network topology graph from wifi_events CSV data

Only shows data that exists in the CSV - no placeholder stats
"""

import csv
import re
import sys
from collections import defaultdict
from datetime import datetime

try:
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    import networkx as nx
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("[WARN] matplotlib/networkx not installed. Install with: pip install matplotlib networkx")

class TopologyFromCSV:
    def __init__(self, csv_file):
        self.csv_file = csv_file
        self.nodes = set()
        self.links = defaultdict(lambda: {'rssi_values': [], 'last_rssi': None, 'bidirectional': False})
        self.routes = defaultdict(list)  # {source_node: [route_paths]}
        self.hop_counts = {}  # {node_id: hop_count}
        self.neighbor_events = []
        self.gateway_id = 1
        
    def parse_csv(self):
        """Parse CSV file and extract topology information"""
        print(f"[INFO] Parsing {self.csv_file}...")
        
        event_count = 0
        with open(self.csv_file, 'r') as f:
            reader = csv.DictReader(f)
            
            for row in reader:
                event_count += 1
                node_id = int(row['Node_ID']) if row['Node_ID'] else None
                event_type = row['Type']
                details = row['Details']
                
                if node_id:
                    self.nodes.add(node_id)
                
                # Parse BIDIR_LINK events to extract links
                if event_type == 'BIDIR_LINK':
                    match = re.search(r'NodeID:(\d+),RSSI:(-?\d+)dBm', details)
                    if match and node_id:
                        neighbor_id = int(match.group(1))
                        rssi = int(match.group(2))
                        self.nodes.add(neighbor_id)
                        
                        # Create link key (sorted tuple for undirected graph)
                        link_key = tuple(sorted([node_id, neighbor_id]))
                        self.links[link_key]['rssi_values'].append(rssi)
                        self.links[link_key]['last_rssi'] = rssi
                        self.links[link_key]['bidirectional'] = True
                
                # Parse GW_RX_DATA to extract routing paths
                elif event_type == 'GW_RX_DATA':
                    # Format: Msg:XXX,From:Y,Hops:Z,Route:[A>B>GW],Lat:XXXms,...
                    match = re.search(r'From:(\d+),Hops:(\d+),Route:\[([^\]]+)\]', details)
                    if match:
                        source = int(match.group(1))
                        hops = int(match.group(2))
                        route_str = match.group(3)
                        
                        # Parse route path
                        route_nodes = []
                        for part in route_str.split('>'):
                            if part == 'GW':
                                route_nodes.append(self.gateway_id)
                            else:
                                route_nodes.append(int(part))
                        
                        if route_nodes and source not in self.routes:
                            self.routes[source] = route_nodes
                            self.hop_counts[source] = hops
                
                # Parse HOP_CHANGE events
                elif event_type == 'HOP_CHANGE':
                    match = re.search(r'Old=(\d+),New=(\d+)', details)
                    if match and node_id:
                        new_hop = int(match.group(2))
                        self.hop_counts[node_id] = new_hop
                
                # Parse NEIGHBOR_ADDED events
                elif event_type == 'NEIGHBOR_ADDED':
                    match = re.search(r'NodeID:(\d+),RSSI:(-?\d+)dBm.*Hop:(\d+)', details)
                    if match and node_id:
                        neighbor_id = int(match.group(1))
                        rssi = int(match.group(2))
                        hop = int(match.group(3))
                        self.nodes.add(neighbor_id)
                        self.neighbor_events.append({
                            'node': node_id,
                            'neighbor': neighbor_id,
                            'rssi': rssi,
                            'hop': hop,
                            'action': 'added'
                        })
                
                # Parse CYCLE_SYNC for hop information
                elif event_type == 'CYCLE_SYNC':
                    match = re.search(r'From=(\d+),Hop=(\d+)', details)
                    if match and node_id:
                        sync_from = int(match.group(1))
                        hop = int(match.group(2))
                        # Node receiving sync is at hop+1 if syncing from parent
                        if sync_from != node_id:
                            inferred_hop = hop + 1
                            if node_id not in self.hop_counts or self.hop_counts[node_id] > inferred_hop:
                                self.hop_counts[node_id] = inferred_hop
        
        # Gateway is always hop 0
        self.hop_counts[self.gateway_id] = 0
        
        print(f"[INFO] Parsed {event_count} events")
        print(f"[INFO] Found {len(self.nodes)} nodes: {sorted(self.nodes)}")
        print(f"[INFO] Found {len(self.links)} bidirectional links")
        print(f"[INFO] Found {len(self.routes)} routing paths")
        
    def compute_link_stats(self):
        """Compute average RSSI for each link"""
        for link_key, link_data in self.links.items():
            if link_data['rssi_values']:
                link_data['avg_rssi'] = sum(link_data['rssi_values']) / len(link_data['rssi_values'])
                link_data['min_rssi'] = min(link_data['rssi_values'])
                link_data['max_rssi'] = max(link_data['rssi_values'])
    
    def print_topology(self):
        """Print topology to console"""
        print("\n" + "="*70)
        print(" NETWORK TOPOLOGY FROM CSV")
        print("="*70)
        
        # Print nodes with hop counts
        print("\n📡 NODES:")
        print("-" * 40)
        for node_id in sorted(self.nodes):
            hop = self.hop_counts.get(node_id, '?')
            node_type = "Gateway" if node_id == self.gateway_id else f"Node"
            print(f"  Node {node_id}: {node_type} (Hop: {hop})")
        
        # Print links with RSSI
        print("\n🔗 BIDIRECTIONAL LINKS (with average RSSI):")
        print("-" * 40)
        self.compute_link_stats()
        for link_key in sorted(self.links.keys()):
            link_data = self.links[link_key]
            avg_rssi = link_data.get('avg_rssi', link_data['last_rssi'])
            if avg_rssi:
                signal_quality = "Strong" if avg_rssi > -90 else "Medium" if avg_rssi > -100 else "Weak"
                print(f"  Node {link_key[0]} <---> Node {link_key[1]} | Avg RSSI: {avg_rssi:.0f} dBm ({signal_quality})")
        
        # Print routing paths
        if self.routes:
            print("\n🛤️  ROUTING PATHS TO GATEWAY:")
            print("-" * 40)
            for source, route in sorted(self.routes.items()):
                route_str = " → ".join([f"Node {n}" if n != self.gateway_id else "Gateway" for n in route])
                hops = self.hop_counts.get(source, '?')
                print(f"  From Node {source}: {route_str} ({hops} hops)")
        
        print("\n" + "="*70)
    
    def visualize(self, output_file='topology_graph.png'):
        """Generate topology visualization"""
        if not HAS_MATPLOTLIB:
            print("[ERROR] matplotlib/networkx required for visualization")
            return
        
        self.compute_link_stats()
        
        # Create graph
        G = nx.Graph()
        
        # Add nodes
        for node_id in self.nodes:
            hop = self.hop_counts.get(node_id, 1)
            G.add_node(node_id, hop=hop)
        
        # Add edges with RSSI as weight
        for link_key, link_data in self.links.items():
            avg_rssi = link_data.get('avg_rssi', link_data['last_rssi'])
            if avg_rssi:
                G.add_edge(link_key[0], link_key[1], rssi=avg_rssi)
        
        # Create figure
        fig, ax = plt.subplots(1, 1, figsize=(12, 10))
        
        # Position nodes based on hop count (layered layout)
        pos = {}
        hop_groups = defaultdict(list)
        for node_id in self.nodes:
            hop = self.hop_counts.get(node_id, 1)
            hop_groups[hop].append(node_id)
        
        for hop in sorted(hop_groups.keys()):
            nodes_at_hop = sorted(hop_groups[hop])
            n_nodes = len(nodes_at_hop)
            for i, node_id in enumerate(nodes_at_hop):
                x = hop * 3
                if n_nodes == 1:
                    y = 0
                else:
                    y = (i - (n_nodes - 1) / 2) * 2
                pos[node_id] = (x, y)
        
        # Node colors based on type
        node_colors = []
        for node_id in G.nodes():
            if node_id == self.gateway_id:
                node_colors.append('#FF6B6B')  # Red for gateway
            else:
                hop = self.hop_counts.get(node_id, 1)
                if hop == 1:
                    node_colors.append('#4ECDC4')  # Teal for hop 1
                elif hop == 2:
                    node_colors.append('#45B7D1')  # Blue for hop 2
                else:
                    node_colors.append('#96CEB4')  # Green for hop 3+
        
        # Draw nodes
        nx.draw_networkx_nodes(G, pos, node_color=node_colors, node_size=2000, ax=ax)
        
        # Node labels
        labels = {}
        for node_id in G.nodes():
            if node_id == self.gateway_id:
                labels[node_id] = f"GW\n(Node 1)"
            else:
                hop = self.hop_counts.get(node_id, '?')
                labels[node_id] = f"Node {node_id}\n(Hop {hop})"
        
        nx.draw_networkx_labels(G, pos, labels, font_size=10, font_weight='bold', ax=ax)
        
        # Edge colors based on RSSI
        edge_colors = []
        edge_widths = []
        for u, v in G.edges():
            rssi = G[u][v].get('rssi', -100)
            if rssi > -90:
                edge_colors.append('#2ECC71')  # Green - strong
                edge_widths.append(3)
            elif rssi > -100:
                edge_colors.append('#F39C12')  # Orange - medium
                edge_widths.append(2)
            else:
                edge_colors.append('#E74C3C')  # Red - weak
                edge_widths.append(1.5)
        
        # Draw edges
        nx.draw_networkx_edges(G, pos, edge_color=edge_colors, width=edge_widths, ax=ax, alpha=0.7)
        
        # Edge labels (RSSI)
        edge_labels = {}
        for u, v in G.edges():
            rssi = G[u][v].get('rssi', 0)
            edge_labels[(u, v)] = f"{rssi:.0f} dBm"
        
        nx.draw_networkx_edge_labels(G, pos, edge_labels, font_size=8, ax=ax)
        
        # Legend
        legend_elements = [
            mpatches.Patch(color='#FF6B6B', label='Gateway (Hop 0)'),
            mpatches.Patch(color='#4ECDC4', label='Hop 1 Node'),
            mpatches.Patch(color='#45B7D1', label='Hop 2 Node'),
            mpatches.Patch(color='#96CEB4', label='Hop 3+ Node'),
            plt.Line2D([0], [0], color='#2ECC71', linewidth=3, label='Strong Link (> -90 dBm)'),
            plt.Line2D([0], [0], color='#F39C12', linewidth=2, label='Medium Link (-100 to -90 dBm)'),
            plt.Line2D([0], [0], color='#E74C3C', linewidth=1.5, label='Weak Link (< -100 dBm)'),
        ]
        ax.legend(handles=legend_elements, loc='upper left', fontsize=9)
        
        # Title
        ax.set_title(f'LoRa Mesh Network Topology\n(from {self.csv_file})', fontsize=14, fontweight='bold')
        ax.axis('off')
        
        plt.tight_layout()
        plt.savefig(output_file, dpi=150, bbox_inches='tight', facecolor='white')
        print(f"\n[SAVED] Topology visualization to {output_file}")
        plt.close()
    
    def export_routes_diagram(self, output_file='routing_diagram.txt'):
        """Export ASCII routing diagram"""
        lines = []
        lines.append("=" * 60)
        lines.append(" ROUTING DIAGRAM")
        lines.append("=" * 60)
        lines.append("")
        
        # Group nodes by hop
        hop_groups = defaultdict(list)
        for node_id in self.nodes:
            hop = self.hop_counts.get(node_id, 1)
            hop_groups[hop].append(node_id)
        
        # Draw layers
        max_hop = max(hop_groups.keys()) if hop_groups else 0
        
        for hop in range(max_hop + 1):
            nodes_at_hop = sorted(hop_groups.get(hop, []))
            if not nodes_at_hop:
                continue
            
            layer_label = "Gateway" if hop == 0 else f"Hop {hop}"
            lines.append(f"[{layer_label}]")
            
            node_strs = []
            for node_id in nodes_at_hop:
                if node_id == self.gateway_id:
                    node_strs.append(f"[GW(1)]")
                else:
                    node_strs.append(f"[N{node_id}]")
            
            lines.append("    " + "    ".join(node_strs))
            
            if hop < max_hop:
                lines.append("       |")
                lines.append("       v")
        
        lines.append("")
        lines.append("=" * 60)
        
        # Print to console
        for line in lines:
            print(line)
        
        # Save to file
        with open(output_file, 'w') as f:
            f.write('\n'.join(lines))
        print(f"\n[SAVED] Routing diagram to {output_file}")


def main():
    # Default CSV file
    csv_file = '/home/naufal/LoRa_Mesh_TDMA/data_collection/wifi_events2.csv'
    
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]
    
    topology = TopologyFromCSV(csv_file)
    topology.parse_csv()
    topology.print_topology()
    topology.export_routes_diagram()
    
    if HAS_MATPLOTLIB:
        topology.visualize()
    else:
        print("\n[TIP] Install matplotlib and networkx for graphical visualization:")
        print("      pip install matplotlib networkx")


if __name__ == '__main__':
    main()
