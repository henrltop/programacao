// ESP32 + SIM7000G - Comedouro Boa Esperança 🐕
// TinyGSM + DHT22 + GPS + Deep Sleep

#define TINY_GSM_MODEM_SIM7000
#include <TinyGsmClient.h>
#include <DHT.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==============================
// CONFIGURAÇÕES
// ==============================
#define TEMPO_DEEP_SLEEP 3600  // 1 hora
#define TIMEOUT_GPS 300000     // 5 minutos máximo para GPS

// Pinos
#define MODEM_TX         27
#define MODEM_RX         26
#define MODEM_PWRKEY     4
#define SENSOR_POWER_PIN 13
#define DHTPIN           32
#define DHTTYPE          DHT22

// Rede (Vivo)
const char apn[]  = "zap.vivo.com.br";
const char user[] = "vivo";
const char pass[] = "vivo";

// Cloudflare
const char serverIP[] = "104.21.52.175";
const int  port = 80;
const char hostHeader[] = "sensor.capivarajogos.shop";
const char workerPath[] = "/";

// Telegram (via Worker)
const char CHAT_ID[] = "1548579351";
const char KEY[]     = "aq1sw2de3";
const char DEVICE_NAME[] = "Comedouro Boa Esperança";

// ==============================
// OBJETOS
// ==============================
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
DHT dht(DHTPIN, DHTTYPE);

// Persistente no deep sleep
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR float ultimoLat = 0.0;
RTC_DATA_ATTR float ultimoLon = 0.0;
RTC_DATA_ATTR bool gpsJaFixou = false;

// Leituras
float temperatura = 0.0;
float umidade = 0.0;
int bateriaPct = 0;
float gpsLat = 0.0;
float gpsLon = 0.0;

// ==============================
// MODEM
// ==============================
void modemPowerOn() {
  Serial.println("📡 Ligando modem...");
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(5000);
}

bool iniciarModem() {
  Serial.println("🔄 Inicializando modem...");
  modem.restart();
  delay(3000);

  String info = modem.getModemInfo();
  Serial.println("📱 Modem: " + info);

  return (info.length() > 0);
}

bool conectarRede() {
  Serial.println("\n🌐 Conectando rede...");

  if (!modem.waitForNetwork(60000)) {
    Serial.println("❌ Timeout na rede");
    return false;
  }

  Serial.println("✅ Rede: " + modem.getOperator());

  Serial.println("⏳ Ativando GPRS...");
  if (!modem.gprsConnect(apn, user, pass)) {
    Serial.println("❌ Falha no GPRS");
    return false;
  }

  IPAddress ip = modem.localIP();
  Serial.print("✅ IP: ");
  Serial.println(ip);

  return (ip != IPAddress(0, 0, 0, 0));
}

// ==============================
// GPS
// ==============================
void obterGPS() {
  Serial.println("\n🛰️ Iniciando GPS...");
  
  // Liga GPS
  modem.sendAT("+CGPIO=0,48,1,1");
  delay(100);
  modem.sendAT("+CGNSPWR=1");
  delay(1000);
  modem.sendAT("+CGNSMOD=1,1,1,1"); // GPS + GLONASS
  delay(100);
  
  // Warm start se já fixou antes
  if (gpsJaFixou) {
    Serial.println("🛰️ Warm start (rápido)...");
    modem.sendAT("+CGNSWARM");
  } else {
    Serial.println("🛰️ Cold start (primeira vez)...");
  }
  delay(500);
  
  unsigned long inicio = millis();
  float spd, alt;
  int vsat, usat;
  bool primeiraVez = !gpsJaFixou;
  
  if (primeiraVez) {
    Serial.println("\n⚠️ PRIMEIRA VEZ - Buscando GPS");
    Serial.println("   Coloque antena ao AR LIVRE!");
    Serial.println("   Lado BRANCO com ponto metálico pro CÉU\n");
  }
  
  int ultimoSegundo = -1;
  
  while (true) {
    int seg = (millis() - inicio) / 1000;
    
    // Log a cada 15 segundos
    if (seg % 15 == 0 && seg != ultimoSegundo && seg > 0) {
      ultimoSegundo = seg;
      
      // Consulta satélites
      SerialAT.println("AT+CGNSINF");
      delay(300);
      String resp = "";
      while (SerialAT.available()) resp += (char)SerialAT.read();
      
      int satCount = 0;
      int commaCount = 0;
      for (int i = 0; i < resp.length(); i++) {
        if (resp[i] == ',') {
          commaCount++;
          if (commaCount == 14) {
            int nextComma = resp.indexOf(',', i + 1);
            if (nextComma > i) {
              satCount = resp.substring(i + 1, nextComma).toInt();
            }
            break;
          }
        }
      }
      
      Serial.printf("🛰️ Tempo: %ds | Satélites: %d\n", seg, satCount);
    }
    
    // Tenta obter fix
    if (modem.getGPS(&gpsLat, &gpsLon, &spd, &alt, &vsat, &usat)) {
      if (gpsLat != 0 && gpsLon != 0) {
        Serial.println("\n🎉 GPS FIX OBTIDO!");
        Serial.printf("📍 Lat: %.6f\n", gpsLat);
        Serial.printf("📍 Lon: %.6f\n", gpsLon);
        Serial.printf("🛰️ Satélites: %d\n\n", usat);
        
        ultimoLat = gpsLat;
        ultimoLon = gpsLon;
        gpsJaFixou = true;
        
        // Desliga GPS
        modem.sendAT("+CGNSPWR=0");
        return;
      }
    }
    
    // Timeout (exceto primeira vez)
    if (!primeiraVez && (millis() - inicio > TIMEOUT_GPS)) {
      Serial.println("\n⏰ Timeout GPS - usando última posição");
      gpsLat = ultimoLat;
      gpsLon = ultimoLon;
      modem.sendAT("+CGNSPWR=0");
      return;
    }
    
    delay(1000);
  }
}

// ==============================
// SENSORES
// ==============================
void lerSensores() {
  Serial.println("\n📊 Lendo sensores...");

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(2000);

  dht.begin();
  delay(500);

  for (int i = 0; i < 3; i++) {
    temperatura = dht.readTemperature();
    umidade = dht.readHumidity();
    if (!isnan(temperatura) && !isnan(umidade)) break;
    delay(500);
  }

  if (isnan(temperatura)) temperatura = 0;
  if (isnan(umidade)) umidade = 0;

  digitalWrite(SENSOR_POWER_PIN, LOW);

  Serial.printf("🌡️ Temp: %.1f°C\n", temperatura);
  Serial.printf("💧 Umid: %.1f%%\n", umidade);
}

void lerBateria() {
  Serial.println("\n🔋 Lendo bateria...");

  int batMV = modem.getBattVoltage();
  float volts = batMV / 1000.0;

  bateriaPct = map(batMV, 3300, 4200, 0, 100);
  bateriaPct = constrain(bateriaPct, 0, 100);

  Serial.printf("🔋 Bateria: %d%% (%.2fV)\n", bateriaPct, volts);
}

// ==============================
// HTTP POST
// ==============================
String buildJsonBody() {
  String body = "{";
  body += "\"key\":\"" + String(KEY) + "\",";
  body += "\"chat_id\":\"" + String(CHAT_ID) + "\",";
  body += "\"device\":\"" + String(DEVICE_NAME) + "\",";
  body += "\"temp\":" + String(temperatura, 1) + ",";
  body += "\"umid\":" + String(umidade, 1) + ",";
  body += "\"bat\":" + String(bateriaPct) + ",";
  body += "\"lat\":" + String(gpsLat, 6) + ",";
  body += "\"lon\":" + String(gpsLon, 6) + ",";
  body += "\"ciclo\":" + String(bootCount);
  body += "}";
  return body;
}

bool enviarDados() {
  Serial.println("\n📤 Enviando dados...");

  String body = buildJsonBody();
  int contentLength = body.length();

  for (int tent = 1; tent <= 3; tent++) {
    Serial.printf("\n📡 Tentativa %d/3\n", tent);

    if (client.connect(serverIP, port)) {
      Serial.println("✅ Conectado!");

      client.print(String("POST ") + workerPath + " HTTP/1.1\r\n");
      client.print(String("Host: ") + hostHeader + "\r\n");
      client.print("Connection: close\r\n");
      client.print("Content-Type: application/json\r\n");
      client.print(String("Content-Length: ") + contentLength + "\r\n\r\n");
      client.print(body);

      Serial.println("📤 Enviado!");

      unsigned long timeout = millis();
      bool sucesso = false;
      String statusLine = "";

      while (client.connected() && millis() - timeout < 12000) {
        while (client.available()) {
          String line = client.readStringUntil('\n');
          line.trim();
          if (statusLine.length() == 0) statusLine = line;
          if (statusLine.indexOf(" 200 ") >= 0) sucesso = true;
        }
      }

      client.stop();

      if (sucesso) {
        Serial.println("\n🎉 SUCESSO!");
        return true;
      }
    } else {
      Serial.println("❌ Falha ao conectar");
    }

    delay(3000);
  }

  return false;
}

// ==============================
// DEEP SLEEP
// ==============================
void entrarDeepSleep() {
  Serial.println("\n💤 Dormindo por 1 hora...");

  modem.gprsDisconnect();
  modem.poweroff();

  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)TEMPO_DEEP_SLEEP * 1000000ULL);
  esp_deep_sleep_start();
}

// ==============================
// SETUP
// ==============================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);

  bootCount++;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  🐕 COMEDOURO BOA ESPERANÇA           ║");
  Serial.printf("║  📊 Ciclo #%-3d                        ║\n", bootCount);
  Serial.println("╚════════════════════════════════════════╝\n");

  // 1) Sensores
  lerSensores();

  // 2) Modem
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  modemPowerOn();

  if (!iniciarModem()) {
    Serial.println("❌ Falha no modem");
    entrarDeepSleep();
    return;
  }

  // 3) Bateria
  lerBateria();

  // 4) GPS
  obterGPS();

  // 5) Rede
  if (!conectarRede()) {
    Serial.println("❌ Sem rede");
    entrarDeepSleep();
    return;
  }

  // 6) Envia
  enviarDados();

  // 7) Dorme
  entrarDeepSleep();
}

void loop() {}