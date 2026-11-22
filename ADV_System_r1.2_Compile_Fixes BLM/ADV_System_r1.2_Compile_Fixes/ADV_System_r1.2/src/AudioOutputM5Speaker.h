#pragma once
// Adapted from M5Stack M5Unified example Advanced/MP3_with_ESP8266Audio.
// https://github.com/m5stack/M5Unified/tree/master/examples/Advanced/MP3_with_ESP8266Audio
#include <AudioOutput.h>
#include <M5Unified.h>
#include <atomic>
class AudioOutputM5Speaker : public AudioOutput {
public:
 AudioOutputM5Speaker(m5::Speaker_Class* sound,uint8_t channel=0):_sound(sound),_channel(channel){}
 void setup(){_sound->setBufferReleaseCallback(this,bufferReleased);}
 bool begin() override {_index=0;_offset=0;return true;}
 bool ConsumeSample(int16_t sample[2]) override {
  if(_offset<bufferSize){_buffers[_index][_offset++]=sample[0];_buffers[_index][_offset++]=sample[1];return true;}
  flush();return false;
 }
 void flush() override {
  if(!_offset)return;
  _busy[_index]=true;
  bool ok=_sound->playRaw(_buffers[_index],_offset,hertz,true,1,_channel);
  if(!ok){_busy[_index]=false;Serial.println("MP3: playRaw rejected buffer");}
  _index=(_index+1)%bufferCount;
  _offset=0;
  while(_busy[_index])vTaskDelay(1);
 }
 bool stop() override {
  flush();
  for(size_t i=0;i<bufferCount;++i)while(_busy[i])vTaskDelay(1);
  return true;
 }
private:
 static constexpr size_t bufferCount=3,bufferSize=1024;
 m5::Speaker_Class* _sound;
 uint8_t _channel;
 int16_t _buffers[bufferCount][bufferSize]{};
 std::atomic<bool> _busy[bufferCount]{{false},{false},{false}};
 size_t _index=0,_offset=0;
 static void bufferReleased(void* ctx,const void* data,uint8_t){
  auto* self=static_cast<AudioOutputM5Speaker*>(ctx);
  for(size_t i=0;i<bufferCount;++i)if(data==self->_buffers[i])self->_busy[i]=false;
 }
};
