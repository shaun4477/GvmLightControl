/*
  Morse.h - Library for controlling GVM (Great Video Maker) lights over WiFi.
  Created by shaun4477, December, 2020.
  Released into the public domain.
*/

#ifndef GvmLightControl_h
#define GvmLightControl_h

#include "util/HexFunctions.h"

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

#define LOG_CHANNEL "gvm_lights"

#define LIGHT_VAR_ON_OFF     0
#define LIGHT_VAR_CHANNEL    1
#define LIGHT_VAR_BRIGHTNESS 2
#define LIGHT_VAR_CCT        3
#define LIGHT_VAR_HUE        4
#define LIGHT_VAR_SATURATION 5

#define LIGHT_MSG_SETVAR     0x57 // Send to set a variable 
#define LIGHT_MSG_VAR_SET    0x2  // Response to a variable set
#define LIGHT_MSG_VAR_ALL    0x3  // Periodic message with all variable settings    

uint16_t calcCrcFromHexStr(const char *str, int len);

class LightStatus {
  public:
    LightStatus() : on_off(-1), channel(-1), hue(-1), brightness(-1), cct(-1), saturation(-1) {};

  public: 
    int on_off;
    int channel;
    int hue;
    int brightness;
    int cct;
    int saturation;    
};

class GvmLightControl {
  public:
    GvmLightControl(bool debug = false);
    void debugOn();
    
    void process_messages();
    int find_and_join_light_wifi(int *networks_found);  
    int wait_msg_or_timeout();
    int send_hello_msg();
    int send_set_cmd(uint8_t setting, uint8_t value);
    int send_set_cmd_and_hello(uint8_t setting, uint8_t value);
    
    void callbackOnWiFiConnectAttempt(void (*callback)(uint8_t *bssid, int attempt));
    void callbackOnStatusUpdated(void (*callback)());

    LightStatus getLightStatus();
    int getOnOff();
    int getChannel();
    int getHue();
    int getBrightness();
    int getCct();
    int getSaturation();

    int setOnOff(int on_off);
    int setChannel(int channel);
    int setHue(int hue);
    int setBrightness(int brightess);
    int setCct(int cct);
    int setSaturation(int saturation);

    int broadcast_udp(const void *d, int len);
    
  private:
    int test_light_connection();
    int read_udp(int fd);
    int try_connect_wifi(const char *ssid, const char *password, int channel, uint8_t *bssid);
  
  private:
    LightStatus light_status;
    int udp_2525_fd;
    int udp_1112_fd;    
    void (*onWiFiConnectAttempt)(uint8_t *bssid, int attempt);
    void (*onStatusUpdated)();
};

static GvmLightControl GVM;

#endif
