#include "WiFi.h"
#include <M5StickC.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>

struct sockaddr_in broadcast_addr;

const char* ssid = "GVM_LED";
const char* password =  "gvm_admin";

int lcd_off = 0;
unsigned long last_button_millis = 0;

/* Messages are sent to the lights with UDP broadcast to 255.255.255.255:2525.
 * Messages are received from the lights with UDP broadcast to 255.255.255.255:1112
 * 
 * Messages are sent as a hexadecimal string and need to be converted to bytes first. 
 * 
 * The decoded messages have a 3 byte header and a 2 byte CRC at the end. The header
 * consists of 'LT' then a byte indicating the payload length (excluding the 3 byte 
 * header but including the 2 byte CRC at the end). The CRC is CRC-16/XMODEM.  
 * 
 * In some cases many messages can be received in a single UDP datagram
 *
 * When the app first connects it broadcasts this message. This 
 * seems to cause the light to broadcast its status every 5 
 * seconds: '4C5409000053000001009474'
 * 
 * Message format for sending to set a value is:
 *  '4C54' - '\x4C\x54' = 'LT'
 *  '09' - Size of following hexadecimal payload, i.e. 9 bytes encoded as hex string, including CRC excluding the 3 header bytes
 *  '00' - Device ID
 *  '30' - Device Type
 *  '57' - Message Type, '\x57' = 87 = 'W', seems to indicate set value
 *  '00' 
 *  '02' - Command - 2 is brightness, 3 is CCT, 4 is hue, 5 is saturation 
 *  '01' 
 *  '00' - Command arguments, e.g. 0% brightness
 *  '5C9E' - 2 byte CRC-16/XMODEM
 *  
 * Example for Brightness = 0% =    '4C5409003057000201005C9E' (0x00 = 0)
 * Example for Brightness = 3% =    '4C5409003057000201036CFD' (0x03 = 3)
 * Example for Brightness = 50% =   '4C5409003057000201324A8F' (0x32 = 50)
 * Example for Brightness = 100% =  '4C5409003057000201644C1E' (0x64 = 100)
 * Example for CCT = 3200k        = '4C5409003057000301204FCC' (0x20 * 1000 = 32k) 
 * Example for CCT = 4400k        = '4C54090030570003012C8E40' (0x2C * 1000 = 44k) 
 * Example for CCT = 5600k        = '4C540900305700030138DCF5' (0x38 * 1000 = 56k) 
 * Example for Hue = 0 degrees    = '4C540900305700040100EE3E' (0x0 x 5 = 0)
 * Example for Hue = 5 degrees    = '4C540900305700040101FE1F' (0x1 x 5 = 5)
 * Example for Hue = 280 degrees  = '4C5409003057000401385965' (0x38 x 5 = 280)
 * Example for Hue = 300 degrees  = '4C54090030570004013C19E1' (0x3c x 5 = 300)
 * Example for Saturation = 0%    = '4C540900305700050100D90E' (0x0 = 0)
 * Example for Saturation = 25%   = '4C5409003057000501195A16' (0x19 = 25)
 * Example for Saturation = 100%  = '4C540900305700050164F52C' (0x64 = 100)
 * 
 * The device broadcasts its status every 5 seconds to port 1112 at 255.255.255.255 
 * (from port 2525), e.g '4C540B0030030102322C3819D268' 
 * '4C54'
 * '0B' - Length of message payload in bytes after hex decode
 * '00' - Device ID
 * '30' - Device Type
 * '03' - Message type (3 == Status Report) 
 * '01' - Unk 1
 * '02' - Channel (01 = 0, 02 = 1, 03 = 2, 08 = 7)
 * '32' - 0x32 = 50% Brightness 
 * '2C' - 0x2c = 44 = 4400k CCT
 * '38' - 0x38 = 56 x 5 = 280 Degree Hue
 * '19' - 0x19 = 25% Saturation
 * 'D268' - CRC-16/XMODEM checksum
 * '4C540B00300301080220485FED15
 */

const char* first_connect = "4C5409000053000001009474";
/* Response msg sometimes   "4C540A00305300000220382B19"  */
 
const char* off_cmd =   "4C54090030570000010032FE"; // Turn light off
const char* on_cmd =    "4C54090030570000010122DF"; // Turn light on
const char* bright_0 =  "4C5409003057000201005C9E"; // Brightness zero 
const char* bright_11 = "4C54090030570002010BEDF5"; // Brightness to 11%
const char* bright_1  = "4C5409003057000201014CBF"; // Brightness to 1%

int udp_2525_fd = -1;
int udp_1112_fd = -1;

int light_on = -1;
int channel = -1;
int hue = -1;
int brightness = -1;
int cct = -1;
int saturation = -1;

void setup() {
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(2525);
  broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;  

  Serial.begin(115200);

#if DEBUG
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  esp_log_level_set("wifi", ESP_LOG_VERBOSE);
  esp_log_level_set("dhcpc", ESP_LOG_VERBOSE);
#endif
  
  // Initialize the M5StickC object
  M5.begin();
  Wire.begin(0,26);
  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0, 2);

  // scan_wifi_networks();

  int networks_found = 0;
  while (find_and_join_light_wifi(&networks_found)) {
    if (networks_found) {
      Serial.println("Couldn't connect to any light, trying again");
      delay(1000);
    } else { 
      Serial.println("No lights found, trying again in 20 seconds");
      delay(20000);
    }
  }

  // Write info in serial logs.
  Serial.print("Connected to the WiFi network. IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Base station is: %s\n", WiFi.BSSIDstr().c_str());
  Serial.printf("Receive strength is: %d\n", WiFi.RSSI());

  // Write info to LCD.
  update_screen_status();

  last_button_millis = millis();
}

// Show all networks available. Technically the doc says this can 
// only be called once you're connected, but that doesn't seem to be
// true
void scan_wifi_networks() {
  // WiFi.scanNetworks will return the number of networks found
  int n = WiFi.scanNetworks();
  Serial.println("Scan done");
  if (n == 0) {
      Serial.println("No networks found");
  } else {
      Serial.printf("%d networks found", n);
      for (int i = 0; i < n; ++i) {
          // Print SSID and RSSI for each network found
          Serial.printf("%d: %s (%d, %s, %d) %s\n", 
                        i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str(),WiFi.channel(i), 
                        WiFi.encryptionType(i) == WIFI_AUTH_OPEN?" ":"*");
          delay(10);
      }
  }
  Serial.println("");  
}

void clear_wifi() {
  Serial.printf("Resetting WiFi, current status %d\n", WiFi.status());
  WiFi.disconnect(true, true); // Switch off WiFi and forget any AP config
  WiFi.mode(WIFI_STA);  
  delay(500);
  
  Serial.printf("Resetting WiFi complete, current status %d\n", WiFi.status());  
  // It takes a little while to completely disconnect, if we don't do this 
  // the next connect may fail 
  delay(500);
}

int find_and_join_light_wifi(int *networks_found) {
  if (networks_found)
    *networks_found = 0;
    
  // Switch off WiFi and forget any prior AP config
  clear_wifi();
    
  // WiFi.scanNetworks will return the number of networks found
  int n = WiFi.scanNetworks();
  if (n == 0) {
      Serial.println("No networks found");
      return -1;
  }

  Serial.printf("%d networks available\n", n);
  for (int i = 0; i < n; ++i) {
      // Print SSID and RSSI for each network found
      Serial.printf("%d: %s (%d, %s, %d) %s\n", 
                    i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str(),WiFi.channel(i), 
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN?" ":"*");
                    
      if (strcmp(WiFi.SSID(i).c_str(), ssid))
        continue;

      (*networks_found)++;

      // We try connecting to each AP at least twice because 
      // sometimes the first connection fails for some strange
      // reason (error in the log: 'E (6714) wifi: Set status to INIT.')
      int connection_attempts = 0;

      while (connection_attempts++ < 2) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.printf("Trying\nSSID %s\nBSSID %s\nAttempt %d\n", 
                      WiFi.SSID(i).c_str(), WiFi.BSSIDstr(i).c_str(), connection_attempts);

        clear_wifi();
        
        // Connect to the Access Point
        Serial.printf("Trying to connect to %s\n", WiFi.BSSIDstr(i).c_str());      
        WiFi.begin(ssid, password, WiFi.channel(i), WiFi.BSSID(i));
        
        // Setting the hostname
        // WiFi.setHostname("controller");
  
        int wait_tests = 10;
        while (WiFi.status() != WL_CONNECTED && wait_tests-- > 0) {
          Serial.printf("... WiFi status %d\n", WiFi.status());
          delay(500);
        }
        
        if (!wait_tests)  {
          Serial.printf("Connect timed out, status %d\n", WiFi.status());
          continue;
        }
      }

      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("Connect failed, status %d, moving on\n", WiFi.status());
        continue;
      }
      
      Serial.print("Connected to the WiFi network. IP: ");
      Serial.println(WiFi.localIP());
      Serial.printf("Base station is: %s\n", WiFi.BSSIDstr().c_str());
      Serial.printf("Receive strength is: %d\n", WiFi.RSSI());

      // Listen on any incoming IP address for UDP port 2525
      // udp.begin(2525);
      open_udp_port(&udp_2525_fd, 2525);
      Serial.printf("Listening port 2525 with FD %d\n", udp_2525_fd);

      open_udp_port(&udp_1112_fd, 1112);
      Serial.printf("Listening port 1112 with FD %d\n", udp_1112_fd);

      // Broadcast the starting message to ask the light(s) to report 
      Serial.println("Broadcasting first connect message\n");
      send_hello_msg();

      Serial.println("Waiting for light message");
      
      int waits = 100;
      while (waits-- >= 0) {
        if (read_udp(udp_1112_fd)) {
          Serial.println("Received light message, proceeding");
          return 0;
        }
        delay(20); 
      }

      Serial.println("No packet received, moving on to other networks");  
       
      // Switch off WiFi and forget any prior AP config
      clear_wifi();                     
      delay(1000);
  }  
  
  return -1;
}

int open_udp_port(int *fd, int port) {
  if (*fd != -1)
    close(*fd);
  *fd = socket(AF_INET, SOCK_DGRAM, 0);

  int yes = 1;
  setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  
  struct sockaddr_in addr;
  memset((char *) &addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(*fd , (struct sockaddr*) &addr, sizeof(addr));

  setsockopt(*fd, SOL_SOCKET, SO_BROADCAST,
             &yes, sizeof(yes));

  // Set non blocking 
  fcntl(*fd, F_SETFL, O_NONBLOCK);

  return *fd; 
}

// CRC-16/XMODEM, see https://crccalc.com/ or https://www.tahapaksu.com/crc/
uint16_t calcCrcFromHexStr(const char *str, int len) {
  uint16_t crc = 0;
  unsigned char c;
  while (len >= 2) {
    // Serial.printf("CRC calc %c%c = ", *(str), *(str + 1));
    c = (charToVal(*str++) << 4) + charToVal(*str++);
    crc ^= c << 8;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc = crc << 1;
    }   
    len -= 2; 
    // Serial.printf("%x CRC = %d\n", c, crc);
  }
  return crc & 0xFFFF;  
}


int read_udp(int fd) {
  int msgs_processed = 0;
  int rx_len;
  int rx_from_size;
  struct sockaddr_in rx_from;
  unsigned char rx_buffer[2048] = {'\0'}; 
  
  rx_from_size = sizeof(rx_from);
  memset((char *) &rx_from, 0, sizeof(rx_from));
  
  unsigned char decodeBuffer[sizeof(rx_buffer) / 2] = {'\0'}; 
  unsigned char *msgDecode = rx_buffer;
  unsigned char *hexDecode = decodeBuffer;

  // Serial.printf("Reading %d\n",fd);
  
  while ((rx_len = recvfrom(fd, rx_buffer, sizeof(rx_buffer) - 1, 0, (sockaddr *) &rx_from, (socklen_t *) &rx_from_size)) > -1) {
    rx_buffer[rx_len] = '\0';
    Serial.printf("Received packet on FD %d\n", fd);
    Serial.printf("From %d.%d.%d.%d:%d\n", 
                  *(((uint8_t *) &rx_from.sin_addr) + 0),
                  *(((uint8_t *) &rx_from.sin_addr) + 1),
                  *(((uint8_t *) &rx_from.sin_addr) + 2),
                  *(((uint8_t *) &rx_from.sin_addr) + 3),
                  ntohs(rx_from.sin_port));

    // serialPrintAsHex((char *) &rx_from, rx_from_size, "Address: ");
    serialPrintAsHex((char *) rx_buffer, rx_len, "Message: ");
    
    // The message from the GVM lights is bytes encoded as a hex string, decode first 
    hexStringToBytes((char *) rx_buffer, rx_len, (unsigned char *) decodeBuffer);

    hexDecode = decodeBuffer;
    msgDecode = rx_buffer; 

    while (rx_len >= (3 * 2) && 
           hexDecode[0] == 'L' /* 0x4C */ && 
           hexDecode[1] == 'T' /* 0x54 */ && 
           hexDecode[2] <= rx_len / 2 - 3) {
      int msglen = 3 + hexDecode[2];
      
      /* Might be a GVM light message, check the CRC */
      Serial.printf("Calc CRC for message of length %d == %d\n", hexDecode[2], (2 * (3 + hexDecode[2] - 2)));
      unsigned short crc = calcCrcFromHexStr((char *) msgDecode, (2 * (3 + hexDecode[2] - 2)));
      unsigned short msgcrc = ntohs(*((unsigned short *) &hexDecode[3 + hexDecode[2] - 2]));

      /* If this is not a light message stop decoding */
      if (crc != msgcrc) {
        Serial.printf("CRC not correct, %d vs %d\n", crc, msgcrc);
        break;
      }

      msgs_processed++;
      
      Serial.printf("GVM light message with %d bytes of payload\n", hexDecode[2]);
      Serial.printf("  Length %d\n  Device ID %d\n  Device Type 0x%x\n  Message Type %d\n",
                    hexDecode[2], hexDecode[3], hexDecode[4], hexDecode[5]);
                        
      if (hexDecode[5] == 3) {
        /* Status message sent periodically by the lights */
        /* Save state */
        light_on = hexDecode[6]; // 0 if light is currently 'soft' off, 1 if it's on
        channel = hexDecode[7] - 1;
        brightness = hexDecode[8];
        cct = hexDecode[9] * 100;
        hue = hexDecode[10] * 5;
        saturation = hexDecode[11];
        Serial.printf("  Status Message: Light On %d Channel %d Brightness %d%% CCT %d Hue %d Saturation %d\n",
                      hexDecode[6], hexDecode[7] - 1, hexDecode[8], hexDecode[9] * 100, hexDecode[10] * 5, hexDecode[11]);
        update_screen_status();
      } else if (hexDecode[5] == 2) {
        /* Updated message, send in response to an update message 
        e.g '4C54080030020002003A89' received from sending a brightness zero message '4C5409003057000201005C9E'
        or  '4C54080030020002030AEA' received from sending a brightness 3% message   '4C5409003057000201036CFD' */
        Serial.printf("  Updated Message: Unknown 1 %d Field %d Value %d\n", 
                      hexDecode[6], hexDecode[7], hexDecode[8]);
        switch (hexDecode[7]) {
          case 0:
            light_on = hexDecode[8];
            break;
          case 2:
            brightness = hexDecode[8];
            break;
          case 3:
            cct = hexDecode[8] * 100;
            break;
          case 4:
            hue = hexDecode[8] * 5;
            break;
          case 5:
            saturation = hexDecode[8];
            break;            
        }
        update_screen_status();        
      } else {
        Serial.printf("  Unknown Payload: %.*s\n", (hexDecode[2] - 3 - 2) * 2, msgDecode + ((3 + 3) * 2)); // FIX not being at start of rxbuffer        
      }

      rx_len -= msglen * 2;
      hexDecode += msglen;
      msgDecode += msglen * 2;
      Serial.printf("RX left %d\n", rx_len);
      if (rx_len >= 6)
        Serial.printf("%x %x %x\n", hexDecode[0], hexDecode[1], hexDecode[2]);
    }
  }

  return msgs_processed;
}

/* This message causes the light to respond with a 0x53 message then 
 * send a 0x03 status message. If the light is on the status messages
 * will continue to be sent every 5 seconds 
 */
int send_hello_msg() {
  if (udp_2525_fd == -1)
    return -1;

  Serial.printf("Sending hello msg, '%.*s'\n", strlen(first_connect), first_connect);
  int rc = sendto(udp_2525_fd, first_connect, strlen(first_connect), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 

  return 0;
}

int send_set_cmd(uint8_t setting, uint8_t value) {
  if (udp_2525_fd == -1)
    return -1;

  unsigned char cmd_buffer[3 + 3 + 4 + 2];
  char encoded_cmd_buffer[sizeof(cmd_buffer) * 2]; 

  /* Example to turn light off '4C5409003057000201005C9E' */
  cmd_buffer[0] = 'L';
  cmd_buffer[1] = 'T';
  cmd_buffer[2] = sizeof(cmd_buffer) - 3;
  cmd_buffer[3] = 0x0;
  cmd_buffer[4] = 0x30;
  cmd_buffer[5] = 0x57;
  cmd_buffer[6] = 0x0;
  cmd_buffer[7] = setting;
  cmd_buffer[8] = 0x1;
  cmd_buffer[9] = value;

  bytesToHexString(cmd_buffer, sizeof(cmd_buffer) - 2, encoded_cmd_buffer);
  unsigned short crc = calcCrcFromHexStr(encoded_cmd_buffer, (sizeof(cmd_buffer) - 2) * 2);
  shortToHex(crc, encoded_cmd_buffer + sizeof(encoded_cmd_buffer) - 4);
  
  Serial.printf("Sending command with len %d, '%.*s'\n", sizeof(encoded_cmd_buffer), sizeof(encoded_cmd_buffer), encoded_cmd_buffer);
  
  sendto(udp_2525_fd, encoded_cmd_buffer, sizeof(encoded_cmd_buffer), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 
  return 0;
}

int set_mode = -1;

void update_screen_status() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0, 2);
  switch (set_mode) {
    case -1:
      M5.Lcd.printf("BS: %s\n", WiFi.BSSIDstr().c_str());
      if (light_on != -1) 
        M5.Lcd.printf("Light On %d\n", light_on);
      if (hue != -1) 
        M5.Lcd.printf("Hue %d ", hue);
      if (brightness != -1) 
        M5.Lcd.printf("Bright. %d", brightness);
      M5.Lcd.println();
      if (cct != -1) 
        M5.Lcd.printf("CCT %d ", cct);
      if (saturation != -1) 
        M5.Lcd.printf("Sat. %d", saturation);
      M5.Lcd.println(); 
      M5.Lcd.printf("Battery %0.1f %%\n", getBatteryLevel(M5.Axp.GetBatVoltage()) * 100);
      break;
    case 0:   
      if (light_on != -1) 
        M5.Lcd.printf("Light On %d\n", light_on);
      break;
    case 1:
      if (channel != -1) 
        M5.Lcd.printf("Channel %d", channel);
      break;    
    case 2:
      if (brightness != -1) 
        M5.Lcd.printf("Bright. %d", brightness);
      break;    
    case 3:
      if (cct != -1) 
        M5.Lcd.printf("CCT %d", cct);
      break;    
    case 4:
      if (hue != -1) 
        M5.Lcd.printf("Hue %d", hue);
      break;    
    case 5: 
      if (saturation != -1) 
        M5.Lcd.printf("Saturation %d", saturation);
      break;    
  }
  // M5.Lcd.print("IP: ");
  // M5.Lcd.println(WiFi.localIP());
}

void loop() {
  read_udp(udp_1112_fd);
  read_udp(udp_2525_fd);

  int button_pressed = 0xff;

  if (millis() - last_button_millis > 10000 && !lcd_off) {
    lcd_off = 1;
    M5.Axp.SetLDO2(false);
    Serial.printf("Battery %% is %f\n", getBatteryLevel(M5.Axp.GetBatVoltage()));
  }
  
  M5.BtnA.read();
  if (M5.BtnA.wasPressed()) {
    button_pressed = 0;
    Serial.println("Home button pressed");
    Serial.printf("Battery %% is %f\n", getBatteryLevel(M5.Axp.GetBatVoltage()));
  }

  // 0x01 long press(1s), 0x02 short press
  if (M5.Axp.GetBtnPress() == 0x02) {
    button_pressed = -1;
    Serial.println("Power button pressed");
  }

  M5.BtnB.read();
  if (M5.BtnB.wasPressed()) {
    button_pressed = 1;
    Serial.println("Side button pressed");
    if (set_mode == -1) 
      send_hello_msg();
  }

  if (button_pressed != 0xff) {
    last_button_millis = millis();
    if (lcd_off) {
      // Turn on tft backlight voltage output
      M5.Axp.SetLDO2(true);
      lcd_off = 0;
    } else {
      int change = button_pressed;
      if (button_pressed == 0) {
        set_mode++;
        if (set_mode > 5)
          set_mode = -1;
        update_screen_status();          
      } else if (change && set_mode != -1) {
        int8_t newVal;
        switch (set_mode) {
          case 0:   
            newVal = light_on ? 0 : 1;
            /* Optimistic update */
            light_on = newVal;
            break;
          case 1:   
            newVal = (channel + 1) + change;
            /* Optimistic update */
            if (newVal > 12)
              newVal = 1;
            else if (newVal < 1)
              newVal = 12;
            /* Optimistic update */
            channel = newVal - 1;
            break;
          case 2:
            newVal = brightness + change;
            if (newVal > 100)
              newVal = 0;
            else if (newVal < 0)
              newVal = 100;
            /* Optimistic update */
            brightness = newVal;
            break;    
          case 3:
            newVal = (cct / 100) + change; 
            if (newVal < 32)
              newVal = 56;
            else if (newVal > 56)
              newVal = 32; 
            /* Optimistic update */
            cct = newVal * 100;
            break;    
          case 4:
            newVal = hue / 5 + change;
            if (newVal < 0)
              newVal = 72;
            else if (newVal > 72)
              newVal = 0; 
            /* Optimistic update */
            hue = newVal * 5;
            break;    
          case 5: 
            newVal = saturation + change;
            if (newVal > 100)
              newVal = 0;
            else if (newVal < 0)
              newVal = 100;
            /* Optimistic update */
            saturation = newVal;
            break;          
        }
        
        send_set_cmd(set_mode, newVal);
    
        /* When switching the light off sometimes we don't get a response 
         *  message. Send a hello message as well so we we'll get a status
         *  message to process the change */
        /* Sometimes when messages are sent too quickly we don't get a 
         *  'setting updated' message from the light. Send a hello to get 
         *  a full setting update as well
         */
        // if (set_mode == 0 && newVal == 0)
          send_hello_msg();
      }
    }
  }



  if (Serial.available()) {
    char inChar = Serial.read();
    switch (inChar) {
      case 'o': {
        int rc = sendto(udp_2525_fd, on_cmd, strlen(on_cmd), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 
        Serial.print("Send on ");
        Serial.println(rc);
        break;
      }
      case 'O': {
        int rc = sendto(udp_2525_fd, off_cmd, strlen(off_cmd), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 
        Serial.print("Send off ");
        Serial.println(rc);
        break;
      }
      case 'b': {
        int rc = sendto(udp_2525_fd, bright_11, strlen(bright_11), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 
        Serial.print("Send bright 11 ");
        Serial.println(rc);
        break;   
      }
      case 'r': {
        String toSend = Serial.readStringUntil(';');
        int rc = sendto(udp_2525_fd, toSend.c_str(), toSend.length(), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 
        Serial.printf("Send %d '%s' %d\n", toSend.length(), toSend.c_str(), rc);
        break;   
      }
      case 'R': {
        String toSend = Serial.readStringUntil(';');
        Serial.printf("Send with CRC length %d\n", toSend.length());
        unsigned short crc = calcCrcFromHexStr(toSend.c_str(), toSend.length());
        char crc_str[5];
        shortToHex(crc, crc_str);
        crc_str[4] = '\0';
        toSend += crc_str;
        int rc = sendto(udp_2525_fd, toSend.c_str(), toSend.length(), 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr)); 
        Serial.printf("Send %d '%s' %d\n", toSend.length(), toSend.c_str(), rc);
        break;   
      }      
      case 'c': {
        String toCalc = Serial.readStringUntil(';');
        Serial.printf("Calc %d %s %d\n", toCalc.length(), toCalc.c_str(), calcCrcFromHexStr(toCalc.c_str(), toCalc.length()));
        break;   
      }      
      case 'X': {
        Serial.println("Restarting");
        ESP.restart();
      }
    }
  }

  fd_set readSet;
  struct timeval t;
  t.tv_sec = 0;
  t.tv_usec = 10000;
  FD_SET(udp_1112_fd, &readSet);
  select(udp_1112_fd + 1, &readSet, NULL, NULL, &t);  
}
