#!/usr/bin/env python3
"""
LoRa Mesh Network Topology Visualizer
Visualizes network topology with PDR and Latency statistics for each node.
Supports linear and branching topologies.
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
from dataclasses import dataclass
from typing import Dict, List, Tuple, Optional
from collections import defaultdict

@dataclass
class NodeStats:
    """Statistics for each node"""
    node_id: int
    pdr: float  # Packet Delivery Ratio (0-100%)
    avg_latency: float  # Average latency in ms
    position: Tuple[float, float]  # (x, y) position for visualization
    hop_count: int = 0  # Number of hops to gateway (0 for gateway)
    tx_count: int = 0  # Total packets transmitted (for sensor nodes)
    rx_at_gateway: int = 0  # Packets from this node received at gateway
    rx_count: int = 0  # Total packets received (for gateway only)

@dataclass
class RouteLink:
    """Represents a routing link between two nodes"""
    from_node: int
    to_node: int
    packets_sent: int = 0
    packets_received: int = 0
    is_primary: bool = True  # True for primary path, False for alternate

class TopologyVisualizer:
    def __init__(self, title: str = "LoRa Mesh Network Topology"):
        self.title = title
        self.nodes: Dict[int, NodeStats] = {}
        self.links: List[RouteLink] = []
        self.gateway_id: Optional[int] = None
        
    def add_node(self, node_id: int, pdr: float, avg_latency: float, 
                 position: Tuple[float, float], hop_count: int = 0,
                 tx_count: int = 0, rx_at_gateway: int = 0, rx_count: int = 0):
        """Add a node with its statistics"""
        self.nodes[node_id] = NodeStats(
            node_id=node_id,
            pdr=pdr,
            avg_latency=avg_latency,
            position=position,
            hop_count=hop_count,
            tx_count=tx_count,
            rx_at_gateway=rx_at_gateway,
            rx_count=rx_count
        )
    
    def set_gateway(self, node_id: int):
        """Set which node is the gateway"""
        self.gateway_id = node_id
    
    def add_link(self, from_node: int, to_node: int, 
                 packets_sent: int = 0, packets_received: int = 0, is_primary: bool = True):
        """Add a routing link between two nodes"""
        self.links.append(RouteLink(from_node, to_node, packets_sent, packets_received, is_primary))
    

    
    def visualize(self, output_file: str = "topology.png", figsize: Tuple[int, int] = (16, 12)):
        """Generate the topology visualization"""
        fig, ax = plt.subplots(figsize=figsize)
        ax.set_aspect('equal')
        
        # Draw links first (so they appear behind nodes)
        # Group links by from_node to handle multiple paths
        from_node_links = defaultdict(list)
        for link in self.links:
            if link.from_node in self.nodes and link.to_node in self.nodes:
                from_node_links[link.from_node].append(link)
        
        for from_node, node_links in from_node_links.items():
            # Sort by is_primary (primary first) then by packets
            node_links.sort(key=lambda x: (not x.is_primary, -x.packets_sent))
            
            for idx, link in enumerate(node_links):
                from_pos = self.nodes[link.from_node].position
                to_pos = self.nodes[link.to_node].position
                
                # Different styles for primary vs alternate paths
                if link.is_primary:
                    # Primary path: solid line, darker color
                    linestyle = '-'
                    color = '#2c3e50'
                    linewidth = 2.5
                    alpha = 1.0
                    arc_rad = 0.1  # Slight curve
                else:
                    # Alternate path: dashed line, lighter color, more curve
                    linestyle = '--'
                    color = '#95a5a6'
                    linewidth = 2.0
                    alpha = 0.6
                    arc_rad = -0.3  # Curve in opposite direction
                
                # Draw arrow for routing direction
                ax.annotate('', 
                    xy=to_pos, 
                    xytext=from_pos,
                    arrowprops=dict(
                        arrowstyle='-|>',
                        color=color,
                        lw=linewidth,
                        linestyle=linestyle,
                        alpha=alpha,
                        shrinkA=35,
                        shrinkB=35,
                        connectionstyle=f'arc3,rad={arc_rad}'
                    )
                )
                
                # Add packet count label for paths with traffic
                if link.packets_sent > 0:
                    # Calculate label position based on arc
                    dx = to_pos[0] - from_pos[0]
                    dy = to_pos[1] - from_pos[1]
                    
                    # Position label closer to source node to avoid stats box overlap
                    # Use 30% along the path instead of 50% (midpoint)
                    label_x = from_pos[0] + dx * 0.3
                    label_y = from_pos[1] + dy * 0.3
                    
                    # Minimal offset perpendicular to line
                    if not link.is_primary:
                        # Perpendicular offset for alternate paths
                        perp_x = -dy * 0.05
                        perp_y = dx * 0.05
                        label_x += perp_x
                        label_y += perp_y
                    else:
                        # Very minimal offset for primary path
                        perp_x = -dy * 0.02
                        perp_y = dx * 0.02
                        label_x += perp_x
                        label_y += perp_y
                    
                    label_text = f"{link.packets_sent}pkts"
                    ax.text(label_x, label_y, label_text, fontsize=10,
                           ha='center', va='center', fontweight='bold',
                           bbox=dict(boxstyle='round,pad=0.25', facecolor='white',
                                   edgecolor=color, alpha=0.9, linewidth=1.5))
        
        # Draw nodes
        node_radius = 0.6
        for node_id, node in self.nodes.items():
            x, y = node.position
            
            # Determine node style based on type
            if node_id == self.gateway_id:
                # Gateway node - larger, different color
                circle = plt.Circle((x, y), node_radius * 1.2, 
                                   color='#9b59b6', ec='#8e44ad', lw=3, zorder=10)
            else:
                # Regular node - single color
                circle = plt.Circle((x, y), node_radius, 
                                   color='#3498db', ec='#2c3e50', lw=2, zorder=10)
            
            ax.add_patch(circle)
            
            # Node ID in center
            ax.text(x, y, str(node_id), fontsize=20, fontweight='bold',
                   ha='center', va='center', color='white', zorder=11)
            
            # Stats box next to node (only for non-gateway nodes)
            if node_id == self.gateway_id:
                # Gateway label with RX count
                stats_text = f"Gateway\n(Receiver)\nRX: {node.rx_count}"
                stats_x = x + node_radius * 1.2 + 0.3
                stats_y = y
                bbox_props = dict(
                    boxstyle='round,pad=0.4',
                    facecolor='#9b59b6',
                    edgecolor='#8e44ad',
                    alpha=0.3
                )
                ax.text(stats_x, stats_y, stats_text, fontsize=14,
                       ha='left', va='center', fontweight='bold',
                       bbox=bbox_props, zorder=12)
            else:
                stats_text = f"Hop: {node.hop_count}\nTX: {node.tx_count}\nRX: {node.rx_at_gateway}\nPDR: {node.pdr:.1f}%\nE2E Latency: {node.avg_latency:.1f}ms"
                
                # Position stats box to the right of node
                stats_x = x + node_radius + 0.3
                stats_y = y
                
                # Create stats box with colored indicators
                bbox_props = dict(
                    boxstyle='round,pad=0.4',
                    facecolor='white',
                    edgecolor='#bdc3c7',
                    alpha=0.95
                )
                
                ax.text(stats_x, stats_y, stats_text, fontsize=14,
                       ha='left', va='center', 
                       bbox=bbox_props, zorder=12)
        
        # Create legend with routing path explanation
        legend_elements = [
            mpatches.Patch(color='#9b59b6', label='Gateway'),
            mpatches.Patch(color='#3498db', label='Node'),
            mpatches.Patch(color='#2c3e50', label='Primary Path (≥50%)'),
            mpatches.Patch(color='#95a5a6', label='Alternate Path'),
        ]
        ax.legend(handles=legend_elements, loc='upper left', framealpha=0.95, fontsize=16)
        
        # Calculate and display total packets
        total_tx = sum(n.tx_count for n in self.nodes.values() if n.node_id != self.gateway_id)
        gateway_rx = self.nodes[self.gateway_id].rx_count if self.gateway_id in self.nodes else 0
        
        # Add summary text below legend
        summary_text = f"Total TX (all nodes): {total_tx}\nTotal RX (gateway): {gateway_rx}"
        ax.text(0.02, 0.15, summary_text, transform=ax.transAxes,
               fontsize=14, ha='left', va='top',
               bbox=dict(boxstyle='round,pad=0.4', facecolor='white', 
                        edgecolor='#bdc3c7', alpha=0.95))
        
        # Add note about data packets
        note_text = "* Only data packets counted\n  (excludes control packets)"
        ax.text(0.02, 0.05, note_text, transform=ax.transAxes,
               fontsize=12, ha='left', va='top', style='italic',
               color='#7f8c8d')
        
        # Set axis limits with padding
        all_x = [n.position[0] for n in self.nodes.values()]
        all_y = [n.position[1] for n in self.nodes.values()]
        
        padding = 2.5
        ax.set_xlim(min(all_x) - padding, max(all_x) + padding + 2)
        ax.set_ylim(min(all_y) - padding, max(all_y) + padding)
        
        # Style
        ax.set_title(self.title, fontsize=18, fontweight='bold', pad=20)
        ax.axis('off')
        ax.set_facecolor('#ecf0f1')
        fig.patch.set_facecolor('#ecf0f1')
        
        # Add timestamp and info
        from datetime import datetime
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        ax.text(0.99, 0.01, f'Generated: {timestamp}', transform=ax.transAxes,
               fontsize=12, ha='right', va='bottom', color='#7f8c8d')
        
        plt.tight_layout()
        plt.savefig(output_file, dpi=150, bbox_inches='tight', 
                   facecolor=fig.get_facecolor())
        plt.close()
        
        print(f"✓ Topology visualization saved to: {output_file}")
        return output_file


def create_linear_topology_demo():
    """Create a demo with linear topology (chain)"""
    viz = TopologyVisualizer("LoRa Mesh - Linear Topology (Chain)")
    
    # Dummy data for linear topology: Gateway(1) <- Node2 <- Node3 <- Node4 <- Node5 <- Node6
    # Gateway (Node 1) - receives all data
    # Total RX = sum of all rx_at_gateway from all nodes = 197+190+177+164+151 = 879
    viz.add_node(1, pdr=None, avg_latency=None, position=(0, 0), hop_count=0, rx_count=879)
    viz.set_gateway(1)
    
    # Chain of nodes with increasing hop count
    # PDR = (rx_at_gateway / tx_count) * 100
    viz.add_node(2, pdr=98.5, avg_latency=45.2, position=(3, 0), hop_count=1, tx_count=200, rx_at_gateway=197)
    viz.add_node(3, pdr=95.0, avg_latency=92.8, position=(6, 0), hop_count=2, tx_count=200, rx_at_gateway=190)
    viz.add_node(4, pdr=88.5, avg_latency=156.3, position=(9, 0), hop_count=3, tx_count=200, rx_at_gateway=177)
    viz.add_node(5, pdr=82.0, avg_latency=245.6, position=(12, 0), hop_count=4, tx_count=200, rx_at_gateway=164)
    viz.add_node(6, pdr=75.5, avg_latency=312.4, position=(15, 0), hop_count=5, tx_count=200, rx_at_gateway=151)
    
    # Links (routing paths) - data flows toward gateway
    viz.add_link(2, 1)
    viz.add_link(3, 2)
    viz.add_link(4, 3)
    viz.add_link(5, 4)
    viz.add_link(6, 5)
    
    return viz


def create_branching_topology_demo():
    """Create a demo with branching topology (tree)"""
    viz = TopologyVisualizer("LoRa Mesh - Branching Topology (Tree)")
    
    # Gateway (Node 1) - receives all data
    # Total RX = 489+482+277+268+256 = 1772
    viz.add_node(1, pdr=None, avg_latency=None, position=(0, 3), hop_count=0, rx_count=1772)
    viz.set_gateway(1)
    
    # Hop count 1 - direct children of gateway
    viz.add_node(2, pdr=97.8, avg_latency=42.5, position=(3, 5), hop_count=1, tx_count=500, rx_at_gateway=489)
    viz.add_node(3, pdr=96.4, avg_latency=48.3, position=(3, 1), hop_count=1, tx_count=500, rx_at_gateway=482)
    
    # Hop count 2 - connected via hop count 1 nodes
    viz.add_node(4, pdr=92.3, avg_latency=98.7, position=(6, 5.5), hop_count=2, tx_count=300, rx_at_gateway=277)
    viz.add_node(5, pdr=89.3, avg_latency=112.4, position=(6, 3), hop_count=2, tx_count=300, rx_at_gateway=268)
    viz.add_node(6, pdr=85.3, avg_latency=156.3, position=(6, 0.5), hop_count=2, tx_count=300, rx_at_gateway=256)
    
    # Links - routing paths
    # Hop 1 to Gateway
    viz.add_link(2, 1)
    viz.add_link(3, 1)
    
    # Hop 2 to Hop 1
    viz.add_link(4, 2)
    viz.add_link(5, 2)
    viz.add_link(6, 3)
    
    return viz


def create_mixed_topology_demo():
    """Create a more complex mixed topology"""
    viz = TopologyVisualizer("LoRa Mesh - Mixed Topology (Realistic)")
    
    # Gateway (Node 1) - receives all data
    # Total RX = 982+378+766+311+247 = 2684
    viz.add_node(1, pdr=None, avg_latency=None, position=(0, 3), hop_count=0, rx_count=2684)
    viz.set_gateway(1)
    
    # Hop count 1 - direct to gateway
    viz.add_node(2, pdr=98.2, avg_latency=35.4, position=(3, 3), hop_count=1, tx_count=1000, rx_at_gateway=982)
    viz.add_node(4, pdr=94.5, avg_latency=68.2, position=(3, 5.5), hop_count=1, tx_count=400, rx_at_gateway=378)
    
    # Hop count 2 - via hop count 1 nodes
    viz.add_node(3, pdr=95.8, avg_latency=78.9, position=(6, 3), hop_count=2, tx_count=800, rx_at_gateway=766)
    viz.add_node(5, pdr=88.9, avg_latency=156.3, position=(6, 0.5), hop_count=2, tx_count=350, rx_at_gateway=311)
    viz.add_node(6, pdr=82.3, avg_latency=198.2, position=(6, 5.5), hop_count=2, tx_count=300, rx_at_gateway=247)
    
    # Links
    # Hop 1 to Gateway
    viz.add_link(2, 1)
    viz.add_link(4, 1)
    
    # Hop 2 to Hop 1
    viz.add_link(3, 2)
    viz.add_link(5, 2)
    viz.add_link(6, 4)
    
    return viz


def main():
    print("=" * 60)
    print("LoRa Mesh Network Topology Visualizer")
    print("=" * 60)
    print()
    
    # Generate Linear Topology
    print("[1/3] Generating Linear Topology...")
    linear_viz = create_linear_topology_demo()
    linear_viz.visualize("topology_linear.png")
    
    # Generate Branching Topology
    print("[2/3] Generating Branching Topology...")
    branch_viz = create_branching_topology_demo()
    branch_viz.visualize("topology_branching.png")
    
    # Generate Mixed Topology
    print("[3/3] Generating Mixed Topology...")
    mixed_viz = create_mixed_topology_demo()
    mixed_viz.visualize("topology_mixed.png")
    
    print()
    print("=" * 60)
    print("All visualizations generated successfully!")
    print("=" * 60)
    print("\nFiles created:")
    print("  - topology_linear.png    (Chain topology)")
    print("  - topology_branching.png (Tree topology)")  
    print("  - topology_mixed.png     (Complex mixed topology)")
    print()


if __name__ == "__main__":
    main()
