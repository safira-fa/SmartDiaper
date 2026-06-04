#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_pm.h>
#include "SparkFunLIS3DH.h"
#include "Wire.h"
#include <GyverDS18.h>

// BLE Service and Characteristics UUIDs
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" 

//глобальные переменные
bool micro = false;
bool accel = false;
bool water = false;
float temp;
unsigned long start_time;

//настройка BLE
BLECharacteristic *txCharacteristic;
bool deviceConnected = false;

class ServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
  }
};


void initBLE() {
  // Очищаем предыдущую инициализацию BLE
  if (BLEDevice::getInitialized()) {
    BLEDevice::deinit();
    start_time = millis();
    while(millis() - start_time < 100);
  }
  
  // Инициализируем BLE заново
  BLEDevice::init("ESP_C3_BLE");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  
  BLEService *service = server->createService(SERVICE_UUID);
  
  txCharacteristic = service->createCharacteristic(
    TX_UUID, BLECharacteristic::PROPERTY_NOTIFY
  );
  txCharacteristic->addDescriptor(new BLE2902());
  
  service->start();
  server->getAdvertising()->start();
}

//настройка акселерометра
LIS3DH myIMU(I2C_MODE, 0x19); 

void setAccel(){
  myIMU.settings.accelSampleRate = 50;  //Hz.  Can be: 0,1,10,25,50,100,200,400,1600,5000 Hz
  myIMU.settings.accelRange = 4;      //Max G force readable.  Can be: 2, 4, 8, 16
  myIMU.settings.adcEnabled = 0;
  myIMU.settings.tempEnabled = 0;
  myIMU.settings.xAccelEnabled = 1;
  myIMU.settings.yAccelEnabled = 1;
  myIMU.settings.zAccelEnabled = 1;
  myIMU.begin();
  
  //настройки прерывания INT1
  //ось
  myIMU.writeRegister(LIS3DH_INT1_CFG, 0x20); //z high

  //сила ускорения свободного падения  
  myIMU.writeRegister(LIS3DH_INT1_THS, 0x30); // 1,5/8 range
  
  //длительность
  myIMU.writeRegister(LIS3DH_INT1_DURATION, 0x04); // 1 * 1/50 s = 20ms

  //задержка значения до считывания
  uint8_t dataToWrite;
  myIMU.readRegister(&dataToWrite, LIS3DH_CTRL_REG5);
  dataToWrite &= 0xF3; //Clear bits of interest
  dataToWrite |= 0x08; //Latch interrupt (Cleared by reading int1_src)
  myIMU.writeRegister(LIS3DH_CTRL_REG5, dataToWrite);

  //генерация прерывания на INT1, на INT2 - нет 
  myIMU.writeRegister(LIS3DH_CTRL_REG3, 0x60);
  myIMU.writeRegister(LIS3DH_CTRL_REG6, 0x00);
}

//настройка датчика температуры
GyverDS18Single ds(3);  // пин температуры

void setup() {
  //настройка пинов
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << GPIO_NUM_1), //micro
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .intr_type = GPIO_INTR_LOW_LEVEL
  };
  gpio_config(&io_conf);
  
  io_conf = {
    .pin_bit_mask = (1ULL << GPIO_NUM_2), //accel
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .intr_type = GPIO_INTR_HIGH_LEVEL
  };
  gpio_config(&io_conf);

  io_conf = {
    .pin_bit_mask = (1ULL << GPIO_NUM_0), //water
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE
  };
  gpio_config(&io_conf);

  io_conf = {
    .pin_bit_mask = (1ULL << GPIO_NUM_3), //temperature
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE
  };
  gpio_config(&io_conf);
  
  ds.setResolution(12);

  setAccel();
  
  initBLE();

  //настройка Light Sleep по прерыванию
  gpio_wakeup_enable(GPIO_NUM_1, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_2, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  accel = false;
}

void loop() {
  water = false;
  micro = false;
  
  //настройка Light Sleep по таймеру 2 мин
  esp_sleep_enable_timer_wakeup(120000000ULL); 
  esp_light_sleep_start();

  //опрос датчиков прерываний
  if(gpio_get_level(GPIO_NUM_1) == 0){
    micro = true;
    }
  if(gpio_get_level(GPIO_NUM_2) == 1){
    accel = true;
    }
  
  //запуск Bluetooth
  if (!deviceConnected) {
    BLEDevice::startAdvertising();
  }
  
  //измерение температуры
  if (!ds.tick()) {
        temp = ds.getTemp();
    }
  
  //определение положения
  if(myIMU.readFloatAccelZ()<0.4 && myIMU.readFloatAccelZ()>-0.4){
    accel = false;
    }

  //мокрый ли подгузник
  if(gpio_get_level(GPIO_NUM_0) == 0){
      water = true;
  }
  
  //повторная проверка микрофона
  if(gpio_get_level(GPIO_NUM_1) == 0){
    micro = true;
  }
  
  //задержка для установки связи BLE
  start_time = millis();
  while(millis() - start_time < 2000);
  
  //повторное измерение температуры
  if (!ds.tick()) {
        temp = ds.getTemp();
    }
    
  //отправка уведомления
  if (deviceConnected) {
    char buffer[50];
    sprintf(buffer, "wake: micro %d, accel %d, water %d, temp %4.1f", micro, accel, water, temp);
    txCharacteristic->setValue(buffer);
    txCharacteristic->notify();
    start_time = millis();
    while(millis() - start_time < 100);
  }

  //очищение пина прерываний акселерометра
  uint8_t dataRead;
  myIMU.readRegister(&dataRead, LIS3DH_INT1_SRC);

  start_time = millis();
  while(millis() - start_time < 20);
}