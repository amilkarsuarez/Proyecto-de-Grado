

#define LED_PIN 2

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("PRUEBA DE LED ESP32");
  Serial.println("Iniciando parpadeo...");
}

void loop() {

  // ENCENDER LED
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ENCENDIDO");
  delay(1000);

  // APAGAR LED
  digitalWrite(LED_PIN, LOW);
  Serial.println("LED APAGADO");
  delay(1000);

}
