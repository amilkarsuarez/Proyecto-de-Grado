#include <WiFi.h>

const char* ssid = "ESP32_Prueba";
const char* password = "12345678";

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("INICIANDO PRUEBA WIFI");

  Serial.print("Memoria libre: ");
  Serial.println(ESP.getFreeHeap());

  Serial.println("Activando punto de acceso...");
  Serial.flush();

  // Inicia directamente el punto de acceso
  bool resultado = WiFi.softAP(ssid, password);

  if (resultado) {
    Serial.println("WIFI FUNCIONANDO");
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR AL INICIAR WIFI");
  }
}

void loop() {
  delay(1000);
}
