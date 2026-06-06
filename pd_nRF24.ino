/*
  PD RF — NRF24L01 версия
  ESP32-C3 Super Mini + NRF24L01 + SSD1306 0.96" OLED + SD + 3 кнопки

  Распиновка:
  ─────────────────────────────────────────────────
  SPI (общий для NRF24 + SD):
    MOSI  → GPIO7
    MISO  → GPIO2
    SCK   → GPIO6

  NRF24L01:
    CE    → GPIO20
    CSN   → GPIO19
    IRQ   → GPIO18  (опционально)
    + MOSI/MISO/SCK общий SPI

  SD карта:
    CS    → GPIO8
    + MOSI/MISO/SCK общий SPI

  OLED SSD1306 I2C:
    SDA   → GPIO9
    SCL   → GPIO10

  Кнопки (active LOW, internal pull-up):
    UP    → GPIO0
    DN    → GPIO1
    OK    → GPIO21

  Библиотеки (установить через Library Manager):
    - RF24           (TMRh20)
    - U8g2           (Oli Kraus)
    - SD             (встроена в ESP32 core)
    - SPI            (встроена)

  Вкладки:
    SPEC  — спектр 2400-2525 MHz (126 каналов), обновляется непрерывно
    REC   — захват пакетов в promiscuous-режиме, Flipper-стиль
    PLAY  — ретрансляция пойманных пакетов
    SET   — Data Rate, PA уровень, длина адреса, CRC
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RF24.h>
#include <U8g2lib.h>
#include <SD.h>

// ── Пины ─────────────────────────────────────────────────────────
#define PIN_CE      20
#define PIN_CSN     19
#define PIN_IRQ     18
#define PIN_SD_CS   8
#define PIN_SDA     9
#define PIN_SCL     10
#define PIN_BTN_UP  0
#define PIN_BTN_DN  1
#define PIN_BTN_OK  21

// ── Объекты ───────────────────────────────────────────────────────
RF24    radio(PIN_CE, PIN_CSN);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

// ── Вкладки ───────────────────────────────────────────────────────
enum Tab { TAB_SPECTRUM = 0, TAB_RECORD, TAB_PLAY, TAB_SETTINGS, TAB_COUNT };
const char* tabNames[] = {"SPEC","REC","PLAY","SET"};
int activeTab = TAB_SPECTRUM;

// ── Настройки ─────────────────────────────────────────────────────
struct Settings {
  uint8_t  dataRate;    // 0=1Mbps 1=2Mbps 2=250kbps
  uint8_t  paLevel;     // 0=MIN 1=LOW 2=HIGH 3=MAX
  uint8_t  addrLen;     // 3,4,5 байт
  bool     crcEnabled;
};
Settings cfg = {0, 3, 5, true};

const char* drLabels[]  = {"1Mbps","2Mbps","250k"};
const char* paLabels[]  = {"MIN","LOW","HIGH","MAX"};
const rf24_datarate_e drValues[] = {RF24_1MBPS, RF24_2MBPS, RF24_250KBPS};
const rf24_pa_dbm_e   paValues[] = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};

#define DR_COUNT  3
#define PA_COUNT  4
#define SET_ITEMS 4   // DataRate, PA, AddrLen, CRC  (нет выбора частоты — NRF читает всё)

// ── Дебаунс кнопок ────────────────────────────────────────────────
struct Button { uint8_t pin; bool last; uint32_t t; bool pressed; };
Button btnUp = {PIN_BTN_UP, HIGH, 0, false};
Button btnDn = {PIN_BTN_DN, HIGH, 0, false};
Button btnOk = {PIN_BTN_OK, HIGH, 0, false};

void updateBtn(Button& b) {
  bool cur = digitalRead(b.pin);
  b.pressed = (cur == LOW && b.last == HIGH && millis() - b.t > 40);
  if (b.pressed) b.t = millis();
  b.last = cur;
}

// ── Спектр анализатор ─────────────────────────────────────────────
// NRF24 поддерживает каналы 0..125 = 2400..2525 MHz
#define SPEC_BINS 128          // все 126 каналов + 2 крайних
uint8_t  specBars[SPEC_BINS];  // 0..63 пикселей высота
uint8_t  specPeak[SPEC_BINS];  // пиковые значения (медленно спадают)
uint8_t  peakHold[SPEC_BINS];  // счётчик удержания пика

void specSweep() {
  // NRF24 умеет замерять carrier detect на каждом канале
  // Техника: ставим RPD (received power detector) флаг после короткого слушания
  radio.stopListening();
  for (int ch = 0; ch < SPEC_BINS; ch++) {
    radio.setChannel(ch < 126 ? ch : 125);
    radio.startListening();
    delayMicroseconds(130);   // минимальное время для RPD
    bool cd = radio.testCarrier();
    radio.stopListening();

    // Накапливаем: если сигнал есть — поднимаем, иначе плавно спускаем
    if (cd) {
      specBars[ch] = min((int)specBars[ch] + 8, 50);
    } else {
      if (specBars[ch] > 0) specBars[ch]--;
    }

    // Пик с удержанием
    if (specBars[ch] >= specPeak[ch]) {
      specPeak[ch]  = specBars[ch];
      peakHold[ch]  = 60;
    } else if (peakHold[ch] > 0) {
      peakHold[ch]--;
    } else if (specPeak[ch] > 0) {
      specPeak[ch]--;
    }
  }
}

void drawSpectrum() {
  u8g2.setFont(u8g2_font_5x7_mr);
  u8g2.drawStr(0, 20, "2.4GHz  2.4-2.525");

  // 128 бинов на 128 пикселей — 1 бин = 1 пиксель
  for (int i = 0; i < SPEC_BINS; i++) {
    int h = specBars[i];
    if (h > 0) {
      u8g2.drawVLine(i, 63 - h, h);
    }
    // Пик
    if (specPeak[i] > 0) {
      u8g2.drawPixel(i, 63 - specPeak[i] - 1);
    }
  }

  // Метки каналов
  u8g2.drawHLine(0, 63, 128);
  u8g2.drawStr(0,  63, "2400");
  u8g2.drawStr(52, 63, "2462");  // WiFi ch1
  u8g2.drawStr(96, 63, "2525");
}

// ── Запись пакетов ────────────────────────────────────────────────
#define PKT_BUF_SIZE  256
#define PKT_PAYLOAD   32

struct Packet {
  uint8_t  channel;
  uint8_t  len;
  uint8_t  data[PKT_PAYLOAD];
};

Packet   recPkts[PKT_BUF_SIZE];
uint16_t recCount    = 0;
bool     isRecording = false;
bool     hasRecording= false;
uint32_t recStartMs  = 0;

// Promiscuous-like: фиксированный адрес 0xAA для захвата случайных пакетов
const uint8_t PROMISC_ADDR[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

// Waveform preview из захваченных пакетов
uint8_t  wavePreview[128];
uint8_t  waveLen = 0;

void buildWavePreview() {
  if (recCount == 0) { waveLen = 0; return; }
  waveLen = 0;
  for (uint16_t p = 0; p < recCount && waveLen < 128; p++) {
    for (int b = 0; b < recPkts[p].len && waveLen < 128; b++) {
      for (int bit = 7; bit >= 0 && waveLen < 128; bit--) {
        wavePreview[waveLen++] = (recPkts[p].data[b] >> bit) & 1;
      }
    }
  }
}

void recStart() {
  recCount    = 0;
  isRecording = true;
  recStartMs  = millis();

  radio.stopListening();
  radio.setChannel(2);  // начальный канал — будем прыгать
  radio.setPayloadSize(PKT_PAYLOAD);
  radio.openReadingPipe(1, PROMISC_ADDR);
  radio.startListening();
}

void recStop() {
  isRecording  = false;
  hasRecording = recCount > 0;
  radio.stopListening();
  buildWavePreview();
}

void recSave() {
  if (!hasRecording) return;
  File f = SD.open("/nrf_pkts.bin", FILE_WRITE);
  if (!f) return;
  for (uint16_t i = 0; i < recCount; i++) {
    f.write(recPkts[i].channel);
    f.write(recPkts[i].len);
    f.write(recPkts[i].data, PKT_PAYLOAD);
  }
  f.close();
}

void recLoad() {
  File f = SD.open("/nrf_pkts.bin");
  if (!f) return;
  recCount = 0;
  while (f.available() && recCount < PKT_BUF_SIZE) {
    recPkts[recCount].channel = f.read();
    recPkts[recCount].len     = f.read();
    f.read(recPkts[recCount].data, PKT_PAYLOAD);
    recCount++;
  }
  f.close();
  hasRecording = recCount > 0;
  buildWavePreview();
}

// Прыжки по каналам во время записи для широкополосного захвата
uint8_t  recChannel    = 2;
uint32_t recChanTimer  = 0;

void recUpdate() {
  if (!isRecording) return;

  // Прыгаем по каналу каждые 5мс
  if (millis() - recChanTimer > 5) {
    recChanTimer = millis();
    recChannel   = (recChannel + 3) % 126;
    radio.stopListening();
    radio.setChannel(recChannel);
    radio.startListening();
  }

  if (radio.available()) {
    if (recCount < PKT_BUF_SIZE) {
      recPkts[recCount].channel = recChannel;
      recPkts[recCount].len     = PKT_PAYLOAD;
      radio.read(recPkts[recCount].data, PKT_PAYLOAD);
      recCount++;
    } else {
      recStop();
    }
  }
}

void drawWave(uint8_t* wave, uint8_t len, int yH, int yL) {
  for (int i = 0; i < (int)len - 1; i++) {
    int x  = i + (128 - len) / 2;
    int y  = wave[i]   ? yH : yL;
    int yn = wave[i+1] ? yH : yL;
    u8g2.drawHLine(x, y, 1);
    if (y != yn) u8g2.drawVLine(x, min(y,yn), abs(y-yn)+1);
  }
}

void drawRecord() {
  u8g2.setFont(u8g2_font_5x7_mr);

  if (isRecording) {
    // Заголовок + канал + счётчик пакетов
    char buf[24];
    snprintf(buf, sizeof(buf), "CH:%03d PKT:%u", recChannel, recCount);
    u8g2.drawStr(0, 20, buf);
    u8g2.drawHLine(0, 22, 128);

    // Живая визуализация последнего пакета (Flipper-стиль)
    if (recCount > 0) {
      uint8_t liveWave[64];
      uint8_t lLen = 0;
      for (int b = 0; b < recPkts[recCount-1].len && lLen < 64; b++) {
        for (int bit = 7; bit >= 0 && lLen < 64; bit--) {
          liveWave[lLen++] = (recPkts[recCount-1].data[b] >> bit) & 1;
        }
      }
      drawWave(liveWave, lLen, 28, 42);
    }

    // Мигающий REC
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawDisc(120, 56, 3);
    }
    uint32_t sec = (millis() - recStartMs) / 1000;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", sec/60, sec%60);
    u8g2.drawStr(0, 62, buf);
    u8g2.drawStr(60, 62, "● REC");

  } else if (hasRecording) {
    char buf[24];
    snprintf(buf, sizeof(buf), "PKT:%u  saved", recCount);
    u8g2.drawStr(0, 20, buf);
    u8g2.drawHLine(0, 22, 128);
    drawWave(wavePreview, waveLen, 28, 42);
    u8g2.drawStr(0, 62, "[OK=SAVE]");

  } else {
    u8g2.drawStr(10, 35, "No packets");
    u8g2.drawStr(10, 48, "[OK] to record");
  }
}

// ── Воспроизведение ───────────────────────────────────────────────
bool     isPlaying  = false;
uint16_t playIdx    = 0;
uint32_t playTimer  = 0;
#define  PLAY_INTERVAL_MS 10

const uint8_t TX_ADDR[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

void playStart() {
  if (!hasRecording) return;
  playIdx   = 0;
  isPlaying = true;
  radio.stopListening();
  radio.openWritingPipe(TX_ADDR);
}

void playStop() {
  isPlaying = false;
  radio.stopListening();
}

void playUpdate() {
  if (!isPlaying) return;
  if (playIdx >= recCount) { playStop(); return; }
  if (millis() - playTimer < PLAY_INTERVAL_MS) return;
  playTimer = millis();

  radio.setChannel(recPkts[playIdx].channel);
  radio.write(recPkts[playIdx].data, recPkts[playIdx].len);
  playIdx++;
}

void drawPlay() {
  u8g2.setFont(u8g2_font_5x7_mr);

  if (!hasRecording) {
    u8g2.drawStr(10, 35, "No recording");
    u8g2.drawStr(10, 48, "Go to REC tab");
    return;
  }

  char buf[24];
  snprintf(buf, sizeof(buf), "PKT:%u", recCount);
  u8g2.drawStr(0, 20, buf);
  u8g2.drawHLine(0, 22, 128);

  drawWave(wavePreview, waveLen, 28, 42);

  // Прогресс
  int prog = isPlaying ? (int)(124 * (long)playIdx / recCount) : 0;
  u8g2.drawFrame(2, 50, 124, 4);
  u8g2.drawBox(2, 50, constrain(prog, 0, 124), 4);

  snprintf(buf, sizeof(buf), "%u/%u", playIdx, recCount);
  u8g2.drawStr(0, 62, buf);
  u8g2.drawStr(100, 62, isPlaying ? "TX>" : "[ ]");
}

// ── Настройки ─────────────────────────────────────────────────────
int  setItem    = 0;
bool setEditing = false;

void applySettings() {
  radio.stopListening();
  radio.setDataRate(drValues[cfg.dataRate]);
  radio.setPALevel(paValues[cfg.paLevel]);
  radio.setAddressWidth(cfg.addrLen);
  if (cfg.crcEnabled) {
    radio.setCRCLength(RF24_CRC_16);
  } else {
    radio.disableCRC();
  }
}

void drawSettings() {
  u8g2.setFont(u8g2_font_5x7_mr);

  // Подсказка что частота не выбирается
  if (setItem == 0 && !setEditing) {
    u8g2.drawStr(0, 20, "NRF24: 2400-2525MHz");
    u8g2.drawStr(0, 28, "all channels auto");
    u8g2.drawHLine(0, 30, 128);
  } else {
    u8g2.drawHLine(0, 12, 128);
  }

  const char* labels[] = {"RATE","PA","ADDR","CRC"};
  char vals[SET_ITEMS][12];
  snprintf(vals[0], 12, "%s", drLabels[cfg.dataRate]);
  snprintf(vals[1], 12, "%s", paLabels[cfg.paLevel]);
  snprintf(vals[2], 12, "%dB", cfg.addrLen);
  snprintf(vals[3], 12, "%s", cfg.crcEnabled ? "ON" : "OFF");

  int yStart = (setItem == 0 && !setEditing) ? 42 : 22;
  for (int i = 0; i < SET_ITEMS; i++) {
    int y = yStart + i * 10;
    if (y > 63) break;
    if (i == setItem) {
      u8g2.drawBox(0, y - 8, 128, 10);
      u8g2.setDrawColor(0);
    }
    char line[24];
    snprintf(line, sizeof(line), "%-5s%s", labels[i], vals[i]);
    u8g2.drawStr(2, y, line);
    u8g2.setDrawColor(1);
  }
}

// ── Хедер ─────────────────────────────────────────────────────────
void drawHeader() {
  u8g2.drawBox(0, 0, 128, 11);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x12_mr);
  u8g2.drawStr(1, 9, "PD RF");
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_5x7_mr);
  for (int i = 0; i < TAB_COUNT; i++) {
    int x = 45 + i * 21;
    if (i == activeTab) {
      u8g2.drawBox(x - 1, 0, 21, 11);
      u8g2.setDrawColor(0);
    }
    u8g2.drawStr(x, 9, tabNames[i]);
    u8g2.setDrawColor(1);
  }
  u8g2.drawHLine(0, 11, 128);
}

// ── Кнопки ────────────────────────────────────────────────────────
void handleButtons() {
  updateBtn(btnUp);
  updateBtn(btnDn);
  updateBtn(btnOk);

  // Переключение вкладок (если не в режиме редактирования настроек)
  bool inEdit = (activeTab == TAB_SETTINGS && setEditing);
  if (!inEdit) {
    if (btnUp.pressed) {
      if (isRecording) recStop();
      if (isPlaying)   playStop();
      activeTab = (activeTab - 1 + TAB_COUNT) % TAB_COUNT;
      if (activeTab == TAB_SPECTRUM) specSweep();
    }
    if (btnDn.pressed) {
      if (isRecording) recStop();
      if (isPlaying)   playStop();
      activeTab = (activeTab + 1) % TAB_COUNT;
      if (activeTab == TAB_SPECTRUM) specSweep();
    }
  }

  switch (activeTab) {
    case TAB_SPECTRUM:
      break;

    case TAB_RECORD:
      if (btnOk.pressed) {
        if (!isRecording) {
          recStart();
        } else {
          recStop();
          recSave();
        }
      }
      break;

    case TAB_PLAY:
      if (btnOk.pressed) {
        if (!isPlaying) playStart();
        else            playStop();
      }
      break;

    case TAB_SETTINGS:
      if (!setEditing) {
        if (btnUp.pressed) setItem = (setItem - 1 + SET_ITEMS) % SET_ITEMS;
        if (btnDn.pressed) setItem = (setItem + 1) % SET_ITEMS;
        if (btnOk.pressed) setEditing = true;
      } else {
        switch (setItem) {
          case 0: // DataRate
            if (btnUp.pressed) cfg.dataRate = (cfg.dataRate + 1) % DR_COUNT;
            if (btnDn.pressed) cfg.dataRate = (cfg.dataRate - 1 + DR_COUNT) % DR_COUNT;
            break;
          case 1: // PA Level
            if (btnUp.pressed) cfg.paLevel = (cfg.paLevel + 1) % PA_COUNT;
            if (btnDn.pressed) cfg.paLevel = (cfg.paLevel - 1 + PA_COUNT) % PA_COUNT;
            break;
          case 2: // Addr len
            if (btnUp.pressed && cfg.addrLen < 5) cfg.addrLen++;
            if (btnDn.pressed && cfg.addrLen > 3) cfg.addrLen--;
            break;
          case 3: // CRC
            if (btnUp.pressed || btnDn.pressed) cfg.crcEnabled = !cfg.crcEnabled;
            break;
        }
        if (btnOk.pressed) {
          setEditing = false;
          applySettings();
        }
      }
      break;
  }
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setContrast(200);

  // Splash
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_9x18B_mr);
  u8g2.drawStr(20, 28, "PD RF");
  u8g2.setFont(u8g2_font_5x7_mr);
  u8g2.drawStr(25, 42, "NRF24 edition");
  u8g2.drawStr(20, 54, "ESP32-C3 mini");
  u8g2.sendBuffer();
  delay(1500);

  // SPI
  SPI.begin(6, 2, 7);  // SCK, MISO, MOSI

  // NRF24
  if (!radio.begin()) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_mr);
    u8g2.drawStr(10, 35, "NRF24 not found!");
    u8g2.sendBuffer();
    while (1) delay(1000);
  }
  radio.setAutoAck(false);        // promiscuous-like
  radio.setRetries(0, 0);
  radio.setPayloadSize(32);
  applySettings();

  // SD
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("SD not found");
  } else {
    recLoad();
  }

  // Спектр — нулевой проход
  memset(specBars, 0, sizeof(specBars));
  memset(specPeak, 0, sizeof(specPeak));
  memset(peakHold, 0, sizeof(peakHold));
}

// ── Loop ──────────────────────────────────────────────────────────
uint32_t lastSpecUpdate = 0;
uint32_t lastDraw       = 0;

void loop() {
  handleButtons();
  recUpdate();
  playUpdate();

  // Спектр: непрерывное сканирование
  if (activeTab == TAB_SPECTRUM && millis() - lastSpecUpdate > 10) {
    specSweep();
    lastSpecUpdate = millis();
  }

  // Дисплей ~20fps
  if (millis() - lastDraw > 50) {
    lastDraw = millis();
    u8g2.clearBuffer();
    drawHeader();
    switch (activeTab) {
      case TAB_SPECTRUM: drawSpectrum(); break;
      case TAB_RECORD:   drawRecord();   break;
      case TAB_PLAY:     drawPlay();     break;
      case TAB_SETTINGS: drawSettings(); break;
    }
    u8g2.sendBuffer();
  }
}
