#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <PubSubClient.h>

//характеристики BLE
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define TARGET_DEVICE_NAME "ESP_C3_BLE"

// настройки MQTT
const char* mqtt_server = "dev.rightech.io";
const int mqtt_port = 1883;
const char* clientId = "smart_diaper_wqtt";
char accel, micro, water;
char temp[5];
const int sending_period = 5;
const bool retain_flag = false;
const char* accel_topic = "base/state/accel";
const char* micro_topic = "base/state/micro";
const char* water_topic = "base/state/water";
const char* temp_topic = "base/state/temp";

WiFiClient espClient;
PubSubClient client(espClient);

// настройки WiFi
const char* AP_SSID = "SmartDiaper_Config";
const char* AP_PASSWORD = "12345678";
Preferences prefs;
const char* PREF_NS = "wifi_cfg";
const char* PREF_SSID_KEY = "ssid";
const char* PREF_PASS_KEY = "pass";

WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
unsigned long configTimeout = 120000;
unsigned long configStart = 0;

//страница подключения к сети
const char* CONFIG_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head><meta charset="UTF-8"><title>WiFi</title></head>
<body>
<form id="f">
  SSID: <input name="ssid" required><br>
  Пароль: <input type="password" name="pass"><br><br>
  <button>Сохранить</button>
</form>
<div id="s"></div>
<script>
  const s = document.getElementById('s');
  document.getElementById('f').addEventListener('submit', async e => {
    e.preventDefault();
    s.textContent = '⏳ Подключение...';
    try {
      const r = await fetch('/save', { method: 'POST', body: new FormData(e.target) });
      const j = await r.json();
      s.textContent = j.success ? '✅ Успешно!' : '❌ ' + (j.error || 'Ошибка');
      if (j.success) setTimeout(() => location.reload(), 2000);
    } catch { s.textContent = '❌ Ошибка сети'; }
  });
</script>
</body>
</html>
)rawliteral";

//подключение к сохраненному Wi-fi
bool tryConnectToWiFi() {
  if (!prefs.isKey(PREF_SSID_KEY) || !prefs.isKey(PREF_PASS_KEY)) return false;
  String ssid = prefs.getString(PREF_SSID_KEY, "");
  String pass = prefs.getString(PREF_PASS_KEY, "");
  if (ssid.isEmpty()) return false;
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 30; i++) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  WiFi.disconnect();
  return false;
}

//создание точки доступа Wi-fi
void startAP() {
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", CONFIG_PAGE);
  });
  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    if (ssid.length() < 1) {
      server.send(200, "application/json", R"({"success":false,"error":"Проверьте данные"})");
      return;
    }
    prefs.begin(PREF_NS, false);
    prefs.putString(PREF_SSID_KEY, ssid);
    prefs.putString(PREF_PASS_KEY, pass);
    prefs.end();
    
    server.send(200, "application/json", R"({"success":true})");
    delay(500);
    ESP.restart();
  });
  
  server.onNotFound([]() {
    server.send(200, "text/html", CONFIG_PAGE);
  });
  server.begin();
}

//проверка таймаута подключения к Wi-fi
void checkConfigTimeout() {
  if (millis() - configStart > configTimeout) ESP.restart();
}

// настройки BLE клиента
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pTXCharacteristic = nullptr;
bool deviceConnected = false;
bool doConnect = false;
bool doScan = true;
String targetDeviceAddress = "";
unsigned long lastScanTime = 0;
unsigned long lastAttempt = 0;
unsigned long lastConnectionCheck = 0;
const unsigned long SCAN_INTERVAL = 1000;
const unsigned long CONNECTION_CHECK_INTERVAL = 500;

//обработка (пересылка по MQTT) сообщения от BLE
static void notificationCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  for (int i = 0; i < length; i++) {
    Serial.print((char)pData[i]);
  }
  Serial.println();
  
  accel = (char)pData[21];
  micro = (char)pData[12];
  water = (char)pData[30];
  for (int i = 38; i < 42; i++){
    temp[i-38] = (char)pData[i];
  }
  client.publish(accel_topic, String(accel).c_str(), retain_flag);
  yield();
  client.publish(micro_topic, String(micro).c_str(), retain_flag);
  yield();
  client.publish(water_topic, String(water).c_str(), retain_flag);
  yield();
  client.publish(temp_topic, temp, retain_flag);
}

//класс для клиента BLE
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveName() && advertisedDevice.getName() == TARGET_DEVICE_NAME) {
      BLEDevice::getScan()->stop();
      targetDeviceAddress = advertisedDevice.getAddress().toString().c_str();
      doConnect = true;
      doScan = false;
    }
  }
};

//подключение к BLE серверу
bool connectToServer(BLEAddress pAddress) {
  pClient = BLEDevice::createClient();
  if (!pClient->connect(pAddress)) return false;
  
  BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);
  if (!pRemoteService) { pClient->disconnect(); return false; }
  
  pTXCharacteristic = pRemoteService->getCharacteristic(TX_UUID);
  if (!pTXCharacteristic) { pClient->disconnect(); return false; }
  
  if (pTXCharacteristic->canNotify()) {
    pTXCharacteristic->registerForNotify(notificationCallback);
    uint8_t notifyOn[] = {0x01, 0x00};
    BLERemoteDescriptor* desc = pTXCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));
    if (desc) desc->writeValue(notifyOn, 2, true);
  }
  deviceConnected = true;
  return true;
}

//поиск устройств BLE по близости
void scanForDevices() {
  if (millis() - lastScanTime < SCAN_INTERVAL) return;
  lastScanTime = millis();
  
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(50);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(1, false);
}

//проверка активности BLE соединения
void checkConnection() {
  if (millis() - lastConnectionCheck < CONNECTION_CHECK_INTERVAL) return;
  lastConnectionCheck = millis();
  
  if (deviceConnected && !pClient->isConnected()) {
    deviceConnected = false;
    delete pClient; 
    pClient = nullptr;
    pTXCharacteristic = nullptr;
    doScan = true;
  }
}

//переподключение по BLE
void tryReconnect() {
  lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;  // Пробуем раз в 5 сек
  lastAttempt = millis();
  
  if (client.connect(clientId)) {
    Serial.println("MQTT connected");
  } else {
    Serial.println("MQTT connect failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  prefs.begin(PREF_NS, false);
  //пытаемся подключиться по сохраненным данным сети
  if (tryConnectToWiFi()) {
    Serial.println("Подключился к Wi-fi");
    //настраиваем и начинаем поиск BLE устройств
    client.setServer(mqtt_server, mqtt_port);
    client.setKeepAlive(30);
    client.setBufferSize(512);
    BLEDevice::init("ESP32_BLE_Client");
    scanForDevices();
    return;
  }
  //если не удалось - запускаем точку доступа Wi-fi
  configStart = millis();
  startAP();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) { //если Wi-fi подключен
    if (!client.connected()) tryReconnect();
    client.loop(); //подключаемся к BLE серверу
    
    if (doConnect) { //если подключились 
      if (connectToServer(BLEAddress(targetDeviceAddress.c_str()))) {
        Serial.println("Successfully connected!");
        doConnect = false;
      } else {
        Serial.println("Connection failed, will retry...");
        doConnect = false;
        doScan = true;
      }
    }
    if (doScan) scanForDevices(); //иначе продолжаем поиск
    checkConnection(); //проверяем соединение
    delay(10);
    return;
  }
  //если не удалось подключиться к Wi-fi, включаем режим точки доступа  
  dnsServer.processNextRequest(); //перенаправляем пользователя на IP страницы авторизации
  server.handleClient(); //запускаем обработку html-страницы
  checkConfigTimeout();
  delay(10);
}