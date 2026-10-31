#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <I2S.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <vector>

// --- PIN DEFINITIONS ---
#define PIN_BTN_CONTROL   15
#define PIN_POT_VOL       26  //ADC0

#define SD_CS             13
#define SD_SCK            10
#define SD_MOSI           11
#define SD_MISO           12

#define I2S_LRC           7
#define I2S_BCLK          6
#define I2S_DIN           8

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
volatile float volumeGain = 1.0f; // Range: 0.0f (mute) to 1.0f (max volume)

File videoFile;

// --- RANDOM EPISODE SELECTION ---
int getRandomVideoIndex(int currentIndex) {
  if (videoFiles.size() <= 1) return 0;

  int newIndex = currentIndex;
  // Ensure the next randomly selected episode is not the current one
  while (newIndex == currentIndex) {
    newIndex = rp2040.hwrand32() % videoFiles.size();
  }
  return newIndex;
}

// --- JPEGDEC CALLBACK FUNCTIONS FOR SD CARD STREAMING ---
void *jpegOpen(const char *filename, int32_t *size) {
  videoFile = SD.open(filename, FILE_READ);
  if (videoFile) {
    *size = videoFile.size();
    return &videoFile;
  }
  return NULL;
}

void jpegClose(void *handle) {
  File *f = (File *)handle;
  if (f && *f) {
    f->close();
  }
}

int32_t jpegRead(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = (File *)pFile->fHandle;
  if (f && *f) {
    return f->read(pBuf, iLen);
  }
  return 0;
}

int32_t jpegSeek(JPEGFILE *pFile, int32_t iPosition) {
  File *f = (File *)pFile->fHandle;
  if (f && *f) {
    if (f->seek(iPosition)) {
      return iPosition;
    }
  }
  return 0;
}

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
      if (name.endsWith(".AVI") || name.endsWith(".avi") || name.endsWith(".JPG")) {
        videoFiles.push_back("/" + name);
      }
    }
    entry.close();
  }
  root.close();
}

// Audio Volume Scaling Helper
void writeAudioWithVolume(int16_t *buffer, size_t sampleCount) {
  for (size_t i = 0; i < sampleCount; i++) {
    buffer[i] = (int16_t)(buffer[i] * volumeGain);
  }
  i2sOutput.write((uint8_t*)buffer, sampleCount * sizeof(int16_t));
}

// ================================================================================
// CORE 0: BUTTON SENSING, VOLUME ADC, FILE SYSTEM, & AUDIO PROCESSING
// ================================================================================
void setup() {
  pinMode(PIN_BTN_CONTROL, INPUT_PULLUP);
  analogReadResolution(10); // Standard 0-1023 ADC Scale

  Serial.begin(115200);

  // Initialize SPI1 for SD Card
  SPI1.setSCK(SD_SCK);
  SPI1.setTX(SD_MOSI);
  SPI1.setRX(SD_MISO);
  SPI1.begin();

  // Earle Philhower RP2040 core method for initializing SD over SPI1
  if (!SD.begin(SD_CS, SPI1)) {
    Serial.println("SD Card Initialization Failed!");
    return;
  }

  scanSDCard();

  // Pick a random episode index to play
  if (!videoFiles.empty()) {
    currentVideoIndex = rp2040.hwrand32() % videoFiles.size();
  }

  // Initialize I2S Audio
  i2sOutput.setBCLK(I2S_BCLK);
  i2sOutput.setDATA(I2S_DIN);
  i2sOutput.setBitsPerSample(16);
  i2sOutput.begin(22050); // Match 22.05 kHz PCM sample rate
}

void loop() {
  // 1. Single Button Next Track/Restart Track Logic
  static bool lastBtnState = HIGH;
  static unsigned long lastDebounceTime = 0;
  static unsigned long lastBtnPresstime = 0;
  static int btnClickCount = 0;

  bool currentBtnState = digitalRead(PIN_BTN_CONTROL);

  // Debounce the button transition
  if (currentBtnState != lastBtnState) {
    if (millis() - lastDebounceTime > 40) { //40MS debounce
      lastDebounceTime = millis();

      if (lastBtnState == HIGH && currentBtnState == LOW) {
        unsigned long now = millis();
        if (now - lastBtnPresstime < 750) {
          btnClickCount++;
        } else {
          btnClickCount = 1;
        }
        lastBtnPresstime = now;

        // Immediate restart trigger on 2nd tap within 750ms
        if (btnClickCount == 2) {
          restartRequested = true;
          btnClickCount = 0;
        }
      }
    }
    lastBtnState = currentBtnState;
  }

  // Timeout check for a single button press
  if (btnClickCount == 1 && (millis() - lastBtnPresstime >= 750)) {
    nextRequested = true;
    btnClickCount = 0;
  }

  // Analog Volume Pot Sampling
  static unsigned long lastVolReadTime = 0;
  if (millis() - lastVolReadTime >= 50) {
    lastVolReadTime = millis();

    int rawAdc = analogRead(PIN_POT_VOL); //Read 0 to 1023 from ADC
    volumeGain = (float)rawAdc / 1023.0f; //Scale reading
  }

delay(5);
}

// ================================================================================
// CORE 1: VIDEO DECODING & SPI DISPLAY RENDERER
// ================================================================================
void setup1() {
  tft.init();
  tft.setRotation(1); // Landscape 160x128
  tft.fillScreen(TFT_BLACK);
}

void loop1() {
  if (videoFiles.empty()) {
    delay(500);
    return;
  }

  // Handle user-requested skip to next random episode
  if (nextRequested) {
    nextRequested = false;
    currentVideoIndex = getRandomVideoIndex(currentVideoIndex);
  }

  if (restartRequested) {
    restartRequested = false;
  }

  String currentPath = videoFiles[currentVideoIndex];
  isPlaying = true;

  // Open file using SD I/O callbacks
  if (jpeg.open(currentPath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, drawJPEGCallback)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

    // Decode frame sequence
    while (isPlaying && !nextRequested && !restartRequested) {
      if (!jpeg.decode(0, 0, 0)) {
        break; // End of file or decode complete
      }
    }
    jpeg.close();
  } else {
    Serial.println("Failed to open file via JPEGDEC");
    delay(100);
  }

  isPlaying = false;

  // Handle natural video end with new random episode
  if (!nextRequested && !restartRequested) {
    currentVideoIndex = getRandomVideoIndex(currentVideoIndex);
  }
}