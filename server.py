import os
import sys
import time
import socket
import threading

# Auto-install/check Flask dependency
try:
    from flask import Flask, Response, jsonify, render_template, request
except ImportError:
    print("Flask is not installed. Installing it now...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "Flask"])
    from flask import Flask, Response, jsonify, render_template, request

app = Flask(__name__, template_folder='templates', static_folder='static')

# Global state
latest_frame = None
client_connected = False
tcp_client_active = False
last_http_frame_time = 0.0
bytes_received = 0
frame_count = 0
fps = 0.0
kbps = 0.0
last_bytes_received = 0
stats_lock = threading.Lock()

# Ngrok tunnel state
ngrok_url = None
ngrok_auth_file = 'ngrok_token.txt'

# TCP Server details
TCP_PORT = 5001

def tcp_listener():
    global latest_frame, client_connected, tcp_client_active, bytes_received, frame_count, fps, kbps, last_bytes_received
    
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(('0.0.0.0', TCP_PORT))
        server_socket.listen(1)
        print(f"[TCP Server] Listening for ESP32 on port {TCP_PORT}...")
    except Exception as e:
        print(f"[TCP Server] Bind failed: {e}")
        return
    
    while True:
        try:
            client_socket, addr = server_socket.accept()
            print(f"[TCP Server] Connected to ESP32 / A7670C from {addr}")
            client_socket.settimeout(10.0)  # 10 seconds timeout
            
            with stats_lock:
                client_connected = True
                tcp_client_active = True
                frame_count = 0
                bytes_received = 0
                last_bytes_received = 0
                fps = 0.0
                kbps = 0.0
                
            buffer = bytearray()
            last_frame_time = time.time()
            
            while True:
                data = client_socket.recv(8192)
                if not data:
                    break
                
                buffer.extend(data)
                with stats_lock:
                    bytes_received += len(data)
                
                # Parse frames
                while True:
                    # Search for frame start: 0xAA 0xBB 0xCC 0xDD
                    header_idx = buffer.find(b'\xaa\xbb\xcc\xdd')
                    if header_idx == -1:
                        if len(buffer) > 3:
                            buffer = buffer[-3:]
                        break
                    
                    # Ensure we have the length field (4 bytes after header)
                    if len(buffer) < header_idx + 8:
                        if header_idx > 0:
                            buffer = buffer[header_idx:]
                        break
                    
                    # Read frame length (big endian)
                    len_bytes = buffer[header_idx + 4 : header_idx + 8]
                    frame_len = int.from_bytes(len_bytes, byteorder='big')
                    
                    if frame_len <= 0 or frame_len > 1024 * 1024:
                        buffer = buffer[header_idx + 4:]
                        continue
                    
                    # Ensure we have the full JPEG payload
                    if len(buffer) < header_idx + 8 + frame_len:
                        if header_idx > 0:
                            buffer = buffer[header_idx:]
                        break
                    
                    # Extract raw JPEG
                    frame_start = header_idx + 8
                    frame_end = frame_start + frame_len
                    jpeg_data = buffer[frame_start:frame_end]
                    
                    latest_frame = bytes(jpeg_data)
                    
                    now = time.time()
                    dt = now - last_frame_time
                    last_frame_time = now
                    
                    with stats_lock:
                        frame_count += 1
                        if dt > 0:
                            fps = 1.0 / dt
                    
                    buffer = buffer[frame_end:]
                    
        except socket.timeout:
            print("[TCP Server] Connection timed out (no data received for 10 seconds)")
        except Exception as e:
            print(f"[TCP Server] Error during connection: {e}")
        finally:
            try:
                client_socket.close()
            except:
                pass
            with stats_lock:
                tcp_client_active = False
                client_connected = False
            print("[TCP Server] ESP32 Disconnected")

def stats_tracker():
    """Calculates data rate (KB/s) periodically and handles HTTP idle timeout."""
    global bytes_received, last_bytes_received, kbps, client_connected, last_http_frame_time, tcp_client_active
    while True:
        time.sleep(1.0)
        now = time.time()
        with stats_lock:
            diff = bytes_received - last_bytes_received
            last_bytes_received = bytes_received
            kbps = (diff / 1024.0)
            
            # Idle timeout for HTTP upload mode
            if not tcp_client_active and last_http_frame_time > 0:
                if now - last_http_frame_time > 5.0:
                    client_connected = False

def start_ngrok():
    global ngrok_url
    authtoken = os.environ.get("NGROK_AUTHTOKEN")
    
    # Check if local configuration file exists
    if not authtoken and os.path.exists(ngrok_auth_file):
        try:
            with open(ngrok_auth_file, 'r') as f:
                authtoken = f.read().strip()
        except Exception as e:
            print(f"[Ngrok] Error reading {ngrok_auth_file}: {e}")
            
    if authtoken:
        try:
            import ngrok
            print("[Ngrok] Initializing native TCP tunnel to port 5001...")
            # Forward port 5001 over TCP
            listener = ngrok.forward(TCP_PORT, "tcp", authtoken=authtoken)
            ngrok_url = listener.url()
            print(f"[Ngrok] Tunnel established! Public URL: {ngrok_url}")
            
            clean_url = ngrok_url.replace("tcp://", "")
            parts = clean_url.split(":")
            if len(parts) == 2:
                print(f"[Ngrok] Copy these settings into your ESP32 code:")
                print(f"        serverIp = \"{parts[0]}\";")
                print(f"        serverPort = {parts[1]};")
        except Exception as e:
            print(f"[Ngrok] Failed to establish tunnel: {e}")
            print("\n" + "="*80)
            print("[TUNNEL ALTERNATIVE] FREE TCP Tunnel (No Credit Card / No Sign Up Required):")
            print("Since Ngrok requires credit card verification for free TCP tunnels, use Pinggy instead!")
            print("Open a new PowerShell or Command Prompt on your laptop and run:")
            print("    ssh -p 443 -R0:127.0.0.1:5001 tcp@a.pinggy.io")
            print("When prompted, press Enter. It will output a line like:")
            print("    tcp://xxxxxx.a.pinggy.link:yyyyy")
            print("Copy 'xxxxxx.a.pinggy.link' as your serverIp and 'yyyyy' as your serverPort in your ESP32 code!")
            print("="*80 + "\n")
    else:
        print("\n" + "="*80)
        print("[TUNNEL REQUIRED] To stream via GSM, you need a public TCP tunnel:")
        print("Open a new PowerShell or Command Prompt on your laptop and run:")
        print("    ssh -p 443 -R0:127.0.0.1:5001 tcp@a.pinggy.io")
        print("When prompted, press Enter. It will output a line like:")
        print("    tcp://xxxxxx.a.pinggy.link:yyyyy")
        print("Copy 'xxxxxx.a.pinggy.link' as your serverIp and 'yyyyy' as your serverPort in your ESP32 code!")
        print("="*80 + "\n")

# Start TCP socket listener thread
tcp_thread = threading.Thread(target=tcp_listener, daemon=True)
tcp_thread.start()

# Start Stats Tracker thread
stats_thread = threading.Thread(target=stats_tracker, daemon=True)
stats_thread.start()

# Start Ngrok Tunneling thread
ngrok_thread = threading.Thread(target=start_ngrok, daemon=True)
ngrok_thread.start()

# Flask Routes
@app.route('/')
def index():
    return render_template('index.html')

@app.route('/upload', methods=['POST'])
def upload():
    global latest_frame, client_connected, bytes_received, frame_count, fps, last_http_frame_time
    try:
        data = request.get_data()
        if not data:
            return "No data received", 400
            
        now = time.time()
        with stats_lock:
            latest_frame = data
            client_connected = True
            frame_count += 1
            bytes_received += len(data)
            
            # Calculate dynamic FPS for HTTP POST mode
            if last_http_frame_time > 0:
                dt = now - last_http_frame_time
                if dt > 0:
                    fps = 1.0 / dt
            last_http_frame_time = now
            
        return "OK", 200
    except Exception as e:
        print(f"[HTTP Upload] Error: {e}")
        return str(e), 500

def gen_mjpeg():
    global latest_frame
    last_sent = None
    while True:
        if latest_frame and latest_frame != last_sent:
            last_sent = latest_frame
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + latest_frame + b'\r\n')
        else:
            time.sleep(0.01)

@app.route('/stream')
def stream():
    return Response(gen_mjpeg(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/status')
def status():
    with stats_lock:
        return jsonify({
            'connected': client_connected,
            'fps': round(fps, 1) if client_connected else 0.0,
            'kbps': round(kbps, 1) if client_connected else 0.0,
            'frame_count': frame_count if client_connected else 0,
            'bytes_received': bytes_received if client_connected else 0,
            'ngrok_url': ngrok_url.replace("tcp://", "") if ngrok_url else None
        })

if __name__ == '__main__':
    # Ensure static and template directories exist
    os.makedirs('templates', exist_ok=True)
    os.makedirs('static', exist_ok=True)
    
    # Start thread to automatically open the web browser
    def auto_open():
        time.sleep(1.0)
        import webbrowser
        print("[Flask] Opening dashboard in browser: http://localhost:5000")
        webbrowser.open("http://localhost:5000")
        
    threading.Thread(target=auto_open, daemon=True).start()
    
    print("[Flask] Starting Web Server on http://localhost:5000")
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)
