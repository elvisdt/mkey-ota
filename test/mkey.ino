/**********************************LIBRERIAS**************************************/
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>
#include <BLEServer.h>
#include <Preferences.h>
#include "esp_system.h"
#include "rom/ets_sys.h"
#if CONFIG_IDF_TARGET_ESP32C3
#include "esp32c3/rom/rtc.h"
#else 
#error Target CONFIG_IDF_TARGET is not supported
#endif
#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
/*******************************PIN DEEP SLEEP*************************************/ 
String Device1="bc:57:29:0b:29:a7"; // asignar llaverito de fabrica todo en minuscula
String Device2="bc:57:29:0b:29:e0"; // asignar llaverito de fabrica todo en minuscula
#define BUTTON_PIN_BITMASK 0x20 /*GPIO05*/
  /*TIEMPO PARA ENABLE WDT*/
/*******************CREAMOS LOS CONDICIONADORES BOLEANOS**************************/
bool CHECK_NAME = 0;
bool CHECK_MAC = 0;
bool CHECK_RSSI = 0;
bool CHECK_DATA = 0;
bool CHECK_DATA2 = 0;
bool CHECK_MAC2 = 0;
bool CHECK_RSSI2 = 0;
bool flanco_door =1;
bool ACTIVO = 0;
bool led = 0;
bool bajo_consumo = 0;
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool Confirmacion_IGN = false;
bool Confirmacion_BLOQUEO = false;
bool sinbeacon = false;
bool bloq = false;
/*******************************RSSI LECTOR*****************************************/
int CHECK_SENAL = -120;
int CHECK_SENAL2 = -120;
int SCAN = 0;
int contador = 0;

unsigned long previousMillis = 0;  // Guarda el último tiempo que se ejecutó la función
const long interval = 10;          // Intervalo de 50 ms

/**********************************************************************************/
/*******************************EEPROM*********************************************/
/**********************************************************************************/
String Comandos;
Preferences device; // BEACON 1
Preferences device2; // BEACON 2 // SMARTWATCH(AUN NO RECOMENDADO).
Preferences data;  // PARA ACTIVAR DATOS BEACON URL
Preferences data2; // PARA ACTIVAR MODO SOS
Preferences RSSI1;
Preferences RSSI2;

String Datastring;

void print_reset_reason(int reason){
  switch ( reason)
  {
    case 1 : Serial.println ("POWERON_RESET");break;          /**<1,  Vbat power on reset*/
    case 3 : Serial.println ("TIMER_WDT_RESET");break;               /**<3,  Software reset digital core*/
    case 4 : Serial.println ("OWDT_RESET");break;             /**<4,  Legacy watch dog reset digital core*/
    case 5 : Serial.println ("DEEPSLEEP_RESET");break;        /**<5,  Deep Sleep reset digital core*/
    case 6 : Serial.println ("SDIO_RESET");break;             /**<6,  Reset by SLC module, reset digital core*/
    case 7 : Serial.println ("TG0WDT_SYS_RESET");break;       /**<7,  Timer Group0 Watch dog reset digital core*/
    case 8 : Serial.println ("TG1WDT_SYS_RESET");break;       /**<8,  Timer Group1 Watch dog reset digital core*/
    case 9 : Serial.println ("RTCWDT_SYS_RESET");break;       /**<9,  RTC Watch dog Reset digital core*/
    case 10 : Serial.println ("INTRUSION_RESET");break;       /**<10, Instrusion tested to reset CPU*/
    case 11 : Serial.println ("TGWDT_CPU_RESET");break;       /**<11, Time Group reset CPU*/
    case 12 : Serial.println ("SW_CPU_RESET");break;          /**<12, Software reset CPU*/
    case 13 : Serial.println ("RTCWDT_CPU_RESET");break;      /**<13, RTC Watch dog Reset CPU*/
    case 14 : Serial.println ("EXT_CPU_RESET");break;         /**<14, for APP CPU, reseted by PRO CPU*/
    case 15 : Serial.println ("RTCWDT_BROWN_OUT_RESET");break;/**<15, Reset when the vdd voltage is not stable*/
    case 16 : Serial.println ("RTCWDT_RTC_RESET");break;      /**<16, RTC Watch dog reset digital core and rtc module*/
    default : Serial.println ("NO_MEAN");
  }
}
/***************************TIEMPO DE ESCANEO************************************/
int scanTime = 1; //In seconds
BLEScan* pBLEScan;

const int wdtTimeout = 10000;  //time in ms to trigger the watchdog
hw_timer_t * timer = NULL;
void ARDUINO_ISR_ATTR resetModule(){
  ets_printf("reboot\n");
  esp_restart();
}
void setup()
{ 
  //reset_pines();
  /********************************************************************************/
  Serial.begin(115200);// CONFIGURACIONES DE BUAD PARA TRANSMISION DE DATOS
  /********************************************************************************/
  Serial.print("CPU0 reset reason: ");
  print_reset_reason(rtc_get_reset_reason(0));
  timer = timerBegin(1000000);                   //timer 1Mhz resolution
  timerAttachInterrupt(timer, &resetModule);           //attach callback
  timerAlarm(timer, wdtTimeout * 1000, false, 0); //set time in us
 
  //inicioServidor.begin("server");
  /********************************************************************************/
  //CONFIGURACIONES DE ENTRADAS.
  pinMode(0, GPIO_MODE_OUTPUT);// BUZZER
  pinMode(2, GPIO_MODE_OUTPUT);// RELAY
  pinMode(3, GPIO_MODE_OUTPUT);// OUT1
  pinMode(4, GPIO_MODE_OUTPUT);// OUT2
  pinMode(7, GPIO_MODE_OUTPUT);// LED TESTER
  pinMode(5, GPIO_MODE_INPUT);// PUERTA
  pinMode(6, GPIO_MODE_INPUT);// IN1
  pinMode(1, GPIO_MODE_INPUT);//IGN

 /********************************************************************************/
  
  digitalWrite(0, 0);//APAGAD0
  digitalWrite(2, 1);//ENCENDIDO
  digitalWrite(3, 0);//APAGADO
  digitalWrite(4, 0);//APAGADO SIRENA
  digitalWrite(7, 0);//APAGADO
  
  
  Serial.println(Device1);
  Serial.println(Device2);
  Serial.println(Datastring);
  //Serial.println(veces);
  // NVS obtener parametro guardado
  /********************************************************************************/
  /************************** DEEP SLEEP ******************************************/
  
  
  esp_deep_sleep_enable_gpio_wakeup(BUTTON_PIN_BITMASK, ESP_GPIO_WAKEUP_GPIO_LOW);
  //esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST, ESP_PD_OPTION_OFF);
  esp_sleep_config_gpio_isolate();

  timerWrite(timer, 0); //reset timer (feed watchdog)
  /**********************************WHILE*****************************************/


  Serial.println("Desperte");
  ACTIVO = true;
  Serial.println(" || MODO ACTIVO 1 || ");
  while(ACTIVO) // MODO BEACON
  { 
    // PRIMER CONTADOR( BUCADOR DE BEACON).
    if(SCAN>250) // CONTADOR 5 minutos
    { 
      timerWrite(timer, 0); //reset timer (feed watchdog)
      Serial.println(" ||  FINALLY TIME  ||");
      digitalWrite(7,0); //apagdo
      ACTIVO = 0; // SALIDOS DEL WHILE PRINCIPAL.
      SCAN = 0; // REESTABLECEMOS EL SCAN.
      bajo_consumo = 1; // BOLEANO PARA ENTRAR EN MODO BAJO CONSUMO.
    }
    else // BUSCAN BEACONS Y ESPERANDO CLAVE PARA ENTRAR EN MODO PROGRAMADOR
    {
        digitalWrite(7,1);//RGB
        SCAN_BLE();
        digitalWrite(7,0);//RGB
        while((CHECK_MAC && CHECK_RSSI)||(CHECK_MAC && CHECK_RSSI && CHECK_DATA)||(CHECK_MAC2 && CHECK_RSSI2)||(CHECK_MAC2 && CHECK_RSSI2 && CHECK_DATA2))// ESTE WHILE SOLO FUNCIONA CUANDO DETECTA EL BEACON CERCA.
        { 
          int IGN_OFF2 = digitalRead(1);
          int PUERTA_ON = digitalRead(5);
          //Serial.println(PUERTA_ON);
          if(IGN_OFF2 == 1)
          {
            Serial.print(" || IGN OFF:");
            contador++;
            Serial.println(contador/500);
            Serial.println("  ||");
            timerWrite(timer, 0); //reset timer (feed watchdog)
            digitalWrite(2, 1);//RELAY
            digitalWrite(7,1);//RGB
            //Serial.println(flanco_door);
            if(PUERTA_ON == 0)
            {
              flanco_door=0;
              contador = 0;
              Serial.println("|| PUERTA ABIERTA ||");
            }
            if(contador>15000 &&  contador<16000 &&  flanco_door==0)//contador de 30 segundos
            {
              
              flanco_door=1;
              digitalWrite(2, 1);//RELAY
              digitalWrite(7,0); // RGB
              Serial.println("|| IGN OFF 30S ||");
              contador = 0;
              CHECK_MAC = 0;
              CHECK_MAC2 = 0;
              ACTIVO = 0;
              Confirmacion_IGN = 1;
            }
            if(contador>300000)//contador de 10 minutos
            {
              flanco_door=1;
              digitalWrite(2, 1);//RELAY
              digitalWrite(7,0); // RGB
              Serial.println("|| IGN OFF 600S ||");
              contador = 0;
              CHECK_MAC = 0;
              CHECK_MAC2 = 0;
              ACTIVO = 0;
              Confirmacion_IGN = 1;
            }
          }
          if(IGN_OFF2 == 0)
          {
            
            flanco_door=1;
            contador=0;
            Serial.println(" || IGN ON || ");
            digitalWrite(2, 0);//RELAY
            digitalWrite(7,0);//RGB
            CHECK_MAC = 1;
            CHECK_MAC2 = 1;
            CHECK_DATA = 1;
            CHECK_DATA2 = 1;
            CHECK_RSSI = 1;
            CHECK_RSSI2 = 1;
            timerWrite(timer, 0); //reset timer (feed watchdog) 
          }
          //digitalWrite(2, 0);//RELAY
          delay(1);
        }
      //*******************************************************************************************
      timerWrite(timer, 0); //reset timer (feed watchdog)
      Serial.print("SCAN:");
      Serial.println(SCAN);
      //*******************************************************************************************
    }
  }
  if(Confirmacion_IGN == 1)
  {
    digitalWrite(2, 1);//RELAY
    digitalWrite(7,0);//RGB
    flanco_door=1;
    Serial.println("|| DEEPSLEEP IGN OFF ||");
    Serial.println("3");
    Serial.println("2");
    Serial.println("1");
    esp_deep_sleep_start();   
  }
  if(bajo_consumo == 1)
  {
    digitalWrite(2, 1);//RELAY
    digitalWrite(7,0);//RGB
    flanco_door=1;
    Serial.println("|| DEEPSLEEP SCANER OFF ||");
    Serial.println("3");
    Serial.println("2");
    Serial.println("1");
    esp_deep_sleep_start(); 
  }
}
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String beaconAddress = advertisedDevice.getAddress().toString().c_str();
    
    // Verifica si la dirección coincide con tus dispositivos
    if (beaconAddress == Device1) {
      Serial.println("Device1 encontrado!");
      handleDevice(advertisedDevice, CHECK_SENAL, true); // Manejar Device1
    } 
    else if (beaconAddress == Device2) {
      Serial.println("Device2 encontrado!");
      handleDevice(advertisedDevice, CHECK_SENAL2, true); // Manejar Device2
    }

  }
};
void SCAN_BLE() {
  Serial.print("|| SCANEANDO");
  Serial.print(" || flancos:");
  
  BLEDevice::init("JBL CHARGER 6H");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  BLEScanResults *foundDevices = pBLEScan->start(scanTime, false);
  Serial.print(" || (");
  Serial.print(foundDevices->getCount());
  Serial.print(") || ");
  SCAN++;
}
// Función para manejar dispositivos individuales
void handleDevice(BLEAdvertisedDevice &device, int signalThreshold, bool isDevice1) {
  String manufData = device.toString().c_str();
  String metaData = manufData.substring(114, 123);
  int beaconRSSI = device.getRSSI();
  
  // Variables de estado para cada dispositivo
  bool &checkMac = isDevice1 ? CHECK_MAC : CHECK_MAC2;
  bool &checkRSSI = isDevice1 ? CHECK_RSSI : CHECK_RSSI2;
  bool &checkData = isDevice1 ? CHECK_DATA : CHECK_DATA2;

  checkMac = true;
  Serial.print(" || MetaData:");
  Serial.print(metaData);
  Serial.print(isDevice1 ? " || TAG1=TRUE" : " || TAG2=TRUE");
  Serial.print(" || RSSI:");
  Serial.print(beaconRSSI);

  // Verifica si el RSSI supera el umbral
  if (beaconRSSI >= signalThreshold) {
    checkRSSI = true;
    Serial.print(isDevice1 ? " || RSSI1=TRUE" : " || RSSI2=TRUE");
    // Verifica si los datos coinciden
    if (metaData == "&H123$") {
      checkData = true;
      Serial.print(" || Data:OK");
      
      // Acciones de desbloqueo
      digitalWrite(2, LOW);  // Relay desbloqueo
      digitalWrite(7, HIGH); // LED tester
      analogWrite(0, 150);   // Buzzer pitido
      analogWrite(0, 0);     // Apaga buzzer
      delay(50);
      pBLEScan->setActiveScan(false);
    }
  } else {
    checkRSSI = false;
    Serial.print(isDevice1 ? " || RSSI1=FALSE" : " || RSSI2=FALSE");
  }
}
void loop() 
{  
}