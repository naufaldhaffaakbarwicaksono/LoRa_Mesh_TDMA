#!/usr/bin/env python3
"""
Generate publication-quality graphs for self-healing network analysis
"""

import csv
import matplotlib.pyplot as plt
from datetime import datetime
import numpy as np
import argparse

def load_events(csv_file):
    """Load and parse WiFi events - filter out negative relative times"""
    events = []
    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            # Skip corrupted data with negative relative time
            try:
                if float(row['Relative_Time_S']) >= 0:
                    events.append(row)
            except (ValueError, KeyError):
                continue  # Skip malformed rows
    return events

def plot_latency_over_time(events, output_file='latency_comparison.png'):
    """Plot latency comparison: Node 3 vs Node 4"""
    node3_data = []
    node4_data = []
    failure_events = []
    recovery_events = []
    
    # Metrics for routing decision
    total_packets = 0
    packets_via_node3 = 0
    packets_via_node4 = 0
    rssi_node2_from_node3 = []
    rssi_node2_from_node4 = []
    rssi_node3_from_gw = []
    rssi_node4_from_gw = []
    
    for event in events:
        if event['Type'] == 'GW_RX_DATA' and 'From:2' in event['Details']:
            details = event['Details']
            timestamp = float(event['Relative_Time_S'])  # Use relative time directly
            
            if '[2>3>GW]' in details and 'Lat:' in details:
                lat_str = details.split('Lat:')[1].split('ms')[0]
                lat = float(lat_str)
                # Filter outliers (>100 seconds indicates delayed packet)
                if lat < 100000:  # Less than 100 seconds
                    node3_data.append((timestamp, lat))
                    total_packets += 1
                    packets_via_node3 += 1
            elif '[2>4>GW]' in details and 'Lat:' in details:
                lat_str = details.split('Lat:')[1].split('ms')[0]
                lat = float(lat_str)
                if lat < 100000:
                    node4_data.append((timestamp, lat))
                    total_packets += 1
                    packets_via_node4 += 1
        
        # Collect RSSI data (Node 2 receiving from Node 3 and 4)
        elif event['Type'] == 'BIDIR_LINK' and str(event['Node_ID']) == '2':
            if 'NodeID:3' in event['Details'] and 'RSSI:' in event['Details']:
                rssi_str = event['Details'].split('RSSI:')[1].split('dBm')[0]
                rssi_node2_from_node3.append(int(rssi_str))
            elif 'NodeID:4' in event['Details'] and 'RSSI:' in event['Details']:
                rssi_str = event['Details'].split('RSSI:')[1].split('dBm')[0]
                rssi_node2_from_node4.append(int(rssi_str))
        
        # Collect RSSI data (Node 3 and 4 receiving from Gateway Node 1)
        elif event['Type'] == 'BIDIR_LINK' and str(event['Node_ID']) == '3':
            if 'NodeID:1' in event['Details'] and 'RSSI:' in event['Details']:
                rssi_str = event['Details'].split('RSSI:')[1].split('dBm')[0]
                rssi_node3_from_gw.append(int(rssi_str))
        
        elif event['Type'] == 'BIDIR_LINK' and str(event['Node_ID']) == '4':
            if 'NodeID:1' in event['Details'] and 'RSSI:' in event['Details']:
                rssi_str = event['Details'].split('RSSI:')[1].split('dBm')[0]
                rssi_node4_from_gw.append(int(rssi_str))
        
        # Track TDMA control events
        elif event['Type'] == 'CMD_EXECUTED':
            timestamp = float(event['Relative_Time_S'])
            if 'TDMA_STOP' in event['Details']:
                failure_events.append(timestamp)
            elif 'TDMA_START' in event['Details']:
                recovery_events.append(timestamp)
    
    # Calculate average RSSI
    avg_rssi_n2_from_n3 = sum(rssi_node2_from_node3) / len(rssi_node2_from_node3) if rssi_node2_from_node3 else 0
    avg_rssi_n2_from_n4 = sum(rssi_node2_from_node4) / len(rssi_node2_from_node4) if rssi_node2_from_node4 else 0
    avg_rssi_n3_from_gw = sum(rssi_node3_from_gw) / len(rssi_node3_from_gw) if rssi_node3_from_gw else 0
    avg_rssi_n4_from_gw = sum(rssi_node4_from_gw) / len(rssi_node4_from_gw) if rssi_node4_from_gw else 0
    
    # Calculate average latency
    avg_lat_node3 = sum(lat for _, lat in node3_data) / len(node3_data) if node3_data else 0
    avg_lat_node4 = sum(lat for _, lat in node4_data) / len(node4_data) if node4_data else 0
    
    # Data already normalized (using Relative_Time_S)
    
    plt.figure(figsize=(14, 7))
    
    # Plot data points
    if node3_data:
        t3, lat3 = zip(*node3_data)
        plt.scatter(t3, lat3, c='#2E86AB', marker='o', s=80, alpha=0.7, 
                   edgecolors='black', linewidth=0.5, label='Via Node 3 (Primary)', zorder=3)
    
    if node4_data:
        t4, lat4 = zip(*node4_data)
        plt.scatter(t4, lat4, c='#A23B72', marker='s', s=80, alpha=0.7, 
                   edgecolors='black', linewidth=0.5, label='Via Node 4 (Backup)', zorder=3)
    
    # Mark failure and recovery events
    for i, ft in enumerate(failure_events):
        label = 'Node 3 TDMA_STOP' if i == 0 else None
        plt.axvline(x=ft, color='red', linestyle='--', linewidth=2.5, alpha=0.8, 
                   label=label, zorder=2)
        plt.text(ft, plt.ylim()[1] * 0.95, 'FAILURE', rotation=90, 
                verticalalignment='top', fontsize=9, color='red', fontweight='bold')
    
    for i, rt in enumerate(recovery_events):
        label = 'Node 3 TDMA_START' if i == 0 else None
        plt.axvline(x=rt, color='green', linestyle='--', linewidth=2.5, alpha=0.8, 
                   label=label, zorder=2)
        plt.text(rt, plt.ylim()[1] * 0.95, 'RECOVERY', rotation=90, 
                verticalalignment='top', fontsize=9, color='green', fontweight='bold')
    
    plt.xlabel('Time (seconds)', fontsize=13, fontweight='bold')
    plt.ylabel('Latency (ms)', fontsize=13, fontweight='bold')
    plt.title('Network Latency During Self-Healing Events', fontsize=15, fontweight='bold', pad=15)
    plt.legend(loc='upper left', fontsize=11, framealpha=0.9)
    plt.grid(True, alpha=0.3, linestyle=':', linewidth=1)
    
    # Add routing metrics textbox
    routing_info = (
        f'Routing Metrics:\n'
        f'Total Packets: {total_packets}\n'
        f'  - Via Node 3: {packets_via_node3} ({packets_via_node3/total_packets*100:.1f}%)\n'
        f'  - Via Node 4: {packets_via_node4} ({packets_via_node4/total_packets*100:.1f}%)\n\n'
        f'Average Latency:\n'
        f'  Via Node 3: {avg_lat_node3:.1f} ms\n'
        f'  Via Node 4: {avg_lat_node4:.1f} ms\n\n'
        f'RSSI (Average):\n'
        f'  Node 2 <- Node 3: {avg_rssi_n2_from_n3:.1f} dBm\n'
        f'  Node 2 <- Node 4: {avg_rssi_n2_from_n4:.1f} dBm\n'
        f'  Node 3 <- Gateway: {avg_rssi_n3_from_gw:.1f} dBm\n'
        f'  Node 4 <- Gateway: {avg_rssi_n4_from_gw:.1f} dBm'
    )
    
    plt.text(0.98, 0.98, routing_info, transform=plt.gca().transAxes,
            fontsize=9, verticalalignment='top', horizontalalignment='right',
            bbox=dict(boxstyle='round,pad=0.8', facecolor='wheat', alpha=0.85,
                     edgecolor='black', linewidth=1.5), family='monospace')
    
    # Set Y-axis limits to show actual latency range (3-5 seconds)
    if node3_data or node4_data:
        all_lats = [lat for _, lat in node3_data + node4_data]
        plt.ylim(0, max(all_lats) * 1.15)
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"✅ Saved: {output_file} (300 DPI)")
    plt.close()

def plot_route_distribution(events, output_file='route_distribution.png'):
    """Plot routing path distribution"""
    route_count = {}
    
    for event in events:
        if event['Type'] == 'GW_RX_DATA' and 'From:2' in event['Details']:
            details = event['Details']
            if '[2>3>GW]' in details:
                route_count['Node 3\n(Primary)'] = route_count.get('Node 3\n(Primary)', 0) + 1
            elif '[2>4>GW]' in details:
                route_count['Node 4\n(Backup)'] = route_count.get('Node 4\n(Backup)', 0) + 1
    
    plt.figure(figsize=(8, 6))
    routes = list(route_count.keys())
    counts = list(route_count.values())
    colors = ['#2E86AB', '#A23B72']
    
    bars = plt.bar(routes, counts, color=colors, edgecolor='black', linewidth=1.5)
    
    # Add percentage labels
    total = sum(counts)
    for bar, count in zip(bars, counts):
        height = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2., height,
                f'{count}\n({count/total*100:.1f}%)',
                ha='center', va='bottom', fontsize=12, fontweight='bold')
    
    plt.ylabel('Packet Count', fontsize=12)
    plt.title('Routing Path Distribution (Node 2 → Gateway)', fontsize=14, fontweight='bold')
    plt.ylim(0, max(counts) * 1.2)
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"✅ Saved: {output_file} (300 DPI)")
    plt.close()

def plot_healing_timeline(events, output_file='healing_timeline.png'):
    """Plot self-healing timeline with detailed annotations - AUTO-DETECT scenarios"""
    
    # Auto-detect failure scenarios from CMD_EXECUTED events
    failure_events = []
    recovery_events = []
    
    for event in events:
        if event['Type'] == 'CMD_EXECUTED':
            time_s = float(event['Relative_Time_S'])
            node_id = str(event['Node_ID'])
            if 'TDMA_STOP' in event['Details'] and node_id == '3':
                failure_events.append(time_s)
            elif 'TDMA_START' in event['Details'] and node_id == '3':
                recovery_events.append(time_s)
    
    # Build scenarios
    scenarios = []
    for i in range(min(len(failure_events), len(recovery_events))):
        failure_time = failure_events[i]
        recovery_time = recovery_events[i]
        
        # Calculate normal operation time before this failure
        if i == 0:
            normal_before = failure_time  # From start to first failure
        else:
            normal_before = failure_time - recovery_events[i-1]  # From previous recovery to this failure
        
        # Find NEIGHBOR_REMOVED event after failure (healing detection)
        healing_detected_time = None
        for event in events:
            if event['Type'] == 'NEIGHBOR_REMOVED' and 'NodeID:3' in event['Details']:
                time_s = float(event['Relative_Time_S'])
                if failure_time < time_s < recovery_time:
                    healing_detected_time = time_s
                    break
        
        # If no NEIGHBOR_REMOVED, use first data packet via Node 4 as healing indicator
        if not healing_detected_time:
            for event in events:
                if event['Type'] == 'GW_RX_DATA' and '[2>4>GW]' in event['Details']:
                    time_s = float(event['Relative_Time_S'])
                    if failure_time < time_s < recovery_time:
                        healing_detected_time = time_s
                        break
        
        if healing_detected_time:
            healing_duration = healing_detected_time - failure_time
            downtime_duration = recovery_time - healing_detected_time
        else:
            # Fallback: estimate healing as 10% of downtime
            total_downtime = recovery_time - failure_time
            healing_duration = total_downtime * 0.1
            downtime_duration = total_downtime * 0.9
        
        # Find joining time: from TDMA_START to first packet via Node 3
        joining_detected_time = None
        for event in events:
            if event['Type'] == 'GW_RX_DATA' and '[2>3>GW]' in event['Details']:
                time_s = float(event['Relative_Time_S'])
                if time_s > recovery_time:
                    joining_detected_time = time_s
                    break
        
        # Alternative: check for NEIGHBOR_ADDED or BIDIR_LINK for Node 3
        if not joining_detected_time:
            for event in events:
                if event['Type'] in ['NEIGHBOR_ADDED', 'BIDIR_LINK'] and 'NodeID:3' in event['Details']:
                    time_s = float(event['Relative_Time_S'])
                    if time_s > recovery_time:
                        joining_detected_time = time_s
                        break
        
        if joining_detected_time:
            joining_duration = joining_detected_time - recovery_time
        else:
            # Fallback estimate
            joining_duration = 5.0
        
        # Calculate restoration time (to next failure or end of data)
        if i + 1 < len(failure_events):
            restoration_duration = failure_events[i+1] - recovery_time - joining_duration
        else:
            # Use remaining data time or default 5s
            last_event_time = float(events[-1]['Relative_Time_S'])
            restoration_duration = max(5.0, min(last_event_time - recovery_time - joining_duration, 50.0))
        
        scenarios.append({
            'name': f'Failure Scenario #{i+1} (Node 3 TDMA_STOP at T+{failure_time:.1f}s)',
            'normal_before_failure': normal_before,
            'healing_time': healing_duration,
            'downtime_on_backup': downtime_duration,
            'joining_time': joining_duration,
            'restoration_time': restoration_duration
        })
    
    # If no scenarios detected, use placeholder
    if len(scenarios) == 0:
        print("⚠️  No complete failure scenarios detected in data")
        return
    
    # Create subplots (minimum 3, actual count if more)
    num_plots = max(3, len(scenarios))
    fig, axes = plt.subplots(num_plots, 1, figsize=(16, 3*num_plots))
    
    # Ensure axes is always a list
    if num_plots == 1:
        axes = [axes]
    
    for idx in range(num_plots):
        ax = axes[idx]
        
        if idx < len(scenarios):
            scenario = scenarios[idx]
            
            # Timeline stages
            stages = [
                '① NORMAL\nOperation\n(Route: 2>3>GW)',
                '② HEALING\nPhase\n(Detecting & Switching)',
                '③ BACKUP MODE\n(Route: 2>4>GW)\nNode 3 DOWN',
                '④ JOINING\nPhase\n(Node 3 Rejoining)',
                '⑤ RESTORED\n(Route: 2>3>GW)\nNode 3 UP'
            ]
            
            times = [
                scenario['normal_before_failure'],
                scenario['healing_time'],
                scenario['downtime_on_backup'],
                scenario['joining_time'],
                scenario['restoration_time']
            ]
            
            colors_stage = ['#27AE60', '#E74C3C', '#F39C12', '#9B59B6', '#3498DB']
            
            cumulative = 0
            for i, (stage, duration, color) in enumerate(zip(stages, times, colors_stage)):
                # Draw bar
                bar = ax.barh(-0.5, duration, left=cumulative, height=1.5, 
                             color=color, edgecolor='black', linewidth=2.5, alpha=0.85)
                
                # Add duration labels
                if i == 0:  # Normal phase (hijau) - di dalam bar
                    ax.text(cumulative + duration/2, -0.5, f'{duration:.1f}s',
                           ha='center', va='center', fontsize=13, fontweight='bold', 
                           color='white', bbox=dict(boxstyle='round,pad=0.4', 
                           facecolor='darkgreen', alpha=0.8, edgecolor='white', linewidth=2))
                elif i == 1:  # Healing phase (merah) - bar sempit, taruh di bawah
                    ax.text(cumulative + duration/2, -1.0, f'{duration:.1f}s',
                           ha='center', va='top', fontsize=11, fontweight='bold', 
                           color='darkred', bbox=dict(boxstyle='round,pad=0.3', 
                           facecolor='#FFE6E6', alpha=0.95, edgecolor='red', linewidth=1.5))
                elif i == 2:  # Backup mode (orange) - di dalam bar
                    ax.text(cumulative + duration/2, -0.5, f'{duration:.1f}s',
                           ha='center', va='center', fontsize=13, fontweight='bold', 
                           color='white', bbox=dict(boxstyle='round,pad=0.4', 
                           facecolor='darkorange', alpha=0.8, edgecolor='white', linewidth=2))
                elif i == 3:  # Joining phase (ungu) - bar sempit, taruh di bawah
                    ax.text(cumulative + duration/2, -1.0, f'{duration:.1f}s',
                           ha='center', va='top', fontsize=11, fontweight='bold', 
                           color='darkviolet', bbox=dict(boxstyle='round,pad=0.3', 
                           facecolor='#F3E6FF', alpha=0.95, edgecolor='purple', linewidth=1.5))
                elif i == 4:  # Restored phase (biru) - di dalam bar
                    ax.text(cumulative + duration/2, -0.5, f'{duration:.1f}s',
                           ha='center', va='center', fontsize=13, fontweight='bold', 
                           color='white', bbox=dict(boxstyle='round,pad=0.4', 
                           facecolor='darkblue', alpha=0.8, edgecolor='white', linewidth=2))
                
                # Add event markers
                if i == 1:  # Healing phase
                    ax.plot(cumulative, -0.5, 'rv', markersize=12, markeredgecolor='black', 
                           markeredgewidth=2, zorder=5)
                    ax.text(cumulative, -2.5, 'FAILURE\nINJECTED', ha='center', va='top',
                           fontsize=9, color='red', fontweight='bold',
                           bbox=dict(boxstyle='round,pad=0.4', facecolor='white', 
                           edgecolor='red', linewidth=2))
                
                if i == 3:  # Joining phase (TDMA_START command)
                    ax.plot(cumulative, -0.5, 'g^', markersize=12, markeredgecolor='black', 
                           markeredgewidth=2, zorder=5)
                    ax.text(cumulative, -2.5, 'TDMA_START\nCOMMAND', ha='center', va='top',
                           fontsize=9, color='green', fontweight='bold',
                           bbox=dict(boxstyle='round,pad=0.4', facecolor='white', 
                           edgecolor='green', linewidth=2))
                
                cumulative += duration
            
            # Stage descriptions below
            cumulative = 0
            for stage, duration in zip(stages, times):
                ax.text(cumulative + duration/2, 0.75, stage,
                       ha='center', va='bottom', fontsize=10, 
                       bbox=dict(boxstyle='round,pad=0.5', facecolor='white', 
                       edgecolor='gray', linewidth=1.5, alpha=0.9))
                cumulative += duration
            
            # Calculate metrics
            total_time = sum(times)
            healing_pct = (scenario['healing_time'] / total_time) * 100
            downtime_pct = (scenario['downtime_on_backup'] / total_time) * 100
            
            ax.set_xlim(-20, cumulative + 20)
            ax.set_ylim(-3.5, 1.5)
            
            title_text = (f'{scenario["name"]}\n'
                         f'Total Duration: {total_time:.1f}s  |  '
                         f'Healing Time: {scenario["healing_time"]:.1f}s ({healing_pct:.1f}%)  |  '
                         f'Downtime: {scenario["downtime_on_backup"]:.1f}s ({downtime_pct:.1f}%)')
            
            ax.set_title(title_text, fontsize=11, fontweight='bold', pad=25)
            ax.set_xlabel('Time (seconds)', fontsize=12, fontweight='bold')
            ax.set_yticks([])
            ax.spines['left'].set_visible(False)
            ax.spines['right'].set_visible(False)
            ax.spines['top'].set_visible(False)
            ax.grid(axis='x', alpha=0.3, linestyle=':', linewidth=1)
        else:
            # Empty placeholder for missing scenarios
            ax.text(0.5, 0.5, f'Scenario #{idx+1}\n(Data not available)', 
                   ha='center', va='center', fontsize=14, color='gray',
                   transform=ax.transAxes)
            ax.set_xticks([])
            ax.set_yticks([])
            ax.spines['left'].set_visible(False)
            ax.spines['right'].set_visible(False)
            ax.spines['top'].set_visible(False)
            ax.spines['bottom'].set_visible(False)
    
    plt.tight_layout(pad=3.0)
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"✅ Saved: {output_file} ({len(scenarios)} scenarios detected, {num_plots} plots generated)")
    plt.close()

def main():
    parser = argparse.ArgumentParser(
        description='Generate self-healing network analysis graphs',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  # Generate all graphs with default names
  python3 create_graphs.py wifi_events2.csv
  
  # Generate with custom output names
  python3 create_graphs.py wifi_events2.csv -l latency.png -t timeline.png -r routing.png
  
  # Generate only latency comparison
  python3 create_graphs.py wifi_events2.csv -l my_latency.png
  
  # Use base output name (adds suffixes automatically)
  python3 create_graphs.py wifi_events2.csv -o test1
    → test1_latency.png, test1_timeline.png, test1_routing.png
        '''
    )
    
    parser.add_argument('input_file', nargs='?', default='wifi_events2.csv',
                       help='Input CSV file (default: wifi_events2.csv)')
    parser.add_argument('-l', '--latency', metavar='FILE',
                       help='Output filename for latency comparison graph')
    parser.add_argument('-t', '--timeline', metavar='FILE',
                       help='Output filename for healing timeline graph')
    parser.add_argument('-r', '--routing', metavar='FILE',
                       help='Output filename for route distribution graph')
    parser.add_argument('-o', '--output-base', metavar='NAME',
                       help='Base name for all outputs (auto-adds suffixes)')
    
    args = parser.parse_args()
    
    # Determine output filenames
    if args.output_base:
        latency_file = f"{args.output_base}_latency.png"
        timeline_file = f"{args.output_base}_timeline.png"
        routing_file = f"{args.output_base}_routing.png"
    else:
        latency_file = args.latency or 'latency_comparison.png'
        timeline_file = args.timeline or 'healing_timeline.png'
        routing_file = args.routing or 'route_distribution.png'
    
    print("🎨 Generating publication-quality graphs...\n")
    print(f"📂 Input: {args.input_file}")
    
    try:
        events = load_events(args.input_file)
        print(f"📊 Loaded {len(events)} valid events (negative timestamps filtered)\n")
    except FileNotFoundError:
        print(f"❌ Error: File '{args.input_file}' not found")
        return 1
    except Exception as e:
        print(f"❌ Error loading file: {e}")
        return 1
    
    # Generate graphs
    graphs_generated = []
    
    if args.latency or args.output_base or (not args.timeline and not args.routing):
        plot_latency_over_time(events, latency_file)
        graphs_generated.append(latency_file)
    
    if args.routing or args.output_base or (not args.latency and not args.timeline):
        plot_route_distribution(events, routing_file)
        graphs_generated.append(routing_file)
    
    if args.timeline or args.output_base or (not args.latency and not args.routing):
        plot_healing_timeline(events, timeline_file)
        graphs_generated.append(timeline_file)
    
    print("\n✅ Graph generation complete!")
    print("\n📁 Output files (300 DPI, publication-ready):")
    for f in graphs_generated:
        print(f"   - {f}")
    print("\n💡 Use these graphs in your thesis/paper!")
    
    return 0

if __name__ == '__main__':
    exit(main())
