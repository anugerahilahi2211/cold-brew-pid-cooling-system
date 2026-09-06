/*
  SISTEM PENDINGIN COLD BREW COFFEE
  ESP32 + DS18B20 + RTC DS3231 + LCD 2004 I2C + 3 Tombol + 2 BTS7960

  UI sederhana:
  - Menu utama: PRESET / MANUAL
  - OK pendek = pilih / lanjut
  - UP/DOWN   = ubah nilai, bisa ditahan

  Alur:
  1. Menu utama
     PRESET
     MANUAL

  2. Jika PRESET:
     pilih preset suhu 8 C, 12 C, 15 C, atau 20 C
     OK = masuk konfirmasi
     OK lagi = START

  3. Jika MANUAL:
     atur suhu pakai UP/DOWN
     OK
     atur waktu pakai UP/DOWN
     OK
     OK lagi = START

  4. Saat running:
     tampil countdown, suhu aktual, setpoint, dan PWM
     OK tahan = STOP

  Pengaturan kontrol:
  - SCALE otomatis = 0.20
  - PWM maksimum = 255
  - SP <= 8 C  : PID DINGIN
  - SP >= 9 C  : PID NORMAL
  - SP 8-9 C   : mempertahankan mode sebelumnya

  PIN ESP32 DEVKIT:
  DS18B20 DATA -> GPIO4

  LCD 2004 I2C + RTC DS3231:
  SDA -> GPIO21
  SCL -> GPIO22

  TOMBOL TERBARU:
  OK / MENU / START -> GPIO5  (modul active-HIGH, VCC 3.3V)
  UP                -> GPIO16 (modul active-HIGH, VCC 3.3V)
  DOWN              -> GPIO13 (active-LOW ke GND)

  DRIVER BTS7960 1:
  RPWM1 -> GPIO25
  LPWM1 -> GPIO26
  R_EN1 -> GPIO27
  L_EN1 -> GPIO14

  DRIVER BTS7960 2:
  RPWM2 -> GPIO32
  LPWM2 -> GPIO33
  R_EN2 -> GPIO18
  L_EN2 -> GPIO19

  Library:
  - OneWire
  - DallasTemperature
  - RTClib by Adafruit
  - LiquidCrystal_I2C
  - PubSubClient
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// ======================= ALAMAT LCD =======================
// Umumnya 0x27. Jika LCD tidak tampil, ubah menjadi 0x3F.
#define LCD_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);


// =================== SETTING ONLINE MQTT ==================
// Isi sesuai WiFi dan HiveMQ Cloud kamu.
// HiveMQ Cloud ESP32 memakai MQTT TLS port 8883.
// Dashboard web memakai WSS/WebSocket port 8884.
const char *WIFI_SSID = "ISI_NAMA_WIFI";
const char *WIFI_PASS = "ISI_PASSWORD_WIFI";

const char *MQTT_HOST = "ISI_CLUSTER_HIVEMQ.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char *MQTT_USER = "ISI_USERNAME_HIVEMQ";
const char *MQTT_PASS = "ISI_PASSWORD_HIVEMQ";

const char *MQTT_TOPIC_DATA = "gerry/coldbrew/data";
const char *MQTT_TOPIC_STATUS = "gerry/coldbrew/status";

// Untuk tugas akhir/demo dibuat true agar mudah konek.
// Untuk sistem produksi sebaiknya diganti sertifikat CA broker.
#define MQTT_TLS_INSECURE true

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

unsigned long waktuMQTTReconnect = 0;
const unsigned long MQTT_RECONNECT_MS = 5000UL;

// ========================== PIN ===========================
#define PIN_SENSOR 4

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

// Pin tombol sesuai wiring terbaru:
// UP   -> GPIO16
// DOWN -> GPIO13
// OK   -> GPIO5
#define PIN_BTN_OK   5
#define PIN_BTN_UP   16
#define PIN_BTN_DOWN 13

// Jika modul tombol aktif HIGH, ubah menjadi false.
// Umumnya tombol ke GND memakai aktif LOW.
// Polaritas tombol:
// DOWN sudah diketahui aktif LOW.
// OK sudah diketahui aktif HIGH.
// UP memakai modul tombol active-HIGH, VCC modul harus ke 3.3V.
#define OK_ACTIVE_LOW   false
#define UP_ACTIVE_LOW   false
#define DOWN_ACTIVE_LOW true

#define PIN_BUZZER 23

// ===================== SETTING BUZZER =====================
// 1 = buzzer pasif/piezo 2 pin memakai tone 2.5 kHz.
// 0 = buzzer aktif/modul 3 pin memakai HIGH/LOW biasa.
#define BUZZER_PAKAI_TONE 1
#define BUZZER_ACTIVE_LOW false
const int BUZZER_TONE_FREQ = 2500;
const int CH_BUZZER = 7;
const unsigned long BUZZER_KLIK_MS = 80;
const unsigned long BUZZER_STEP_MS = 220;
const byte POLA_BUZZER_SELESAI[8] = {1, 1, 0, 0, 1, 1, 0, 0};
const byte BUZZER_SELESAI_ULANG = 6;

enum ModeBuzzer {
  BUZZER_DIAM,
  BUZZER_KLIK_TOMBOL,
  BUZZER_SELESAI
};

ModeBuzzer modeBuzzer = BUZZER_DIAM;
unsigned long waktuBuzzerMs = 0;
byte indeksPolaBuzzer = 0;
byte jumlahUlangPolaBuzzer = 0;

#define PIN_RPWM1 25
#define PIN_LPWM1 26
#define PIN_REN1  27
#define PIN_LEN1  14

#define PIN_RPWM2 32
#define PIN_LPWM2 33
#define PIN_REN2  18
#define PIN_LEN2  19

// ======================= PWM ESP32 ========================
const int PWM_FREQ = 5000;
const int PWM_RESOLUTION = 8;

const int CH_RPWM1 = 0;
const int CH_LPWM1 = 1;
const int CH_RPWM2 = 2;
const int CH_LPWM2 = 3;

// ========================= SENSOR =========================
OneWire oneWire(PIN_SENSOR);
DallasTemperature sensors(&oneWire);

RTC_DS3231 rtc;
bool rtcTersedia = false;

// Kalibrasi DS18B20
const float CAL_A = 1.0642f;
const float CAL_B = -0.6508f;

// ====================== PID ZN NORMAL =====================
const float KP_NORMAL_ZN = 217.6838f;
const float KI_NORMAL_ZN = 5.4421f;
const float KD_NORMAL_ZN = 2176.8380f;

// ====================== PID ZN DINGIN =====================
const float KP_DINGIN_ZN = 156.3625f;
const float KI_DINGIN_ZN = 2.79219f;
const float KD_DINGIN_ZN = 2189.0752f;

// Tetap otomatis sesuai permintaan.
const float SCALE_OTOMATIS = 0.20f;
const int PWM_MAX_TETAP = 255;
const int PWM_MIN = 0;

// ========================= SETPOINT =======================
float setpointC = 8.0f;
int durasiJam = 12;

const float SP_MIN = 5.0f;
const float SP_MAX = 20.0f;
const float BATAS_MODE_DINGIN = 8.0f;
const float BATAS_MODE_NORMAL = 9.0f;

const int DURASI_JAM_MIN = 1;
const int DURASI_JAM_MAX = 24;

// ========================= PRESET =========================
struct PresetColdBrew {
  const char *nama;
  float suhu;
  int jam;
};

const PresetColdBrew presetList[] = {
  {"5 C  / 24 Jam",  5.0f, 24},
  {"8 C  / 20 Jam",  8.0f, 20},
  {"12 C / 16 Jam", 12.0f, 16},
  {"15 C / 15 Jam", 15.0f, 15},
  {"20 C / 12 Jam", 20.0f, 12}
};

const int JUMLAH_PRESET = sizeof(presetList) / sizeof(presetList[0]);

// ========================= SAMPLING =======================
const unsigned long SAMPLE_MS = 1000UL;
const float DT = 1.0f;

const float TF = 1.0f;
const float ALPHA_D = TF / (TF + DT);

const int PWM_RAMP_STEP = 8;

// ========================= PROTEKSI =======================
const float SUHU_MIN_VALID = -10.0f;
const float SUHU_MAX_VALID = 60.0f;
const float SUHU_MIN_ABSOLUT = 4.20f;

const float OVERSHOOT_NORMAL_C = 2.0f;
const float OVERSHOOT_DINGIN_C = 1.0f;

const byte MAX_SENSOR_HOLD = 5;

// ========================== TOMBOL ========================
const unsigned long DEBOUNCE_MS = 80;
const unsigned long LOCKOUT_MS = 180;

struct Tombol {
  byte pin;
  bool stablePressed;
  bool lastReadingPressed;
  unsigned long lastChangeMs;
  unsigned long lastEventMs;
};

Tombol tombolOK   = {PIN_BTN_OK,   false, false, 0, 0};
Tombol tombolUP   = {PIN_BTN_UP,   false, false, 0, 0};
Tombol tombolDOWN = {PIN_BTN_DOWN, false, false, 0, 0};

const byte BTN_NONE  = 0;
const byte BTN_PRESS = 1;

// =========================== MODE =========================
enum ModePID {
  MODE_NORMAL,
  MODE_DINGIN
};

ModePID modeAktif = MODE_DINGIN;

// ========================= UI STATE =======================
enum UIState {
  UI_MAIN_MENU,
  UI_PRESET_SELECT,
  UI_MANUAL_TEMP,
  UI_MANUAL_TIME,
  UI_CONFIRM_START,
  UI_RUNNING,
  UI_FINISHED
};

UIState uiState = UI_MAIN_MENU;

int menuUtamaIndex = 0;      // 0 preset, 1 manual
int presetIndex = 0;
bool dariPreset = true;

// ======================== VARIABEL PID ====================
bool pidAktif = false;

float integralTerm = 0.0f;
float derivativeFilter = 0.0f;
float suhuSebelumnya = NAN;

float suhuValidTerakhir = NAN;
float suhuRawTerakhir = NAN;
bool punyaSuhuValid = false;

int pwmAktual = 0;

unsigned long waktuMulaiSistem = 0;
unsigned long waktuKontrol = 0;
unsigned long waktuStartMs = 0;

// Timer proses cold brew baru mulai setelah suhu mencapai setpoint.
bool timerProsesAktif = false;
bool setpointPernahTercapai = false;
unsigned long waktuMulaiTimerMs = 0;

unsigned long durasiTotalDetik = 0;
unsigned long sisaDetik = 0;

uint32_t epochSelesai = 0;
bool waktuSelesaiValid = false;

unsigned long totalSensorError = 0;
byte errorSensorBeruntun = 0;

String bufferSerial = "";

// LCD update
unsigned long waktuLCDTerakhir = 0;
const unsigned long LCD_UPDATE_MS = 500;

// ======================= PWM ESP32 ========================
void setupPWM() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_RPWM1, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(PIN_LPWM1, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(PIN_RPWM2, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(PIN_LPWM2, PWM_FREQ, PWM_RESOLUTION);
#else
  ledcSetup(CH_RPWM1, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(CH_LPWM1, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(CH_RPWM2, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(CH_LPWM2, PWM_FREQ, PWM_RESOLUTION);

  ledcAttachPin(PIN_RPWM1, CH_RPWM1);
  ledcAttachPin(PIN_LPWM1, CH_LPWM1);
  ledcAttachPin(PIN_RPWM2, CH_RPWM2);
  ledcAttachPin(PIN_LPWM2, CH_LPWM2);
#endif
}

void tulisPWMChannel(int pin, int channel, int nilai) {
  nilai = constrain(nilai, 0, 255);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pin, nilai);
#else
  ledcWrite(channel, nilai);
#endif
}

void tulisPWMDriver(int pwmPendingin) {
  pwmPendingin = constrain(pwmPendingin, 0, 255);

  tulisPWMChannel(PIN_RPWM1, CH_RPWM1, pwmPendingin);
  tulisPWMChannel(PIN_LPWM1, CH_LPWM1, 0);

  tulisPWMChannel(PIN_RPWM2, CH_RPWM2, pwmPendingin);
  tulisPWMChannel(PIN_LPWM2, CH_LPWM2, 0);
}

// ======================== FUNGSI DASAR ====================
float batasiFloat(float nilai, float minimum, float maksimum) {
  if (nilai < minimum) return minimum;
  if (nilai > maksimum) return maksimum;
  return nilai;
}

const char *namaMode(ModePID mode) {
  return (mode == MODE_DINGIN) ? "DINGIN" : "NORMAL";
}

float kpAktif() {
  float kpDasar = (modeAktif == MODE_DINGIN)
                    ? KP_DINGIN_ZN
                    : KP_NORMAL_ZN;
  return kpDasar * SCALE_OTOMATIS;
}

float kiAktif() {
  float kiDasar = (modeAktif == MODE_DINGIN)
                    ? KI_DINGIN_ZN
                    : KI_NORMAL_ZN;
  return kiDasar * SCALE_OTOMATIS;
}

float kdAktif() {
  float kdDasar = (modeAktif == MODE_DINGIN)
                    ? KD_DINGIN_ZN
                    : KD_NORMAL_ZN;
  return kdDasar * SCALE_OTOMATIS;
}

void lcdPrintPad(byte col, byte row, String teks) {
  lcd.setCursor(col, row);

  if (teks.length() > LCD_COLS - col) {
    teks = teks.substring(0, LCD_COLS - col);
  }

  lcd.print(teks);

  int sisa = LCD_COLS - col - teks.length();
  for (int i = 0; i < sisa; i++) {
    lcd.print(' ');
  }
}

String format2(int nilai) {
  if (nilai < 10) {
    return "0" + String(nilai);
  }
  return String(nilai);
}

String formatDurasi(unsigned long detik) {
  unsigned long jam = detik / 3600UL;
  unsigned long menit = (detik % 3600UL) / 60UL;
  unsigned long sec = detik % 60UL;

  return String(jam) + ":" + format2(menit) + ":" + format2(sec);
}

String getJamRTC() {
  if (!rtcTersedia) {
    return "--:--";
  }

  DateTime now = rtc.now();
  return format2(now.hour()) + ":" + format2(now.minute());
}

String getRTCString() {
  if (!rtcTersedia) {
    return "NO_RTC";
  }

  DateTime now = rtc.now();

  char buffer[22];
  snprintf(
    buffer,
    sizeof(buffer),
    "%04d-%02d-%02d_%02d:%02d:%02d",
    now.year(),
    now.month(),
    now.day(),
    now.hour(),
    now.minute(),
    now.second()
  );

  return String(buffer);
}

// ======================== KENDALI PWM =====================
void tulisPWM(int targetPWM) {
  targetPWM = constrain(targetPWM, PWM_MIN, PWM_MAX_TETAP);

  if (targetPWM > pwmAktual + PWM_RAMP_STEP) {
    pwmAktual += PWM_RAMP_STEP;
  } else if (targetPWM < pwmAktual - PWM_RAMP_STEP) {
    pwmAktual -= PWM_RAMP_STEP;
  } else {
    pwmAktual = targetPWM;
  }

  pwmAktual = constrain(pwmAktual, PWM_MIN, PWM_MAX_TETAP);
  tulisPWMDriver(pwmAktual);
}

void pertahankanPWM() {
  pwmAktual = constrain(pwmAktual, PWM_MIN, PWM_MAX_TETAP);
  tulisPWMDriver(pwmAktual);
}

void matikanPeltier() {
  pwmAktual = 0;
  tulisPWMDriver(0);
}


// ========================= BUZZER =========================
void setupBuzzer() {
#if BUZZER_PAKAI_TONE
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_BUZZER, BUZZER_TONE_FREQ, 8);
    ledcWrite(PIN_BUZZER, 0);
  #else
    ledcSetup(CH_BUZZER, BUZZER_TONE_FREQ, 8);
    ledcAttachPin(PIN_BUZZER, CH_BUZZER);
    ledcWrite(CH_BUZZER, 0);
  #endif
#else
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
#endif
}

void tulisBuzzer(bool nyala) {
#if BUZZER_PAKAI_TONE
  int duty = nyala ? 128 : 0;
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_BUZZER, duty);
  #else
    ledcWrite(CH_BUZZER, duty);
  #endif
#else
  if (BUZZER_ACTIVE_LOW) {
    digitalWrite(PIN_BUZZER, nyala ? LOW : HIGH);
  } else {
    digitalWrite(PIN_BUZZER, nyala ? HIGH : LOW);
  }
#endif
}

void hentikanBuzzer() {
  modeBuzzer = BUZZER_DIAM;
  indeksPolaBuzzer = 0;
  jumlahUlangPolaBuzzer = 0;
  tulisBuzzer(false);
}

void bunyiTombol() {
  if (modeBuzzer == BUZZER_SELESAI) return;
  modeBuzzer = BUZZER_KLIK_TOMBOL;
  waktuBuzzerMs = millis();
  tulisBuzzer(true);
}

void mulaiBuzzerSelesai() {
  modeBuzzer = BUZZER_SELESAI;
  indeksPolaBuzzer = 0;
  jumlahUlangPolaBuzzer = 0;
  waktuBuzzerMs = millis();
  tulisBuzzer(POLA_BUZZER_SELESAI[indeksPolaBuzzer] == 1);
}

void updateBuzzer() {
  unsigned long nowMs = millis();

  if (modeBuzzer == BUZZER_DIAM) return;

  if (modeBuzzer == BUZZER_KLIK_TOMBOL) {
    if (nowMs - waktuBuzzerMs >= BUZZER_KLIK_MS) {
      hentikanBuzzer();
    }
    return;
  }

  if (modeBuzzer == BUZZER_SELESAI) {
    if (nowMs - waktuBuzzerMs >= BUZZER_STEP_MS) {
      waktuBuzzerMs = nowMs;
      indeksPolaBuzzer++;

      if (indeksPolaBuzzer >= 8) {
        indeksPolaBuzzer = 0;
        jumlahUlangPolaBuzzer++;

        if (jumlahUlangPolaBuzzer >= BUZZER_SELESAI_ULANG) {
          hentikanBuzzer();
          return;
        }
      }

      tulisBuzzer(POLA_BUZZER_SELESAI[indeksPolaBuzzer] == 1);
    }
  }
}

void tesBuzzerAwal() {
  tulisBuzzer(true);
  delay(120);
  tulisBuzzer(false);
  delay(100);
  tulisBuzzer(true);
  delay(120);
  tulisBuzzer(false);
}

// ========================= SENSOR ========================
bool bacaSuhu(float &rawC, float &kalibrasiC) {
  // Non-blocking: ambil hasil konversi sebelumnya,
  // lalu minta konversi baru untuk pembacaan berikutnya.
  // Ini membuat tombol tidak tertahan proses DS18B20 12-bit.
  rawC = sensors.getTempCByIndex(0);
  sensors.requestTemperatures();

  if (rawC == DEVICE_DISCONNECTED_C ||
      isnan(rawC) ||
      rawC < SUHU_MIN_VALID ||
      rawC > SUHU_MAX_VALID) {
    return false;
  }

  kalibrasiC = CAL_A * rawC + CAL_B;

  if (isnan(kalibrasiC) ||
      kalibrasiC < SUHU_MIN_VALID ||
      kalibrasiC > SUHU_MAX_VALID) {
    return false;
  }

  return true;
}

// ======================= MODE PID ========================
ModePID modeDariSetpoint(float sp, ModePID modeSebelumnya) {
  if (sp <= BATAS_MODE_DINGIN) {
    return MODE_DINGIN;
  }

  if (sp >= BATAS_MODE_NORMAL) {
    return MODE_NORMAL;
  }

  return modeSebelumnya;
}

void perbaruiModeDariSetpoint() {
  ModePID modeBaru = modeDariSetpoint(setpointC, modeAktif);

  if (modeBaru != modeAktif) {
    modeAktif = modeBaru;
    derivativeFilter = 0.0f;
    suhuSebelumnya = NAN;
    integralTerm = 0.0f;
  }
}

void resetPID() {
  integralTerm = 0.0f;
  derivativeFilter = 0.0f;
  suhuSebelumnya = NAN;
}

bool suhuMencapaiSetpoint(float suhuC) {
  // Sistem ini hanya mendinginkan, jadi proses dianggap siap ketika
  // suhu aktual sudah sama dengan atau lebih rendah dari setpoint.
  // Toleransi 0.2 C dipakai agar timer tidak terlalu lama menunggu
  // karena noise sensor kecil.
  return suhuC <= (setpointC + 0.20f);
}

void mulaiTimerProses() {
  if (timerProsesAktif) {
    return;
  }

  timerProsesAktif = true;
  setpointPernahTercapai = true;
  waktuMulaiTimerMs = millis();
  sisaDetik = durasiTotalDetik;

  if (rtcTersedia) {
    DateTime now = rtc.now();
    epochSelesai = now.unixtime() + durasiTotalDetik;
    waktuSelesaiValid = true;
  } else {
    epochSelesai = 0;
    waktuSelesaiValid = false;
  }

  Serial.println("# SETPOINT TERCAPAI - TIMER DIMULAI");
}

// ======================== START/STOP ======================
void mulaiPIDKontrol() {
  perbaruiModeDariSetpoint();
  resetPID();

  errorSensorBeruntun = 0;
  pidAktif = true;

  waktuStartMs = millis();

  // Durasi proses disiapkan, tetapi countdown belum berjalan.
  // Countdown baru dimulai setelah suhu mencapai setpoint.
  timerProsesAktif = false;
  setpointPernahTercapai = false;
  waktuMulaiTimerMs = 0;

  durasiTotalDetik = (unsigned long)durasiJam * 3600UL;
  sisaDetik = durasiTotalDetik;

  epochSelesai = 0;
  waktuSelesaiValid = false;

  uiState = UI_RUNNING;
  lcd.clear();

  Serial.println("# START PENDINGINAN");
  Serial.println("# TIMER AKAN MULAI SETELAH SETPOINT TERCAPAI");
  Serial.print("# SP=");
  Serial.println(setpointC, 1);
  Serial.print("# DURASI_JAM=");
  Serial.println(durasiJam);
}

void stopPIDKontrol(const char *alasan) {
  pidAktif = false;
  resetPID();
  matikanPeltier();

  timerProsesAktif = false;
  setpointPernahTercapai = false;
  waktuMulaiTimerMs = 0;
  waktuSelesaiValid = false;

  Serial.print("# STOP: ");
  Serial.println(alasan);
}

void updateSisaWaktu() {
  if (!pidAktif) {
    return;
  }

  // Selama suhu belum mencapai setpoint, timer belum berjalan.
  if (!timerProsesAktif) {
    sisaDetik = durasiTotalDetik;
    return;
  }

  if (rtcTersedia && waktuSelesaiValid) {
    DateTime now = rtc.now();
    uint32_t nowEpoch = now.unixtime();

    if (nowEpoch >= epochSelesai) {
      sisaDetik = 0;
    } else {
      sisaDetik = epochSelesai - nowEpoch;
    }
  } else {
    unsigned long berjalan = (millis() - waktuMulaiTimerMs) / 1000UL;

    if (berjalan >= durasiTotalDetik) {
      sisaDetik = 0;
    } else {
      sisaDetik = durasiTotalDetik - berjalan;
    }
  }

  if (sisaDetik == 0) {
    stopPIDKontrol("WAKTU SELESAI");
    mulaiBuzzerSelesai();
    uiState = UI_FINISHED;
    lcd.clear();
  }
}

// ======================== TOMBOL =========================
void initTombol(Tombol &t) {
  bool activeLow = true;

  if (t.pin == PIN_BTN_OK) {
    activeLow = OK_ACTIVE_LOW;
  } else if (t.pin == PIN_BTN_UP) {
    activeLow = UP_ACTIVE_LOW;
  } else if (t.pin == PIN_BTN_DOWN) {
    activeLow = DOWN_ACTIVE_LOW;
  }

  if (activeLow) {
    pinMode(t.pin, INPUT_PULLUP);
  } else {
    pinMode(t.pin, INPUT_PULLDOWN);
  }

  t.stablePressed = false;
  t.lastReadingPressed = false;
  t.lastChangeMs = millis();
  t.lastEventMs = 0;
}

bool bacaTombolMentah(Tombol &t) {
  int nilai = digitalRead(t.pin);
  bool activeLow = true;

  if (t.pin == PIN_BTN_OK) {
    activeLow = OK_ACTIVE_LOW;
  } else if (t.pin == PIN_BTN_UP) {
    activeLow = UP_ACTIVE_LOW;
  } else if (t.pin == PIN_BTN_DOWN) {
    activeLow = DOWN_ACTIVE_LOW;
  }

  if (activeLow) {
    return nilai == LOW;
  } else {
    return nilai == HIGH;
  }
}

byte bacaEventTombol(Tombol &t) {
  unsigned long nowMs = millis();
  bool readingPressed = bacaTombolMentah(t);

  if (readingPressed != t.lastReadingPressed) {
    t.lastReadingPressed = readingPressed;
    t.lastChangeMs = nowMs;
  }

  if ((nowMs - t.lastChangeMs) > DEBOUNCE_MS &&
      readingPressed != t.stablePressed) {
    t.stablePressed = readingPressed;

    // Event hanya satu kali saat tombol berubah menjadi ditekan.
    // Tidak ada auto-repeat agar menu tidak turun sendiri.
    if (t.stablePressed &&
        (nowMs - t.lastEventMs >= LOCKOUT_MS)) {
      t.lastEventMs = nowMs;
      return BTN_PRESS;
    }
  }

  return BTN_NONE;
}

void prosesOKPendek() {
  if (uiState == UI_RUNNING) {
    stopPIDKontrol("TOMBOL OK");
    uiState = UI_MAIN_MENU;
    lcd.clear();
    return;
  }

  if (uiState == UI_MAIN_MENU) {
    if (menuUtamaIndex == 0) {
      dariPreset = true;
      uiState = UI_PRESET_SELECT;
    } else {
      dariPreset = false;
      uiState = UI_MANUAL_TEMP;
    }
    lcd.clear();
    return;
  }

  if (uiState == UI_PRESET_SELECT) {
    setpointC = presetList[presetIndex].suhu;
    durasiJam = presetList[presetIndex].jam;
    dariPreset = true;
    perbaruiModeDariSetpoint();
    uiState = UI_CONFIRM_START;
    lcd.clear();
    return;
  }

  if (uiState == UI_MANUAL_TEMP) {
    dariPreset = false;
    perbaruiModeDariSetpoint();
    uiState = UI_MANUAL_TIME;
    lcd.clear();
    return;
  }

  if (uiState == UI_MANUAL_TIME) {
    uiState = UI_CONFIRM_START;
    lcd.clear();
    return;
  }

  if (uiState == UI_CONFIRM_START) {
    mulaiPIDKontrol();
    return;
  }

  if (uiState == UI_FINISHED) {
    uiState = UI_MAIN_MENU;
    lcd.clear();
    return;
  }
}

void prosesUpDown(int arah) {
  if (uiState == UI_MAIN_MENU) {
    menuUtamaIndex += arah;

    if (menuUtamaIndex < 0) menuUtamaIndex = 1;
    if (menuUtamaIndex > 1) menuUtamaIndex = 0;
    return;
  }

  if (uiState == UI_PRESET_SELECT) {
    presetIndex += arah;

    if (presetIndex < 0) presetIndex = JUMLAH_PRESET - 1;
    if (presetIndex >= JUMLAH_PRESET) presetIndex = 0;
    return;
  }

  if (uiState == UI_MANUAL_TEMP) {
    setpointC += 0.5f * arah;
    setpointC = batasiFloat(setpointC, SP_MIN, SP_MAX);
    perbaruiModeDariSetpoint();
    return;
  }

  if (uiState == UI_MANUAL_TIME) {
    durasiJam += arah;
    if (durasiJam < DURASI_JAM_MIN) durasiJam = DURASI_JAM_MIN;
    if (durasiJam > DURASI_JAM_MAX) durasiJam = DURASI_JAM_MAX;
    return;
  }
}

void prosesTombol() {
  byte evOK = bacaEventTombol(tombolOK);
  byte evUP = bacaEventTombol(tombolUP);
  byte evDOWN = bacaEventTombol(tombolDOWN);

  // Jika dua tombol terbaca bersamaan, abaikan untuk mencegah ghost/noise.
  byte jumlahEvent = 0;
  if (evOK == BTN_PRESS) jumlahEvent++;
  if (evUP == BTN_PRESS) jumlahEvent++;
  if (evDOWN == BTN_PRESS) jumlahEvent++;

  if (jumlahEvent != 1) {
    return;
  }

  bunyiTombol();

  if (evOK == BTN_PRESS) {
    prosesOKPendek();
    return;
  }

  if (evUP == BTN_PRESS) {
    prosesUpDown(+1);
    return;
  }

  if (evDOWN == BTN_PRESS) {
    prosesUpDown(-1);
    return;
  }
}

// =========================== LCD =========================
void tampilLCD() {
  if (uiState == UI_MAIN_MENU) {
    lcdPrintPad(0, 0, "Pilih Mode Kopi");
    lcdPrintPad(0, 1, String(menuUtamaIndex == 0 ? ">" : " ") + " PRESET");
    lcdPrintPad(0, 2, String(menuUtamaIndex == 1 ? ">" : " ") + " MANUAL");
    lcdPrintPad(0, 3, "UP/DOWN  OK=Pilih");
    return;
  }

  if (uiState == UI_PRESET_SELECT) {
    lcdPrintPad(0, 0, "Pilih Preset");
    lcdPrintPad(0, 1, String("> ") + presetList[presetIndex].nama);
    lcdPrintPad(0, 2, "UP/DOWN ganti");
    lcdPrintPad(0, 3, "OK lanjut");
    return;
  }

  if (uiState == UI_MANUAL_TEMP) {
    lcdPrintPad(0, 0, "Manual: Atur Suhu");
    lcdPrintPad(0, 1, "Suhu: " + String(setpointC, 1) + " C");
    lcdPrintPad(0, 2, "UP/DOWN ubah");
    lcdPrintPad(0, 3, "OK lanjut");
    return;
  }

  if (uiState == UI_MANUAL_TIME) {
    lcdPrintPad(0, 0, "Manual: Atur Waktu");
    lcdPrintPad(0, 1, "Waktu: " + String(durasiJam) + " Jam");
    lcdPrintPad(0, 2, "UP/DOWN ubah");
    lcdPrintPad(0, 3, "OK lanjut");
    return;
  }

  if (uiState == UI_CONFIRM_START) {
    lcdPrintPad(0, 0, "Mulai Cold Brew?");
    lcdPrintPad(0, 1, "SP: " + String(setpointC, 1) + " C");
    lcdPrintPad(0, 2, "Waktu: " + String(durasiJam) + " Jam");
    lcdPrintPad(0, 3, "OK Start");
    return;
  }

  if (uiState == UI_RUNNING) {
    if (timerProsesAktif) {
      lcdPrintPad(0, 0, "Sisa: " + formatDurasi(sisaDetik));
    } else {
      lcdPrintPad(0, 0, "Tunggu SP tercapai");
    }

    lcdPrintPad(0, 1, "Suhu: " + String(suhuValidTerakhir, 1) + " C");
    lcdPrintPad(0, 2, "SP:" + String(setpointC, 1) + "C PWM:" + String(pwmAktual));

    if (timerProsesAktif) {
      lcdPrintPad(0, 3, String(namaMode(modeAktif)) + "  OK=STOP");
    } else {
      lcdPrintPad(0, 3, "Timer belum jalan");
    }
    return;
  }

  if (uiState == UI_FINISHED) {
    lcdPrintPad(0, 0, "Cold Brew Selesai");
    lcdPrintPad(0, 1, "Suhu: " + String(suhuValidTerakhir, 1) + " C");
    lcdPrintPad(0, 2, "PWM: 0");
    lcdPrintPad(0, 3, "OK ke menu awal");
    return;
  }
}

// ======================= PERINTAH SERIAL =================
void tampilkanStatus() {
  Serial.println("# ================= STATUS =================");
  Serial.print("# RUN=");
  Serial.println(pidAktif ? 1 : 0);
  Serial.print("# UI=");
  Serial.println((int)uiState);
  Serial.print("# RTC=");
  Serial.println(getRTCString());
  Serial.print("# SP=");
  Serial.println(setpointC, 2);
  Serial.print("# DURASI_JAM=");
  Serial.println(durasiJam);
  Serial.print("# TIMER_AKTIF=");
  Serial.println(timerProsesAktif ? 1 : 0);
  Serial.print("# SETPOINT_TERCAPAI=");
  Serial.println(setpointPernahTercapai ? 1 : 0);
  Serial.print("# MODE=");
  Serial.println(namaMode(modeAktif));
  Serial.print("# SCALE=");
  Serial.println(SCALE_OTOMATIS, 2);
  Serial.print("# PMAX=");
  Serial.println(PWM_MAX_TETAP);
  Serial.print("# PWM=");
  Serial.println(pwmAktual);
  Serial.println("# ==========================================");
}

void prosesPerintah(String perintah) {
  perintah.trim();
  perintah.toUpperCase();

  if (perintah == "START" || perintah == "RUN=1") {
    mulaiPIDKontrol();
    return;
  }

  if (perintah == "STOP" || perintah == "RUN=0") {
    stopPIDKontrol("SERIAL");
    uiState = UI_MAIN_MENU;
    lcd.clear();
    return;
  }

  if (perintah.startsWith("SP=")) {
    float nilai = perintah.substring(3).toFloat();

    if (nilai >= SP_MIN && nilai <= SP_MAX) {
      setpointC = nilai;
      perbaruiModeDariSetpoint();
    }
    return;
  }

  if (perintah.startsWith("DUR=")) {
    int nilai = perintah.substring(4).toInt();

    if (nilai >= DURASI_JAM_MIN && nilai <= DURASI_JAM_MAX) {
      durasiJam = nilai;
    }
    return;
  }

  if (perintah == "STATUS") {
    tampilkanStatus();
    return;
  }
}

void bacaPerintahSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (bufferSerial.length() > 0) {
        prosesPerintah(bufferSerial);
        bufferSerial = "";
      }
    } else {
      bufferSerial += c;

      if (bufferSerial.length() > 50) {
        bufferSerial = "";
      }
    }
  }
}


// ========================= MQTT ONLINE ====================
void formatFloatJSON(char *buf, size_t ukuran, float nilai, byte digit) {
  if (isnan(nilai)) {
    snprintf(buf, ukuran, "null");
  } else {
    char fmt[8];
    snprintf(fmt, sizeof(fmt), "%%.%df", digit);
    snprintf(buf, ukuran, fmt, nilai);
  }
}

void setupWiFiMQTT() {
  Serial.println("# Menghubungkan WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 15000UL) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("# WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("# WiFi belum tersambung. Kontrol alat tetap berjalan offline.");
  }

#if MQTT_TLS_INSECURE
  secureClient.setInsecure();
#endif

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(700);
  mqttClient.setSocketTimeout(3);
  mqttClient.setKeepAlive(30);
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  unsigned long nowMs = millis();
  if (nowMs - waktuMQTTReconnect < MQTT_RECONNECT_MS) return;
  waktuMQTTReconnect = nowMs;

  String clientId = "coldbrew-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("# Menghubungkan MQTT...");
  bool ok = mqttClient.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASS
  );

  if (ok) {
    Serial.println("OK");
    mqttClient.publish(MQTT_TOPIC_STATUS, "online", true);
  } else {
    Serial.print("GAGAL, rc=");
    Serial.println(mqttClient.state());
  }
}

void updateMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    reconnectMQTT();
  }

  mqttClient.loop();
}

void publishMQTT(float waktuS,
                 float rawC,
                 float suhuC,
                 float error,
                 float pTerm,
                 float iTerm,
                 float dTerm,
                 bool dataHold,
                 const char *status) {
  if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) return;

  char rawBuf[20], suhuBuf[20], errorBuf[20], pBuf[20], iBuf[20], dBuf[20];
  formatFloatJSON(rawBuf, sizeof(rawBuf), rawC, 3);
  formatFloatJSON(suhuBuf, sizeof(suhuBuf), suhuC, 3);
  formatFloatJSON(errorBuf, sizeof(errorBuf), error, 3);
  formatFloatJSON(pBuf, sizeof(pBuf), pTerm, 3);
  formatFloatJSON(iBuf, sizeof(iBuf), iTerm, 3);
  formatFloatJSON(dBuf, sizeof(dBuf), dTerm, 3);

  String rtcStr = getRTCString();
  String sisaStr = formatDurasi(sisaDetik);

  char payload[700];
  snprintf(
    payload,
    sizeof(payload),
    "{\"alat\":\"coldbrew_esp32_peltier\","
    "\"waktu_s\":%.1f,"
    "\"suhu_raw\":%s,"
    "\"suhu\":%s,"
    "\"setpoint\":%.2f,"
    "\"error\":%s,"
    "\"pwm\":%d,"
    "\"scale\":%.2f,"
    "\"pid_aktif\":%d,"
    "\"timer_aktif\":%d,"
    "\"setpoint_tercapai\":%d,"
    "\"sisa_detik\":%lu,"
    "\"sisa_waktu\":\"%s\","
    "\"status\":\"%s\","
    "\"mode\":\"%s\","
    "\"rtc\":\"%s\","
    "\"p\":%s,"
    "\"i\":%s,"
    "\"d\":%s,"
    "\"data_hold\":%d,"
    "\"total_sensor_error\":%lu}",
    waktuS,
    rawBuf,
    suhuBuf,
    setpointC,
    errorBuf,
    pwmAktual,
    SCALE_OTOMATIS,
    pidAktif ? 1 : 0,
    timerProsesAktif ? 1 : 0,
    setpointPernahTercapai ? 1 : 0,
    sisaDetik,
    sisaStr.c_str(),
    status,
    namaMode(modeAktif),
    rtcStr.c_str(),
    pBuf,
    iBuf,
    dBuf,
    dataHold ? 1 : 0,
    totalSensorError
  );

  mqttClient.publish(MQTT_TOPIC_DATA, payload, false);
}

// ============================ CSV ========================
void kirimCSV(float waktuS,
              float rawC,
              float suhuC,
              float error,
              float pTerm,
              float iTerm,
              float dTerm,
              bool dataHold,
              const char *status) {
  Serial.print(waktuS, 1);
  Serial.print(',');

  if (isnan(rawC)) Serial.print("nan");
  else Serial.print(rawC, 3);

  Serial.print(',');

  if (isnan(suhuC)) Serial.print("nan");
  else Serial.print(suhuC, 3);

  Serial.print(',');
  Serial.print(setpointC, 2);
  Serial.print(',');
  Serial.print(error, 3);
  Serial.print(',');
  Serial.print(kpAktif(), 6);
  Serial.print(',');
  Serial.print(kiAktif(), 6);
  Serial.print(',');
  Serial.print(kdAktif(), 6);
  Serial.print(',');
  Serial.print(pTerm, 3);
  Serial.print(',');
  Serial.print(iTerm, 3);
  Serial.print(',');
  Serial.print(dTerm, 3);
  Serial.print(',');
  Serial.print(pwmAktual);
  Serial.print(',');
  Serial.print(SCALE_OTOMATIS, 2);
  Serial.print(',');
  Serial.print(pidAktif ? 1 : 0);
  Serial.print(',');
  Serial.print(timerProsesAktif ? 1 : 0);
  Serial.print(',');
  Serial.print(totalSensorError);
  Serial.print(',');
  Serial.print(errorSensorBeruntun);
  Serial.print(',');
  Serial.print(dataHold ? 1 : 0);
  Serial.print(',');
  Serial.print(status);
  Serial.print(',');
  Serial.print(getRTCString());
  Serial.print(',');
  Serial.println(namaMode(modeAktif));

  publishMQTT(waktuS, rawC, suhuC, error, pTerm, iTerm, dTerm, dataHold, status);
}

// =========================== SETUP =======================
void setup() {
  Serial.begin(9600);
  delay(1000);

  setupWiFiMQTT();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  setupBuzzer();
  hentikanBuzzer();
  tesBuzzerAwal();

  if (rtc.begin()) {
    rtcTersedia = true;

    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  } else {
    rtcTersedia = false;
  }

  initTombol(tombolOK);
  initTombol(tombolUP);
  initTombol(tombolDOWN);

  pinMode(PIN_REN1, OUTPUT);
  pinMode(PIN_LEN1, OUTPUT);
  pinMode(PIN_REN2, OUTPUT);
  pinMode(PIN_LEN2, OUTPUT);

  digitalWrite(PIN_REN1, HIGH);
  digitalWrite(PIN_LEN1, HIGH);
  digitalWrite(PIN_REN2, HIGH);
  digitalWrite(PIN_LEN2, HIGH);

  setupPWM();
  matikanPeltier();

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  waktuMulaiSistem = millis();
  waktuKontrol = millis();

  perbaruiModeDariSetpoint();

  lcdPrintPad(0, 0, "Cold Brew Cooler");
  lcdPrintPad(0, 1, "ESP32 PID Control");
  lcdPrintPad(0, 2, "Scale 0.20 PMAX255");
  lcdPrintPad(0, 3, "Loading...");
  delay(1500);
  lcd.clear();

  Serial.println("# COLD BREW COOLER ESP32");
  Serial.println("# UI: PRESET / MANUAL");
  Serial.println("# OK=pilih/lanjut/stop, UP/DOWN=ubah");
  Serial.println("waktu_s,suhu_raw_C,suhu_C,setpoint_C,error_C,Kp,Ki,Kd,P,I,D,pwm,scale,pid_aktif,timer_aktif,total_sensor_error,error_beruntun,data_hold,status,rtc_datetime,mode");
}

// ============================ LOOP =======================
void loop() {
  bacaPerintahSerial();
  updateMQTT();
  updateBuzzer();
  prosesTombol();
  updateSisaWaktu();

  unsigned long sekarang = millis();

  if (sekarang - waktuLCDTerakhir >= LCD_UPDATE_MS) {
    waktuLCDTerakhir = sekarang;
    tampilLCD();
  }

  if (sekarang - waktuKontrol < SAMPLE_MS) {
    return;
  }

  waktuKontrol += SAMPLE_MS;

  float rawC = NAN;
  float suhuC = NAN;
  bool sensorValid = bacaSuhu(rawC, suhuC);

  float waktuS = (millis() - waktuMulaiSistem) / 1000.0f;

  if (!sensorValid) {
    totalSensorError++;
    errorSensorBeruntun++;

    if (punyaSuhuValid &&
        errorSensorBeruntun <= MAX_SENSOR_HOLD) {
      pertahankanPWM();

      float errorHold = suhuValidTerakhir - setpointC;
      float pHold = kpAktif() * errorHold;

      kirimCSV(
        waktuS,
        suhuRawTerakhir,
        suhuValidTerakhir,
        errorHold,
        pHold,
        integralTerm,
        0.0f,
        true,
        "HOLD_SUHU_TERAKHIR"
      );

      return;
    }

    stopPIDKontrol("SENSOR ERROR");

    kirimCSV(
      waktuS,
      NAN,
      NAN,
      NAN,
      NAN,
      integralTerm,
      NAN,
      false,
      "SENSOR_ERROR_PID_OFF"
    );

    return;
  }

  errorSensorBeruntun = 0;
  suhuRawTerakhir = rawC;
  suhuValidTerakhir = suhuC;
  punyaSuhuValid = true;

  perbaruiModeDariSetpoint();

  if (pidAktif &&
      !timerProsesAktif &&
      suhuMencapaiSetpoint(suhuC)) {
    mulaiTimerProses();
  }

  float batasOvershoot =
      (modeAktif == MODE_DINGIN)
          ? OVERSHOOT_DINGIN_C
          : OVERSHOOT_NORMAL_C;

  float batasSuhuMode = setpointC - batasOvershoot;
  float batasStop = (SUHU_MIN_ABSOLUT > batasSuhuMode)
                      ? SUHU_MIN_ABSOLUT
                      : batasSuhuMode;

  if (suhuC <= batasStop) {
    stopPIDKontrol("PROTEKSI OVERSHOOT");
    uiState = UI_FINISHED;
    lcd.clear();

    kirimCSV(
      waktuS,
      rawC,
      suhuC,
      suhuC - setpointC,
      0.0f,
      0.0f,
      0.0f,
      false,
      "PROTEKSI_OVERSHOOT"
    );

    return;
  }

  float error = suhuC - setpointC;
  float pTerm = 0.0f;
  float dTerm = 0.0f;

  if (pidAktif) {
    float Kp = kpAktif();
    float Ki = kiAktif();
    float Kd = kdAktif();

    pTerm = Kp * error;

    float dSuhu = 0.0f;

    if (!isnan(suhuSebelumnya)) {
      dSuhu = (suhuC - suhuSebelumnya) / DT;
    }

    derivativeFilter =
        ALPHA_D * derivativeFilter +
        (1.0f - ALPHA_D) * dSuhu;

    dTerm = -Kd * derivativeFilter;

    float integralKandidat =
        integralTerm + Ki * error * DT;

    integralKandidat =
        batasiFloat(integralKandidat, -255.0f, 255.0f);

    float outputKandidat =
        pTerm + integralKandidat + dTerm;

    bool saturasiAtas =
        outputKandidat > PWM_MAX_TETAP && error > 0.0f;

    bool saturasiBawah =
        outputKandidat < PWM_MIN && error < 0.0f;

    if (!(saturasiAtas || saturasiBawah)) {
      integralTerm = integralKandidat;
    }

    float outputPID =
        pTerm + integralTerm + dTerm;

    int pwmTarget =
        (int)(batasiFloat(outputPID, PWM_MIN, PWM_MAX_TETAP) + 0.5f);

    tulisPWM(pwmTarget);
  } else {
    matikanPeltier();
  }

  suhuSebelumnya = suhuC;

  const char *status =
      pidAktif ? "RUNNING" : "OFF";

  if (pidAktif && abs(error) <= 0.20f) {
    status = "DEKAT_SETPOINT";
  }

  kirimCSV(
    waktuS,
    rawC,
    suhuC,
    error,
    pTerm,
    integralTerm,
    dTerm,
    false,
    status
  );
}
