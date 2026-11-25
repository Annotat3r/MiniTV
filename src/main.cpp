#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <I2S.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <vector>

// --- PIN DEFINITIONS ---
#define PIN_BTN_CONTROL   15
#define PIN_POT_VOL       26  // ADC0 (Physical Pin 31)

#define SD_CS             13
#define SD_SCK            10
#define SD_MOSI           11
#define SD_MISO           12

#define I2S_BCLK          6   // Physical Pin 9 (PIO I2S Bit Clock)
#define I2S_LRC           7   // Physical Pin 10 (PIO I2S Word Select)
#define I2S_DIN           8   // Physical Pin 11 (PIO I2S Data)

// --- GLOBAL HARDWARE OBJECTS ---
TFT_eSPI tft = TFT_eSPI();
JPEGDEC jpeg;
I2S i2sOutput(OUTPUT);

// --- INTER-CORE SHARED VARIABLES ---
std::vector<String> videoFiles;
int currentVideoIndex = 0;
volatile bool nextRequested = false;
volatile bool restartRequested = false;
volatile bool isPlaying = false;
volatile float volumeGain = 1.0f;
volatile bool sdReady = false;
volatile bool sdFailed = false;

// --- LOCK-FREE AUDIO RING BUFFER (CORE 1 -> CORE 0) ---
#define AUDIO_RING_SIZE 4096   // 4,096 mono samples (~185 ms cushion for tight A/V sync)
static int16_t audioRing[AUDIO_RING_SIZE];
volatile uint32_t ringHead = 0; // Written only by Core 1
volatile uint32_t ringTail = 0; // Written only by Core 0

// --- STATIC BUFFERS FOR STREAMING ---
static uint8_t jpegBuf[32768];        // 32 KB buffer for JPEG video frames
static int16_t sdAudioReadBuf[512];   // 512 mono samples read from SD per block
static int16_t stereoAudioBuf[512];   // 256 stereo pairs (L + R) pushed to I2S

// --- RANDOM EPISODE SELECTION ---
int getRandomVideoIndex(int currentIndex) {
  if (videoFiles.size() <= 1) return 0;

  int newIndex = currentIndex;
  while (newIndex == currentIndex) {
    newIndex = rp2040.hwrand32() % videoFiles.size();
  }
  return newIndex;
}

// --- JPEGDEC DRAW CALLBACK ---
int drawJPEGCallback(JPEGDRAW *pDraw) {
  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

// --- HELPER FUNCTIONS ---
void scanSDCard() {
  videoFiles.clear();
  File root = SD.open("/");
  while (File entry = root.openNextFile()) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      if (name.endsWith(".AVI") || name.endsWith(".avi")) {
        if (!name.startsWith("/")) {
          name = "/" + name;
        }
        videoFiles.push_back(name);
      }
    }
    entry.close();
  }
  root.close();
}

// Pushes mono PCM samples into the inter-core ring buffer (paces Core 1 to 1.0x speed)
void pushAudioToRing(const int16_t *samples, size_t count) {
  size_t idx = 0;
  while (idx < count && isPlaying && !nextRequested && !restartRequested) {
    uint32_t space = AUDIO_RING_SIZE - (ringHead - ringTail);
    if (space == 0) {
      delay(1); // Wait for Core 0 to drain audio at real-time speed
      continue;
    }

    uint32_t toWrite = min((uint32_t)(count - idx), space);
    uint32_t head = ringHead;
    for (uint32_t i = 0; i < toWrite; i++) {
      audioRing[(head + i) & (AUDIO_RING_SIZE - 1)] = samples[idx + i];
    }
    __dmb();
    ringHead = head + toWrite;
    idx += toWrite;
  }
}

void bufferAudioChunk(File &file, uint32_t chunkSize) {
  uint32_t bytesRemaining = chunkSize;

  while (bytesRemaining > 0 && isPlaying && !nextRequested && !restartRequested) {
    size_t bytesToRead = min((size_t)bytesRemaining, sizeof(sdAudioReadBuf));
    size_t bytesRead = file.read((uint8_t*)sdAudioReadBuf, bytesToRead);
    if (bytesRead == 0) break;

    size_t sampleCount = bytesRead / 2;
    pushAudioToRing(sdAudioReadBuf, sampleCount);

    bytesRemaining -= bytesRead;
  }
}

// Scans up to 32 KB into AVI headers to locate the 'movi' stream block
bool seekToMovi(File &file) {
  uint8_t buf[4];
  file.seek(0);
  while (file.position() < 32768 && file.available() >= 4) {
    if (file.read(buf, 4) == 4) {
      if (buf[0] == 'm' && buf[1] == 'o' && buf[2] == 'v' && buf[3] == 'i') {
        return true;
      }
      file.seek(file.position() - 3);
    }
  }
  return false;
}

// ================================================================================
// CORE 0: CONTINUOUS I2S AUDIO STREAMER, BUTTON SENSING, & VOLUME ADC
// ================================================================================
void setup() {
  pinMode(PIN_BTN_CONTROL, INPUT_PULLUP);
  analogReadResolution(10);

  Serial.begin(115200);

  // Initialize SPI1 for MicroSD Card
  SPI1.setSCK(SD_SCK);
  SPI1.setTX(SD_MOSI);
  SPI1.setRX(SD_MISO);
  SPI1.begin();

  if (!SD.begin(SD_CS, SPI1)) {
    Serial.println("SD Card Initialization Failed!");
    sdFailed = true;
    return;
  }

  scanSDCard();

  if (!videoFiles.empty()) {
    currentVideoIndex = rp2040.hwrand32() % videoFiles.size();
  }

  // Initialize I2S Audio hardware (Expanded to 8 buffers of 1024 bytes)
  i2sOutput.setBCLK(I2S_BCLK);
  i2sOutput.setDATA(I2S_DIN);
  i2sOutput.setBitsPerSample(16);
  i2sOutput.setBuffers(8, 1024);
  i2sOutput.begin(22050);

  sdReady = true;
}

void loop() {
  // 1. Single Button Control (Skip / Restart)
  static bool lastBtnState = HIGH;
  static unsigned long lastDebounceTime = 0;
  static unsigned long lastBtnPresstime = 0;
  static int btnClickCount = 0;

  bool currentBtnState = digitalRead(PIN_BTN_CONTROL);

  if (currentBtnState != lastBtnState) {
    if (millis() - lastDebounceTime > 40) {
      lastDebounceTime = millis();

      if (lastBtnState == HIGH && currentBtnState == LOW) {
        unsigned long now = millis();
        if (now - lastBtnPresstime < 750) {
          btnClickCount++;
        } else {
          btnClickCount = 1;
        }
        lastBtnPresstime = now;

        if (btnClickCount == 2) {
          restartRequested = true;
          btnClickCount = 0;
        }
      }
    }
    lastBtnState = currentBtnState;
  }

  if (btnClickCount == 1 && (millis() - lastBtnPresstime >= 750)) {
    nextRequested = true;
    btnClickCount = 0;
  }

  // 2. Analog Volume Sampling from Potentiometer (GPIO 26)
  static unsigned long lastVolReadTime = 0;
  if (millis() - lastVolReadTime >= 25) {
    lastVolReadTime = millis();
    int rawAdc = analogRead(PIN_POT_VOL);
    
    // Bottom-end mute deadband
    if (rawAdc < 15) {
      volumeGain = 0.0f;
    } else {
      volumeGain = (float)rawAdc / 1023.0f;
    }
  }

  // 3. Continuous I2S Audio Feeder (Optimized Integer Math)
  if (!isPlaying || nextRequested || restartRequested) {
    ringTail = ringHead; // Flush remaining audio immediately on stop/skip/restart
    delay(2);
  } else {
    uint32_t avail = ringHead - ringTail;
    if (avail > 0) {
      uint32_t toRead = min(avail, (uint32_t)256);
      uint32_t tail = ringTail;
      
      // Calculate an integer multiplier (0 to 256) to avoid slow float math in the loop
      int32_t volMult = (int32_t)(volumeGain * 256.0f);

      for (uint32_t i = 0; i < toRead; i++) {
        int16_t sample = audioRing[(tail + i) & (AUDIO_RING_SIZE - 1)];
        // Fast single-cycle integer multiplication and bit-shift
        int16_t scaled = (int16_t)((sample * volMult) >> 8);
        
        stereoAudioBuf[i * 2]     = scaled; // Left channel
        stereoAudioBuf[i * 2 + 1] = scaled; // Right channel
      }

      size_t bytesToWrite = toRead * 2 * sizeof(int16_t);
      size_t bytesWritten = i2sOutput.write((uint8_t*)stereoAudioBuf, bytesToWrite);
      uint32_t framesWritten = bytesWritten / (2 * sizeof(int16_t));

      if (framesWritten > 0) {
        __dmb();
        ringTail = tail + framesWritten;
      } else {
        // DMA buffers full: wait exactly 250us to avoid over-sleeping
        delayMicroseconds(250); 
      }
    } else {
      delayMicroseconds(250);
    }
  }
}

// ================================================================================
// CORE 1: AVI CHUNK PARSER & DISPLAY DECODER
// ================================================================================
void setup1() {
  tft.init();
  tft.setRotation(1);      // Landscape 160x128
  tft.setSwapBytes(true);  // Match RGB565_LITTLE_ENDIAN byte order from JPEGDEC
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, 10);
  tft.println("Mounting SD card...");
}

void loop1() {
  if (!sdReady && !sdFailed) {
    delay(50);
    return;
  }

  if (sdFailed) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(4, 26);
    tft.println("ERROR: SD Init Failed!");
    while (true) delay(500);
  }

  if (videoFiles.empty()) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(4, 26);
    tft.println("SD OK, 0 .avi files");
    while (true) delay(500);
  }

  static bool bootBannerShown = false;
  if (!bootBannerShown) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(4, 26);
    tft.printf("Found %d .avi files\n", videoFiles.size());
    delay(1000);
    tft.fillScreen(TFT_BLACK);
    bootBannerShown = true;
  }

  if (nextRequested) {
    nextRequested = false;
    currentVideoIndex = getRandomVideoIndex(currentVideoIndex);
  }

  if (restartRequested) {
    restartRequested = false;
  }

  String currentPath = videoFiles[currentVideoIndex];
  File aviFile = SD.open(currentPath.c_str(), FILE_READ);

  if (!aviFile) {
    delay(500);
    return;
  }

  if (!seekToMovi(aviFile)) {
    aviFile.close();
    delay(500);
    return;
  }

  isPlaying = true;

  while (aviFile.available() >= 8 && isPlaying && !nextRequested && !restartRequested) {
    char tag[4];
    uint32_t chunkSize = 0;

    if (aviFile.read((uint8_t*)tag, 4) != 4) break;
    if (aviFile.read((uint8_t*)&chunkSize, 4) != 4) break;

    if (chunkSize > aviFile.size()) break;

    // AUDIO CHUNK ('00wb' or '01wb') -> Feed Inter-Core Ring Buffer
    if (tag[2] == 'w' && tag[3] == 'b') {
      bufferAudioChunk(aviFile, chunkSize);
    }
    // VIDEO CHUNK ('00dc', '01dc', '00db') -> Decode & Push to Display
    else if ((tag[2] == 'd' && tag[3] == 'c') || (tag[2] == 'd' && tag[3] == 'b')) {
      uint32_t audioCushion = ringHead - ringTail;

      // Drop a frame only if audio cushion dips below ~23 ms (512 samples)
      if (audioCushion >= 512 && chunkSize <= sizeof(jpegBuf)) {
        if (aviFile.read(jpegBuf, chunkSize) == chunkSize) {
          if (jpeg.openRAM(jpegBuf, chunkSize, drawJPEGCallback)) {
            jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
            
            // Hold the SPI transaction open across the full decode to reduce overhead
            tft.startWrite();
            jpeg.decode(0, 0, 0);
            tft.endWrite();
            
            jpeg.close();
          }
        }
      } else {
        aviFile.seek(aviFile.position() + chunkSize);
      }
    }
    // OTHER CHUNK -> Skip
    else {
      aviFile.seek(aviFile.position() + chunkSize);
    }

    if (chunkSize % 2 != 0) {
      aviFile.seek(aviFile.position() + 1);
    }
  }

  aviFile.close();
  isPlaying = false;

  if (!nextRequested && !restartRequested) {
    currentVideoIndex = getRandomVideoIndex(currentVideoIndex);
  }
}