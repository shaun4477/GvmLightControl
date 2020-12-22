#ifdef ARDUINO_M5Stack_Core_ESP32
#include <M5Stack.h>
#elif defined(ARDUINO_M5Stick_C) 
#include <M5StickC.h>
#else 
#error "This code works on m5stick-c or m5stack core"
#endif

#include "WiFi.h"
#include <lwip/sockets.h>
#include <esp_wifi.h>
#include <lwip/netdb.h>
#include <errno.h>
#include <StreamString.h>

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

#define LIGHT_VAR_ONOFF      0
#define LIGHT_VAR_CHANNEL    1
#define LIGHT_VAR_BRIGHTNESS 2
#define LIGHT_VAR_CCT        3
#define LIGHT_VAR_HUE        4
#define LIGHT_VAR_SATURATION 5

#define LIGHT_MSG_SETVAR     0x57 // Send to set a variable 
#define LIGHT_MSG_VAR_SET    0x2  // Response to a variable set
#define LIGHT_MSG_VAR_ALL    0x3  // Periodic message with all variable settings    

int udp_2525_fd = -1;
int udp_1112_fd = -1;

inline int set_bounded(int val, int min, int max) {
  if (val < min)
    return max;
  else if (val > max)
    return min;
  return val;
}

struct { 
  int light_on;
  int channel;
  int hue;
  int brightness;
  int cct;
  int saturation;
} light_status = { -1, -1, -1, -1, -1, -1 };

// #define DEBUG

void setup() {
  Serial.begin(115200);
  Serial.println("M5 starting...\n");
  Serial.printf("Log level set to %d\n", ARDUHAL_LOG_LEVEL);

#ifdef DEBUG
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  esp_log_level_set("wifi", ESP_LOG_VERBOSE);
  esp_log_level_set("dhcpc", ESP_LOG_VERBOSE);
#endif

  // Initialize the M5 object. Do NOT reset the Serial object, for 
  // some reason if we do that after any ESP32 log function 
  // has been called all further log_e/log_d/log_i will stop working
  Serial.println("Initializing M5...");
  M5.begin(true, true, false);
  Serial.println("... done\n");
  
#ifdef ARDUINO_M5Stick_C  
  M5.Lcd.setRotation(3);
#endif

#ifdef DEBUG
  log_e("Testing ERROR");
  log_i("Testing INFO");
  log_d("Testing DEBUG");
#endif

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0, 2);

#ifdef ARDUINO_M5Stack_Core_ESP32
  M5.Lcd.setTextSize(2); // 15px
#endif 

#ifdef ARDUINO_M5Stack_Core_ESP32
  M5.Power.begin();
  Serial.printf("Currently charging %d\n", battery_power());
#endif

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
  Serial.printf("Resetting WiFi, current status %d (from core %d)\n", WiFi.status(), xPortGetCoreID());
  WiFi.disconnect(true, true); // Switch off WiFi and forget any AP config
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);  
  
  delay(100);
  
  Serial.printf("Resetting WiFi complete, current status %d\n", WiFi.status());  
  // It takes a little while to completely disconnect, if we don't do this 
  // the next connect may fail 
  delay(300);
}

static int disconnected = 0;

void WiFiEvents(WiFiEvent_t event, WiFiEventInfo_t info) {
  /* Not safe for writes from task callback 
  Serial.flush();
  Serial.printf("!! Got WiFi event %d\n", event);
  Serial.printf("From %d\n", xPortGetCoreID());
  Serial.flush();
  */
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  // Handling function code
  /* 
  Serial.flush();
  Serial.printf("!! Disconnected!!\n");
  Serial.printf("From %d\n", xPortGetCoreID());
  Serial.flush();
  */
  disconnected = 1;
}

int find_and_join_light_wifi(int *networks_found) {
  if (networks_found)
    *networks_found = 0;

  Serial.printf("Initializing WiFi\n");
  
  WiFi.mode(WIFI_MODE_STA);
  Serial.printf("Mode set to station\n");

  WiFi.onEvent(WiFiStationDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);
  // WiFi.onEvent(WiFiEvents, SYSTEM_EVENT_MAX);

  // First try to connect to any remembered AP
  wifi_config_t current_conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &current_conf) == ESP_OK) {
    if (!try_connect_wifi((char *) current_conf.sta.ssid, (char *) current_conf.sta.password, current_conf.sta.channel, current_conf.sta.bssid))
      return 0;  
  }
    
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
    Serial.printf("Found %d: %s (%d, %s, %d) %s\n", 
                  i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str(), WiFi.channel(i), 
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN?" ":"*");
                  
    if (strcmp(WiFi.SSID(i).c_str(), ssid))
      continue;

    (*networks_found)++;
    if (!try_connect_wifi(WiFi.SSID(i).c_str(), password, WiFi.channel(i), WiFi.BSSID(i)))
      return 0;
  }

  return -1;
}

int try_connect_wifi(const char *ssid, const char *password, int channel, uint8_t *bssid) {
  // We try connecting to each AP at least twice because 
  // sometimes the first connection fails for some strange
  // reason (error in the log: 'E (6714) wifi: Set status to INIT.')
  int connection_attempts = 0;

  while (connection_attempts++ < 2) {
    Serial.printf("Updating connection info on screen\n");

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);        
    M5.Lcd.printf("Trying\nSSID %s\nBSSID %02x:%02x:%02x:%02x:%02x:%02x\nAttempt %d\n", 
                  ssid, 
                  bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], 
                  connection_attempts);

    /*
    Serial.printf("Dropping any existing WiFi\n");
    clear_wifi();
    */
    
    // Connect to the Access Point
    Serial.printf("Trying to connect to %02x:%02x:%02x:%02x:%02x:%02x, current status %d\n", 
                  bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                  WiFi.status());      
                  
    disconnected = 0;
    WiFi.begin(ssid, password, channel, bssid);

    Serial.printf("Connection begun");
    
    // Setting the hostname
    // WiFi.setHostname("controller");

    int wait_tests = 35;
    while (WiFi.status() != WL_CONNECTED && !disconnected && wait_tests-- > 0) {
      Serial.printf("... WiFi status %d\n", WiFi.status());
      delay(100);
    }

    Serial.printf("Finished waiting, status %d disconnected %d tests remaining %d\n", WiFi.status(), disconnected, wait_tests);

    if (WiFi.status() == WL_CONNECTED)
      break;
      
    Serial.printf("Connect timed out, status %d\n", WiFi.status());
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Connect failed, status %d, moving on\n", WiFi.status());
    return -1;
  }
  
  if (!test_light_connection()) {
    Serial.println("Connected");
    return 0;
  }

  return -1;
}

int test_light_connection() {
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
  
  int waits = 60;
  while (waits-- >= 0) {
    if (read_udp(udp_1112_fd)) {
      Serial.println("Received light message, proceeding");
      return 0;
    }
    delay(20); 
  }

  return -1;
}

int broadcast_udp(const void *d, int len) {
  struct sockaddr_in broadcast_addr;
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(2525);
  broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;    
  return sendto(udp_2525_fd, d, len, 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr));   
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
                        
      if (hexDecode[5] == LIGHT_MSG_VAR_ALL) {
        /* Status message sent periodically by the lights */
        /* Save state */
        light_status.light_on   = hexDecode[6]; // 0 if light is currently 'soft' off, 1 if it's on
        light_status.channel    = hexDecode[7];
        light_status.brightness = hexDecode[8];
        light_status.cct        = hexDecode[9];
        light_status.hue        = hexDecode[10];
        light_status.saturation = hexDecode[11];
        Serial.printf("  Status Message: Light On %d Channel %d Brightness %d%% CCT %d Hue %d Saturation %d\n",
                      hexDecode[6], hexDecode[7] - 1, hexDecode[8], hexDecode[9] * 100, hexDecode[10] * 5, hexDecode[11]);
        update_screen_status();
      } else if (hexDecode[5] == LIGHT_MSG_VAR_SET) {
        /* Updated message, send in response to an update message 
        e.g '4C54080030020002003A89' received from sending a brightness zero message '4C5409003057000201005C9E'
        or  '4C54080030020002030AEA' received from sending a brightness 3% message   '4C5409003057000201036CFD' */
        Serial.printf("  Updated Message: Unknown 1 %d Field %d Value %d\n", 
                      hexDecode[6], hexDecode[7], hexDecode[8]);
        switch (hexDecode[7]) {
          case LIGHT_VAR_ONOFF:
            light_status.light_on = hexDecode[8];
            break;
          case LIGHT_VAR_CHANNEL:
            light_status.channel = hexDecode[8];
            break;
          case LIGHT_VAR_BRIGHTNESS:
            light_status.brightness = hexDecode[8];
            break;
          case LIGHT_VAR_CCT:
            light_status.cct = hexDecode[8];
            break;
          case LIGHT_VAR_HUE:
            light_status.hue = hexDecode[8];
            break;
          case LIGHT_VAR_SATURATION:
            light_status.saturation = hexDecode[8];
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
        Serial.printf("First bytes of potential next message = %02x %02x %02x\n", hexDecode[0], hexDecode[1], hexDecode[2]);
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
  int rc = broadcast_udp(first_connect, strlen(first_connect));

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
  cmd_buffer[5] = LIGHT_MSG_SETVAR;
  cmd_buffer[6] = 0x0;
  cmd_buffer[7] = setting;
  cmd_buffer[8] = 0x1;
  cmd_buffer[9] = value;

  bytesToHexString(cmd_buffer, sizeof(cmd_buffer) - 2, encoded_cmd_buffer);
  unsigned short crc = calcCrcFromHexStr(encoded_cmd_buffer, (sizeof(cmd_buffer) - 2) * 2);
  shortToHex(crc, encoded_cmd_buffer + sizeof(encoded_cmd_buffer) - 4);
  
  Serial.printf("Sending command with len %d, '%.*s'\n", sizeof(encoded_cmd_buffer), sizeof(encoded_cmd_buffer), encoded_cmd_buffer);

  broadcast_udp(encoded_cmd_buffer, sizeof(encoded_cmd_buffer));
  return 0;
}

int screen_mode = -1;

float getBatteryLevel() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  return (float) M5.Power.getBatteryLevel() / 100.0f;
#else  
  return getStickBatteryLevel(M5.Axp.GetBatVoltage());
#endif
}

void update_screen_status() {
  StreamString o;
  static String lastUpdate;

  switch (screen_mode) {
    case -1:
      o.printf("BS: %s\n", WiFi.BSSIDstr().c_str());
      if (light_status.light_on != -1) 
        o.printf("Light On %d\n", light_status.light_on);
      if (light_status.hue != -1) 
        o.printf("Hue %d ", light_status.hue * 5);
      if (light_status.brightness != -1) 
        o.printf("Bright. %d", light_status.brightness);
      o.println();
      if (light_status.cct != -1) 
        o.printf("CCT %d ", light_status.cct * 100);
      if (light_status.saturation != -1) 
        o.printf("Sat. %d", light_status.saturation);
      o.println(); 
      o.printf("Battery %0.1f %%\n", getBatteryLevel() * 100);
      break;
    case LIGHT_VAR_ONOFF:   
      if (light_status.light_on != -1) 
        o.printf("Light On\n%d", light_status.light_on);
      break;
    case LIGHT_VAR_CHANNEL:
      if (light_status.channel != -1) 
        o.printf("Channel\n%d", light_status.channel - 1);
      break;    
    case LIGHT_VAR_BRIGHTNESS:
      if (light_status.brightness != -1) 
        o.printf("Brightness\n%d%%", light_status.brightness);
      break;    
    case LIGHT_VAR_CCT:
      if (light_status.cct != -1) 
        o.printf("CCT\n%d", light_status.cct * 100);
      break;    
    case LIGHT_VAR_HUE:
      if (light_status.hue != -1) 
        o.printf("Hue\n%d", light_status.hue * 5);
      break;    
    case LIGHT_VAR_SATURATION: 
      if (light_status.saturation != -1) 
        o.printf("Saturation\n%d%%", light_status.saturation);
      break;    
  }

  if (lastUpdate != o) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);
    M5.Lcd.setTextFont(screen_mode == -1 ? 2 : 4);
    M5.Lcd.print(o);    
  }
  
  lastUpdate = o;
}

void screen_off() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  M5.Lcd.setBrightness(0);
#else
  M5.Axp.SetLDO2(false);  
#endif
}

void screen_on() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  M5.Lcd.setBrightness(255);
#else  
  // Turn on tft backlight voltage output
  M5.Axp.SetLDO2(true);  
#endif
}

int down_pressed() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  return M5.BtnA.wasPressed();
#else
  // 0x01 long press(1s), 0x02 short press
  return M5.Axp.GetBtnPress() == 0x02;
#endif  
}

int up_pressed() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  return M5.BtnC.wasPressed();
#else
  // 0x01 long press(1s), 0x02 short press
  return M5.BtnB.wasPressed();
#endif    
}

int home_pressed() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  return M5.BtnB.wasPressed();
#else
  return M5.BtnA.wasPressed();
#endif
}

int battery_power() {
#ifdef ARDUINO_M5Stack_Core_ESP32
  return !M5.Power.isCharging();
#else 
  return !M5.Axp.GetIusbinData();
#endif  
}

void test_screen_idle_off() {
  if (millis() - last_button_millis > 10000 && battery_power() && !lcd_off) {
    screen_off();
    lcd_off = 1;
    Serial.printf("Battery %% is %f\n", getBatteryLevel());
  }  
}

int button_screen_on() {
  if (lcd_off) {
    screen_on();
    lcd_off = 0;
    return 1;
  }
  return 0;
}

void loop() {
  read_udp(udp_1112_fd);
  read_udp(udp_2525_fd);

  int button_pressed = 0xff;

  test_screen_idle_off();

  // Read buttons before processing state
  M5.update();
  
  if (home_pressed()) {
    button_pressed = 0;
    Serial.println("Home button pressed");
    Serial.printf("Battery %% is %f\n", getBatteryLevel());
  }

  if (down_pressed()) {
    button_pressed = -1;
    Serial.println("Power button pressed");
  }

  if (up_pressed()) {
    button_pressed = 1;
    Serial.println("Side button pressed");
    if (screen_mode == -1) 
      send_hello_msg();
  }

  if (button_pressed != 0xff) {
    last_button_millis = millis();
    if (!button_screen_on()) {
      int change = button_pressed;
      if (button_pressed == 0) {
        screen_mode++;
        if (screen_mode > 5)
          screen_mode = -1;
        update_screen_status();          
      } else if (change && screen_mode != -1) {
        int8_t newVal;
        switch (screen_mode) {
          case LIGHT_VAR_ONOFF:   
            newVal = light_status.light_on ? 0 : 1;
            /* Optimistic update */
            light_status.light_on = newVal;
            break;
          case LIGHT_VAR_CHANNEL:   
            newVal = set_bounded(light_status.channel + change, 1, 12);
            /* Optimistic update */
            light_status.channel = newVal - 1;
            break;
          case LIGHT_VAR_BRIGHTNESS:
            newVal = set_bounded(light_status.brightness + change, 0, 100);
            /* Optimistic update */
            light_status.brightness = newVal;
            break;    
          case LIGHT_VAR_CCT:
            newVal = set_bounded(light_status.cct + change, 32, 56); 
            /* Optimistic update */
            light_status.cct = newVal;
            break;    
          case LIGHT_VAR_HUE:
            newVal = set_bounded(light_status.hue + change, 0, 72);
            /* Optimistic update */
            light_status.hue = newVal;
            break;    
          case LIGHT_VAR_SATURATION: 
            newVal = set_bounded(light_status.saturation + change, 0, 100);
            /* Optimistic update */
            light_status.saturation = newVal;
            break;          
        }
        
        send_set_cmd(screen_mode, newVal);
    
        /* When switching the light off sometimes we don't get a response 
         *  message. Send a hello message as well so we we'll get a status
         *  message to process the change */
        /* Sometimes when messages are sent too quickly we don't get a 
         *  'setting updated' message from the light. Send a hello to get 
         *  a full setting update as well
         */
        // if (screen_mode == 0 && newVal == 0)
          send_hello_msg();
      }
    }
  }



  if (Serial.available()) {
    char inChar = Serial.read();
    switch (inChar) {
      case 'o': {
        int rc = send_set_cmd(LIGHT_VAR_ONOFF, 1);
        Serial.print("Send on ");
        Serial.println(rc);
        break;
      }
      case 'O': {
        int rc = send_set_cmd(LIGHT_VAR_ONOFF, 0);
        Serial.print("Send off ");
        Serial.println(rc);
        break;
      }
      case 'b': {
        int rc = send_set_cmd(LIGHT_VAR_BRIGHTNESS, 11);
        Serial.print("Send bright 11 ");
        Serial.println(rc);
        break;   
      }
      case 'r': {
        String toSend = Serial.readStringUntil(';');
        int rc = broadcast_udp(toSend.c_str(), toSend.length());
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
        int rc = broadcast_udp(toSend.c_str(), toSend.length());
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
