#ifndef SMART_CATAN_H
#define SMART_CATAN_H

// Public entry points for the smart-catan LED board/web app, only compiled
// in when SMART_CATAN is defined (see platformio.ini's [env:esp32dev]).
// Implementation in smart_catan.cpp.

void smartCatanSetup();
void smartCatanLoop();
void notifySmartCatanRoll(int total);

#endif
