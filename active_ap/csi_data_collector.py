import socket
import csv
import threading
import time
import os
import signal
import sys
from datetime import datetime
from queue import Queue, Empty

class CSIDataCollector:
    def __init__(self, port=9999, output_file="csi_data.csv"):
        self.port = port
        self.output_file = output_file
        self.is_collecting = False
        self.packet_count = 0
        self.data_queue = Queue()
        self.socket = None
        
        self.last_status_time = 0
        self.last_packet_count = 0

        self.csv_headers = [
            'type', 'role', 'mac', 'rssi', 'rate', 'sig_mode', 'mcs', 
            'bandwidth', 'smoothing', 'not_sounding', 'aggregation', 
            'stbc', 'fec_coding', 'sgi', 'noise_floor', 'ampdu_cnt', 
            'channel', 'secondary_channel', 'local_timestamp', 'ant', 
            'sig_len', 'rx_state', 'real_time_set', 'real_timestamp', 
            'len', 'CSI_DATA', 'pc_timestamp'
        ]
        
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
    
    def signal_handler(self, signum, frame):
        print(f"\nReceived signal {signum}. Shutting down gracefully...")
        self.stop_collection()
        sys.exit(0)
    
    def setup_udp_socket(self):
        try:
            # Create UDP socket
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            
            # Allow socket reuse to prevent "Address already in use" errors
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # Bind to all interfaces on specified port
            self.socket.bind(('0.0.0.0', self.port))
            
            # Set socket timeout for non-blocking behavior
            self.socket.settimeout(1.0)
            
            print(f"UDP socket listening on port {self.port}")
            return True
            
        except Exception as e:
            print(f"Error setting up UDP socket: {e}")
            return False
    
    def parse_csi_data(self, raw_data):
        try:
            data_parts = raw_data.strip().split(',')
            
            if len(data_parts) < 20:
                print(f"Warning: Incomplete data packet received: {len(data_parts)} fields")
                return None
            
            csi_data_str = ""
            if '[' in raw_data and ']' in raw_data:
                start_idx = raw_data.find('[') + 1
                end_idx = raw_data.find(']')
                csi_data_str = raw_data[start_idx:end_idx].strip()
            
            # Create CSV row with PC timestamp added
            pc_timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            
            csv_row = []
            
            if len(data_parts) >= 25:
                csv_row = [
                    data_parts[0],   # type (CHANNEL_STATE_INFO)
                    data_parts[1],   # role (collector)
                    data_parts[2],   # mac
                    data_parts[3],   # rssi
                    data_parts[4],   # rate
                    data_parts[5],   # sig_mode
                    data_parts[6],   # mcs
                    data_parts[7],   # bandwidth (cwb)
                    data_parts[8],   # smoothing
                    data_parts[9],   # not_sounding
                    data_parts[10],  # aggregation
                    data_parts[11],  # stbc
                    data_parts[12],  # fec_coding
                    data_parts[13],  # sgi
                    data_parts[14],  # noise_floor
                    data_parts[15],  # ampdu_cnt
                    data_parts[16],  # channel
                    data_parts[17],  # secondary_channel
                    data_parts[18],  # local_timestamp
                    data_parts[19],  # ant
                    data_parts[20],  # sig_len
                    data_parts[21],  # rx_state
                    data_parts[22],  # real_time_set
                    data_parts[23],  # real_timestamp
                    data_parts[24],  # len
                    csi_data_str,    # CSI_DATA
                    pc_timestamp     # pc_timestamp
                ]
            
            return csv_row
        
        except Exception as e:
            print(f"Error parsing CSI data: {e}")
            print(f"Raw data: {raw_data[:100]}...")
            return None
    
    def udp_receiver_thread(self):
        print("UDP receiver thread started")
        
        while self.is_collecting:
            try:
                # Receive data from ESP32 (non-blocking due to timeout)
                data, address = self.socket.recvfrom(4096)  # Match ESP32 buffer size
                
                # Decode bytes to string
                decoded_data = data.decode('utf-8', errors='ignore')
                
                # Add to processing queue with metadata
                packet_info = {
                    'data': decoded_data,
                    'source_ip': address[0],
                    'source_port': address[1],
                    'received_time': time.time()
                }
                
                self.data_queue.put(packet_info)
                self.packet_count += 1
                
            except socket.timeout:
                continue
            except Exception as e:
                if self.is_collecting:
                    print(f"Error receiving UDP data: {e}")
                break
    
    def csv_writer_thread(self):
        print(f"CSV writer thread started - writing to {self.output_file}")
        
        write_headers = not os.path.exists(self.output_file)
        
        try:
            with open(self.output_file, 'a', newline='', encoding='utf-8') as csvfile:
                writer = csv.writer(csvfile)
                
                if write_headers:
                    writer.writerow(self.csv_headers)
                    csvfile.flush()
                    print("CSV headers written")
                
                while self.is_collecting:
                    try:
                        # Get data from queue (with timeout)
                        packet_info = self.data_queue.get(timeout=1.0)
                        
                        # Parse the CSI data
                        csv_row = self.parse_csi_data(packet_info['data'])
                        
                        if csv_row:
                            # Write to CSV file
                            writer.writerow(csv_row)
                            csvfile.flush()
                        
                        self.data_queue.task_done()
                        
                    except Empty:
                        continue
                    except Exception as e:
                        print(f"Error writing to CSV: {e}")
                        continue
        
        except Exception as e:
            print(f"Error opening CSV file: {e}")
    
    def start_collection(self):
        print("Starting CSI data collection...")
        print(f"Output file: {self.output_file}")
        print("Press Ctrl+C to stop collection")
        
        if not self.setup_udp_socket():
            return False
        
        self.is_collecting = True
        
        receiver_thread = threading.Thread(target=self.udp_receiver_thread, daemon=True)
        receiver_thread.start()
        
        writer_thread = threading.Thread(target=self.csv_writer_thread, daemon=True)
        writer_thread.start()
        
        print("Collection started successfully!")
        
        self.last_status_time = time.time()
        self.last_packet_count = self.packet_count

        try:
            while self.is_collecting:
                time.sleep(1)
                
                current_time = time.time()
                time_delta = current_time - self.last_status_time
                
                pps = 0
                if time_delta > 0:
                    current_packet_count = self.packet_count
                    packet_delta = current_packet_count - self.last_packet_count
                    pps = packet_delta / time_delta
                
                queue_size = self.data_queue.qsize()
                
                status_msg = (
                    f"\rStatus: {self.packet_count} total packets | "
                    f"PPS: {pps:.2f} | "
                    f"Queue: {queue_size}    "
                )
                print(status_msg, end='', flush=True)

                self.last_status_time = current_time
                self.last_packet_count = self.packet_count
        
        except KeyboardInterrupt:
            print()
            self.stop_collection()
        
        return True
    
    def stop_collection(self):
        print("\nStopping CSI data collection...")
        self.is_collecting = False
        
        if self.socket:
            self.socket.close()
        
        if not self.data_queue.empty():
            print(f"Processing remaining {self.data_queue.qsize()} items in queue...")
            self.data_queue.join()
        
        print(f"Collection stopped. Total packets processed: {self.packet_count}")

def main():
    print("ESP32 CSI Data Collector")
    print("=" * 40)
    
    DATA_FOLDER = "csi_data"
    if not os.path.exists(DATA_FOLDER):
        try:
            os.makedirs(DATA_FOLDER)
            print(f"Created data directory: {DATA_FOLDER}")
        except OSError as e:
            print(f"Error creating directory {DATA_FOLDER}: {e}")
            sys.exit(1)

    UDP_PORT = 9999  # Must match ESP32 configuration
    file_timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    filename = f"csi_data_{file_timestamp}.csv"
    OUTPUT_FILE = os.path.join(DATA_FOLDER, filename)
    
    collector = CSIDataCollector(port=UDP_PORT, output_file=OUTPUT_FILE)
    
    print(f"Configuration:")
    print(f"  UDP Port: {UDP_PORT}")
    print(f"  Output File: {OUTPUT_FILE}")
    print(f"  Expected ESP32 AP: ESP32-AP")
    print()
    
    print("Setup Instructions:")
    print("1. Connect your computer to the ESP32's WiFi AP (ESP32-AP)")
    print("2. Password: esp32-ap")
    print("3. Run this script")
    print("4. ESP32 will automatically discover your computer and start sending data")
    print()
    
    # Start collection
    collector.start_collection()

if __name__ == "__main__":
    main()