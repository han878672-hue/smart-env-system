#include <DHT.h>

// ===== 腳位 =====
#define DHTPIN 4
#define DHTTYPE DHT11
#define MQ135_PIN A0
#define UART_TX 0
#define UART_RX 1

DHT dht(DHTPIN, DHTTYPE);

// ===== 系統資料 =====
int g_temp = 0;
int g_hum = 0;
int g_air = 0;

// ===== 時間控制 =====
unsigned long t_dht = 0;
unsigned long t_mq = 0;

// ===== 讀DHT11 =====
void task_readDHT(unsigned long now) {
  if (now - t_dht >= 2000) {
    t_dht = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t)) g_temp = (int)t;
    else Serial.println("溫度讀取失敗");
    if (!isnan(h)) g_hum = (int)h;
    else Serial.println("濕度讀取失敗");
  }
}

// ===== 讀MQ135 =====
void task_readMQ135(unsigned long now) {
  if (now - t_mq >= 500) {
    t_mq = now;

    g_air = analogRead(MQ135_PIN);
  }
}

// ===== 顯示資料 =====
void task_print(unsigned long now) {
  static unsigned long t = 0;

  if (now - t >= 2000) {
    t = now;

    //====== 環境資料 ======
    Serial.print("溫度: ");
    Serial.print(g_temp);
    Serial.println(" °C");

    Serial.print("濕度: ");
    Serial.print(g_hum);
    Serial.println(" %");

    Serial.print("空氣品質(ADC): ");
    Serial.println(g_air);
    Serial.println();

    //====== RPi讀取 ======
    Serial1.print("<temp:");
    Serial1.print(g_temp);
    Serial1.print(",hum:");
    Serial1.print(g_hum);
    Serial1.print(",air:");
    Serial1.print(g_air);
    Serial1.print(">\n");

  }
}



// ===== 主程式 =====
void setup() {
  Serial.begin(9600);
  Serial1.begin(9600);
  dht.begin();
}

void loop() {
  unsigned long now = millis();

  task_readDHT(now);
  task_readMQ135(now);
  task_print(now);
}
