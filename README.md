Arduino Library to allow the ESP32/ESP8266 based boards to control GVM 8000-RGB lights via WiFi

Should be easily portable to any ESP32 based board (e.g m5stack etc). Should also work with other GVM lights with WiFi, but not tested

# General notes about the lights

## WiFi

Each light will create a WiFi network with the GVM SSID, even if they are *not* in WiFi mode. 

The networks created by the lights are NOT linked. If you join the network of a light that is off or is not in WiFi mode nothing will happen. 
## Communications

The app communicates with the lights by broadcasting UDP to 255.255.255.255:2525. The lights communicate with the app by broadcasting to 255.255.255.255:1112 (with a source port of 2525). 

The basic protocol is described by Nathan Osman in a stack exchange post, https://security.stackexchange.com/a/222638

# Development

## Running a capture of iOS traffic from the app to the lights

To understand communication between the lights and the iOS app you can use packet captures. The capture process is as follows. 

Plug the iPhone in with a cable

Get the UDID for the device

Use rvictl to create a remote virtual interface. Note that UDID must match _exactly_ including upper case

`rvictl -s 00008030-001E39620E50802E`

If it works you'll get a success message, if the UDID is wrong *you won't see anything*

You can then capture on the interface:

`sudo tcpdump -i rvi0 -w capture.pcap`

Once you're done with the capture close the virtual interface with -x

`rvictl -x 00008030-001E39620E50802E`

