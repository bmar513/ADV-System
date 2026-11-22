#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <USB.h>
#include <USBMSC.h>
#include <tusb.h> // TinyUSB soft disconnect/reconnect for fresh host enumeration
#include <Preferences.h>
#include <Update.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <vector>
#include <algorithm>
#include <freertos/semphr.h>
#include "AudioOutputM5Speaker.h"
#include "SeqVoice.h"

// ADV System r1.2 — Music, USB SD, WAV Sequencer and Diagnostics.
// HOME / browser: ; up, . down, Enter select, ` back.
// Music: Enter play/pause, , previous, / next, Backspace stop.
// Genre folders at SD root: Industrial, Rap, Desert, Metal, Rock, Electro.
// File Player: MP3 and PCM WAV. Recorder has been removed.
static const int SD_CS=12, SD_SCK=40, SD_MISO=39, SD_MOSI=14;
static const int MAX_TRACKS=2500; // Upper bound; actual capacity depends on free heap
static const char* GENRE_NAMES[]={"INDUSTRIAL","RAP","DESERT","METAL","ROCK","ELECTRO"};
static const char* GENRE_DIRS[]={"/Industrial","/Rap","/Desert","/Metal","/Rock","/Electro"};
static const int GENRE_COUNT=sizeof(GENRE_NAMES)/sizeof(GENRE_NAMES[0]);
static const int MENU_COUNT=GENRE_COUNT+1;
static_assert(sizeof(GENRE_NAMES)/sizeof(GENRE_NAMES[0])==sizeof(GENRE_DIRS)/sizeof(GENRE_DIRS[0]), "Genre names and directories must match");
enum Mode { LAUNCHER, HOME, GENRE_PLAYER, FILE_BROWSER, FOLDER_PLAYER, SEQUENCER, SAMPLE_BROWSER, PATTERN_BROWSER, SD_APP, DIAG_APP, NOTES_APP, NOTES_FILES, NOTES_MENU, FLASH_BROWSER, FLASH_CONFIRM };
Mode mode=LAUNCHER;
int diagScroll=0;
// Notes are plain UTF-8/ASCII text; editor uses a fixed-size buffer to bound RAM use.
String noteText="", noteFile="", noteMessage="";
int noteCursor=0, noteScroll=0, noteChoice=0, noteMenuChoice=0, noteCount=0;
bool noteDirty=false;
String noteNames[80];
int flashChoice=0, flashCount=0;
String flashMessage="";
String flashNames[80];
const char* noteActions[]={"SAVE","OPEN","NEW","EXIT","CANCEL"};
// Shared SD state is declared before Notes/FLASH helpers for C++ compilation.
extern bool sdOK;
extern bool sdExported;
extern SemaphoreHandle_t sdMutex;

void scanNotes(){
 noteCount=0;noteChoice=0;
 if(!sdOK||sdExported)return;
 if(xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1200))!=pdTRUE)return;
 if(!SD.exists("/Notes"))SD.mkdir("/Notes");
 File dir=SD.open("/Notes");
 if(dir&&dir.isDirectory()){
  File f=dir.openNextFile();
  while(f&&noteCount<80){String name=String(f.name());int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
   String lower=name;lower.toLowerCase();if(!f.isDirectory()&&lower.endsWith(".txt"))noteNames[noteCount++]=name;
   f.close();f=dir.openNextFile();}
  if(f)f.close();dir.close();
 }
 xSemaphoreGive(sdMutex);
}
void scanFirmware(){
 flashCount=0;flashChoice=0;
 if(!sdOK||sdExported)return;
 if(xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1200))!=pdTRUE)return;
 if(!SD.exists("/Firmware"))SD.mkdir("/Firmware");
 File dir=SD.open("/Firmware");
 if(dir&&dir.isDirectory()){
  File f=dir.openNextFile();
  while(f&&flashCount<80){String name=String(f.name());int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
   String lower=name;lower.toLowerCase();if(!f.isDirectory()&&lower.endsWith(".bin"))flashNames[flashCount++]=name;
   f.close();f=dir.openNextFile();}
  if(f)f.close();dir.close();
 }
 xSemaphoreGive(sdMutex);
}
// FLASH beta: app-only ESP32-S3 binaries. Requires the matching one-shot
// custom bootloader and partition table included with this source package.
// This does NOT accept merged/full-flash images or arbitrary CPU targets.
static const uint32_t ADV_BOOT_MAGIC=0xB007A001u;
bool installAndRunFirmware(const String &filename){
 flashMessage="";
 if(!sdOK||sdExported){flashMessage="SD UNAVAILABLE";return false;}
 const esp_partition_t *factory=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_FACTORY,nullptr);
 const esp_partition_t *target=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
 const esp_partition_t *flag=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,(esp_partition_subtype_t)0x40,"bootflag");
 const esp_partition_t *running=esp_ota_get_running_partition();
 if(!factory||!target||!flag||!running||running->address!=factory->address||
    factory->address!=0x10000||target->address!=0x170000||flag->address!=0xD000){
   flashMessage="BOOT LAYOUT MISMATCH";return false;
 }
 if(!sdMutex||xSemaphoreTake(sdMutex,pdMS_TO_TICKS(2000))!=pdTRUE){flashMessage="SD BUSY";return false;}
 File f=SD.open(String("/Firmware/")+filename,FILE_READ);
 if(!f){xSemaphoreGive(sdMutex);flashMessage="OPEN FAILED";return false;}
 const size_t length=f.size();
 uint8_t hdr[24]={0};
 bool valid=length>=1024&&length<=target->size&&f.read(hdr,24)==24&&hdr[0]==0xE9&&hdr[1]>0&&hdr[1]<=16;
 // A merged flash image generally starts with the bootloader, but is not
 // safe to treat as an OTA app. Require a plausible app entry point.
 if(valid){
  uint32_t entry=(uint32_t)hdr[4]|((uint32_t)hdr[5]<<8)|((uint32_t)hdr[6]<<16)|((uint32_t)hdr[7]<<24);
  valid=(entry>=0x40000000u&&entry<0x50000000u);
 }
 if(!valid){f.close();xSemaphoreGive(sdMutex);flashMessage="BAD/OVERSIZE APP BIN";return false;}
 f.seek(0);
 if(!Update.begin(length,U_FLASH)){
  f.close();xSemaphoreGive(sdMutex);flashMessage="UPDATE BEGIN FAILED";return false;
 }
 static uint8_t block[4096];
 size_t done=0;bool ok=true;
 while(done<length){
  size_t want=std::min((size_t)sizeof(block),length-done);
  int n=f.read(block,want);
  if(n<=0||Update.write(block,(size_t)n)!=(size_t)n){ok=false;break;}
  done+=(size_t)n;
  if((done&0x1FFFF)==0){Serial.printf("FLASH: %u / %u\n",(unsigned)done,(unsigned)length);}
 }
 f.close();
 if(!ok||done!=length){Update.abort();xSemaphoreGive(sdMutex);flashMessage="WRITE FAILED";return false;}
 if(!Update.end(true)){xSemaphoreGive(sdMutex);flashMessage="FINALIZE FAILED";return false;}
 // IMPORTANT: only arm after Update.end has selected ota_0.
 esp_err_t e=esp_partition_erase_range(flag,0,0x1000);
 if(e==ESP_OK)e=esp_partition_write(flag,0,&ADV_BOOT_MAGIC,sizeof(ADV_BOOT_MAGIC));
 xSemaphoreGive(sdMutex);
 if(e!=ESP_OK){flashMessage="BOOT FLAG FAILED";return false;}
 Serial.println("FLASH: launching once; RESET should return to ADV System");
 M5Cardputer.Display.fillScreen(BLACK);
 M5Cardputer.Display.setCursor(5,40);
 M5Cardputer.Display.setTextColor(GREEN,BLACK);
 M5Cardputer.Display.print("LAUNCHING... RESET TO RETURN");
 delay(300);esp_restart();return true;
}

bool saveNote(){
 if(!sdOK||sdExported){noteMessage="SD UNAVAILABLE";return false;}
 if(xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE){noteMessage="SD BUSY";return false;}
 if(!SD.exists("/Notes"))SD.mkdir("/Notes");
 if(noteFile.isEmpty()){
  for(int i=1;i<=9999;i++){char n[32];snprintf(n,sizeof(n),"NOTE_%04d.txt",i);String path=String("/Notes/")+n;
   if(!SD.exists(path)){noteFile=n;break;}}
 }
 if(noteFile.isEmpty()){xSemaphoreGive(sdMutex);noteMessage="FOLDER FULL";return false;}
 String path=String("/Notes/")+noteFile;
 File f=SD.open(path,"w");
 if(!f){xSemaphoreGive(sdMutex);noteMessage="SAVE FAILED";return false;}
 // Opening with "w" truncates existing text before writing.
 size_t n=f.print(noteText);f.flush();f.close();xSemaphoreGive(sdMutex);
 if(n!=noteText.length()){noteMessage="WRITE INCOMPLETE";return false;}
 noteDirty=false;noteMessage="SAVED "+noteFile;return true;
}
bool openNote(String name){
 if(!sdOK||sdExported)return false;
 if(xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE)return false;
 File f=SD.open(String("/Notes/")+name,FILE_READ);
 if(!f){xSemaphoreGive(sdMutex);noteMessage="OPEN FAILED";return false;}
 if(f.size()>8192){f.close();xSemaphoreGive(sdMutex);noteMessage="NOTE OVER 8KB";return false;}
 String txt;txt.reserve(f.size()+1);
 while(f.available())txt+=(char)f.read();
 f.close();xSemaphoreGive(sdMutex);
 noteText=txt;noteFile=name;noteCursor=noteText.length();noteScroll=0;noteDirty=false;noteMessage="";return true;
}

USBMSC msc;
volatile bool hostEjected=false;
volatile uint32_t mscLastIO=0;
bool sdExported=false,sdExportError=false;
volatile bool usbMediaEjectPending=false;
bool usbMSCStarted=false;

static const int MAX_ENTRIES=150;
String browserNames[MAX_ENTRIES],browserPaths[MAX_ENTRIES];
bool browserFolders[MAX_ENTRIES];
int browserCount=0,browserSelection=0;
String browserPath="/", folderPlayingPath="";
bool folderFinished=false;
std::vector<String> tracks;
SemaphoreHandle_t sdMutex=nullptr;
bool folderQueueActive=false;
int trackCount=0, selection=0, volume=4; // User-facing 0..10 steps
// Map ten equal user-facing steps onto the M5 speaker's 0..255 scale.
uint8_t speakerVolume(){return (uint8_t)((volume*255+5)/10);}
// Recorder is intentionally excluded from ADV System.


bool sdOK=false;
int launcherChoice=0;
bool steps[4][16]={};
String samplePaths[4];
String sampleNames[80];
int sampleCount=0,sampleChoice=0,seqChannel=0,seqStep=0,seqBpm=120;
int patternChoice=0,patternCount=0;
String patternNames[80];
String patternMessage="";
int seqCursor=1; // 0 = channel button, 1..16 = step boxes
bool seqRunning=false;
uint32_t seqLastTick=0;
bool waitForDecoderStop();

// r0.9a: Four SD-streamed voices. No full-length PCM sample allocations.
// 1024-byte per-voice source caches; 16 kHz mono mixing in 256-frame blocks.
static const uint32_t SEQ_RATE=16000;
static const size_t SEQ_MAX_FRAMES=SEQ_RATE*3;

SeqVoice seqVoices[4];
SemaphoreHandle_t seqMutex=nullptr;
volatile bool seqAudioEnabled=false;
volatile bool seqResetRequested=false;
volatile uint32_t seqTriggerMask=0;
String sampleError="";
uint16_t wavU16(File &f){uint8_t b[2];if(f.read(b,2)!=2)return 0;return uint16_t(b[0])|(uint16_t(b[1])<<8);}
uint32_t wavU32(File &f){uint8_t b[4];if(f.read(b,4)!=4)return 0;return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);}
void clearSeqVoice(int c){
 if(c<0||c>=4)return;
 if(xSemaphoreTake(seqMutex,pdMS_TO_TICKS(2000))!=pdTRUE)return;
 if(seqVoices[c].file)seqVoices[c].file.close();
 seqVoices[c]=SeqVoice{};
 samplePaths[c]="";
 xSemaphoreGive(seqMutex);
}
void closeSeqFiles(){
 if(!seqMutex)return;
 if(xSemaphoreTake(seqMutex,pdMS_TO_TICKS(2000))!=pdTRUE)return;
 for(int c=0;c<4;c++){
  if(seqVoices[c].file)seqVoices[c].file.close();
  seqVoices[c]=SeqVoice{};
  samplePaths[c]="";
 }
 xSemaphoreGive(seqMutex);
}
// Read a single converted 16kHz output frame from an SD-backed source.
// Called only while holding BOTH seqMutex and sdMutex.
int16_t nextSeqFrame(SeqVoice &v){
 if(!v.loaded||v.pos>=v.frames)return 0;
 const size_t bytesPerFrame=v.channels*(v.bits/8);
 size_t srcFrame=(uint64_t)v.pos*v.rate/SEQ_RATE;
 size_t byteAt=srcFrame*bytesPerFrame;
 if(v.cacheStart==SIZE_MAX||byteAt<v.cacheStart||byteAt+bytesPerFrame>v.cacheStart+v.cacheLen){
  size_t start=(byteAt/SEQ_CACHE)*SEQ_CACHE;
  if(!v.file.seek(v.dataAt+start)){v.pos=v.frames;return 0;}
  v.cacheLen=v.file.read(v.cache,SEQ_CACHE);
  v.cacheStart=start;
  if(byteAt+bytesPerFrame>start+v.cacheLen){v.pos=v.frames;return 0;}
 }
 const uint8_t *p=v.cache+(byteAt-v.cacheStart);
 int32_t sum=0;
 for(int c=0;c<v.channels;c++){
  if(v.bits==8)sum+=((int32_t)p[c]-128)*256;
  else {int o=c*2;sum+=(int16_t)((uint16_t)p[o]|((uint16_t)p[o+1]<<8));}
 }
 v.pos++;
 return (int16_t)(sum/v.channels);
}
bool loadSeqSample(int channel,const String &path){
 if(channel<0||channel>=4||sdExported||!sdOK){sampleError="SD NOT READY";return false;}
 if(!waitForDecoderStop()){sampleError="AUDIO BUSY";return false;}
 // Close old channel first; no transient duplicate allocations or open handles.
 clearSeqVoice(channel);
 if(!sdMutex||xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE){sampleError="SD BUSY";return false;}
 File f=SD.open(path,FILE_READ);
 bool valid=false;uint32_t dataAt=0,dataBytes=0,rate=0;uint16_t fmt=0,chans=0,bits=0;
 if(f&&f.size()>=44){
  char magic[4],wave[4];f.readBytes(magic,4);wavU32(f);f.readBytes(wave,4);
  if(memcmp(magic,"RIFF",4)==0&&memcmp(wave,"WAVE",4)==0){
   while(f.position()+8<=f.size()){
    char id[4];f.readBytes(id,4);uint32_t len=wavU32(f);
    uint32_t at=f.position();if(len>f.size()-at)break;
    if(memcmp(id,"fmt ",4)==0&&len>=16){
     fmt=wavU16(f);chans=wavU16(f);rate=wavU32(f);wavU32(f);wavU16(f);bits=wavU16(f);
    }else if(memcmp(id,"data",4)==0){dataAt=at;dataBytes=len;}
    uint32_t next=at+len+(len&1);if(next>f.size())break;f.seek(next);
   }
   valid=fmt==1&&(chans==1||chans==2)&&(bits==8||bits==16)&&rate>=8000&&rate<=48000&&dataAt>0&&dataBytes>=chans*(bits/8);
  }
 }
 if(!valid){if(f)f.close();xSemaphoreGive(sdMutex);sampleError="UNSUPPORTED PCM WAV";return false;}
 size_t bpf=chans*(bits/8);
 size_t srcFrames=std::min((size_t)(dataBytes/bpf),(size_t)rate*3);
 size_t frames=std::min(SEQ_MAX_FRAMES,(size_t)((uint64_t)srcFrames*SEQ_RATE/rate));
 if(!frames){f.close();xSemaphoreGive(sdMutex);sampleError="EMPTY WAV";return false;}
 if(xSemaphoreTake(seqMutex,pdMS_TO_TICKS(1000))!=pdTRUE){f.close();xSemaphoreGive(sdMutex);sampleError="MIXER BUSY";return false;}
 SeqVoice &v=seqVoices[channel];
 v.file=f;v.dataAt=dataAt;v.rate=rate;v.channels=chans;v.bits=bits;
 v.frames=frames;v.pos=frames;v.cacheStart=SIZE_MAX;v.cacheLen=0;v.loaded=true;
 samplePaths[channel]=path;
 xSemaphoreGive(seqMutex);
 xSemaphoreGive(sdMutex);
 sampleError="";return true;
}
void seqAudioTask(void*){
 static int16_t mix[256];
 for(;;){
  if(!seqAudioEnabled){vTaskDelay(pdMS_TO_TICKS(10));continue;}
  // Order is SD mutex -> seq mutex, matching sample loading/SD operations.
  if(xSemaphoreTake(sdMutex,pdMS_TO_TICKS(20))!=pdTRUE){vTaskDelay(1);continue;}
  if(xSemaphoreTake(seqMutex,pdMS_TO_TICKS(20))!=pdTRUE){xSemaphoreGive(sdMutex);vTaskDelay(1);continue;}
  uint32_t triggers=seqTriggerMask;seqTriggerMask=0;
  if(seqResetRequested){for(auto &v:seqVoices)v.pos=v.frames;seqResetRequested=false;}
  for(int c=0;c<4;c++)if((triggers&(1u<<c))&&seqVoices[c].loaded){seqVoices[c].pos=0;seqVoices[c].cacheStart=SIZE_MAX;}
  for(size_t i=0;i<256;i++){
   int32_t sum=0;
   for(auto &v:seqVoices)if(v.loaded&&v.pos<v.frames)sum+=nextSeqFrame(v);
   mix[i]=(int16_t)std::max<int32_t>(-32768,std::min<int32_t>(32767,sum));
  }
  xSemaphoreGive(seqMutex);xSemaphoreGive(sdMutex);
  M5Cardputer.Speaker.playRaw(mix,256,SEQ_RATE,false,0,false);
  vTaskDelay(pdMS_TO_TICKS(14));
 }
}
void stopSeq(){seqRunning=false;seqAudioEnabled=false;seqResetRequested=true;seqTriggerMask=0;seqStep=0;M5Cardputer.Speaker.stop();}
void fireSeqStep(int step){
 uint32_t mask=0;
 for(int c=0;c<4;c++)if(steps[c][step]&&seqVoices[c].loaded)mask|=1u<<c;
 seqTriggerMask|=mask;
}

// SD card USB Mass Storage callbacks. All access must be exclusive.
// SD library uses SPI and provides sector-level readRAW/writeRAW.
// MSC callbacks run in the USB task, not the Arduino loop task.
// Never block indefinitely here: a stalled callback can hang Windows transfers.
// A bounded mutex prevents raw-sector I/O from racing local filesystem work.
static int32_t mscRead(uint32_t lba,uint32_t offset,void *buffer,uint32_t size){
 if(!sdMutex || xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1000))!=pdTRUE)return -1;
 int32_t result=-1;
 do {
  if(!sdExported||!sdOK||offset!=0)break;
  const uint32_t sec=SD.sectorSize();
  if(!sec||size%sec||lba>=SD.numSectors()||size/sec>SD.numSectors()-lba)break;
  uint8_t *p=(uint8_t*)buffer;
  bool ok=true;
  for(uint32_t i=0;i<size/sec;i++)if(!SD.readRAW(p+i*sec,lba+i)){ok=false;break;}
  if(ok){result=size;mscLastIO=millis();}
 }while(false);
 xSemaphoreGive(sdMutex);
 return result;
}
static int32_t mscWrite(uint32_t lba,uint32_t offset,uint8_t *buffer,uint32_t size){
 if(!sdMutex || xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1000))!=pdTRUE)return -1;
 int32_t result=-1;
 do {
  if(!sdExported||!sdOK||offset!=0)break;
  const uint32_t sec=SD.sectorSize();
  if(!sec||size%sec||lba>=SD.numSectors()||size/sec>SD.numSectors()-lba)break;
  bool ok=true;
  for(uint32_t i=0;i<size/sec;i++)if(!SD.writeRAW(buffer+i*sec,lba+i)){ok=false;break;}
  if(ok){result=size;mscLastIO=millis();}
 }while(false);
 xSemaphoreGive(sdMutex);
 return result;
}
static bool mscStartStop(uint8_t power,bool start,bool loadEject){
 if(loadEject&&!start){hostEjected=true;usbMediaEjectPending=true;}
 return true;
}
// Check the filesystem is usable locally, not merely that SD.begin returned true.
bool localSDReady(){
 if(!sdOK)return false;
 File root=SD.open("/");
 bool ok=(bool)root && root.isDirectory();
 if(root)root.close();
 return ok;
}
// Reinitialize the SD card with bounded retries after relinquishing USB MSC.
// Never let another app access the card unless its filesystem is verified.
bool remountLocalSD(){
 if(sdMutex&&xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE)return false;
 bool ok=false;
 for(int attempt=0;attempt<4;attempt++){
  SD.end();
  delay(80+attempt*100);
  sdOK=SD.begin(SD_CS,SPI,25000000);
  ok=localSDReady();
  Serial.printf("SD: local remount attempt=%d begin=%d root=%d\n",attempt+1,sdOK,ok);
  if(ok)break;
 }
 sdOK=ok;
 if(sdMutex)xSemaphoreGive(sdMutex);
 return ok;
}
// Force Windows to re-enumerate the already-initialized USB device.
// The Arduino ESPUSB class has no USB.end(); TinyUSB supplies soft detach.
// Only call from main loop, NEVER from USB callbacks.
void usbReenumerate(bool mediumPresent){
  tud_disconnect();
  delay(300); // Host must see a real disconnect before reconnect.
  msc.mediaPresent(mediumPresent);
  delay(20);
  tud_connect();
  Serial.printf("MSC: USB re-enumerated; medium=%d at %lu ms\n",mediumPresent,(unsigned long)millis());
}
void beginSDExport(){
 stopSeq();
 closeSeqFiles();
 sdExportError=false;hostEjected=false;usbMediaEjectPending=false;mscLastIO=millis();
 if(!waitForDecoderStop()){sdExportError=true;return;}
 if(sdMutex&&xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1000))!=pdTRUE){sdExportError=true;return;}
 bool ready=localSDReady();
 if(!ready){
  SD.end();
  sdOK=SD.begin(SD_CS,SPI,25000000);
  ready=localSDReady();
 }
 if(ready&&SD.sectorSize()==512&&SD.numSectors()>0){
  // Reinitialize MSC geometry in case card was changed while locally mounted.
  msc.mediaPresent(false);
  if(msc.begin(SD.numSectors(),SD.sectorSize())){
   usbMSCStarted=true;
   sdExported=true;
  }else{sdExportError=true;sdExported=false;}
 }else{sdExportError=true;sdExported=false;}
 if(sdMutex)xSemaphoreGive(sdMutex);
 if(sdExported){
  // Re-enumerate only after releasing sdMutex; USB callbacks may need it.
  usbReenumerate(true);
 }
}
bool endSDExport(){
 // User must EJECT IN WINDOWS before pressing BACKTICK.
 // Withdraw medium even if the host did not send the eject callback.
 if(sdMutex&&xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE){
  sdExportError=true;return false;
 }
 sdExported=false;
 usbMediaEjectPending=false;
 if(sdMutex)xSemaphoreGive(sdMutex);
 usbReenumerate(false);
 // USB is now connected without an SD medium. Windows sees its removal.
 delay(100);
 bool ok=remountLocalSD();
 sdExportError=!ok;
 hostEjected=false;
 Serial.printf("SD: returned to ADV; filesystem=%s\n",ok?"READY":"FAILED");
 return ok;
}
void scanSamples(){
 sampleCount=0;sampleChoice=0;
 if(!sdOK)return;
 if(sdMutex && xSemaphoreTake(sdMutex,pdMS_TO_TICKS(300))!=pdTRUE)return;
 File dir=SD.open("/Samples");
 if(dir && dir.isDirectory()){
  File f=dir.openNextFile();
  while(f&&sampleCount<80){
   String n=String(f.name());
   if(!f.isDirectory()){
    String lower=n;lower.toLowerCase();
    if(lower.endsWith(".wav"))
     sampleNames[sampleCount++]=n.startsWith("/")?n.substring(n.lastIndexOf('/')+1):n;
   }
   f.close();f=dir.openNextFile();
  }
  dir.close();
 }
 if(sdMutex)xSemaphoreGive(sdMutex);
}
// Pattern format: ADVPAT1, BPM, four sample paths, four 16-bit masks.
// Auto-numbered saves avoid needing a text-entry UI on the small keyboard.
void scanPatterns(){
 patternCount=0;patternChoice=0;patternMessage="";
 if(!sdOK||sdExported)return;
 if(!sdMutex||xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1000))!=pdTRUE)return;
 File dir=SD.open("/Samples/Patterns");
 if(dir&&dir.isDirectory()){
  File f=dir.openNextFile();
  while(f&&patternCount<80){
   String n=String(f.name()),low=n;low.toLowerCase();
   if(!f.isDirectory()&&low.endsWith(".pat"))
    patternNames[patternCount++]=n.substring(n.lastIndexOf('/')+1);
   f.close();f=dir.openNextFile();
  }
  dir.close();
 }
 xSemaphoreGive(sdMutex);
}
bool savePattern(){
 if(sdExported||!sdOK){patternMessage="SD NOT READY";return false;}
 if(!sdMutex||xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE){patternMessage="SD BUSY";return false;}
 if(!SD.exists("/Samples"))SD.mkdir("/Samples");
 if(!SD.exists("/Samples/Patterns"))SD.mkdir("/Samples/Patterns");
 String path="";
 for(int n=1;n<=9999;n++){
  char name[56];snprintf(name,sizeof(name),"/Samples/Patterns/PAT_%04d.pat",n);
  if(!SD.exists(name)){path=name;break;}
 }
 if(!path.length()){xSemaphoreGive(sdMutex);patternMessage="PATTERN FOLDER FULL";return false;}
 File f=SD.open(path,FILE_WRITE);
 if(!f){xSemaphoreGive(sdMutex);patternMessage="SAVE FAILED";return false;}
 f.println("ADVPAT1");f.println(seqBpm);
 for(int c=0;c<4;c++){
  f.println(samplePaths[c]);
  uint16_t mask=0;for(int t=0;t<16;t++)if(steps[c][t])mask|=(1u<<t);
  f.println(mask);
 }
 f.flush();f.close();xSemaphoreGive(sdMutex);
 patternMessage=path.substring(path.lastIndexOf('/')+1)+" SAVED";
 return true;
}
bool openPattern(const String &name){
 if(sdExported||!sdOK){patternMessage="SD NOT READY";return false;}
 if(!sdMutex||xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1500))!=pdTRUE){patternMessage="SD BUSY";return false;}
 File f=SD.open("/Samples/Patterns/"+name,FILE_READ);
 String paths[4];uint16_t masks[4];int bpm=0;bool ok=false;
 if(f){
  String signature=f.readStringUntil('\n');signature.trim();
  String tempo=f.readStringUntil('\n');tempo.trim();bpm=tempo.toInt();
  ok=(signature=="ADVPAT1"&&bpm>=40&&bpm<=240);
  for(int c=0;c<4&&ok;c++){
   paths[c]=f.readStringUntil('\n');paths[c].trim();
   String mask=f.readStringUntil('\n');mask.trim();
   for(size_t i=0;i<mask.length();i++)if(!isDigit(mask[i]))ok=false;
   long v=mask.toInt();if(mask.length()==0||v<0||v>65535)ok=false;
   masks[c]=(uint16_t)v;
  }
  f.close();
 }
 xSemaphoreGive(sdMutex);
 if(!ok){patternMessage="INVALID PATTERN";return false;}
 stopSeq();
 seqBpm=bpm;
 for(int c=0;c<4;c++){
  for(int t=0;t<16;t++)steps[c][t]=(masks[c]&(1u<<t))!=0;
  if(paths[c].length()){
   if(!loadSeqSample(c,paths[c])){
    // Keep pattern steps but clearly indicate the sample couldn't load.
    if(xSemaphoreTake(seqMutex,pdMS_TO_TICKS(1000))==pdTRUE){
     if(seqVoices[c].file)seqVoices[c].file.close();seqVoices[c]=SeqVoice{};samplePaths[c]="";
     xSemaphoreGive(seqMutex);
    }
   }
  }else{
   if(xSemaphoreTake(seqMutex,pdMS_TO_TICKS(1000))==pdTRUE){
    if(seqVoices[c].file)seqVoices[c].file.close();seqVoices[c]=SeqVoice{};samplePaths[c]="";
    xSemaphoreGive(seqMutex);
   }
  }
 }
 patternMessage=name+" OPENED";
 return true;
}
int menuSelection=0, lastCycleTrack=-1;
bool inMenu=true;
uint32_t lastPaint=0, lastKey=0, lastBatteryPaint=0;
uint32_t volumeChangedAt=0;
bool volumeDirty=false;
Preferences prefs;

// Decoder and SD file ownership belong only to decodeTask.
AudioFileSourceSD *mp3File=nullptr;
AudioGenerator *decoder=nullptr;
AudioOutputM5Speaker mp3Output(&M5Cardputer.Speaker,0);
std::atomic<bool> playing{false}, paused{false}, engineIdle{true};
std::atomic<int> active{-1};
std::atomic<int> requestedTrack{-1}; // -1 no request, -2 stop, >=0 queue index
std::atomic<int> finishedTrack{-1};

String basename(const String &s){int p=s.lastIndexOf('/');return p>=0?s.substring(p+1):s;}
// Trim by measured pixel width, not a guessed character count. No auto-wrap into
// the battery column or outside the physical display.
String fitPixels(const String &s,int maxPixels,bool ellipsis=false){
 auto &d=M5Cardputer.Display;
 if(maxPixels<=0)return "";
 if((int)d.textWidth(s)<=maxPixels)return s;
 String out=s;
 String tail=ellipsis?"...":"";
 while(out.length() && (int)d.textWidth(out+tail)>maxPixels)out.remove(out.length()-1);
 return out+tail;
}
// Page long titles instead of moving one character at a time.
// A page stays visible for 2.4 seconds; no animated redraws.
String titlePage(const String &s,int maxPixels,uint32_t page){
 auto &d=M5Cardputer.Display;
 if((int)d.textWidth(s)<=maxPixels)return s;
 std::vector<String> pages;
 int pos=0;
 while(pos<(int)s.length()){
  int end=pos;
  while(end<(int)s.length() && (int)d.textWidth(s.substring(pos,end+1))<=maxPixels)end++;
  if(end==pos)end++;
  pages.push_back(s.substring(pos,end));
  pos=end;
 }
 return pages.empty()?"":pages[page%pages.size()];
}
int menuFirst=0;
void keepMenuVisible(){
 int rows=max(1, (int)((M5Cardputer.Display.height()-29)/19));
 if(menuSelection<menuFirst)menuFirst=menuSelection;
 if(menuSelection>=menuFirst+rows)menuFirst=menuSelection-rows+1;
}
void batteryAtTop(){
 int battery=M5.Power.getBatteryLevel();
 if(battery>=0&&battery<=100){
  auto &d=M5Cardputer.Display;
  d.setTextColor(WHITE,BLACK);d.setCursor(d.width()-51,3);d.printf("%d%%",battery);
 }
}
void draw(bool full=true){
 auto &d=M5Cardputer.Display;
 int w=d.width(),h=d.height();
 uint32_t phase=millis()/2400;
 if(full)d.fillScreen(BLACK);
 d.setTextSize(1);d.setTextFont(2);
 d.setTextWrap(false);
 // Battery occupies only the top-right header. All body rows get full width.
 d.fillRect(0,0,w,25,BLACK);
 d.setCursor(4,3);d.setTextColor(WHITE,BLACK);
 String header;
 if(mode==HOME)header="MUSIC";
 else if(mode==FILE_BROWSER)header="FILE PLAYER";
 else if(mode==LAUNCHER)header="ADV SYSTEM r1.2";
 else if(mode==SEQUENCER)header=String(seqBpm)+" BPM";
 else if(mode==SAMPLE_BROWSER)header="SAMPLES";
 else if(mode==PATTERN_BROWSER)header="PATTERNS";
 else if(mode==SD_APP)header="SD";
 else if(mode==DIAG_APP)header="DIAG";
 else if(mode==NOTES_APP||mode==NOTES_FILES||mode==NOTES_MENU)header="NOTES";
 else if(mode==FLASH_BROWSER)header="FLASH";
 else if(mode==GENRE_PLAYER && menuSelection>=0 && menuSelection<GENRE_COUNT)header=GENRE_NAMES[menuSelection];
 else header="PLAYER";
 d.print(fitPixels(header,w-59,true));
 batteryAtTop();
 if(!full)return;
 d.drawFastHLine(0,25,w,0x7BEF);
 if(mode==LAUNCHER){
  const char* labels[]={"MUSIC","SD","SEQUENCER","FLASH","NOTES","DIAG"};
  int gap=5, top=28, boxW=(w-3*gap)/2, boxH=(h-top-4*gap)/3;
  for(int i=0;i<6;i++){
   int x=gap+(i%2)*(boxW+gap),y=top+gap+(i/2)*(boxH+gap);
   d.fillRoundRect(x,y,boxW,boxH,5,i==launcherChoice?0x2945:0x2104);
   d.drawRoundRect(x,y,boxW,boxH,5,i==launcherChoice?0x07E0:0x8410);
   d.setTextColor(WHITE,i==launcherChoice?0x2945:0x2104);
   d.setCursor(x+(boxW-d.textWidth(labels[i]))/2,y+(boxH-d.fontHeight())/2);d.print(labels[i]);
  }
  return;
 }
 if(mode==NOTES_APP){
  d.setTextColor(0xBDF7,BLACK);d.setCursor(4,28);
  d.print(fitPixels(noteFile.isEmpty()?String("NEW NOTE"):noteFile,w-8,true));
  d.drawFastHLine(0,45,w,0x4208);
  // A minimal text viewport; manual save/open via the BACKTICK command menu.
  String visible=noteText.substring(noteScroll);
  int y=48;String line="";
  for(int i=0;i<(int)visible.length()&&y<h-14;i++){
   char c=visible[i];if(c=='\n'||d.textWidth(line+String(c))>w-9){
    d.setCursor(4,y);d.setTextColor(WHITE,BLACK);d.print(line);line="";y+=16;
    if(c=='\n')continue;
   }line+=c;
  }
  if(y<h-14){d.setCursor(4,y);d.setTextColor(WHITE,BLACK);d.print(line);}
  d.setTextColor(noteDirty?0xFD20:0x8410,BLACK);d.setCursor(4,h-13);
  d.print(fitPixels(noteMessage.length()?noteMessage:String("` MENU | ENTER NEWLINE"),w-8));
  return;
 }
 if(mode==FLASH_CONFIRM){
  d.setTextColor(WHITE,BLACK);d.setCursor(4,30);d.print("INSTALL AND RUN ONCE?");
  d.setCursor(4,55);d.print(fitPixels(flashCount?flashNames[flashChoice]:String("NO FILE"),w-8,true));
  d.setTextColor(0xFD20,BLACK);d.setCursor(4,82);d.print("ENTER INSTALL / ` CANCEL");
  d.setCursor(4,105);d.print(fitPixels(flashMessage,w-8,true));return;
 }
 if(mode==NOTES_FILES||mode==FLASH_BROWSER||mode==NOTES_MENU){
  bool flash=mode==FLASH_BROWSER;bool menu=mode==NOTES_MENU;
  d.setTextColor(WHITE,BLACK);d.setCursor(4,28);
  d.print(menu?"NOTE ACTIONS":(flash?"/Firmware/*.bin":"/Notes/*.txt"));
  int count=menu?5:(flash?flashCount:noteCount),selected=menu?noteMenuChoice:(flash?flashChoice:noteChoice);
  int start=max(0,selected-4);
  for(int i=start;i<count&&i<start+5;i++){
   d.setTextColor(i==selected?GREEN:WHITE,BLACK);d.setCursor(4,47+(i-start)*17);
   String name=menu?String(noteActions[i]):(flash?flashNames[i]:noteNames[i]);
   d.print(fitPixels(String(i==selected?"> ":"  ")+name,w-8,true));
  }
  if(!count){d.setCursor(4,50);d.print(flash?"NO .BIN FILES":"NO NOTES");}
  if(flash){d.setTextColor(0xFD20,BLACK);d.setCursor(4,h-14);d.print("ENTER: SELECT APP BIN");}
  return;
 }
 if(mode==SEQUENCER){
  const uint16_t colors[]={0x05FF,0xFD20,0xA51F,0x07E0};
  int x0=26, gap=2, sw=(w-x0-4-15*gap)/16;
  for(int c=0;c<4;c++){
   int y=30+c*24;
   // Channel-number button: white outline indicates loaded sample.
   d.fillRoundRect(2,y-1,21,19,3,BLACK);
   d.drawRoundRect(2,y-1,21,19,3,samplePaths[c].length()?WHITE:colors[c]);
   if(c==seqChannel&&seqCursor==0)d.drawRoundRect(1,y-2,23,21,3,WHITE);
   d.setTextColor(colors[c],BLACK);d.setCursor(8,y+2);
   d.printf("%d",c+1);
   for(int t=0;t<16;t++){
    int x=x0+t*(sw+gap);
    d.fillRect(x,y,sw,17,steps[c][t]?colors[c]:0x8410);
    if(c==seqChannel&&seqCursor==t+1)d.drawRect(x-1,y-1,sw+2,19,WHITE);
   }
  }
  if(patternMessage.length()){
   d.fillRect(0,h-12,w,12,BLACK);d.setTextColor(WHITE,BLACK);
   d.setCursor(4,h-13);d.print(fitPixels(patternMessage,w-8));
  }
  return;
 }
 if(mode==PATTERN_BROWSER){
  d.setTextColor(WHITE,BLACK);d.setCursor(4,28);d.print("/Samples/Patterns/");
  int start=patternChoice>=5?patternChoice-4:0;
  for(int i=start;i<patternCount&&i<start+5;i++){
   d.setTextColor(i==patternChoice?GREEN:WHITE,BLACK);
   d.setCursor(4,47+(i-start)*17);
   d.print(fitPixels(String(i==patternChoice?"> ":"  ")+patternNames[i],w-8,true));
  }
  if(!patternCount){d.setCursor(4,50);d.print("NO SAVED PATTERNS");}
  return;
 }
 if(mode==SAMPLE_BROWSER){
  d.setTextColor(WHITE,BLACK);d.setCursor(4,28);d.print("/Samples/");
  for(int i=0;i<sampleCount&&i<5;i++){
   d.setTextColor(i==sampleChoice?GREEN:WHITE,BLACK);
   d.setCursor(4,47+i*17);d.print(fitPixels(String(i==sampleChoice?"> ":"  ")+sampleNames[i],w-8,true));
  }
  if(!sampleCount){d.setCursor(4,50);d.print("NO WAV IN /Samples");}
  if(sampleError.length()){d.setTextColor(RED,BLACK);d.setCursor(4,h-16);d.print(fitPixels(sampleError,w-8));}
  return;
 }
 if(mode==SD_APP){
  d.setTextColor(WHITE,BLACK);d.setCursor(4,35);d.print("USB SD DRIVE");
  d.setCursor(4,56);d.print(sdExportError?"SD ERROR - ` RETRY":(sdExported?"USB DRIVE ACTIVE":"SD NOT EXPORTED"));
  d.setCursor(4,79);d.print("1. Eject in Windows");
  d.setCursor(4,102);d.print("2. ` to exit SD");return;
 }
 if(mode==DIAG_APP){
  // Read-only status dashboard; values are sampled when the screen is redrawn.
  // Do not claim a physical peripheral is connected merely because a driver exists.
  String mac=String((uint32_t)(ESP.getEfuseMac()>>32),HEX)+String((uint32_t)ESP.getEfuseMac(),HEX);
  mac.toUpperCase();
  String entries[]={
   String("SD mounted: ")+(sdOK&&!sdExported?"YES":"NO"),
   String("SD USB export: ")+(sdExported?"ACTIVE":"OFF"),
   String("SD capacity: ")+(sdOK?String((uint32_t)(SD.cardSize()/1048576ULL))+" MB":"N/A"),
   String("Battery: ")+String(M5.Power.getBatteryLevel())+"%",
   String("Mic: DISABLED"),
   String("Speaker: INITIALIZED"),
   String("Free heap: ")+String(ESP.getFreeHeap()/1024)+" KB",
   String("Min heap: ")+String(ESP.getMinFreeHeap()/1024)+" KB",
   String("Largest block: ")+String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)/1024)+" KB",
   String("CPU: ")+String(ESP.getCpuFreqMHz())+" MHz",
   String("Chip: ")+ESP.getChipModel(),
   String("Flash: ")+String(ESP.getFlashChipSize()/1048576)+" MB",
   String("Chip ID: ")+mac,
   String("Uptime: ")+String(millis()/1000)+" sec",
   String("Firmware: r1.1")
  };
  const int count=sizeof(entries)/sizeof(entries[0]);
  int rows=(h-30)/17;
  if(rows<1)rows=1;
  diagScroll=constrain(diagScroll,0,max(0,count-rows));
  for(int i=0;i<rows&&diagScroll+i<count;i++){
   d.setCursor(4,30+i*17);
   d.setTextColor(WHITE,BLACK);
   d.print(fitPixels(entries[diagScroll+i],w-14));
  }
  if(diagScroll>0)d.fillTriangle(w-8,30,w-13,37,w-3,37,WHITE);
  if(diagScroll+rows<count)d.fillTriangle(w-8,h-3,w-13,h-10,w-3,h-10,WHITE);
  return;
 }
 if(mode==HOME){
  int rows=max(1,(int)((h-29)/19));
  keepMenuVisible();
  int start=menuFirst;
  for(int i=start;i<MENU_COUNT&&i<start+rows;i++){
   int y=29+(i-start)*19;
   d.setTextColor(i==menuSelection?GREEN:WHITE,BLACK);d.setCursor(5,y);
   d.print(i==menuSelection?"> ":"  ");
   d.print(i<GENRE_COUNT?GENRE_NAMES[i]:"FILE PLAYER");
  }
  if(!sdOK){d.setTextColor(RED,BLACK);d.setCursor(4,h-18);d.print("SD CARD ERROR");}return;
 }
 if(mode==FILE_BROWSER){
  d.setTextColor(WHITE,BLACK);d.setCursor(4,28);d.print(fitPixels(browserPath,w-8,true));
  // Playing title gets its own full-width line. The list never animates.
  if(folderQueueActive&&active.load()>=0&&active.load()<trackCount){
   d.setTextColor(playing.load()?(paused.load()?0xFD20:GREEN):WHITE,BLACK);
   d.setCursor(4,46);
   d.print(titlePage(basename(tracks[active.load()]),w-8,phase));
  }
  int rows=max(1,(h-73)/18), start=max(0,browserSelection-rows/2);
  if(start+rows>browserCount)start=max(0,browserCount-rows);
  for(int i=start;i<browserCount&&i<start+rows;i++){
   bool isCurrent=folderQueueActive && active.load()>=0 && active.load()<trackCount && browserPaths[i]==tracks[active.load()];
   d.setTextColor(isCurrent&&playing.load()&&paused.load()?0xFD20:(i==browserSelection?GREEN:WHITE),BLACK);
   d.setCursor(4,69+(i-start)*18);
   String line=String(i==browserSelection?">":" ")+(browserFolders[i]?"[D] ":"    ")+browserNames[i];
   d.print(fitPixels(line,w-8,true));
  }
  if(!browserCount){d.setCursor(4,69);d.print("EMPTY FOLDER");}
  return;
 }
 if(mode==GENRE_PLAYER){
  if(!trackCount){d.setTextColor(WHITE,BLACK);d.setCursor(4,33);d.print("NO MP3 FILES");return;}
  int a=active.load();if(a<0||a>=trackCount)a=selection;
  if(a<0||a>=trackCount)return;
  // Song title directly under the header; no second copy of the genre name.
  d.setTextColor(playing.load()?(paused.load()?0xFD20:GREEN):WHITE,BLACK);
  d.setCursor(4,30);
  d.print(titlePage(basename(tracks[a]),w-8,phase));
  int indices[]={a-1,a,a+1};
  for(int row=0;row<3;row++){
   int idx=indices[row];if(idx<0||idx>=trackCount)continue;
   bool current=(row==1);
   d.setTextColor(current?(playing.load()?(paused.load()?0xFD20:GREEN):WHITE):WHITE,BLACK);
   d.setCursor(4,54+row*22);
   String line=String(current?"> ":"  ")+basename(tracks[idx]);
   d.print(fitPixels(line,w-8,true));
  }
  return;
 }
}
void stopDecoder(){
 if(decoder){decoder->stop();delete decoder;decoder=nullptr;}
 if(mp3File){mp3File->close();delete mp3File;mp3File=nullptr;}
 M5Cardputer.Speaker.stop();
 playing=false;paused=false;active=-1;
}
bool openMp3(int index){
 stopDecoder();
 if(index<0||index>=trackCount)return false;
 Serial.printf("AUDIO: opening %s\n",tracks[index].c_str());
 mp3File=new AudioFileSourceSD(tracks[index].c_str());
 if(!mp3File||!mp3File->isOpen()){Serial.println("MP3: open failed");stopDecoder();return false;}
 String path=tracks[index];path.toLowerCase();
 decoder=path.endsWith(".wav") ? (AudioGenerator*)new AudioGeneratorWAV() : (AudioGenerator*)new AudioGeneratorMP3();
 if(!decoder||!decoder->begin(mp3File,&mp3Output)){
  Serial.println("MP3: decoder begin failed");stopDecoder();return false;
 }
 active=index;playing=true;paused=false;
 Serial.println("MP3: started");
 return true;
}
void decodeTask(void*){
 for(;;){
  int req=requestedTrack.exchange(-1);
  if(req!=-1){
   engineIdle=false;
   if(sdMutex)xSemaphoreTake(sdMutex,portMAX_DELAY);
   if(req==-2)stopDecoder();
   else if(!openMp3(req))finishedTrack=req; // Skip unplayable files.
   engineIdle=true;
   if(sdMutex)xSemaphoreGive(sdMutex);
  }
  if(playing.load()&&!paused.load()&&decoder){
   engineIdle=false;
   if(sdMutex)xSemaphoreTake(sdMutex,portMAX_DELAY);
   if(!decoder->isRunning()||!decoder->loop()){
    int completed=active.load();
    Serial.printf("MP3: finished queue index %d\n",completed);
    stopDecoder();
    if(completed>=0)finishedTrack=completed;
   }
   engineIdle=true;
   if(sdMutex)xSemaphoreGive(sdMutex);
  }else vTaskDelay(pdMS_TO_TICKS(5));
  // Give folder navigation a chance to acquire the SD mutex between audio frames.
  // taskYIELD alone may immediately reschedule this higher-priority decoder task.
  vTaskDelay(pdMS_TO_TICKS(1));
 }
}
void stopTrack(){
 finishedTrack=-1;
 requestedTrack=-2;
 playing=false;paused=false;active=-1;
}
// All scanning/shuffling occurs on the UI task, never during decoder SD reads.
// Wait for decoder to acknowledge stop before touching the directory structure.
bool waitForDecoderStop(){
 stopTrack();
 uint32_t start=millis();
 while(millis()-start<3000){
  if(requestedTrack.load()==-1&&engineIdle.load()&&!playing.load())return true;
  delay(5);
 }
 Serial.println("MP3: stop timeout; scan canceled");
 return false;
}
void scanDir(const String &path,int depth){
 if(depth>8||trackCount>=MAX_TRACKS)return;
 File dir=SD.open(path);
 if(!dir||!dir.isDirectory())return;
 File f=dir.openNextFile();
 while(f&&trackCount<MAX_TRACKS){
  String n=String(f.name());
  String full=n.startsWith("/")?n:path+"/"+n;
  bool isdir=f.isDirectory();f.close();
  if(isdir)scanDir(full,depth+1);
  else{
   String lower=full;lower.toLowerCase();
   if(lower.endsWith(".mp3")){
   // Avoid memory exhaustion on very large collections.
   if(ESP.getFreeHeap()<45000){Serial.println("JUKEBOX: low heap, ending scan");break;}
   tracks.push_back(full);trackCount=(int)tracks.size();
  }
  }
  f=dir.openNextFile();
 }
 dir.close();
}
void shuffleTracks(int avoidLast){
 // Fisher-Yates shuffle: each track occurs exactly once per cycle.
 for(int i=trackCount-1;i>0;i--){
  int j=(int)random(i+1);
  String temp=tracks[i];tracks[i]=tracks[j];tracks[j]=temp;
 }
 if(trackCount>1&&avoidLast>=0&&avoidLast<trackCount){
  // avoidLast is the old last track's position only before a shuffle.
  // The previous song is saved as a path by the caller instead.
 }
}
void requestPlay(int index){
 if(index<0||index>=trackCount)return;
 finishedTrack=-1;
 requestedTrack=index;
 active=index;playing=true;paused=false;
 draw();
}
void startCategory(int category){
 if(!sdOK)return;
 if(!waitForDecoderStop())return;
 tracks.clear();trackCount=0;selection=0;lastCycleTrack=-1;
 String dir=GENRE_DIRS[category];
 scanDir(dir,0);
 Serial.printf("JUKEBOX: scanned %s, %d MP3 files; free heap=%u\n",dir.c_str(),trackCount,(unsigned)ESP.getFreeHeap());
 if(trackCount>0)shuffleTracks(-1);
 menuSelection=category;mode=GENRE_PLAYER;inMenu=false;
 draw();
 if(trackCount>0)requestPlay(0);
}
void returnToMenu(){
 if(!waitForDecoderStop())return;
 folderQueueActive=false;inMenu=true;mode=HOME;
 tracks.clear();trackCount=0;selection=0;lastCycleTrack=-1;
 // Do not scan SD here; the decoder may still be finishing a frame.
 draw();
}
void advance(int delta){
 if(mode==HOME||trackCount==0)return;
 int base=active.load();
 if(base<0||base>=trackCount)base=selection;
 selection=(base+delta+trackCount)%trackCount;
 requestPlay(selection);
}
void autoAdvance(int completed){
 if((mode!=GENRE_PLAYER&&!folderQueueActive)||trackCount==0||completed<0||completed>=trackCount)return;
 // A queued manual play or stop always wins over an end-of-track notification.
 if(requestedTrack.load()!=-1)return;
 int next=completed+1;
 if(next>=trackCount){
  if(folderQueueActive){folderFinished=true;selection=completed;draw();return;}
  String previous=tracks[completed];
  shuffleTracks(-1);
  if(trackCount>1&&tracks[0]==previous){
   int j=1+(int)random(trackCount-1);
   String temp=tracks[0];tracks[0]=tracks[j];tracks[j]=temp;
  }
  next=0;
  Serial.println("JUKEBOX: new shuffled cycle");
 }
 selection=next;
 requestPlay(next);
}
bool audioPath(String p){p.toLowerCase();return p.endsWith(".mp3")||p.endsWith(".wav");}
bool browserScanPending=false;
void scanBrowser(){
 // Never freeze the UI waiting indefinitely for SD while a track is decoding.
 if(sdMutex && xSemaphoreTake(sdMutex,pdMS_TO_TICKS(1000))!=pdTRUE){
  Serial.println("FILES: SD busy, folder scan deferred");
  browserScanPending=true;
  return;
 }
 browserScanPending=false;
 browserCount=0;browserSelection=0;
 File dir=SD.open(browserPath);
 if(!dir||!dir.isDirectory()){if(dir)dir.close();if(sdMutex)xSemaphoreGive(sdMutex);return;}
 File f=dir.openNextFile();
 while(f&&browserCount<MAX_ENTRIES){
  String name=String(f.name());
  String full=name.startsWith("/")?name:(browserPath=="/"?"/"+name:browserPath+"/"+name);
  bool folder=f.isDirectory();f.close();
  if(folder||audioPath(full)){
   browserNames[browserCount]=basename(full);browserPaths[browserCount]=full;
   browserFolders[browserCount++]=folder;
  }
  f=dir.openNextFile();
 }
 dir.close();
 if(sdMutex)xSemaphoreGive(sdMutex);
}
void openBrowser(){
 if(!sdOK||!waitForDecoderStop())return;
 mode=FILE_BROWSER;folderQueueActive=false;inMenu=false;browserPath="/";scanBrowser();draw();
}
void browserBack(){
 if(browserPath=="/"){folderQueueActive=false;returnToMenu();return;}
 int slash=browserPath.lastIndexOf('/');
 browserPath=slash<=0?"/":browserPath.substring(0,slash);
 scanBrowser();draw();
}
void chooseBrowserEntry(){
 if(browserCount==0)return;
 String target=browserPaths[browserSelection];
 if(browserFolders[browserSelection]){browserPath=target;scanBrowser();draw();return;}
 if(folderQueueActive && playing.load() && paused.load() && active.load()>=0 && active.load()<trackCount && tracks[active.load()]==target){paused=false;draw();return;}
 if(folderQueueActive && !playing.load() && selection>=0 && selection<trackCount && tracks[selection]==target){requestPlay(selection);return;}
 if(!waitForDecoderStop())return;
 folderPlayingPath=browserPath;
 tracks.clear();trackCount=0;selection=0;folderFinished=false;
 // Scan only this directory, not nested subdirectories, in directory order.
 File dir=SD.open(folderPlayingPath);
 if(dir&&dir.isDirectory()){
  File f=dir.openNextFile();
  while(f&&trackCount<MAX_TRACKS){
   String name=String(f.name());
   String full=name.startsWith("/")?name:(folderPlayingPath=="/"?"/"+name:folderPlayingPath+"/"+name);
   bool folder=f.isDirectory();f.close();
   if(!folder&&audioPath(full)){
    if(ESP.getFreeHeap()<45000){Serial.println("FILES: low heap, ending scan");break;}
    tracks.push_back(full);trackCount=(int)tracks.size();
   }
   f=dir.openNextFile();
  }
 }
 if(dir)dir.close();
 for(int i=0;i<trackCount;i++)if(tracks[i]==target){selection=i;break;}
 folderQueueActive=true;mode=FILE_BROWSER;scanBrowser();draw();requestPlay(selection);
}
void setup(){
 Serial.begin(115200);
 auto cfg=M5.config();
 cfg.internal_mic=false; // Explicitly request ADV ES8311 ADC microphone initialization.
 cfg.internal_spk=true;
 M5Cardputer.begin(cfg,true);
 
 M5Cardputer.Display.setRotation(0);
 prefs.begin("adv-jukebox",false);
 // Migrate existing 0..255 setting only once; thereafter persist 0..10.
 int oldVolume=constrain(prefs.getInt("volume",90),0,255);
 volume=constrain(prefs.getInt("vol10",(oldVolume*10+127)/255),0,10);
 M5Cardputer.Speaker.begin();M5Cardputer.Speaker.setVolume(speakerVolume());
 mp3Output.setup();
 SPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
 sdOK=SD.begin(SD_CS,SPI,25000000);
 sdMutex=xSemaphoreCreateMutex();
 seqMutex=xSemaphoreCreateMutex();
 // Initialize USB MSC before starting USB; medium is absent until SD app.
 msc.vendorID("M5STACK");msc.productID("ADV SD CARD");msc.productRevision("0.5");
 msc.onRead(mscRead);msc.onWrite(mscWrite);msc.onStartStop(mscStartStop);
 msc.mediaPresent(false);
 if(sdOK&&SD.sectorSize()==512&&SD.numSectors()>0){
  usbMSCStarted=msc.begin(SD.numSectors(),512);
 }else{
  // No card at boot: capacity unavailable; report via SD screen.
  sdExportError=true;
 }
 USB.begin();
 randomSeed((uint32_t)esp_random());
 Serial.printf("ADV SYSTEM r1.2 SD=%d volume=%d/10\n",sdOK,volume);
 draw();
 BaseType_t taskResult=xTaskCreatePinnedToCore(decodeTask,"mp3decode",8192,nullptr,2,nullptr,1);
 Serial.printf("MP3 decode task created=%d\n",taskResult==pdPASS);
 BaseType_t seqTaskResult=xTaskCreatePinnedToCore(seqAudioTask,"seqmix",4096,nullptr,1,nullptr,0);
 Serial.printf("Sequencer audio task created=%d\n",seqTaskResult==pdPASS);
}
void loop(){
 M5Cardputer.update();
 // Windows sent SCSI START STOP UNIT with LoEj=1, Start=0.
 // Do not re-enumerate here: Windows is completing the eject transaction.
 // Mark the medium absent. BACKTICK will do a full fresh USB enumeration.
 if(usbMediaEjectPending){
  usbMediaEjectPending=false;
  msc.mediaPresent(false);
  Serial.println("MSC: Windows eject received; medium marked absent");
 }

 static uint32_t lastBrowserRetry=0;
 if(mode==FILE_BROWSER && browserScanPending && millis()-lastBrowserRetry>=200){
  lastBrowserRetry=millis();scanBrowser();
  if(!browserScanPending)draw();
 }
 // Persist volume once after changes settle, to reduce NVS writes.
 if(volumeDirty&&millis()-volumeChangedAt>1800){
  prefs.putInt("vol10",volume);volumeDirty=false;
  Serial.printf("JUKEBOX: saved volume %d\n",volume);
 }
 int completed=finishedTrack.exchange(-1);
 if(completed>=0&&(mode==GENRE_PLAYER||folderQueueActive))autoAdvance(completed);
 static bool oldPlaying=false,oldPaused=false;
 static int oldActive=-1;
 static bool oldMenu=true;
 if(oldPlaying!=playing.load()||oldPaused!=paused.load()||oldActive!=active.load()||oldMenu!=inMenu){
  oldPlaying=playing;oldPaused=paused;oldActive=active;oldMenu=inMenu;
  draw();
 }
 if(millis()-lastPaint>=2400){
  lastPaint=millis();
  auto &d=M5Cardputer.Display;
  d.setTextSize(1);d.setTextFont(2);
  if(mode==GENRE_PLAYER&&trackCount>0){
   int a=active.load();if(a<0||a>=trackCount)a=selection;
   if(a>=0&&a<trackCount && (int)d.textWidth(basename(tracks[a]))>d.width()-8)draw();
  }else if(mode==FILE_BROWSER&&folderQueueActive&&active.load()>=0&&active.load()<trackCount){
   if((int)d.textWidth(basename(tracks[active.load()]))>d.width()-8)draw();
  }
 }
 if(millis()-lastBatteryPaint>=30000){lastBatteryPaint=millis();draw(false);}
 if(M5Cardputer.Keyboard.isChange()&&M5Cardputer.Keyboard.isPressed()&&millis()-lastKey>120){
  lastKey=millis();auto &k=M5Cardputer.Keyboard;
  // Direct physical keys: no Fn modifier required.
  // The colon-labelled key normally reports ';' without Fn/Shift.
  // Up/down are used in menus and browser only; genre shuffle has no list navigation.
  bool up=k.isKeyPressed(';')||k.isKeyPressed(':');
  bool down=k.isKeyPressed('.');
  bool left=k.isKeyPressed(',');
  bool right=k.isKeyPressed('/');
  bool stop=k.isKeyPressed(KEY_BACKSPACE);
  if(mode==LAUNCHER){
   if(left||right)launcherChoice=(launcherChoice%2==0)?launcherChoice+1:launcherChoice-1;
   else if(up)launcherChoice=(launcherChoice+4)%6;
   else if(down)launcherChoice=(launcherChoice+2)%6;
   else if(k.isKeyPressed(KEY_ENTER)){
    if(launcherChoice==0){mode=HOME;menuSelection=0;}
    else if(launcherChoice==1){mode=SD_APP;beginSDExport();}
    else if(launcherChoice==2){waitForDecoderStop();mode=SEQUENCER;}
    else if(launcherChoice==3){waitForDecoderStop();stopSeq();scanFirmware();mode=FLASH_BROWSER;}
    else if(launcherChoice==4){waitForDecoderStop();stopSeq();noteMessage="";mode=NOTES_APP;}
    else {mode=DIAG_APP;diagScroll=0;}
   }
   draw();
  }else if(mode==NOTES_APP){
   if(k.isKeyPressed('`')){noteMenuChoice=0;mode=NOTES_MENU;draw();}
   else if(k.isKeyPressed(KEY_BACKSPACE)){
    if(noteCursor>0){noteText.remove(noteCursor-1,1);noteCursor--;noteDirty=true;noteMessage="";draw();}
   }else if(left){noteCursor=max(0,noteCursor-1);}
   else if(right){noteCursor=min((int)noteText.length(),noteCursor+1);}
   else if(k.isKeyPressed(KEY_ENTER)){
    if(noteText.length()<8192){noteText=noteText.substring(0,noteCursor)+"\n"+noteText.substring(noteCursor);noteCursor++;noteDirty=true;noteMessage="";draw();}
   }else{
    String typed;
    for(char ch : k.keysState().word) typed += ch;
    if(typed.length()&&noteText.length()+typed.length()<=8192){
     noteText=noteText.substring(0,noteCursor)+typed+noteText.substring(noteCursor);
     noteCursor+=typed.length();noteDirty=true;noteMessage="";
     // Keep tail visible for long notes, without unbounded redraw work.
     if(noteCursor>noteScroll+120)noteScroll=noteCursor-120;
     draw();
    }
   }
  }else if(mode==NOTES_MENU){
   if(k.isKeyPressed('`')){mode=NOTES_APP;draw();}
   else if(up){noteMenuChoice=(noteMenuChoice+4)%5;draw();}
   else if(down){noteMenuChoice=(noteMenuChoice+1)%5;draw();}
   else if(k.isKeyPressed(KEY_ENTER)){
    if(noteMenuChoice==0){saveNote();mode=NOTES_APP;}
    else if(noteMenuChoice==1){if(noteDirty){noteMessage="SAVE FIRST";mode=NOTES_APP;}else{scanNotes();mode=NOTES_FILES;}}
    else if(noteMenuChoice==2){if(noteDirty){noteMessage="SAVE FIRST";mode=NOTES_APP;}else{noteText="";noteFile="";noteCursor=0;noteScroll=0;noteMessage="";mode=NOTES_APP;}}
    else if(noteMenuChoice==3){if(noteDirty){noteMessage="SAVE FIRST";mode=NOTES_APP;}else mode=LAUNCHER;}
    else mode=NOTES_APP;
    draw();
   }
  }else if(mode==NOTES_FILES){
   if(k.isKeyPressed('`')){mode=NOTES_APP;draw();}
   else if(up&&noteCount){noteChoice=(noteChoice+noteCount-1)%noteCount;draw();}
   else if(down&&noteCount){noteChoice=(noteChoice+1)%noteCount;draw();}
   else if(k.isKeyPressed(KEY_ENTER)&&noteCount){if(openNote(noteNames[noteChoice]))mode=NOTES_APP;draw();}
  }else if(mode==FLASH_CONFIRM){
   if(k.isKeyPressed('`')){mode=FLASH_BROWSER;draw();}
   else if(k.isKeyPressed(KEY_ENTER)&&flashCount){
    waitForDecoderStop();stopSeq();
    if(!installAndRunFirmware(flashNames[flashChoice]))draw();
   }
  }else if(mode==FLASH_BROWSER){
   if(k.isKeyPressed('`')){mode=LAUNCHER;draw();}
   else if(up&&flashCount){flashChoice=(flashChoice+flashCount-1)%flashCount;draw();}
   else if(down&&flashCount){flashChoice=(flashChoice+1)%flashCount;draw();}
   else if(k.isKeyPressed(KEY_ENTER)&&flashCount){flashMessage="";mode=FLASH_CONFIRM;draw();}
  }else if(mode==HOME){
   if(k.isKeyPressed('`')){mode=LAUNCHER;draw();}
   else if(up){menuSelection=(menuSelection+MENU_COUNT-1)%MENU_COUNT;keepMenuVisible();draw();}
   else if(down){menuSelection=(menuSelection+1)%MENU_COUNT;keepMenuVisible();draw();}
   else if(k.isKeyPressed(KEY_ENTER)){
    if(menuSelection<GENRE_COUNT)startCategory(menuSelection);
    else openBrowser();
   }
  }else if(mode==SEQUENCER){
   if(k.isKeyPressed('`')){stopSeq();mode=LAUNCHER;draw();}
   else if(left){seqCursor=(seqCursor+16)%17;draw();}
   else if(right){seqCursor=(seqCursor+1)%17;draw();}
   else if(up){seqChannel=(seqChannel+3)%4;draw();}
   else if(down){seqChannel=(seqChannel+1)%4;draw();}
   else if(k.isKeyPressed('w')){seqBpm=max(40,seqBpm-5);draw();}
   else if(k.isKeyPressed('e')){seqBpm=min(240,seqBpm+5);draw();}
   else if(k.isKeyPressed('s')){savePattern();draw();}
   else if(k.isKeyPressed('o')){stopSeq();scanPatterns();mode=PATTERN_BROWSER;draw();}
   else if(k.isKeyPressed(' ')&&seqCursor>0){steps[seqChannel][seqCursor-1]=!steps[seqChannel][seqCursor-1];draw();}
   else if(k.isKeyPressed(KEY_ENTER)){
    if(seqCursor==0){stopSeq();scanSamples();mode=SAMPLE_BROWSER;}
    else {if(seqRunning)stopSeq();else {seqRunning=true;seqAudioEnabled=true;seqStep=0;seqLastTick=millis();fireSeqStep(0);}}
    draw();
   }
   else if(k.isKeyPressed('1')||k.isKeyPressed('2')||k.isKeyPressed('3')||k.isKeyPressed('4')){
    for(int i=0;i<4;i++)if(k.isKeyPressed('1'+i)){seqChannel=i;break;}
    seqCursor=0;scanSamples();mode=SAMPLE_BROWSER;draw();
   }
  }else if(mode==PATTERN_BROWSER){
   if(k.isKeyPressed('`')){mode=SEQUENCER;draw();}
   else if(up&&patternCount){patternChoice=(patternChoice+patternCount-1)%patternCount;draw();}
   else if(down&&patternCount){patternChoice=(patternChoice+1)%patternCount;draw();}
   else if(k.isKeyPressed(KEY_ENTER)&&patternCount){
    if(openPattern(patternNames[patternChoice]))mode=SEQUENCER;
    draw();
   }
  }else if(mode==SAMPLE_BROWSER){
   if(k.isKeyPressed('`')){mode=SEQUENCER;draw();}
   else if(up&&sampleCount){sampleChoice=(sampleChoice+sampleCount-1)%sampleCount;draw();}
   else if(down&&sampleCount){sampleChoice=(sampleChoice+1)%sampleCount;draw();}
   else if(k.isKeyPressed(KEY_ENTER)&&sampleCount){
    String selected="/Samples/"+sampleNames[sampleChoice];
    if(loadSeqSample(seqChannel,selected))mode=SEQUENCER;
    draw();
   }
  }else if(mode==SD_APP){
   if(k.isKeyPressed('`')){
    // Manual release after Windows Eject. Some hosts do not send START_STOP.
    // Never depend on hostEjected to return to the launcher.
    // Also allow retry if the previous remount failed.
    bool ready=sdExported?endSDExport():remountLocalSD();
    if(ready){mode=LAUNCHER;draw();}
    else{sdExportError=true;draw();Serial.println("SD: remount failed; BACKTICK retries");}
   }
  }else if(mode==DIAG_APP){
   if(k.isKeyPressed('`')){mode=LAUNCHER;draw();}
   else if(up){diagScroll=max(0,diagScroll-1);draw();}
   else if(down){diagScroll++;draw();}
  }else if(mode==FILE_BROWSER){
   if(k.isKeyPressed('`'))browserBack();
   else if(up){if(browserCount)browserSelection=(browserSelection+browserCount-1)%browserCount;draw();}
   else if(down){if(browserCount)browserSelection=(browserSelection+1)%browserCount;draw();}
   else if(k.isKeyPressed(KEY_ENTER)){
    if(folderQueueActive&&playing.load()&&browserCount&&active.load()>=0&&active.load()<trackCount&&browserPaths[browserSelection]==tracks[active.load()]){
     paused=!paused.load();draw();
    }else chooseBrowserEntry();
   }
   else if(stop&&folderQueueActive){stopTrack();draw();}
   else if(left&&folderQueueActive)advance(-1);
   else if(right&&folderQueueActive)advance(1);
  }else{
   if(k.isKeyPressed('`')){if(mode==FOLDER_PLAYER){if(waitForDecoderStop()){mode=FILE_BROWSER;browserPath=folderPlayingPath;scanBrowser();draw();}}else returnToMenu();}
   else if(k.isKeyPressed(KEY_ENTER)){if(playing.load()){paused=!paused.load();draw();}else if(trackCount>0){requestPlay(selection);}}
   else if(left)advance(-1);
   else if(right)advance(1);
   else if(stop){stopTrack();draw();}
  }
  if(k.isKeyPressed('=')){
   volume=min(10,volume+1);M5Cardputer.Speaker.setVolume(speakerVolume());
   volumeDirty=true;volumeChangedAt=millis();
   Serial.printf("VOL: %d/10 (%d/255)\n",volume,(int)speakerVolume());
  }else if(k.isKeyPressed('-')){
   volume=max(0,volume-1);M5Cardputer.Speaker.setVolume(speakerVolume());
   volumeDirty=true;volumeChangedAt=millis();
   Serial.printf("VOL: %d/10 (%d/255)\n",volume,(int)speakerVolume());
  }
 }
 if(mode==SEQUENCER&&seqRunning&&millis()-seqLastTick>=(uint32_t)(60000UL/(seqBpm*4))){
  seqLastTick=millis();seqStep=(seqStep+1)%16;fireSeqStep(seqStep);draw();
 }
 delay(1);
}
