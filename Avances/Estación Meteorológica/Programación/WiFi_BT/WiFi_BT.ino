
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// LED INTEGRADO
#define LED_PIN 2

bool estadoLED = false;

// WIFI
const char* ssid = "ESP32_Ecomundo";
const char* password = "12345678";

WebServer server(80);

// BLUETOOTH BLE
#define SERVICE_UUID "12345678-1234-1234-1234-123456789abc"
#define CHAR_UUID "abcdefab-1234-1234-1234-abcdefabcdef"

BLECharacteristic* caracteristicaBLE = nullptr;
volatile bool reiniciarBLE = false;

// CONTROL DEL LED
void controlarLED(bool encender) {
  estadoLED = encender;

  digitalWrite(LED_PIN, encender ? HIGH : LOW);

  if (caracteristicaBLE != nullptr) {
    caracteristicaBLE->setValue(encender ? "ON" : "OFF");
  }

  Serial.println(encender ? "LED ENCENDIDO" : "LED APAGADO");
}

// BLUETOOTH
class ConexionBLE : public BLEServerCallbacks {
  void onConnect(BLEServer* servidor) override {
    Serial.println("Bluetooth conectado");
  }

  void onDisconnect(BLEServer* servidor) override {
    Serial.println("Bluetooth desconectado");
    reiniciarBLE = true;
  }
};

// PAGINA WEB
void paginaPrincipal() {

  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ESP32 Ecomundo</title>
    <style>
      body {
        text-align: center;
        font-family: Arial;
        background: #f2f2f2;
        padding: 30px;
      }
      button {
        width: 180px;
        padding: 18px;
        margin: 10px;
        font-size: 20px;
        color: white;
        border: none;
        border-radius: 10px;
      }
      .on { background: green; }
      .off { background: red; }
    </style>
  </head>
  <body>
    <h2>Estacion Meteorologica</h2>
    <h3>Ecomundo</h3>
  )rawliteral";

  html += "<h2>LED: ";
  html += estadoLED ? "ENCENDIDO" : "APAGADO";
  html += "</h2>";

  html += R"rawliteral(
    <form action="/on">
      <button class="on">ENCENDER</button>
    </form>

    <form action="/off">
      <button class="off">APAGAR</button>
    </form>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html; charset=utf-8", html);
}

void setup() {

  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // INICIAR WIFI
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  Serial.println("WIFI ACTIVADO");
  Serial.println(WiFi.softAPIP());

  // RUTAS WEB
  server.on("/", HTTP_GET, paginaPrincipal);

  server.on("/on", HTTP_GET, []() {
    controlarLED(true);
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  });

  server.on("/off", HTTP_GET, []() {
    controlarLED(false);
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  });

  server.begin();

  // INICIAR BLUETOOTH BLE
  BLEDevice::init("ESP32_Ecomundo_BLE");

  BLEServer* servidorBLE = BLEDevice::createServer();
  servidorBLE->setCallbacks(new ConexionBLE());

  BLEService* servicio =
    servidorBLE->createService(SERVICE_UUID);

  caracteristicaBLE =
    servicio->createCharacteristic(
      CHAR_UUID,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_WRITE
    );

  caracteristicaBLE->setValue("OFF");

  servicio->start();

  BLEAdvertising* publicidad =
    BLEDevice::getAdvertising();

  publicidad->addServiceUUID(SERVICE_UUID);
  BLEDevice::startAdvertising();

  Serial.println("BLUETOOTH ACTIVADO");
  Serial.println("Sistema listo");
}

void loop() {

  server.handleClient();

  // REINICIAR PUBLICIDAD BLUETOOTH
  if (reiniciarBLE) {
    reiniciarBLE = false;
    BLEDevice::startAdvertising();
  }

  // LEER ORDENES BLUETOOTH
  if (caracteristicaBLE != nullptr) {

    String comando =
      String(caracteristicaBLE->getValue().c_str());

    comando.trim();
    comando.toUpperCase();

    if ((comando == "ON" || comando == "1") && !estadoLED) {
      controlarLED(true);
    }

    if ((comando == "OFF" || comando == "0") && estadoLED) {
      controlarLED(false);
    }
  }

  delay(20);
}
