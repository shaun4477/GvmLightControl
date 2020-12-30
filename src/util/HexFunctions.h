#ifndef HexFunctions_h
#define HexFunctions_h

#include <StreamString.h>

uint8_t charToVal(char c);
char valToChar(uint8_t v);
void hexStringToBytes(char *hexstr, int len, unsigned char *out);
void bytesToHexString(unsigned char *in, int len, char *out);
void shortToHex(unsigned short num, char *out);
StreamString printAsHex(char *buf, int len, char *prompt);

#endif
