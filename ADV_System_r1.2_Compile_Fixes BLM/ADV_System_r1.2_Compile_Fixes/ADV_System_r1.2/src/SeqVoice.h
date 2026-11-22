#pragma once
#include <Arduino.h>
#include <SD.h>
#include <stdint.h>
#include <stddef.h>

static const size_t SEQ_CACHE=1024;
struct SeqVoice {
 File file;
 uint32_t dataAt=0,rate=0;
 uint16_t channels=0,bits=0;
 size_t frames=0,pos=0;
 size_t cacheStart=SIZE_MAX,cacheLen=0;
 uint8_t cache[SEQ_CACHE];
 bool loaded=false;
};
