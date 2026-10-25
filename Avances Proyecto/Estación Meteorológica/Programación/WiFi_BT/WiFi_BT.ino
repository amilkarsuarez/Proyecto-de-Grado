
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("PRUEBA DE WIFI ESP32");

  Serial.print("Memoria disponible: ");
  Serial.println(ESP.getFreeHeap());

  Serial.println("Iniciando WiFi STA...");
  Serial.flush();

  WiFi.mode(WIFI_STA);

  Serial.println("WIFI STA INICIADO CORRECTAMENTE");
  Serial.flush();

  Serial.print("Direccion MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.println("PRUEBA FINALIZADA");
}

void loop() {
  delay(1000);
}
