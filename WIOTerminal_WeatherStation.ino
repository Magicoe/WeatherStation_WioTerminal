#include "TFT_eSPI.h"
TFT_eSPI tft;

#include "RTC_SAMD51.h"
#include "DateTime.h"
RTC_SAMD51 rtc;
DateTime now;
char daysOfTheWeek[7][4] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

#include "AnimatedGIF.h"
#define LCD_Type tft

#include <SPI.h>
#include <Seeed_FS.h>
#include "SD/Seeed_SD.h"

#include <Wire.h> 
#include "Adafruit_SHT31.h"  /* SHT32 humidity/temperature sensor */
#include "paj7620.h"         /* PAJ7620 Gesture Sensor */

#include "PMS.h"             /* PM2.5 Sensor*/

#include "rpcWiFi.h"         /* WIFI */

const char* ssid1 = "Mi 11";
const char* password1 = "123456789";

#include <HTTPClient.h>      /* GET/POST get weather report */
#include <math.h> 
#include <ArduinoJson.h>     /* decode JSON */
#include <vector>

#include <PNGdec.h>          /* decode GIF */
PNG png;

char g_DispBuf[128];

///////////////////////// GIF Player /////////////////////////
AnimatedGIF gif;

// rule: loop GIF at least during 3s, maximum 5 times, and don't loop/animate longer than 30s per GIF
const int maxLoopIterations =     5; // stop after this amount of loops
const int maxLoopsDuration  =  1000; // ms, max cumulated time after the GIF will break loop
const int maxGifDuration    = 10000; // ms, max GIF duration

// used to center image based on GIF dimensions
static int xOffset = 0;
static int yOffset = 0;

static int totalFiles = 0; // GIF files count
static int currentFile = 0;
static int lastFile = -1;
static File FSGifFile; // temp gif file holder
static File GifRootFolder; // directory listing
char GifComment[256];

static void * GIFOpenFile(const char *fname, int32_t *pSize)
{
  Serial.println("GIFOpenFile\n");
  FSGifFile = SD.open(fname, FILE_READ);
  if (FSGifFile) {
    *pSize = FSGifFile.size();
//    log_n("GIFOpenFile Success %d %x\n", *pSize, &FSGifFile);
    return &FSGifFile;
  }
//  log_n("GIFOpenFile Failed\n");
  return NULL;
}

static void GIFCloseFile(void *pHandle)
{
  File *f = static_cast<File *>(pHandle);
  if (f != NULL)
     f->close();
}

static int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  int32_t iBytesRead;
  iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  // Note: If you read a file all the way to the last byte, seek() stops working
  if ((pFile->iSize - pFile->iPos) < iLen)
      iBytesRead = pFile->iSize - pFile->iPos - 1; // <-- ugly work-around
  if (iBytesRead <= 0)
  {
//      log_n("GIFReadFile : iBytesRead <= 0\n");
      return 0;
  }
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}

static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition)
{
  int i = micros();
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  i = micros() - i;
//  log_d("Seek time = %d us\n", i);
  return pFile->iPos;
}

static void TFTDraw(int x, int y, int w, int h, uint16_t* lBuf )
{
  LCD_Type.pushRect( x+xOffset, y+yOffset, w, h, lBuf );
}

// Draw a line of image directly on the LCD
void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y, iWidth;

  iWidth = pDraw->iWidth;
  if (iWidth > LCD_Type.width())
      iWidth = LCD_Type.width();
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y + 18; // current line

  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) {// restore to background color
    for (x=0; x<iWidth; x++) {
      if (s[x] == pDraw->ucTransparent)
          s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }
  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) { // if transparency used
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0; // count non-transparent pixels
    while(x < iWidth) {
      c = ucTransparent-1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) { // done, stop
          s--; // back up to treat it like transparent
        } else { // opaque
            *d++ = usPalette[c];
            iCount++;
        }
      } // while looking for opaque pixels
      if (iCount) { // any opaque pixels?
        TFTDraw( pDraw->iX+x, y, iCount, 1, (uint16_t*)usTemp );
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent)
            iCount++;
        else
            s--;
      }
      if (iCount) {
        x += iCount; // skip these
        iCount = 0;
      }
    }
  } else {
    s = pDraw->pPixels;
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    for (x=0; x<iWidth; x++)
      usTemp[x] = usPalette[*s++];
   // log_n("color %x\n", *(uint16_t*)usTemp);
    TFTDraw( pDraw->iX, y, iWidth, 1, (uint16_t*)usTemp );
  }
} /* GIFDraw() */

int GIF_PlayInit(char* gifPath) // "/weather/human.gif"
{
  gif.begin(BIG_ENDIAN_PIXELS);
  int ret;
  ret = gif.open((const char *)gifPath, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw );
  if( ! ret ) {
    int lasterror;
    lasterror = gif.getLastError();
//    log_n("Could not open gif %s %d %d", gifPath, ret, lasterror );
    return maxLoopsDuration;
  }
  int frameDelay = 0; // store delay for the last frame
  int then = 0; // store overall delay
  bool showcomment = false;

  // center the GIF !!
  int w = gif.getCanvasWidth();
  int h = gif.getCanvasHeight();
  xOffset = ( LCD_Type.width()  - w )  /2;
  yOffset = ( LCD_Type.height() - h ) /2;

  if( lastFile != currentFile ) {
//    log_n("Playing %s [%d,%d] with offset [%d,%d]", gifPath, w, h, xOffset, yOffset );
    lastFile = currentFile;
    showcomment = true;
  }

  //LCD_Type.fillScreen(0x1ddf);    //Fill the screen with lightgray
  LCD_Type.fillScreen(0xDF1D);    //Fill the screen with lightgray
}

void GIF_PlayTask(void)
{
  int frameDelay = 0; // store delay for the last frame
  int then = 0; // store overall delay
  bool showcomment = false;
  
  while (gif.playFrame(true, &frameDelay)) {
    then += frameDelay;
    if( then > maxGifDuration ) { // avoid being trapped in infinite GIF's
      //log_w("Broke the GIF loop, max duration exceeded");
      break;
    }
  }
}

void GIF_PlayEnd(void)
{
  gif.close();
}

///////////////////////// SHT32 Sensor Setup&Task /////////////////////////
Adafruit_SHT31 sht31 = Adafruit_SHT31();
void SHT31_Init(void)
{
  tft.println("Hello SHT31");
  if (! sht31.begin(0x44)) {   // Set to 0x45 for alternate i2c addr
    tft.println("Couldn't find SHT31");
    while (1) delay(1);
  }
  tft.print("Heater Enabled State: ");
  if (sht31.isHeaterEnabled())
    tft.println("ENABLED");
  else
    tft.println("DISABLED");
}

void SHT31_Task(float *t, float *h)
{
  // put your main code here, to run repeatedly:
  *t = sht31.readTemperature();
  *h = sht31.readHumidity();
}

///////////////////////// PM2.5 Sensor Setup /////////////////////////
PMS pms(Serial1);
PMS::DATA data;

void PM25Sensor_Init(void)
{
  Serial1.begin(9600);
}

void PM25Sensor_Read(uint16_t *value)
{
  if (pms.read(data))
  {
//    tft.print("(ug/m3) PM 1.0 : ");
//    tft.print(data.PM_AE_UG_1_0);

//    tft.print("  PM 2.5 : ");
//    tft.print(data.PM_AE_UG_2_5);

//    tft.print("  PM 10.0 : ");
//    tft.println(data.PM_AE_UG_10_0);
    *value = data.PM_AE_UG_2_5;
    Serial.print("  PM 2.5 : ");
    Serial.println(data.PM_AE_UG_2_5);
  }
}

///////////////////////// PAJ7620 Gesture Sensor Setup&Task /////////////////////////
#define GES_REACTION_TIME    500       // You can adjust the reaction time according to the actual circumstance.
#define GES_ENTRY_TIME       800       // When you want to recognize the Forward/Backward gestures, your gestures' reaction time must less than GES_ENTRY_TIME(0.8s). 
#define GES_QUIT_TIME        1000

void GestureSensor_Init(void)
{
  uint8_t error = 0;
  error = paj7620Init();      // initialize Paj7620 registers
   if (error) {
        LCD_Type.print("INIT ERROR,CODE:");
        LCD_Type.println(error);
    } else {
        LCD_Type.println("INIT OK");
    }
    LCD_Type.println("Please input your gestures:\n");
    
}

void GestureSensor_Task(char *value)
{
    uint8_t data = 0, data1 = 0, error;
    
    error = paj7620ReadReg(0x43, 1, &data);        // Read Bank_0_Reg_0x43/0x44 for gesture result.
    if (!error) {
        switch (data) {               // When different gestures be detected, the variable 'data' will be set to different values by paj7620ReadReg(0x43, 1, &data).
            case GES_RIGHT_FLAG:
                delay(GES_ENTRY_TIME);
                paj7620ReadReg(0x43, 1, &data);
                if (data == GES_FORWARD_FLAG) {
                    Serial.println("Forward");
                    delay(GES_QUIT_TIME);
                } else if (data == GES_BACKWARD_FLAG) {
                    Serial.println("Backward");
                    delay(GES_QUIT_TIME);
                } else {
                    Serial.println("Right");
                    *value = 0x01; //====> LEFT
                }
                break;
            case GES_LEFT_FLAG:
                delay(GES_ENTRY_TIME);
                paj7620ReadReg(0x43, 1, &data);
                if (data == GES_FORWARD_FLAG) {
                    Serial.println("Forward");
                    delay(GES_QUIT_TIME);
                } else if (data == GES_BACKWARD_FLAG) {
                    Serial.println("Backward");
                    delay(GES_QUIT_TIME);
                } else {
                    Serial.println("Left");
                    *value = 0x02;
                }
                break;
            case GES_UP_FLAG:
                delay(GES_ENTRY_TIME);
                paj7620ReadReg(0x43, 1, &data);
                if (data == GES_FORWARD_FLAG) {
                    Serial.println("Forward");
                    delay(GES_QUIT_TIME);
                } else if (data == GES_BACKWARD_FLAG) {
                    Serial.println("Backward");
                    delay(GES_QUIT_TIME);
                } else {
                    Serial.println("Up");
                    *value = 0x04;
                }
                break;
            case GES_DOWN_FLAG:
                delay(GES_ENTRY_TIME);
                paj7620ReadReg(0x43, 1, &data);
                if (data == GES_FORWARD_FLAG) {
                    Serial.println("Forward");
                    delay(GES_QUIT_TIME);
                } else if (data == GES_BACKWARD_FLAG) {
                    Serial.println("Backward");
                    delay(GES_QUIT_TIME);
                } else {
                    Serial.println("Down");
                    *value = 0x08;
                }
                break;
            case GES_FORWARD_FLAG:
                Serial.println("Forward");
                delay(GES_QUIT_TIME);
                break;
            case GES_BACKWARD_FLAG:
                Serial.println("Backward");
                delay(GES_QUIT_TIME);
                *value = 0x10;
                break;
            case GES_CLOCKWISE_FLAG:
                Serial.println("Clockwise");
                *value = 0x20;
                break;
            case GES_COUNT_CLOCKWISE_FLAG:
                Serial.println("anti-clockwise");
                *value = 0x40;
                break;
            default:
                paj7620ReadReg(0x44, 1, &data1);
                if (data1 == GES_WAVE_FLAG) {
                    Serial.println("wave");
                    *value = 0x80;
                }
                break;
        }
    }
}

///////////////////////// NTP sync Task /////////////////////////

unsigned int localPort = 2390;      // local port to listen for UDP packets
char timeServer[] = "time.nist.gov"; // extenral NTP server e.g. time.nist.gov
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
// define WiFI client
WiFiClient client;
//The udp library class
WiFiUDP udp;
// localtime
unsigned long devicetime;
 
// send an NTP request to the time server at the given address
unsigned long sendNTPpacket(const char* address) {
    // set all bytes in the buffer to 0
    for (int i = 0; i < NTP_PACKET_SIZE; ++i) {
        packetBuffer[i] = 0;
    }
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;
 
    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    udp.beginPacket(address, 123); //NTP requests are to port 123
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
}

 
void NTPSync_Task(void)
{
  // Set WiFi to station mode and disconnect from an AP if it was previously connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  LCD_Type.println("Connecting to WiFi..");
  WiFi.begin(ssid1, password1);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      LCD_Type.println("Connecting to WiFi..");
      WiFi.begin(ssid1, password1);
  }
  LCD_Type.println("Connected to the WiFi network");
  LCD_Type.print("IP Address: ");
  LCD_Type.println (WiFi.localIP()); // prints out the device's IP address
  
  udp.begin(WiFi.localIP(), localPort);
  sendNTPpacket(timeServer); // send an NTP packet to a time server
  // wait to see if a reply is available
  delay(1000);

  // getNTPtime
  if (udp.parsePacket()) {
      LCD_Type.println("udp packet received");
      LCD_Type.println("");
      // We've received a packet, read the data from it
      udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

      //the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, extract the two words:

      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
      const unsigned long seventyYears = 2208988800UL;
      // subtract seventy years:
      unsigned long epoch = secsSince1900 - seventyYears;

      // adjust time for timezone offset in secs +/- from UTC
      // WA time offset from UTC is +8 hours (28,800 secs)
      // + East of GMT
      // - West of GMT
      long tzOffset = 28800UL;

      // WA local time 
      unsigned long adjustedTime;
      //return adjustedTime = epoch + tzOffset;
      adjustedTime = epoch + tzOffset;
      rtc.adjust(DateTime(adjustedTime));
      // get and print the adjusted rtc time
      now = rtc.now();
      LCD_Type.print("Adjusted RTC time is: ");
      LCD_Type.println(now.timestamp(DateTime::TIMESTAMP_FULL));
  }
  else {
      // were not able to parse the udp packet successfully
      // clear down the udp connection
      udp.stop();
      LCD_Type.println("NTP sync Time failed!");
      //return 0; // zero indicates a failure
  }
  // not calling ntp time frequently, stop releases resources
  udp.stop();
  WiFi.disconnect(true);
  delay(2000); // Delay 2 seconds and will back to Time/Data display Screen
  //while(1);
}

///////////////////////// Get wether report Task /////////////////////////
#define WEATHER_0  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322513924-8d39d7ad-2558-4775-80aa-ed2bc61dce66.png"
#define WEATHER_1  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322513835-60507db3-4688-40b5-a9cd-4859de4a56e4.png"
#define WEATHER_2  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322513872-7d61a2ec-297e-4ed4-b039-ac81147ed5ca.png"
#define WEATHER_3  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322513870-36aaae3f-230b-4717-b5a8-a937978df48c.png"
#define WEATHER_4  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322513924-80d1b83c-7aaf-4a64-80c3-e0cd93b18b61.png"
#define WEATHER_5  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514334-3f8e5599-faf9-4551-855f-f6cc62d203e3.png"
#define WEATHER_6  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514343-f48ebe31-cba6-4179-a157-4e5b2d4d12ae.png"
#define WEATHER_7  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514455-c13da7b9-cd30-48ad-b6ef-af0e018e8f26.png"
#define WEATHER_8  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514477-ee5f1465-1df0-45f6-a18d-8e341668a264.png"
#define WEATHER_9  "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514484-9fd9584e-3d22-4dc3-8a0a-ee5972222b51.png"
#define WEATHER_10 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514773-6cc8758b-08fe-48cb-b769-7c6d0d10cd56.png"
#define WEATHER_11 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322514822-ae80b7be-a988-4503-8515-f45b548d8dd9.png"
#define WEATHER_12 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515055-240079dc-1fcf-49c3-8284-705b55a9ffb5.png"
#define WEATHER_13 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515069-d7fe8530-bb74-4033-8aaf-838e8ea00cd9.png"
#define WEATHER_14 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515170-f7b6361a-c430-43a0-92bc-e77d4ca122ee.png"
#define WEATHER_15 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515257-3bb269c2-0c65-4a92-be42-8df4e4197c52.png"
#define WEATHER_16 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515380-ac4caa6d-2e68-457b-a016-e1b60b884064.png"
#define WEATHER_17 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516301-94df2c74-eb20-4671-80bc-3bfddcbfa1e4.png"
#define WEATHER_18 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515679-46d9b6e0-458d-4308-888a-b23cf13689f2.png"
#define WEATHER_19 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515788-3a4bfbf3-de34-4a3e-a6a5-b424cef191a3.png"
#define WEATHER_20 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322515881-2a668ed4-082d-4129-8b24-66f79355f75d.png"
#define WEATHER_21 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516017-291c49e3-6014-47a1-b9cf-700a1129992e.png"
#define WEATHER_22 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516334-67913a0c-0436-4e88-baac-4262c3f47a9d.png"
#define WEATHER_23 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516441-ad6f5030-270d-4877-9e52-565aa8a35f80.png"
#define WEATHER_24 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516491-2a49cd2f-0554-4b63-81b8-8f10bb6a4d5c.png"
#define WEATHER_25 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516798-d02ca1b2-142b-4368-b9ed-7444e2135af1.png"
#define WEATHER_26 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516776-cc4d0148-d640-429d-8d6a-dee1ca60389e.png"
#define WEATHER_27 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516810-82cc50d8-129c-4a88-a9d1-fcc4281be7e8.png"
#define WEATHER_28 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322516962-236feb6b-a6f3-4ae8-be10-a98c19954bb8.png"
#define WEATHER_29 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517080-2cc1cd8f-6a3b-4f06-9c58-fb266dbd25fb.png"
#define WEATHER_30 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517351-4f14defc-65d9-411a-8ff9-a273c4e0ae12.png"
#define WEATHER_31 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517295-36b5ec42-5157-41cd-bd59-715075a13208.png"
#define WEATHER_32 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517532-bf49e806-2e6f-4496-86c5-a13208c022fa.png"
#define WEATHER_33 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517488-59030409-ffbd-4f94-b0ba-a3984f6f233e.png"
#define WEATHER_34 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517670-dd633d28-9b6c-43de-9a6f-a120fad9d856.png"
#define WEATHER_35 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517791-6968fa92-7382-489f-91dc-31131c57850f.png"
#define WEATHER_36 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322517908-0dfc7d9e-0d13-49c5-9e17-167571d62456.png"
#define WEATHER_37 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322518040-766e01dc-1532-4f57-8275-88777deeb017.png"
#define WEATHER_38 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322518031-14f319d2-5a5f-4f0a-af96-41ad800a2ccf.png"
#define WEATHER_99 "http://cdn.nlark.com/yuque/0/2021/png/1013239/1619322518462-422e9768-739f-4686-8daa-418c53f613f8.png"

const char *host = "api.seniverse.com";
const char *privateKey = "SbiDME8QHMpPNcLux";
const char *city = "shanghai";
const char *language = "en";

struct WetherData
{
    char city[32];
    char date[16];
    char weather[64];
    char code_day[8];
    char code_night[8];
    char high[32];
    char low[32];
    char humi[32];
};

/* Display PNG */
double g_scale = 1.0;
int x_offset = 160 - 30;
int y_offset = 120 - 20 - 50;

int remain = 0;
int httptimeout = 0;
int fed;

char file_name[128];
static File myfile; // temp gif file holder
int g_FileLength = 0;

void * myOpen(const char *filename, int32_t *size) {
  Serial.printf("Attempting to open %s\n", filename);
  myfile = SD.open(filename);
  *size = myfile.size();
  return &myfile;
}

void myClose(void *handle) {
  if (myfile) myfile.close();
}

int32_t myRead(PNGFILE *handle, uint8_t *buffer, int32_t length) {
  if (!myfile) return 0;
  return myfile.read(buffer, length);
}

int32_t mySeek(PNGFILE *handle, int32_t position) {
  if (!myfile) return 0;
  return myfile.seek(position);
}

// Function to draw pixels to the display
void PNGDraw(PNGDRAW *pDraw) {
  uint16_t usPixels[320];
  png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

  int i;
  for(i=0; i<pDraw->iWidth; i++)
  {
    LCD_Type.drawPixel(x_offset+i, pDraw->y + y_offset, usPixels[i]);
  }
}

void load_png_sdcard(int index, int x, int y, double scale = 1.0)
{
  x_offset = x;
  y_offset = y;
  
  Serial.printf("load_png_sdcard png %d\n", index);
  memset(file_name, 0x00, 128);
  sprintf(file_name, "/weather/white/%d@1x.png", index);
  int rc;
  rc = png.open((const char *)file_name, myOpen, myClose, myRead, mySeek, PNGDraw);
  if (rc == PNG_SUCCESS) {
    Serial.printf("image specs: (%d x %d), %d bpp, pixel type: %d\n", png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType());
    rc = png.decode(NULL, 0);
    png.close();
  }
  delay(10);
}

StaticJsonDocument<4096> doc;
DeserializationError error;
void WeatherNowDisp(void)  // Json数据解析并串口打印
{
  LCD_Type.fillScreen(TFT_BLACK);
  LCD_Type.setCursor(0, 0);
  LCD_Type.setTextColor(TFT_WHITE);
  LCD_Type.setTextSize(1);
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  LCD_Type.println("Connecting to WiFi..");
  WiFi.begin(ssid1, password1);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      LCD_Type.println("Connecting to WiFi..");
      WiFi.begin(ssid1, password1);
  }

  LCD_Type.println("Connected to the WiFi network");
  LCD_Type.print("IP Address: ");
  LCD_Type.println (WiFi.localIP()); // prints out the device's IP address
    
  HTTPClient http;
// change XXXXXXXXXXX as your key
#ifdef XXXXXXXXXXX
  http.begin("http://api.seniverse.com/v3/weather/now.json?key=XXXXXXXXXXX&location=shanghai&language=en&unit=c");
#endif
  int httpCode = http.GET();
  if(httpCode > 0) {
    LCD_Type.print("[HTTP] GET... ");
  }
  else {
    LCD_Type.print("HTTP ERROR:");
    http.end();
    return ;
  }

  String reportdata = http.getString(); 
  
  http.end();

  error = deserializeJson(doc, reportdata);

  if (error) {
    LCD_Type.print(F("deserializeJson() failed: "));
    LCD_Type.println(error.f_str());
    return;
  }
  JsonObject results_0 = doc["results"][0];

  JsonObject results_0_location = results_0["location"];
  const char* results_0_location_name = results_0_location["name"]; // City Information, like shanghai

  JsonObject results_0_now = results_0["now"];
  const char* results_0_now_text = results_0_now["text"]; // "Cloudy"天气状况
  const char* results_0_now_code = results_0_now["code"]; // "4"风力大小
  const char* results_0_now_temperature = results_0_now["temperature"]; // "31"温度

  LCD_Type.println(results_0_location_name);//通过串口打印出需要的信息
  LCD_Type.println(results_0_now_text);
  LCD_Type.println(results_0_now_code);
  LCD_Type.println(results_0_now_temperature);
  LCD_Type.print("\\r\\n");

  String weather_status;
  weather_status = String(results_0_now_text);

  String now_code = String(results_0_now_code);

  LCD_Type.fillScreen(TFT_BLACK);    //Fill the screen with red

  LCD_Type.setTextColor(TFT_YELLOW, TFT_BLACK);
  LCD_Type.setTextDatum(TC_DATUM);
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", results_0_location_name);
  LCD_Type.drawString(g_DispBuf, 160, 120, 4);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", results_0_now_text);
  LCD_Type.drawString(g_DispBuf, 160, 150, 4);
  
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "Temp: %s%cC", results_0_now_temperature, (char)248 );
  LCD_Type.drawString(g_DispBuf, 160, 180, 4);

  int pic_index = now_code.toInt();
  Serial.printf("File index %d\n", pic_index);
  load_png_sdcard(pic_index, 130, 50, 1);

  LCD_Type.setTextColor(TFT_WHITE, TFT_BLACK);
  LCD_Type.setTextDatum(L_BASELINE);
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "get now weather done");
  LCD_Type.drawString(g_DispBuf, 10, 230, 2);
  
  //log_n("get now weather done");
  WiFi.disconnect();
}


struct WetherData weatherdata_day1 = {0};
struct WetherData weatherdata_day2 = {0};
struct WetherData weatherdata_day3 = {0};

String day1_code;
String day2_code;
String day3_code;
  
void Weather3DaysDisp(void)  // Json数据解析并串口打印
{
  LCD_Type.fillScreen(TFT_BLACK);
  LCD_Type.setCursor(0, 0);
  LCD_Type.setTextColor(TFT_WHITE);
  LCD_Type.setTextSize(1);
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  LCD_Type.println("Connecting to WiFi..");
  WiFi.begin(ssid1, password1);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      LCD_Type.println("Connecting to WiFi..");
      WiFi.begin(ssid1, password1);
  }

  LCD_Type.println("Connected to the WiFi network");
  LCD_Type.print("IP Address: ");
  LCD_Type.println (WiFi.localIP()); // prints out the device's IP address
  
  HTTPClient http;

// change XXXXXXXXXXX as your key
#ifdef XXXXXXXXXXX
  http.begin("http://api.seniverse.com/v3/weather/daily.json?key=XXXXXXXXXXX&location=shanghai&language=en&unit=c&start=0&days=3");
#endif

  int httpCode = http.GET();
  if(httpCode > 0) {
    LCD_Type.print("[HTTP] GET... ");
  }
  else {
    LCD_Type.print("HTTP ERROR:\n");
    http.end();
    return ;
  }

  String reportdata = http.getString(); 
  
  http.end();

  WiFi.disconnect();
  
  error = deserializeJson(doc, reportdata);

  if (error) {
    LCD_Type.print(F("deserializeJson() failed: "));
    LCD_Type.println(error.f_str());
    return;
  }

  weatherdata_day1 = {0};
  weatherdata_day2 = {0};
  weatherdata_day3 = {0};
  
  strcpy(weatherdata_day1.city, doc["results"][0]["location"]["name"].as<const char *>());
  strcpy(weatherdata_day1.date, doc["results"][0]["daily"][0]["date"].as<const char *>());
  strcpy(weatherdata_day1.weather, doc["results"][0]["daily"][0]["text_day"].as<const char *>());
  strcpy(weatherdata_day1.code_day, doc["results"][0]["daily"][0]["code_day"].as<const char *>());
  strcpy(weatherdata_day1.high, doc["results"][0]["daily"][0]["high"].as<const char *>());
  strcpy(weatherdata_day1.low, doc["results"][0]["daily"][0]["low"].as<const char *>());
  strcpy(weatherdata_day1.humi, doc["results"][0]["daily"][0]["humidity"].as<const char *>());
  Serial.print("City  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day1.city);//通过串口打印出需要的信息
  Serial.print("day1 text  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day1.weather);//通过串口打印出需要的信息
  Serial.print("day1 high  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day1.high);//通过串口打印出需要的信息
  Serial.print("day1 low   ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day1.low);//通过串口打印出需要的信息
  Serial.print("day1 humi  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day1.humi);//通过串口打印出需要的信息

  strcpy(weatherdata_day2.date, doc["results"][0]["daily"][1]["date"].as<const char *>());
  strcpy(weatherdata_day2.weather, doc["results"][0]["daily"][1]["text_day"].as<const char *>());
  strcpy(weatherdata_day2.code_day, doc["results"][0]["daily"][1]["code_day"].as<const char *>());
  strcpy(weatherdata_day2.high, doc["results"][0]["daily"][1]["high"].as<const char *>());
  strcpy(weatherdata_day2.low, doc["results"][0]["daily"][1]["low"].as<const char *>());
  strcpy(weatherdata_day2.humi, doc["results"][0]["daily"][1]["humidity"].as<const char *>());
  Serial.print("day2 text  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day2.weather);//通过串口打印出需要的信息
  Serial.print("day2 high  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day2.high);//通过串口打印出需要的信息
  Serial.print("day2 low   ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day2.low);//通过串口打印出需要的信息
  Serial.print("day2 humi  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day2.humi);//通过串口打印出需要的信息

  strcpy(weatherdata_day3.date, doc["results"][0]["daily"][2]["date"].as<const char *>());
  strcpy(weatherdata_day3.weather, doc["results"][0]["daily"][2]["text_day"].as<const char *>());
  strcpy(weatherdata_day3.code_day, doc["results"][0]["daily"][2]["code_day"].as<const char *>());
  strcpy(weatherdata_day3.high, doc["results"][0]["daily"][2]["high"].as<const char *>());
  strcpy(weatherdata_day3.low, doc["results"][0]["daily"][2]["low"].as<const char *>());
  strcpy(weatherdata_day3.humi, doc["results"][0]["daily"][2]["humidity"].as<const char *>());
  Serial.print("day3 text  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day3.weather);//通过串口打印出需要的信息
  Serial.print("day3 high  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day3.high);//通过串口打印出需要的信息
  Serial.print("day3 low   ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day3.low);//通过串口打印出需要的信息
  Serial.print("day3 humi  ");//通过串口打印出需要的信息
  Serial.println(weatherdata_day3.humi);//通过串口打印出需要的信息

  day1_code= String(weatherdata_day1.code_day);
  day2_code= String(weatherdata_day2.code_day);
  day3_code= String(weatherdata_day3.code_day);
  
  LCD_Type.fillScreen(TFT_BLACK);    //Fill the screen with red

  LCD_Type.setTextColor(TFT_PINK, TFT_BLACK);
  LCD_Type.setTextDatum(BL_DATUM);

  int day1_x = 24;
  int day1_y = 40;

  LCD_Type.setTextDatum(BC_DATUM); // Center, Bottom Align
  
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", weatherdata_day1.date);
  LCD_Type.drawString(g_DispBuf, 54, 10, 2);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", weatherdata_day2.date);
  LCD_Type.drawString(g_DispBuf, 162, 10, 2);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", weatherdata_day3.date);
  LCD_Type.drawString(g_DispBuf, 270, 10, 2);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s - %s C", weatherdata_day1.low, weatherdata_day1.high);
  LCD_Type.drawString(g_DispBuf, 54,  150, 4);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s - %s C", weatherdata_day2.low, weatherdata_day2.high);
  LCD_Type.drawString(g_DispBuf, 162, 150, 4);
  
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s - %s C", weatherdata_day3.low, weatherdata_day3.high);
  LCD_Type.drawString(g_DispBuf, 270, 150, 4);


  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", weatherdata_day1.humi);
  LCD_Type.drawString(g_DispBuf, 54,  180, 4);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", weatherdata_day2.humi);
  LCD_Type.drawString(g_DispBuf, 162,  180, 4);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%s", weatherdata_day3.humi);
  LCD_Type.drawString(g_DispBuf, 270,  180, 4);
  
  LCD_Type.setTextDatum(BL_DATUM);

  int pic_index1 = day1_code.toInt();
  Serial.printf("File index1 %d\n", pic_index1);
  load_png_sdcard(pic_index1, day1_x, day1_y, 1);
  
  day1_x = 132;
  int pic_index2 = day2_code.toInt();
  Serial.printf("File index2 %d\n", pic_index2);
  load_png_sdcard(pic_index2, day1_x, day1_y, 1);
  
  day1_x = 240;
  int pic_index3 = day3_code.toInt();
  Serial.printf("File index3 %d\n", pic_index3);
  load_png_sdcard(pic_index3, day1_x, day1_y, 1);
  delay(10);
  
  LCD_Type.setTextColor(TFT_WHITE, TFT_BLACK);
  LCD_Type.setTextDatum(L_BASELINE);
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "get 3days weather done");
  LCD_Type.drawString(g_DispBuf, 10, 230, 2);
}

///////////////////////// Task Setup /////////////////////////

uint32_t g_SysTick = 0;
uint16_t g_pm25value = 0;
float g_temperature, g_humidity;
short DN;   //Returns the number of day in the year
short WN;   //Returns the number of the week in the year
char g_GuestureValue = 0;

void DayWeekNumber(unsigned int y, unsigned int m, unsigned int d, unsigned int w){
  int days[]={0,31,59,90,120,151,181,212,243,273,304,334};    // Number of days at the beginning of the month in a not leap year.
//Start to calculate the number of day
  if (m==1 || m==2){
    DN = days[(m-1)]+d;                     //for any type of year, it calculate the number of days for January or february
  }                        // Now, try to calculate for the other months
  else if ((y % 4 == 0 && y % 100 != 0) ||  y % 400 == 0){  //those are the conditions to have a leap year
    DN = days[(m-1)]+d+1;     // if leap year, calculate in the same way but increasing one day
  }
  else {                                //if not a leap year, calculate in the normal way, such as January or February
    DN = days[(m-1)]+d;
  }
// Now start to calculate Week number
  if (w==0){
    WN = (DN-7+10)/7;             //if it is sunday (time library returns 0)
  }
  else{
    WN = (DN-w+10)/7;        // for the other days of week
  }
}

void RoomStatus_DispTask(void)
{
 // if(g_SysTick%5000 == 10)
  {
    SHT31_Task(&g_temperature, &g_humidity);
    memset(g_DispBuf, 0x00, 128);
    sprintf(g_DispBuf, "TEMP");
    LCD_Type.drawString(g_DispBuf, 15, 110, 4);
    memset(g_DispBuf, 0x00, 128);
    sprintf(g_DispBuf, "%3.1f %cC", g_temperature, 0x6F);
    LCD_Type.fillRect(15, 130, 60, 20, 0xDF1D); /* Clean area */
    LCD_Type.drawString(g_DispBuf, 15, 130, 4);

    memset(g_DispBuf, 0x00, 128);
    sprintf(g_DispBuf, "HUMI");
    LCD_Type.drawString(g_DispBuf, 235, 110, 4);
    memset(g_DispBuf, 0x00, 128);
    sprintf(g_DispBuf, "%3.1f%c", g_humidity, 0x25);
    LCD_Type.fillRect(235, 130, 60, 20, 0xDF1D); /* Clean area */
    LCD_Type.drawString(g_DispBuf, 235, 130, 4);
  }

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "PM2.5");
  LCD_Type.drawString(g_DispBuf, 235, 190, 4);
  
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%d", g_pm25value);
  LCD_Type.fillRect(235, 210, 60, 20, 0xDF1D); /* Clean area */
  LCD_Type.drawString(g_DispBuf, 235, 210, 4);


  now = rtc.now();
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%02d:%02d", now.hour(), now.minute());
 // LCD_Type.fillRect(30, 5, 210, 64, 0xDF1D); /* Clean area */
  LCD_Type.drawString(g_DispBuf, 30, 5, 8);
  
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%02d", now.second());
  LCD_Type.drawString(g_DispBuf, 280, 60, 4);

  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "%02d-%02d", now.month(), now.day());
  LCD_Type.drawString(g_DispBuf, 10, 185, 6);

  LCD_Type.drawString(daysOfTheWeek[now.dayOfTheWeek()], 145, 190, 2);
  DayWeekNumber(now.year(), now.month(), now.day(), now.dayOfTheWeek());
  memset(g_DispBuf, 0x00, 128);
  sprintf(g_DispBuf, "WW%02d", WN);
  LCD_Type.drawString(g_DispBuf, 145, 210, 4);
  
  GIF_PlayTask();
}


int g_MenuIndex = 0;
void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  rtc.begin();
  now = DateTime(F(__DATE__), F(__TIME__));
  rtc.adjust(now);
    
  LCD_Type.begin();
  LCD_Type.setRotation(3);
 
  LCD_Type.fillScreen(TFT_BLACK); // fills entire the screen with colour red

  LCD_Type.println("Hello Magicoe");
  
  LCD_Type.print("Initializing SD card...");
  if (!SD.begin(SDCARD_SS_PIN, SDCARD_SPI)) {
    LCD_Type.println("initialization failed!");
    while (1);
  }
  LCD_Type.println("initialization done.");

#if 1
  SHT31_Init();
#endif

#if 1
  PM25Sensor_Init();
#endif

#if 1
  GestureSensor_Init(); 
#endif

  SHT31_Task(&g_temperature, &g_humidity);
  PM25Sensor_Read(&g_pm25value);
  GIF_PlayInit("/weather/human.gif"); // "/weather/human.gif"
  LCD_Type.setTextColor(TFT_BLACK, 0xDF1D);
  
  g_MenuIndex = 0;
  
}


void loop() {
  // put your main code here, to run repeatedly:
  GestureSensor_Task(&g_GuestureValue);
  if(g_GuestureValue != 0)
  {
    if(g_GuestureValue == 0x08) // =====> UP
    {
        if(g_MenuIndex == 1)
        {
          GIF_PlayEnd();
          g_MenuIndex = 2; /* GOTO NTP sync menu */
        }
        
    }
    
    if(g_GuestureValue == 0x01) // =====> LEFT
    {
       if(g_MenuIndex == 1)
       {
         GIF_PlayEnd();
         g_MenuIndex = 6; /* GOTO Get 3 days Weather men*/
       }

       if(g_MenuIndex == 5) g_MenuIndex = 0; /* GOTO Get 3 days Weather menu */
       if(g_MenuIndex == 7) g_MenuIndex = 0; /* GOTO Time/Date menu */
        
    }
    if(g_GuestureValue == 0x02) // =====> RIGHT
    {
        if(g_MenuIndex == 1){
          GIF_PlayEnd();
          g_MenuIndex = 4; /* GOTO Get daily Weather menu */
        }
        if(g_MenuIndex == 5) g_MenuIndex = 6; /* GOTO Get 3 days Weather menu */
        if(g_MenuIndex == 7) g_MenuIndex = 0; /* GOTO Get Time.Date menu */
        
    }
    g_GuestureValue = 0x00;
  }
  /*------------ Time/Date Display --------------*/
  if(g_MenuIndex == 0) /* Initialze Menu page 1 : display Time/Data */
  {
    LCD_Type.setTextDatum(TL_DATUM);
    GIF_PlayInit("/weather/human.gif"); // "/weather/human.gif"
    LCD_Type.setTextColor(TFT_BLACK, 0xDF1D);
    g_MenuIndex = 1;
  }
  if(g_MenuIndex == 1)
  {
    RoomStatus_DispTask();
  }
  
  /*------------ NTP Time Sync --------------*/
  if(g_MenuIndex == 2)
  {
    g_MenuIndex = 3;
    LCD_Type.fillScreen(TFT_BLACK); // fills entire the screen with colour black  
    LCD_Type.setTextColor(TFT_WHITE, TFT_BLACK);
    LCD_Type.drawString("NTP Sync ing...", 0, 00, 4);
    LCD_Type.setCursor(0, 30);
  }
  if(g_MenuIndex == 3)
  {
    // NTP Sync here  
    NTPSync_Task();
    g_MenuIndex = 0;
  }
  /*------------ Get Daily Weather Report Menu --------------*/
  if(g_MenuIndex == 4)
  {
    g_MenuIndex = 5;
// Get Daily Weather Report
    WeatherNowDisp();
  }
  if(g_MenuIndex == 5)
  {
    
  }
  /*------------ Get 3 days Weather Report Menu --------------*/
  if(g_MenuIndex == 6)
  {
    g_MenuIndex = 7;
// Get 3 days Weather Report
    Weather3DaysDisp();
  }
  if(g_MenuIndex == 7)
  {
    
  }
  if(g_SysTick%5000 == 10)
  {
    SHT31_Task(&g_temperature, &g_humidity);
  }
  PM25Sensor_Read(&g_pm25value);
  delay(1);
  g_SysTick++;
}
