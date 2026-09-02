#include <HX711.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ================= WIFI & TELEGRAM =================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID   "YOUR_TELEGRAM_CHAT_ID"

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ================= LOAD CELL =================
#define DOUT1 4
#define CLK1  5
#define DOUT2 18
#define CLK2  19

HX711 scale1;
HX711 scale2;

float calibration_factor1 = 1051.1;
float calibration_factor2 = 1020.00;
float min_berat = 30.0;
float max_berat = 100.0;

// ✅ Rolling average buffer — baca 1 sample per interval, rata-ratakan manual
#define BERAT_SAMPLES 5
float bufBerat1[BERAT_SAMPLES] = {0};
float bufBerat2[BERAT_SAMPLES] = {0};
int   bufIdx = 0;

float getAvg(float* buf) {
  float sum = 0;
  for (int i = 0; i < BERAT_SAMPLES; i++) sum += buf[i];
  return sum / BERAT_SAMPLES;
}

// ================= SERVO =================
#define SERVO1_PIN 26
#define SERVO2_PIN 27
Servo servo1;
Servo servo2;
int lastServo1Pos = 0;
int lastServo2Pos = 0;

// ================= WATER LEVEL =================
#define WATER_SENSOR_1_PIN 34
#define WATER_SENSOR_2_PIN 35
#define RELAY1_PIN 14
#define RELAY2_PIN 12

#define TINGGI_SENSOR_CM 3.0
#define ADC_MIN_1 1300
#define ADC_MAX_1 1700
#define ADC_MIN_2 1400
#define ADC_MAX_2 1900
#define AIR_HABIS_CM  1.0
#define AIR_PENUH_CM  3.0

// ================= KONVEYOR =================
#define PWM_PIN        25
#define IR1_PIN        2
#define IR2_PIN        15
#define PWM_CHANNEL    4
#define PWM_FREQ       20000
#define PWM_RESOLUTION 8
#define MOTOR_SPEED    95

volatile int countIn  = 0;
volatile int countOut = 0;
bool motorRunning = false;
bool stopLocked   = false;

// ✅ Flag dari ISR — diproses di loop() bukan di dalam ISR
volatile bool ir1Triggered = false;
volatile bool ir2Triggered = false;
volatile unsigned long lastIR1 = 0;
volatile unsigned long lastIR2 = 0;
const unsigned long debounceDelay = 80;

// ================= TELEGRAM MESSAGE QUEUE =================
// ✅ Kunci utama: sendMessage TIDAK dipanggil langsung saat event terjadi
// Pesan dimasukkan ke antrian, lalu dikirim di slot Telegram terpisah
#define MAX_QUEUE 8
String telegramQueue[MAX_QUEUE];
int queueHead = 0;
int queueTail = 0;

void enqueueTelegram(String msg) {
  int next = (queueTail + 1) % MAX_QUEUE;
  if (next != queueHead) {           // queue tidak penuh
    telegramQueue[queueTail] = msg;
    queueTail = next;
  }
}

bool dequeueAndSend() {
  if (queueHead == queueTail) return false;  // queue kosong
  bot.sendMessage(CHAT_ID, telegramQueue[queueHead], "Markdown");
  queueHead = (queueHead + 1) % MAX_QUEUE;
  return true;
}

// ================= INTERRUPT SERVICE ROUTINE =================
// ✅ ISR hanya set flag + catat waktu, TIDAK ada logika berat/Serial/WiFi
void IRAM_ATTR onIR1() {
  unsigned long now = millis();
  if (now - lastIR1 > debounceDelay) {
    ir1Triggered = true;
    lastIR1 = now;
  }
}

void IRAM_ATTR onIR2() {
  unsigned long now = millis();
  if (now - lastIR2 > debounceDelay) {
    ir2Triggered = true;
    lastIR2 = now;
  }
}

// ================= TIMER =================
unsigned long lastBeratCheck  = 0;
unsigned long lastPesanCheck  = 0;
unsigned long lastSensorTime  = 0;
unsigned long lastTelegramSend = 0;

const unsigned long intervalBerat      = 300;   // servo update cepat
const unsigned long intervalPesan      = 3000;  // polling Telegram tiap 3 detik
const unsigned long intervalSensor     = 200;   // pompa update sangat cepat
const unsigned long intervalTelegramSend = 500; // kirim 1 antrian tiap 500ms

// ================= HELPER =================
float adcToCm(int adc, int adcMin, int adcMax) {
  adc = constrain(adc, adcMin, adcMax);
  return ((float)(adc - adcMin) / (adcMax - adcMin)) * TINGGI_SENSOR_CM;
}

void startMotor() {
  if (!motorRunning) {
    ledcWrite(PWM_CHANNEL, MOTOR_SPEED);
    motorRunning = true;
    Serial.println("Motor ON");
  }
}

void stopMotor() {
  if (motorRunning) {
    ledcWrite(PWM_CHANNEL, 0);
    motorRunning = false;
    Serial.println("Motor OFF");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  // Load cell
  scale1.begin(DOUT1, CLK1);
  scale2.begin(DOUT2, CLK2);
  delay(500);
  scale1.set_scale(calibration_factor1);
  scale2.set_scale(calibration_factor2);
  scale1.tare();
  scale2.tare();

  // Servo
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo1.write(0);
  servo2.write(0);

  // Water level + relay
  pinMode(WATER_SENSOR_1_PIN, INPUT);
  pinMode(WATER_SENSOR_2_PIN, INPUT);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  // IR sensor — pakai INTERRUPT bukan polling
  pinMode(IR1_PIN, INPUT);
  pinMode(IR2_PIN, INPUT);
  // ✅ RISING: trigger saat sinyal naik (obyek terdeteksi)
  attachInterrupt(digitalPinToInterrupt(IR1_PIN), onIR1, RISING);
  attachInterrupt(digitalPinToInterrupt(IR2_PIN), onIR2, RISING);

  // Motor konveyor
  pinMode(PWM_PIN, OUTPUT);
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);

  // WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  client.setInsecure();
  bot.sendMessage(CHAT_ID,
    "🐔💧 Sistem Pakan & Air Aktif\n🤖 Konveyor Telur Siap", "");
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  // ===================================================
  // ✅ BLOK 1: PROSES FLAG IR (dari interrupt, instan)
  // Motor dan logika counter dijalankan di sini, bukan di ISR
  // sendMessage TIDAK dipanggil — hanya enqueue
  // ===================================================
  if (ir1Triggered) {
    ir1Triggered = false;  // reset flag sebelum proses
    countIn++;
    stopLocked = false;
    startMotor();  // ← langsung, tanpa blocking apapun

    String msg = "*Telur Masuk* 🥚\n";
    msg += "⚙️ Motor: ON\n";
    msg += "📥 Masuk: " + String(countIn) + "\n";
    msg += "📤 Keluar: " + String(countOut);
    enqueueTelegram(msg);  // ← taruh di antrian, bukan kirim sekarang
  }

  if (ir2Triggered) {
    ir2Triggered = false;
    countOut++;

    String msg = "*Telur Keluar* 🥚\n";
    msg += "⚙️ Motor: " + String(motorRunning ? "ON" : "OFF") + "\n";
    msg += "📥 Masuk: " + String(countIn) + "\n";
    msg += "📤 Keluar: " + String(countOut);
    enqueueTelegram(msg);
  }

  if (countIn == countOut && countIn > 0 && !stopLocked) {
    stopMotor();
    stopLocked = true;

    String msg = "*Semua Telur Keluar* ✅\n";
    msg += "📥 Total: " + String(countIn);
    enqueueTelegram(msg);
  }

  // ===================================================
  // ✅ BLOK 2: CEK BERAT + SERVO (non-blocking)
  // get_units(1) = ~5ms, tidak blocking
  // Servo hanya ditulis jika posisi BERUBAH (hemat waktu)
  // ===================================================
  if (now - lastBeratCheck > intervalBerat) {
    if (scale1.is_ready()) bufBerat1[bufIdx] = scale1.get_units(1);
    if (scale2.is_ready()) bufBerat2[bufIdx] = scale2.get_units(1);
    bufIdx = (bufIdx + 1) % BERAT_SAMPLES;

    float berat1 = getAvg(bufBerat1);
    float berat2 = getAvg(bufBerat2);

    // ✅ Tentukan posisi target servo
    int target1 = lastServo1Pos;
    int target2 = lastServo2Pos;

    if      (berat1 < min_berat)       target1 = 90;
    else if (berat1 >= max_berat)      target1 = 180;

    if      (berat2 < min_berat)       target2 = 90;
    else if (berat2 >= max_berat)      target2 = 0;

    // ✅ Tulis servo hanya jika posisi beda — hindari tulis ulang terus-menerus
    if (target1 != lastServo1Pos) {
      servo1.write(target1);
      lastServo1Pos = target1;
    }
    if (target2 != lastServo2Pos) {
      servo2.write(target2);
      lastServo2Pos = target2;
    }

    lastBeratCheck = now;
  }

  // ===================================================
  // ✅ BLOK 3: CEK AIR + POMPA (interval 200ms)
  // analogRead cepat, relay diset langsung
  // ===================================================
  if (now - lastSensorTime > intervalSensor) {
    float cm1 = adcToCm(analogRead(WATER_SENSOR_1_PIN), ADC_MIN_1, ADC_MAX_1);
    float cm2 = adcToCm(analogRead(WATER_SENSOR_2_PIN), ADC_MIN_2, ADC_MAX_2);

    if      (cm1 <= AIR_HABIS_CM)  digitalWrite(RELAY1_PIN, HIGH);
    else if (cm1 >= AIR_PENUH_CM)  digitalWrite(RELAY1_PIN, LOW);

    if      (cm2 <= AIR_HABIS_CM)  digitalWrite(RELAY2_PIN, HIGH);
    else if (cm2 >= AIR_PENUH_CM)  digitalWrite(RELAY2_PIN, LOW);

    lastSensorTime = now;
  }

  // ===================================================
  // ✅ BLOK 4: KIRIM 1 PESAN DARI ANTRIAN (tiap 500ms)
  // Dipisah dari blok polling getUpdates
  // Tidak pernah blocking loop utama
  // ===================================================
  if (now - lastTelegramSend > intervalTelegramSend) {
    dequeueAndSend();
    lastTelegramSend = now;
  }

  // ===================================================
  // ✅ BLOK 5: TERIMA PERINTAH TELEGRAM (tiap 3 detik)
  // ===================================================
  if (now - lastPesanCheck > intervalPesan) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while (numNewMessages) {
      for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot.messages[i].chat_id;
        String pesan   = bot.messages[i].text;

        if (pesan == "/cekberat") {
          float b1 = getAvg(bufBerat1);
          float b2 = getAvg(bufBerat2);
          bot.sendMessage(chat_id,
            "📏 Berat:\nS1: " + String(b1,1) + " g\nS2: " + String(b2,1) + " g", "");
        }
        else if (pesan == "/cekair") {
          float cm1 = adcToCm(analogRead(WATER_SENSOR_1_PIN), ADC_MIN_1, ADC_MAX_1);
          float cm2 = adcToCm(analogRead(WATER_SENSOR_2_PIN), ADC_MIN_2, ADC_MAX_2);
          bot.sendMessage(chat_id,
            "💧 Air:\nS1: " + String(cm1,1) + " cm\nS2: " + String(cm2,1) + " cm", "");
        }
        else if (pesan == "/buka1")      { servo1.write(90);  lastServo1Pos = 90; }
        else if (pesan == "/tutup1")     { servo1.write(180); lastServo1Pos = 180; }
        else if (pesan == "/buka2")      { servo2.write(90);  lastServo2Pos = 90; }
        else if (pesan == "/tutup2")     { servo2.write(0);   lastServo2Pos = 0; }
        else if (pesan == "/pompa1_on")  digitalWrite(RELAY1_PIN, HIGH);
        else if (pesan == "/pompa1_off") digitalWrite(RELAY1_PIN, LOW);
        else if (pesan == "/pompa2_on")  digitalWrite(RELAY2_PIN, HIGH);
        else if (pesan == "/pompa2_off") digitalWrite(RELAY2_PIN, LOW);
        else if (pesan == "/motor_on")   startMotor();
        else if (pesan == "/motor_off")  { stopMotor(); stopLocked = true; }

        else if (pesan == "/cektelur") {
          String msg = "🥚 Konveyor:\n";
          msg += "⚙️ Motor: " + String(motorRunning ? "ON" : "OFF") + "\n";
          msg += "📥 Masuk: " + String(countIn) + "\n";
          msg += "📤 Keluar: " + String(countOut);
          bot.sendMessage(chat_id, msg, "");
        }
        else if (pesan == "/tare") {
          scale1.tare();
          scale2.tare();
          bot.sendMessage(chat_id, "Loadcell di-reset", "");
        }
        else if (pesan == "/start" || pesan == "/help") {
          String help = "📋 *Perintah Tersedia:*\n\n";
          help += "📏 /cekberat — Cek berat pakan\n";
          help += "💧 /cekair — Cek level air\n";
          help += "🐔 /buka1 /tutup1 — Servo pakan 1\n";
          help += "🐔 /buka2 /tutup2 — Servo pakan 2\n";
          help += "💦 /pompa1\\_on /pompa1\\_off — Pompa air 1\n";
          help += "💦 /pompa2\\_on /pompa2\\_off — Pompa air 2\n";
          help += "🥚 /cektelur — Status konveyor telur\n";
          help += "⚙️ /motor\\_on /motor\\_off — Kontrol motor\n";
          help += "⚖️ /tare — Reset loadcell";
          bot.sendMessage(chat_id, help, "Markdown");
        }
      }
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastPesanCheck = now;
  }

  // ✅ Tidak ada delay() di sini — loop berjalan secepat mungkin
}
