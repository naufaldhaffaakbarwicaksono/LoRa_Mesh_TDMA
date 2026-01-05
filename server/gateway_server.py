#!/usr/bin/env python3
"""
Gateway UDP Server for LoRa Mesh TDMA
=====================================
Receives data from gateway node via UDP and displays/logs it.

Ports:
- 5001: Monitor port (receive events from gateway)
- 5002: Command port (send commands to gateway)

Usage:
    python gateway_server.py
    python gateway_server.py --log output.csv
"""

import socket
import threading
import argparse
import datetime
import sys
import os

# Configuration
MONITOR_PORT = 5001
COMMAND_PORT = 5002
BUFFER_SIZE = 1024

# ANSI colors for terminal
class Colors:
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

def colorize(text, color):
    return f"{color}{text}{Colors.ENDC}"

class GatewayServer:
    def __init__(self, log_file=None):
        self.log_file = log_file
        self.running = True
        self.packet_count = 0
        self.start_time = datetime.datetime.now()
        
        # Statistics
        self.stats = {
            'total_packets': 0,
            'route_packets': 0,
            'pdr_packets': 0,
            'lat_packets': 0,
            'other_packets': 0
        }
        
        # Create UDP sockets
        self.monitor_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.monitor_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.monitor_socket.bind(('0.0.0.0', MONITOR_PORT))
        
        self.command_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.command_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.command_socket.bind(('0.0.0.0', COMMAND_PORT))
        
        # Open log file if specified
        if self.log_file:
            self.log_handle = open(self.log_file, 'a')
            self.log_handle.write(f"# Session started: {self.start_time.isoformat()}\n")
            self.log_handle.write("timestamp,type,raw_data\n")
        else:
            self.log_handle = None
    
    def parse_message(self, data):
        """Parse incoming message and categorize it"""
        msg = data.decode('utf-8', errors='ignore').strip()
        
        msg_type = 'OTHER'
        if '[ROUTE]' in msg:
            msg_type = 'ROUTE'
            self.stats['route_packets'] += 1
        elif '[PDR]' in msg:
            msg_type = 'PDR'
            self.stats['pdr_packets'] += 1
        elif '[LAT]' in msg:
            msg_type = 'LAT'
            self.stats['lat_packets'] += 1
        else:
            self.stats['other_packets'] += 1
        
        self.stats['total_packets'] += 1
        return msg_type, msg
    
    def format_message(self, msg_type, msg, addr):
        """Format message for display with colors"""
        timestamp = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
        
        if msg_type == 'ROUTE':
            color = Colors.CYAN
            icon = "[->]"
        elif msg_type == 'PDR':
            color = Colors.GREEN
            icon = "[%]"
        elif msg_type == 'LAT':
            color = Colors.YELLOW
            icon = "[T]"
        else:
            color = Colors.ENDC
            icon = "[?]"
        
        return f"{colorize(timestamp, Colors.BLUE)} {colorize(icon, color)} {msg}"
    
    def log_message(self, msg_type, msg):
        """Log message to file"""
        if self.log_handle:
            timestamp = datetime.datetime.now().isoformat()
            # Escape quotes and newlines
            safe_msg = msg.replace('"', '""').replace('\n', ' ')
            self.log_handle.write(f'{timestamp},{msg_type},"{safe_msg}"\n')
            self.log_handle.flush()
    
    def monitor_listener(self):
        """Listen for incoming monitor data from gateway"""
        print(colorize(f"[*] Monitor listener started on port {MONITOR_PORT}", Colors.GREEN))
        
        while self.running:
            try:
                data, addr = self.monitor_socket.recvfrom(BUFFER_SIZE)
                if data:
                    msg_type, msg = self.parse_message(data)
                    formatted = self.format_message(msg_type, msg, addr)
                    print(formatted)
                    self.log_message(msg_type, msg)
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(colorize(f"[!] Monitor error: {e}", Colors.RED))
    
    def command_handler(self):
        """Handle incoming commands (if gateway sends any responses)"""
        print(colorize(f"[*] Command handler started on port {COMMAND_PORT}", Colors.GREEN))
        
        while self.running:
            try:
                self.command_socket.settimeout(1.0)
                data, addr = self.command_socket.recvfrom(BUFFER_SIZE)
                if data:
                    msg = data.decode('utf-8', errors='ignore').strip()
                    timestamp = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
                    print(colorize(f"{timestamp} [CMD_RESP] {msg}", Colors.HEADER))
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(colorize(f"[!] Command error: {e}", Colors.RED))
    
    def send_command(self, gateway_ip, command):
        """Send command to gateway"""
        try:
            self.command_socket.sendto(command.encode(), (gateway_ip, COMMAND_PORT))
            print(colorize(f"[>] Sent to {gateway_ip}: {command}", Colors.HEADER))
        except Exception as e:
            print(colorize(f"[!] Failed to send command: {e}", Colors.RED))
    
    def print_stats(self):
        """Print current statistics"""
        runtime = datetime.datetime.now() - self.start_time
        print("\n" + "="*60)
        print(colorize("               GATEWAY SERVER STATISTICS", Colors.BOLD))
        print("="*60)
        print(f"  Runtime:        {runtime}")
        print(f"  Total Packets:  {self.stats['total_packets']}")
        print(f"  Route Packets:  {colorize(str(self.stats['route_packets']), Colors.CYAN)}")
        print(f"  PDR Packets:    {colorize(str(self.stats['pdr_packets']), Colors.GREEN)}")
        print(f"  Latency Packets:{colorize(str(self.stats['lat_packets']), Colors.YELLOW)}")
        print(f"  Other Packets:  {self.stats['other_packets']}")
        print("="*60 + "\n")
    
    def interactive_menu(self):
        """Interactive command menu"""
        print("\n" + colorize("Commands:", Colors.BOLD))
        print("  stats    - Show statistics")
        print("  send <ip> <cmd> - Send command to gateway")
        print("  clear    - Clear screen")
        print("  quit     - Exit server")
        print()
    
    def run(self):
        """Start the server"""
        print("\n" + "="*60)
        print(colorize("    LoRa Mesh TDMA - Gateway UDP Server", Colors.BOLD))
        print("="*60)
        print(f"  Monitor Port: {MONITOR_PORT}")
        print(f"  Command Port: {COMMAND_PORT}")
        if self.log_file:
            print(f"  Log File:     {self.log_file}")
        print("="*60)
        print(colorize("  Press Ctrl+C to show menu, Ctrl+C again to quit", Colors.YELLOW))
        print("="*60 + "\n")
        
        # Start listener threads
        monitor_thread = threading.Thread(target=self.monitor_listener, daemon=True)
        command_thread = threading.Thread(target=self.command_handler, daemon=True)
        
        monitor_thread.start()
        command_thread.start()
        
        try:
            while self.running:
                try:
                    # Wait for user input
                    cmd = input()
                    
                    if cmd.lower() == 'quit' or cmd.lower() == 'exit':
                        self.running = False
                        break
                    elif cmd.lower() == 'stats':
                        self.print_stats()
                    elif cmd.lower() == 'clear':
                        os.system('clear' if os.name != 'nt' else 'cls')
                    elif cmd.lower().startswith('send '):
                        parts = cmd.split(' ', 2)
                        if len(parts) >= 3:
                            self.send_command(parts[1], parts[2])
                        else:
                            print("Usage: send <gateway_ip> <command>")
                    elif cmd.lower() == 'help':
                        self.interactive_menu()
                    elif cmd:
                        print(colorize(f"Unknown command: {cmd}. Type 'help' for commands.", Colors.RED))
                        
                except EOFError:
                    break
                    
        except KeyboardInterrupt:
            print("\n")
            self.interactive_menu()
            try:
                while True:
                    cmd = input(colorize("gateway> ", Colors.GREEN))
                    
                    if cmd.lower() == 'quit' or cmd.lower() == 'exit':
                        break
                    elif cmd.lower() == 'stats':
                        self.print_stats()
                    elif cmd.lower() == 'clear':
                        os.system('clear' if os.name != 'nt' else 'cls')
                    elif cmd.lower().startswith('send '):
                        parts = cmd.split(' ', 2)
                        if len(parts) >= 3:
                            self.send_command(parts[1], parts[2])
                        else:
                            print("Usage: send <gateway_ip> <command>")
                    elif cmd.lower() == 'help':
                        self.interactive_menu()
                    elif cmd:
                        print(colorize(f"Unknown command: {cmd}", Colors.RED))
                        
            except KeyboardInterrupt:
                pass
        
        self.shutdown()
    
    def shutdown(self):
        """Clean shutdown"""
        print(colorize("\n[*] Shutting down...", Colors.YELLOW))
        self.running = False
        
        self.print_stats()
        
        if self.log_handle:
            self.log_handle.write(f"# Session ended: {datetime.datetime.now().isoformat()}\n")
            self.log_handle.close()
            print(colorize(f"[*] Log saved to: {self.log_file}", Colors.GREEN))
        
        self.monitor_socket.close()
        self.command_socket.close()
        print(colorize("[*] Server stopped.", Colors.GREEN))


def main():
    parser = argparse.ArgumentParser(
        description='Gateway UDP Server for LoRa Mesh TDMA',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python gateway_server.py                    # Run without logging
  python gateway_server.py --log data.csv    # Log to CSV file
  python gateway_server.py -l output.csv     # Short form
        """
    )
    parser.add_argument('-l', '--log', 
                        help='Log file path (CSV format)',
                        default=None)
    
    args = parser.parse_args()
    
    server = GatewayServer(log_file=args.log)
    server.run()


if __name__ == '__main__':
    main()
