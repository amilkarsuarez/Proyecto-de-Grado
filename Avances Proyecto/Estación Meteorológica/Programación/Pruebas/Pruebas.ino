

#define LED_PIN 2
#define LEDa 25
#define LEDb 26
#define LEDc 15
#define Buz 32

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
    pinMode(LEDa, OUTPUT);
  digitalWrite(LEDa, LOW);
    pinMode(LEDb, OUTPUT);
  digitalWrite(LEDb, LOW);
    pinMode(LEDc, OUTPUT);
  digitalWrite(LEDc, LOW);
      pinMode(Buz, OUTPUT);
  digitalWrite(Buz, LOW);
  

  Serial.println("PRUEBA DE LED ESP32");
  Serial.println("Iniciando parpadeo...");
}

void loop() {

  // ENCENDER LED
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(LEDa, LOW);
  digitalWrite(LEDb, HIGH);
  digitalWrite(LEDc, LOW);
   digitalWrite(Buz, HIGH);
  Serial.println("LED ENCENDIDO");
  delay(1000);

  // APAGAR LED
  digitalWrite(LED_PIN, LOW);
    digitalWrite(LEDa, HIGH);
  digitalWrite(LEDb, LOW);
  digitalWrite(LEDc, HIGH);
   digitalWrite(Buz, LOW);
  Serial.println("LED APAGADO");
  delay(1000);

}
