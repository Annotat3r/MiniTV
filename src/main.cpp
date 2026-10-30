#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <I2S.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <vector>

// --- PIN DEFINITIONS ---
#define PIN_PWR_EN        14
#define PIN_BTN_PWR_NEXT  15
#define PIN_BTN_RESTART   21

#define SD_CS             13
#define SD_SCK            10
#define SD_MOSI           11
#define SD_MISO           12

#define I2S_LRC           26
#define I2S_BCLK          27
#define I2S_DIN           28

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

File videoFile;

// --- HELPER FUNCTIONS ---
void scanSDCard() {
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

int drawJPEGCallback(JPEGDRAW *pDraw) {
  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

// ================================================================================
// CORE 0: POWER LATCH, BUTTON SENSING, FILE SYSTEM, & AUDIO PROCESSING
// ================================================================================
void setup() {
  // 1. Immediately driver Power Hold pin HIGH to latch system power
  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, HIGH);

  pinMode(PIN_BTN_PWR_NEXT, INPUT);
  pinMode(PIN_BTN_RESTART, INPUT_PULLUP);

  Serial.begin(115200);

  // 2. Initialize SPI1 for SD Card
  SPI1.setSCK(SD_SCK);
  SPI1.setTX(SD_MOSI);
  SPI1.setRX(SD_MISO);
  SPI1.begin();

  if (!SD.begin(SD_CS, SPI1, 25000000)) {
    Serial.println("SD Card Initialization Failed!");
    return;
  }

  scanSDCard();

  // 3. Initialize I2S Audio
  i2sOutput.setBCLK(I2S_BCLK);
  i2sOutput.setDATA(I2S_DIN);
  i2sOutput.setBitsPerSample(16);
  i2sOutput.begin(22050); // Match 22.05 kHz PCM sample rate
}

void loop() {
  static unsigned long btnPressStart = 0;
  static bool btnWasPressed = false;

  // --- Switch 1: Power Off (3s Hold) / Next Video (Short Press) ---
  bool pwrBtnState = (digitalRead(PIN_BTN_PWR_NEXT) == HIGH);

  if (pwrBtnState && !btnWasPressed) {
    btnWasPressed = true;
    btnPressStart = millis();
  } else if (pwrBtnState && btnWasPressed) {
    if (millis() - btnPressStart >= 3000) {
      // Shut Down: Release Power Latch
      digitalWrite(PIN_PWR_EN, LOW);
      while (true) { delay(100); } // Wait for power rail to collapse
    }
  } else if (!pwrBtnState && btnWasPressed) {
    btnWasPressed = false;
    if (millis() - btnPressStart < 3000) {
      nextRequested = true;
    }
  }

  // --- Switch 2: Restart Video ---
  if (digitalRead(PIN_BTN_RESTART) == LOW) {
    restartRequested = true;
    delay(150); // Basic debounce delay
  }

  delay(10);
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

  if (nextRequested) {
    nextRequested = false;
    currentVideoIndex = (currentVideoIndex + 1) % videoFiles.size();
  }

  if (restartRequested) {
    restartRequested = false;
  }

  String currentPath = videoFiles[currentVideoIndex];
  isPlaying = true;

  // Open file directly by path string
  if (jpeg.open(currentPath.c_str(), drawJPEGCallback)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

    // Decode frame
    while (isPlaying && !nextRequested && !restartRequested) {
      if (!jpeg.decode(0, 0, 0)) {
        break; // End of file or frame decode finished
      }
    }
    jpeg.close();
  } else {
    Serial.println("Failed to open file via JPEGDEC");
    delay(100);
  }
  
  isPlaying = false;
}