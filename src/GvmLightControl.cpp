#include "WiFi.h"
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>
#include <StreamString.h>
#include <esp_vfs_dev.h>
#include "util/HexFunctions.h"
#include "GvmLightControl.h"


#ifdef IDF_VER
// If compiled using ESP IDF with a recent release we use the new enum for WiFi events
#define DISC_EVENT ARDUINO_EVENT_WIFI_STA_DISCONNECTED
#else 
// If compiled using Arduino IDE we need to use the old define 
#define DISC_EVENT SYSTEM_EVENT_STA_DISCONNECTED
#endif 

static int debugMsgs = 0;
#define DEBUG(format, ...) if (debugMsgs) { log_printf(format, ##__VA_ARGS__); };

static int open_udp_port(int *fd, int port);

const char* ssid = "GVM_LED";
const char* password =  "gvm_admin";

const char* first_connect = "4C5409000053000001009474";
/* Response msg sometimes   "4C540A00305300000220382B19"  */

static int disconnected = 0;

inline int set_bounded(int val, int min, int max) {
  if (val < min)
    return max;
  else if (val > max)
    return min;
  return val;
}

GvmLightControl::GvmLightControl(bool debug) {
  udp_2525_fd = -1;
  udp_1112_fd = -1;
  onWiFiConnectAttempt = NULL;
  onStatusUpdated = NULL;
  if (debug) 
    debugOn();
}

void GvmLightControl::debugOn() {
  debugMsgs = 1;
}

void GvmLightControl::callbackOnWiFiConnectAttempt(void (*callback)(uint8_t *bssid, int attempt)) {
  onWiFiConnectAttempt = callback;
}

void GvmLightControl::callbackOnStatusUpdated(void (*callback)()) {
  onStatusUpdated = callback;
}

// Show all networks available. Technically the doc says this can 
// only be called once you're connected, but that doesn't seem to be
// true
static void scan_wifi_networks() {
  // WiFi.scanNetworks will return the number of networks found
  int n = WiFi.scanNetworks();
  DEBUG("Scan done\n");
  if (n == 0) {
      DEBUG("No networks found\n");
  } else {
      DEBUG("%d networks found\n", n);
      for (int i = 0; i < n; ++i) {
          // Print SSID and RSSI for each network found
          DEBUG("%d: %s (%d, %s, %d) %s\n", 
                        i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str(), WiFi.channel(i), 
                        WiFi.encryptionType(i) == WIFI_AUTH_OPEN?" ":"*");
          delay(10);
      }
  }
  DEBUG("");  
}

static void clear_wifi() {
  DEBUG("Resetting WiFi, current status %d (from core %d)\n", WiFi.status(), xPortGetCoreID());
  WiFi.disconnect(true, true); // Switch off WiFi and forget any AP config
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);  
  
  delay(100);
  
  DEBUG("Resetting WiFi complete, current status %d\n", WiFi.status());  
  // It takes a little while to completely disconnect, if we don't do this 
  // the next connect may fail 
  delay(300);
}

void WiFiEvents(WiFiEvent_t event, WiFiEventInfo_t info) {
  /* Not safe for writes from task callback */
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  // Handling function code
  disconnected = 1;
}

void GvmLightControl::process_messages() {
  read_udp(udp_1112_fd);
  read_udp(udp_2525_fd);
}

int GvmLightControl::find_and_join_light_wifi(int *networks_found) {
  if (networks_found)
    *networks_found = 0;

  DEBUG("Initializing WiFi\n");
  
  WiFi.mode(WIFI_MODE_STA);
  DEBUG("Mode set to station\n");

  WiFi.onEvent(WiFiStationDisconnected, DISC_EVENT);
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
      DEBUG("No networks found");
      return -1;
  }

  DEBUG("%d networks available\n", n);
  for (int i = 0; i < n; ++i) {
    // Print SSID and RSSI for each network found
    DEBUG("Found %d: %s (%d, %s, %d) %s\n", 
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

int GvmLightControl::try_connect_wifi(const char *ssid, const char *password, int channel, uint8_t *bssid) {
  // We try connecting to each AP at least twice because 
  // sometimes the first connection fails for some strange
  // reason (error in the log: 'E (6714) wifi: Set status to INIT.')
  int connection_attempts = 0;

  while (connection_attempts++ < 2) {
    DEBUG("Updating connection info on screen\n");

    if (onWiFiConnectAttempt)
      onWiFiConnectAttempt(bssid, connection_attempts);
      /*
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);        
    M5.Lcd.printf("Trying\nSSID %s\nBSSID %02x:%02x:%02x:%02x:%02x:%02x\nAttempt %d\n", 
                  ssid, 
                  bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], 
                  connection_attempts);
                 */

    /*
    DEBUG("Dropping any existing WiFi\n");
    clear_wifi();
    */
    
    // Connect to the Access Point
    DEBUG("Trying to connect to %02x:%02x:%02x:%02x:%02x:%02x, current status %d\n", 
                  bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                  WiFi.status());      
                  
    disconnected = 0;
    WiFi.begin(ssid, password, channel, bssid);

    DEBUG("Connection begun\n");
    
    // Setting the hostname
    // WiFi.setHostname("controller");

    int wait_tests = 35;
    while (WiFi.status() != WL_CONNECTED && !disconnected && wait_tests-- > 0) {
      DEBUG("... WiFi status %d\n", WiFi.status());
      delay(100);
    }

    DEBUG("Finished waiting, status %d disconnected %d tests remaining %d\n", WiFi.status(), disconnected, wait_tests);

    if (WiFi.status() == WL_CONNECTED)
      break;
      
    DEBUG("Connect timed out, status %d\n", WiFi.status());
  }

  if (WiFi.status() != WL_CONNECTED) {
    DEBUG("Connect failed, status %d, moving on\n", WiFi.status());
    return -1;
  }
  
  if (!test_light_connection()) {
    DEBUG("GVM Light Connected\n");
    return 0;
  }

  return -1;
}

int GvmLightControl::test_light_connection() {
  DEBUG("Connected to the WiFi network. IP: ");
  // DEBUG(WiFi.localIP());
  DEBUG("Base station is: %s\n", WiFi.BSSIDstr().c_str());
  DEBUG("Receive strength is: %d\n", WiFi.RSSI());

  // Listen on any incoming IP address for UDP port 2525
  // udp.begin(2525);
  open_udp_port(&udp_2525_fd, 2525);
  DEBUG("Listening port 2525 with FD %d\n", udp_2525_fd);

  open_udp_port(&udp_1112_fd, 1112);
  DEBUG("Listening port 1112 with FD %d\n", udp_1112_fd);

  // Broadcast the starting message to ask the light(s) to report 
  DEBUG("Broadcasting first connect message\n");
  send_hello_msg();

  DEBUG("Waiting for light message\n");
  
  int waits = 60;
  while (waits-- >= 0) {
    if (read_udp(udp_1112_fd)) {
      DEBUG("Received light message, proceeding\n");
      return 0;
    }
    delay(20); 
  }

  return -1;
}

int GvmLightControl::broadcast_udp(const void *d, int len) {
  struct sockaddr_in broadcast_addr;
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(2525);
  broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;    
  return sendto(udp_2525_fd, d, len, 0, (const sockaddr *) &broadcast_addr, sizeof(broadcast_addr));   
}

static int open_udp_port(int *fd, int port) {
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
    // DEBUG("CRC calc %c%c = ", *(str), *(str + 1));
    c = (charToVal(*str++) << 4);
    c += charToVal(*str++);
    crc ^= c << 8;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc = crc << 1;
    }   
    len -= 2; 
    // DEBUG("%x CRC = %d\n", c, crc);
  }
  return crc & 0xFFFF;  
}

LightStatus GvmLightControl::getLightStatus() {
  return light_status;
}

int GvmLightControl::getOnOff() {
  return light_status.on_off;
}

int GvmLightControl::getChannel() {
  return light_status.channel;
}

int GvmLightControl::getHue() {
  return light_status.hue;
}

int GvmLightControl::getBrightness() {
  return light_status.brightness;
}

int GvmLightControl::getCct() {
  return light_status.cct;
}

int GvmLightControl::getSaturation() {
  return light_status.saturation;
}

int GvmLightControl::setOnOff(int on_off) {
  int8_t newVal = set_bounded(on_off, 0, 1);  
  light_status.on_off = newVal;  
  send_set_cmd_and_hello(LIGHT_VAR_ON_OFF, newVal);
  return newVal;
}

int GvmLightControl::setChannel(int channel) {
  int8_t newVal = set_bounded(channel, 1, 12);
  light_status.channel = newVal;  
  send_set_cmd_and_hello(LIGHT_VAR_CHANNEL, newVal);  
  return newVal;
}

int GvmLightControl::setBrightness(int brightness) {
  int8_t newVal = set_bounded(brightness, 0, 100);
  light_status.brightness = newVal;  
  send_set_cmd_and_hello(LIGHT_VAR_BRIGHTNESS, newVal);  
  return newVal;    
}

int GvmLightControl::setCct(int cct) {
  int8_t newVal = set_bounded(cct, 32, 56);
  light_status.cct = newVal;  
  send_set_cmd_and_hello(LIGHT_VAR_CCT, newVal);  
  return newVal;      
}

int GvmLightControl::setHue(int hue) {
  int8_t newVal = set_bounded(hue, 0, 72);
  light_status.hue = newVal;  
  send_set_cmd_and_hello(LIGHT_VAR_HUE, newVal);  
  return newVal;  
}

int GvmLightControl::setSaturation(int saturation) {
  int8_t newVal = set_bounded(saturation, 0, 100);
  light_status.hue = newVal;  
  send_set_cmd_and_hello(LIGHT_VAR_HUE, newVal);  
  return newVal;  
}

int GvmLightControl::read_udp(int fd) {
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

  // DEBUG("Reading %d\n",fd);
  
  while ((rx_len = recvfrom(fd, rx_buffer, sizeof(rx_buffer) - 1, 0, (sockaddr *) &rx_from, (socklen_t *) &rx_from_size)) > -1) {
    rx_buffer[rx_len] = '\0';
    DEBUG("Received packet on FD %d\n", fd);
    DEBUG("From %d.%d.%d.%d:%d\n", 
                  *(((uint8_t *) &rx_from.sin_addr) + 0),
                  *(((uint8_t *) &rx_from.sin_addr) + 1),
                  *(((uint8_t *) &rx_from.sin_addr) + 2),
                  *(((uint8_t *) &rx_from.sin_addr) + 3),
                  ntohs(rx_from.sin_port));

    // serialPrintAsHex((char *) rx_buffer, rx_len, "Message: ");
    DEBUG("%s", printAsHex((char *) rx_buffer, rx_len, "Message: ").c_str());
    
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
      DEBUG("Calc CRC for message of length %d == %d\n", hexDecode[2], (2 * (3 + hexDecode[2] - 2)));
      unsigned short crc = calcCrcFromHexStr((char *) msgDecode, (2 * (3 + hexDecode[2] - 2)));
      unsigned short msgcrc = ntohs(*((unsigned short *) &hexDecode[3 + hexDecode[2] - 2]));

      /* If this is not a light message stop decoding */
      if (crc != msgcrc) {
        DEBUG("CRC not correct, %d vs %d\n", crc, msgcrc);
        break;
      }

      msgs_processed++;
      
      DEBUG("GVM light message with %d bytes of payload\n", hexDecode[2]);
      DEBUG("  Length %d\n  Device ID %d\n  Device Type 0x%x\n  Message Type %d\n",
                    hexDecode[2], hexDecode[3], hexDecode[4], hexDecode[5]);
                        
      if (hexDecode[5] == LIGHT_MSG_VAR_ALL) {
        /* Status message sent periodically by the lights */
        /* Save state */
        light_status.on_off     = hexDecode[6]; // 0 if light is currently 'soft' off, 1 if it's on
        light_status.channel    = hexDecode[7];
        light_status.brightness = hexDecode[8];
        light_status.cct        = hexDecode[9];
        light_status.hue        = hexDecode[10];
        light_status.saturation = hexDecode[11];
        DEBUG("  Status Message: Light On %d Channel %d Brightness %d%% CCT %d Hue %d Saturation %d\n",
                      hexDecode[6], hexDecode[7] - 1, hexDecode[8], hexDecode[9] * 100, hexDecode[10] * 5, hexDecode[11]);
        if (onStatusUpdated)
          onStatusUpdated();
      } else if (hexDecode[5] == LIGHT_MSG_VAR_SET) {
        /* Updated message, send in response to an update message 
        e.g '4C54080030020002003A89' received from sending a brightness zero message '4C5409003057000201005C9E'
        or  '4C54080030020002030AEA' received from sending a brightness 3% message   '4C5409003057000201036CFD' */
        DEBUG("  Updated Message: Unknown 1 %d Field %d Value %d\n", 
                      hexDecode[6], hexDecode[7], hexDecode[8]);
        switch (hexDecode[7]) {
          case LIGHT_VAR_ON_OFF:
            light_status.on_off = hexDecode[8];
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
        if (onStatusUpdated)
          onStatusUpdated();
      } else {
        DEBUG("  Unknown Payload: %.*s\n", (hexDecode[2] - 3 - 2) * 2, msgDecode + ((3 + 3) * 2)); // FIX not being at start of rxbuffer        
      }

      rx_len -= msglen * 2;
      hexDecode += msglen;
      msgDecode += msglen * 2;
      DEBUG("RX left %d\n", rx_len);
      if (rx_len >= 6)
        DEBUG("First bytes of potential next message = %02x %02x %02x\n", hexDecode[0], hexDecode[1], hexDecode[2]);
    }
  }

  return msgs_processed;
}

/* This message causes the light to respond with a 0x53 message then 
 * send a 0x03 status message. If the light is on the status messages
 * will continue to be sent every 5 seconds 
 */
int GvmLightControl::send_hello_msg() {
  if (udp_2525_fd == -1)
    return -1;

  DEBUG("Sending hello msg, '%.*s'\n", strlen(first_connect), first_connect);
  int rc = broadcast_udp(first_connect, strlen(first_connect));

  return 0;
}

int GvmLightControl::send_set_cmd_and_hello(uint8_t setting, uint8_t value) {
  /* When switching the light off sometimes we don't get a response 
   *  message. Send a hello message as well so we we'll get a status
   *  message to process the change */
  /* Sometimes when messages are sent too quickly we don't get a 
   *  'setting updated' message from the light. Send a hello to get 
   *  a full setting update as well
   */  
   int rc = send_set_cmd(setting, value);
   if (rc)
     return rc;
   rc = send_hello_msg();
   return rc; 
}

int GvmLightControl::send_set_cmd(uint8_t setting, uint8_t value) {
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
  
  DEBUG("Sending command with len %d, '%.*s'\n", sizeof(encoded_cmd_buffer), sizeof(encoded_cmd_buffer), encoded_cmd_buffer);

  broadcast_udp(encoded_cmd_buffer, sizeof(encoded_cmd_buffer));
  return 0;
}

int GvmLightControl::wait_msg_or_timeout() {
  fd_set readSet;
  struct timeval t;
  t.tv_sec = 0;
  t.tv_usec = 10000;
  FD_ZERO(&readSet);
  FD_SET(udp_1112_fd, &readSet);
  int rc = select(udp_1112_fd + 1, &readSet, NULL, NULL, &t);    
  if (rc < 0)
    DEBUG("Wait on FD %d returned %d\n", udp_1112_fd, rc);
  if (rc == 1)
    process_messages();
  return 0;
}
