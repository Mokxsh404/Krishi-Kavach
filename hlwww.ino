#include "esp_camera.h"

// ==========================================
// Camera Pin Definitions (XIAO ESP32S3 Sense)
// ==========================================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// ==========================================
// Streaming Quality Mode
// ==========================================
// Mode 1: High FPS Mode (QVGA 320x240, ~10-15 FPS, very fluid)
// Mode 2: High Detail Mode (VGA 640x480, ~3-5 FPS, super sharp)
#define STREAM_MODE 2

// ==========================================
// Connection Mode Configuration
// ==========================================
// Mode 0: Raw TCP Socket Stream (Magic Bytes 0xAA 0xBB 0xCC 0xDD, port 5001)
// Mode 1: HTTP POST Stream over TCP (Plain HTTP upload, port 80)
// Mode 2: HTTPS POST Stream over SSL (Secure HTTPS upload, port 443)
#define CONNECTION_MODE 2

// IMPORTANT: Enter your server address.
// For Mode 0: public endpoint from ngrok tcp (e.g. "0.tcp.ngrok.io") or pinggy.
// For Mode 1/2: your hosted web server address (e.g. "your-app.onrender.com" or "your-app.up.railway.app").
// DO NOT include "http://" or "https://" or "/" in the serverIp!
const char* serverIp = "krishi-kavach.onrender.com"; 

// Server Port:
// - For Mode 0: Use the TCP port given by the tunnel (e.g. 12345).
// - For Mode 1: Use 80 (HTTP).
// - For Mode 2: Use 443 (HTTPS).
const uint16_t serverPort = 443; 

// APN config (usually empty/automatic for LTE Cat-1, but can be set if needed)
const char* apn = ""; 

// ==========================================
// GSM Module Serial Pins & Wiring Details
// ==========================================
// • Connect ESP32 Pin D4 (GPIO 5) -> A7670C TX
// • Connect ESP32 Pin D5 (GPIO 6) -> A7670C RX
// • Connect ESP32 GND             -> A7670C GND (Common Ground is REQUIRED)
#ifdef D4
  #define GSM_RX_PIN D4
#else
  #define GSM_RX_PIN 5
#endif

#ifdef D5
  #define GSM_TX_PIN D5
#else
  #define GSM_TX_PIN 6
#endif

// Serial connection to A7670C SIM module
HardwareSerial gsm(1);

// State Machine for GSM connectivity
enum GsmState {
  STATE_INIT,
  STATE_CHECK_NET,
  STATE_OPEN_NET,
  STATE_CONNECT_TCP,
  STATE_STREAMING,
  STATE_DISCONNECT
};

GsmState currentState = STATE_INIT;
uint32_t lastStateChangeTime = 0;
uint32_t lastFrameTime = 0;

// Reconnection attempt counter
int reconnectAttempts = 0;

// Helper to check state timeout
bool stateTimeout(uint32_t ms) {
  return (millis() - lastStateChangeTime > ms);
}

// Change state helper
void changeState(GsmState newState, const char* reason) {
  Serial.printf("State: %d -> %d | Reason: %s\n", currentState, newState, reason);
  currentState = newState;
  lastStateChangeTime = millis();
}

// Helper to send AT command and wait for expected response
bool sendATCommand(const String& cmd, const String& expected, uint32_t timeout) {
  while (gsm.available()) gsm.read(); // Clear RX buffer
  
  gsm.println(cmd);
  
  uint32_t startTime = millis();
  String response = "";
  while (millis() - startTime < timeout) {
    while (gsm.available()) {
      char c = gsm.read();
      response += c;
      if (response.indexOf(expected) != -1) {
        return true;
      }
    }
    delay(10);
  }
  Serial.print("AT Failed: ");
  Serial.print(cmd);
  Serial.print(" | Response: ");
  Serial.println(response);
  return false;
}

// Safely exit transparent data mode
void exitTransparentMode() {
  Serial.println("Exiting transparent mode...");
  delay(1100); // 1s guard time before
  gsm.print("+++");
  delay(1100); // 1s guard time after
  
  // Send a simple AT command to verify we are back in command mode
  if (sendATCommand("AT", "OK", 2000)) {
    Serial.println("Successfully back in command mode.");
  } else {
    Serial.println("Failed to exit transparent mode, retrying...");
    // Try sending AT again
    gsm.println("AT");
    delay(100);
  }
}

// Camera initialization
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  
  // Note: QVGA (320x240) is optimized for 115200 baud streaming.
  // If you upgrade the GSM baud rate, you can try FRAMESIZE_VGA (640x480).
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  
  #if STREAM_MODE == 1
    // High FPS Mode (QVGA)
    config.frame_size = FRAMESIZE_QVGA; 
    if (psramFound()) {
      config.fb_location = CAMERA_FB_IN_PSRAM;
      config.fb_count = 2;
      config.jpeg_quality = 10; // Optimized size/quality balance for high FPS
      Serial.println("PSRAM found. QVGA configured in PSRAM.");
    } else {
      config.fb_location = CAMERA_FB_IN_DRAM;
      config.fb_count = 1;
      config.jpeg_quality = 12; // Optimized size/quality balance for DRAM fallback
      Serial.println("PSRAM NOT found! QVGA configured in internal DRAM.");
    }
  #else
    // Detail Mode (VGA)
    config.frame_size = FRAMESIZE_VGA; 
    if (psramFound()) {
      config.fb_location = CAMERA_FB_IN_PSRAM;
      config.fb_count = 2;
      config.jpeg_quality = 9; // High-detail VGA compression
      Serial.println("PSRAM found. VGA configured in PSRAM.");
    } else {
      config.fb_location = CAMERA_FB_IN_DRAM;
      config.fb_count = 1;
      config.jpeg_quality = 12;
      Serial.println("PSRAM NOT found! VGA configured in internal DRAM.");
    }
  #endif

  // Initialize camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  s->set_vflip(s, 0);       // Flip vertically if needed
  s->set_hmirror(s, 0);     // Mirror horizontally if needed
  
  Serial.println("Camera Initialized Successfully.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("XIAO ESP32S3 Sense GSM Streaming Node Starting...");

  const uint32_t highBaud = 921600;
  bool isHighBaud = false;

  // Step 1: Try communicating at 115200 first
  Serial.println("Checking GSM module connection at 115200 baud...");
  gsm.setTxBufferSize(16384); // 16KB TX buffer (fits entire frame)
  gsm.setRxBufferSize(2048);
  gsm.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(500);
  
  // Clear any noise from buffer
  while(gsm.available()) gsm.read();
  
  gsm.println("AT");
  delay(100);
  if (gsm.available()) {
    String resp = gsm.readString();
    if (resp.indexOf("OK") != -1) {
      Serial.println("GSM responded at 115200. Sending upgrade command to 921600 baud...");
      gsm.println("AT+IPR=921600");
      delay(200);
      gsm.end();
      
      // Re-open at 921600 with large buffers
      gsm.setTxBufferSize(16384);
      gsm.setRxBufferSize(2048);
      gsm.begin(highBaud, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
      delay(500);
      isHighBaud = true;
    }
  }

  // Step 2: If no response at 115200, try 921600 directly (case where board reset but module retained speed)
  if (!isHighBaud) {
    Serial.println("No response at 115200. Trying to hook at 921600 baud...");
    gsm.end();
    
    gsm.setTxBufferSize(16384);
    gsm.setRxBufferSize(2048);
    gsm.begin(highBaud, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    delay(500);
    
    while(gsm.available()) gsm.read();
    
    gsm.println("AT");
    delay(100);
    if (gsm.available()) {
      String resp = gsm.readString();
      if (resp.indexOf("OK") != -1) {
        Serial.println("GSM successfully connected at 921600!");
        isHighBaud = true;
      }
    }
  }

  // Step 3: Fallback if everything failed
  if (!isHighBaud) {
    Serial.println("WARNING: Failed to negotiate 921600. Falling back to 115200 baud.");
    gsm.end();
    
    gsm.setTxBufferSize(16384);
    gsm.setRxBufferSize(2048);
    gsm.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    delay(500);
  }
  
  initCamera();
  changeState(STATE_INIT, "System booted");
}

void loop() {
  switch (currentState) {
    
    case STATE_INIT: {
      Serial.println("Initializing GSM module...");
      
      // Force exit transparent mode if left active from a previous ESP32 boot
      exitTransparentMode();
      
      // Close any active TCP socket and shut down IP stack to allow changing CIPMODE
      sendATCommand("AT+CIPCLOSE=0", "OK", 2000);
      #if CONNECTION_MODE == 2
        sendATCommand("AT+CCHCLOSE=0", "OK", 2000);
        sendATCommand("AT+CCHSTOP", "OK", 2000);
      #endif
      sendATCommand("AT+NETCLOSE", "OK", 2000);
      
      // Send standard init commands
      if (sendATCommand("AT", "OK", 2000)) {
        sendATCommand("ATE0", "OK", 2000); // Turn off character echo
        
        // Configure APN if specified
        if (strlen(apn) > 0) {
          sendATCommand("AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"", "OK", 3000);
        }
        
        #if CONNECTION_MODE == 2
          // ---- SSL/HTTPS Mode (uses CCH commands, NOT CIPMODE) ----
          // Configure SSL Context 0 for TLS 1.2 and bypass cert validation
          sendATCommand("AT+CSSLCFG=\"sslversion\",0,4", "OK", 2000);
          sendATCommand("AT+CSSLCFG=\"authmode\",0,0", "OK", 2000);
          // Ignore certificate expiration date/time checks (important if board clock is unset)
          sendATCommand("AT+CSSLCFG=\"ignorertctime\",0,1", "OK", 2000);
          // *** CRITICAL: Enable SNI (Server Name Indication) ***
          // Required by cloud hosts like Render that use shared hosting/reverse proxies.
          // Without SNI, the TLS handshake fails with CCHOPEN error 15.
          sendATCommand("AT+CSSLCFG=\"enableSNI\",0,1", "OK", 2000);
          // Enable CCH transparent mode (data piped directly to/from serial)
          sendATCommand("AT+CCHMODE=1", "OK", 2000);
          // Enable +CCHSEND result reporting
          sendATCommand("AT+CCHSET=1", "OK", 2000);
          // Bind CCH session 0 to SSL context 0
          sendATCommand("AT+CCHSSLCFG=0,0", "OK", 2000);
          // Start common channel SSL service
          if (sendATCommand("AT+CCHSTART", "OK", 5000)) {
            changeState(STATE_CHECK_NET, "GSM module & SSL service initialized");
          } else {
            Serial.println("Failed to start SSL service. Retrying...");
            delay(2000);
          }
        #else
          // ---- TCP/HTTP Mode (uses CIPOPEN with transparent mode) ----
          // Set transparent TCP mode (only for CIPOPEN, not CCHOPEN)
          if (sendATCommand("AT+CIPMODE=1", "OK", 3000)) {
            changeState(STATE_CHECK_NET, "GSM module initialized");
          } else {
            Serial.println("Failed to set Transparent Mode. Retrying...");
            delay(2000);
          }
        #endif
      } else {
        Serial.println("A7670C not responding to AT. Check wiring and power.");
        delay(3000);
      }
      break;
    }
    
    case STATE_CHECK_NET: {
      Serial.println("Checking GPRS network attachment...");
      if (sendATCommand("AT+CGATT?", "+CGATT: 1", 3000)) {
        changeState(STATE_OPEN_NET, "Network attached");
      } else {
        Serial.println("Network not attached yet. Retrying...");
        delay(3000);
      }
      break;
    }
    
    case STATE_OPEN_NET: {
      Serial.println("Opening network IP stack...");
      // NETOPEN opens the wireless network connection
      // A7670C returns OK, and then +NETOPEN: 0 if successfully opened or if it's already open.
      gsm.println("AT+NETOPEN");
      
      // Wait for OK or +NETOPEN: 0
      uint32_t start = millis();
      bool netOpen = false;
      while (millis() - start < 5000) {
        if (gsm.available()) {
          String resp = gsm.readString();
          Serial.print(resp);
          if (resp.indexOf("OK") != -1 || resp.indexOf("+NETOPEN: 0") != -1 || resp.indexOf("Already") != -1) {
            netOpen = true;
            break;
          }
        }
        delay(10);
      }
      
      if (netOpen) {
        changeState(STATE_CONNECT_TCP, "Network IP stack opened");
      } else {
        Serial.println("Failed to open network IP stack. Retrying...");
        delay(3000);
      }
      break;
    }
    
    case STATE_CONNECT_TCP: {
      #if CONNECTION_MODE == 2
        Serial.printf("Connecting to HTTPS server %s:%d using SSL...\n", serverIp, serverPort);
        // CCHOPEN: session 0, host, port, 2=SSL
        // In CCH transparent mode, successful connection returns +CCHOPEN: 0,0 then enters data mode
        String connCmd = "AT+CCHOPEN=0,\"" + String(serverIp) + "\"," + String(serverPort) + ",2";
        
        // For CCHOPEN in transparent mode, the module returns +CCHOPEN: 0,0 on success
        // and then enters transparent data mode (like CONNECT for CIPOPEN)
        if (sendATCommand(connCmd, "CONNECT", 60000)) {
          Serial.println("SSL Connection established! Entering Streaming mode.");
          reconnectAttempts = 0;
          changeState(STATE_STREAMING, "SSL socket connected");
        } else {
          Serial.println("SSL Connection failed.");
          reconnectAttempts++;
          if (reconnectAttempts > 3) {
            reconnectAttempts = 0;
            changeState(STATE_DISCONNECT, "Too many SSL connection failures, resetting");
          } else {
            delay(5000);
          }
        }
      #else
        Serial.printf("Connecting to TCP/HTTP server %s:%d...\n", serverIp, serverPort);
        String connCmd = "AT+CIPOPEN=0,\"TCP\",\"" + String(serverIp) + "\"," + String(serverPort);
        
        // In transparent mode, successful connection returns CONNECT
        if (sendATCommand(connCmd, "CONNECT", 35000)) {
          Serial.println("Connection established! Entering Streaming mode.");
          reconnectAttempts = 0;
          changeState(STATE_STREAMING, "Socket connected");
        } else {
          Serial.println("Connection failed.");
          reconnectAttempts++;
          if (reconnectAttempts > 3) {
            reconnectAttempts = 0;
            changeState(STATE_DISCONNECT, "Too many connection failures, resetting connection");
          } else {
            delay(5000);
          }
        }
      #endif
      break;
    }
    
    case STATE_STREAMING: {
      // Stream camera frames
      camera_fb_t * fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("Camera capture failed");
        delay(500);
        break;
      }
      
      #if CONNECTION_MODE == 0
        // Send framing header
        // Magic Bytes: 0xAA 0xBB 0xCC 0xDD
        uint8_t header[8];
        header[0] = 0xAA;
        header[1] = 0xBB;
        header[2] = 0xCC;
        header[3] = 0xDD;
        
        // Frame size (big-endian 32-bit uint)
        header[4] = (fb->len >> 24) & 0xFF;
        header[5] = (fb->len >> 16) & 0xFF;
        header[6] = (fb->len >> 8) & 0xFF;
        header[7] = fb->len & 0xFF;
        
        // Write header to cellular module
        gsm.write(header, 8);
        
        // Write camera frame bytes directly
        gsm.write(fb->buf, fb->len);
      #else
        // Send HTTP / HTTPS POST request
        String httpHeader = "POST /upload HTTP/1.1\r\n";
        httpHeader += "Host: " + String(serverIp) + "\r\n";
        httpHeader += "Content-Type: image/jpeg\r\n";
        httpHeader += "Content-Length: " + String(fb->len) + "\r\n";
        httpHeader += "Connection: keep-alive\r\n\r\n";
        
        // Send HTTP headers
        gsm.print(httpHeader);
        
        // Send JPEG data
        gsm.write(fb->buf, fb->len);
      #endif
      
      esp_camera_fb_return(fb);
      
      // Calculate and display streaming stats in Serial Monitor
      uint32_t now = millis();
      float fps = 1000.0 / (now - lastFrameTime);
      lastFrameTime = now;
      Serial.printf("Frame sent: %d bytes | Est. FPS: %.2f\n", fb->len, fps);
      
      // Check for incoming data (disconnect signals or HTTP response)
      #if CONNECTION_MODE == 0
        if (gsm.available()) {
          String rx = "";
          while (gsm.available() && rx.length() < 128) {
            rx += (char)gsm.read();
          }
          while (gsm.available()) gsm.read();
          
          if (rx.indexOf("CLOSED") != -1 || rx.indexOf("CLOSE") != -1 || rx.indexOf("+IPCLOSE") != -1) {
            Serial.println("[GSM] Connection closed by remote server.");
            changeState(STATE_DISCONNECT, "Remote connection closed");
          }
        }
      #else
        // Read the HTTP response
        uint32_t respStart = millis();
        String rx = "";
        bool headerEnded = false;
        while (millis() - respStart < 2000) {
          if (gsm.available()) {
            char c = gsm.read();
            rx += c;
            if (rx.indexOf("\r\n\r\n") != -1) {
              headerEnded = true;
              break;
            }
          }
          delay(2);
        }
        
        // Let any remaining body bytes arrive
        delay(10);
        while (gsm.available()) {
          rx += (char)gsm.read();
        }
        
        if (rx.length() > 0) {
          if (rx.indexOf("CLOSED") != -1 || rx.indexOf("CLOSE") != -1 || rx.indexOf("+IPCLOSE") != -1 || rx.indexOf("400 Bad Request") != -1) {
            Serial.println("[GSM] Connection closed or bad response from server.");
            changeState(STATE_DISCONNECT, "Server connection closed or error");
          } else if (rx.indexOf("200 OK") == -1) {
            Serial.print("[GSM] Non-200 HTTP Response received: ");
            Serial.println(rx.substring(0, 100));
          }
        } else {
          Serial.println("[GSM] Timeout waiting for HTTP response");
          changeState(STATE_DISCONNECT, "HTTP timeout");
        }
      #endif
      break;
    }
    
    case STATE_DISCONNECT: {
      exitTransparentMode();
      
      #if CONNECTION_MODE == 2
        // Close SSL socket and stop CCH service
        sendATCommand("AT+CCHCLOSE=0", "OK", 3000);
        sendATCommand("AT+CCHSTOP", "OK", 3000);
      #else
        // Close TCP socket
        sendATCommand("AT+CIPCLOSE=0", "OK", 3000);
      #endif
      
      // Close network
      sendATCommand("AT+NETCLOSE", "OK", 5000);
      
      delay(2000);
      // Go back to INIT to fully re-initialize SSL service
      changeState(STATE_INIT, "Connection reset, re-initializing");
      break;
    }
  }
}