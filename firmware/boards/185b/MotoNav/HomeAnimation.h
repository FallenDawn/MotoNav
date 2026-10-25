#pragma once

#include <stdint.h>

#define RIDER_W 40
#define RIDER_H 26
#define RIDER_FRAMES 7
#define HOME_GIF_W 50
#define HOME_GIF_H 50
#define HOME_GIF_SCALE 4
#define HOME_CONNECTED_FRAMES 17
#define HOME_DISCONNECTED_FRAMES 30

// Home screen animation assets and renderer state, implemented in HomeGifAnimation.cpp.
extern const char *const RIDER[RIDER_FRAMES][RIDER_H];
extern const uint16_t HOME_GIF_PALETTE[255];
extern const uint8_t HOME_CONNECTED_PIXELS[HOME_CONNECTED_FRAMES][HOME_GIF_W * HOME_GIF_H];
extern const uint16_t HOME_CONNECTED_FRAME_MS[HOME_CONNECTED_FRAMES];
extern const uint8_t HOME_DISCONNECTED_PIXELS[HOME_DISCONNECTED_FRAMES][HOME_GIF_W * HOME_GIF_H];
extern const uint16_t HOME_DISCONNECTED_FRAME_MS[HOME_DISCONNECTED_FRAMES];

extern bool onWaitScreen;
extern bool sliding;
extern int waitSt;
extern int riderPrevF, riderPrevZ;
extern bool catForce;
extern int riderAnimMode;

void riderBindRenderer(void (*fillRect)(int16_t, int16_t, int16_t, int16_t, uint16_t),
                       void (*printText)(const char *, int, int, int, uint16_t),
                       void (*drawStatusBar)());
void drawRider(int frame, int sleepFrame);
void drawHomeGifFrame(uint8_t frame, bool connected);
uint8_t homeGifFrameAt(bool connected, uint32_t elapsedMs);
void animateidle();
