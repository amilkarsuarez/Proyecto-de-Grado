#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RtcDS1302.h>

// =====================================
// ESTACION METEOROLOGICA ECOMUNDO
// SENSORES, WIFI Y RTC AUTOMATICO
// =====================================

// PINES
#define LED_PIN 2
#define DHT_PIN 27
#define SUELO_PIN 34
#define LDR_PIN 35

#define SDA_OLED 21
#define SCL_OLED 22

#define RTC_CLK 18
#define RTC_DAT 19
#define RTC_RST 33

// WIFI: MISMA CONFIGURACION FUNCIONAL
const char* ssid = "ESP32_Ecomundo";
const char* password = "12345678";

WebServer server(80);
bool wifiOK = false;

// DHT11
DHT dht(DHT_PIN, DHT11);

// OLED SSD1306
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

bool oledDisponible = false;
bool oledResponde = false;
uint8_t direccionOLED = 0x3C;

// RTC DS1302
ThreeWire rtcWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> rtc(rtcWire);

bool rtcOK = false;

// DATOS
float temperatura = NAN;
float humedadAire = NAN;

int sueloADC = 0;
int luzADC = 0;

char fechaHora[24] = "RTC SIN HORA";

bool sistemaOK = false;

// LECTURA CADA 3 SEGUNDOS
const unsigned long INTERVALO = 3000;
unsigned long ultimaLectura = 0;

// =====================================
// CLASIFICACIONES DE LOS SENSORES
// =====================================

// Escala operativa de humedad del suelo. Los limites no
// se corresponden aun con porcentajes de humedad reales:
// 0-700    Demasiado Humeda
// 701-900  Humeda
// 901-1300 Neutro
// 1301-2500 Seca
// 2501-4095 Demasiado Seca
const char* clasificarSuelo(int adc) {
  if (adc <= 700) return "Demasiado Húmeda";
  if (adc <= 900) return "Húmeda";
  if (adc <= 1300) return "Neutro";
  if (adc <= 2500) return "Seca";
  return "Demasiado Seca";
}

const char* clasificarLuz(int adc) {

  if (adc >= 3000) {
    return "Demasiado oscuro";
  }
  else if (adc >= 1900) {
    return "Oscuro";
  }
  else if (adc >= 600) {
    return "Óptimo";
  }
  else if (adc >= 250) {
    return "Iluminado";
  }
  else {
    return "Demasiado iluminado";
  }
}

// =====================================
// PAGINA WEB
// =====================================

const char paginaHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport"
content="width=device-width,initial-scale=1">

<title>Estacion Ecomundo</title>

<style>
body {
  font-family: Arial;
  background: #eef4f0;
  text-align: center;
  padding: 12px;
}

h2 { color: #176b46; }

.tarjeta {
  background: white;
  padding: 16px;
  margin: 12px auto;
  max-width: 380px;
  border-radius: 12px;
  box-shadow: 0 2px 8px #0002;
}

.valor {
  font-size: 25px;
  font-weight: bold;
  color: #176b46;
}

#estadoSincronizacion {
  display: block;
  margin-top: 10px;
  font-size: 13px;
  color: #536575;
}
</style>
</head>

<body>

<h2>Estacion Meteorologica</h2>
<h3>Ecomundo</h3>

<div class="tarjeta" id="tarjetaTemp">
  Temperatura ambiental
  <div class="valor" id="temp">--</div>
</div>

<div class="tarjeta" id="tarjetaHumedad">
Humedad ambiental
<div class="valor" id="aire">--</div>
</div>

<div class="tarjeta" id="tarjetaSuelo">
Humedad del suelo
<div class="valor" id="suelo">--</div>
</div>

<div class="tarjeta" id="tarjetaLuz">
  Luminosidad ambiental
  <div class="valor" id="luz">--</div>
</div>

<div class="tarjeta">
Fecha y hora
<div class="valor" id="fecha"
style="font-size:18px">--</div>
<small id="estadoSincronizacion">Sincronizacion automatica pendiente...</small>
</div>

<div class="tarjeta">
Estado del sistema
<div class="valor" id="estado">--</div>
</div>

<script>
// La pagina recibe la fecha/hora LOCAL del telefono.
// No requiere acceso a Internet y no usa servicios externos.
let sincronizacionEnCurso = false;
let enlaceInterrumpido = false;

// Solo sincroniza desde el reloj local del celular.
// Se ejecuta al abrir la pagina y cada 10 minutos mientras este abierta.
async function sincronizarRTC() {
  if (sincronizacionEnCurso) return;

  const estado = document.getElementById("estadoSincronizacion");
  sincronizacionEnCurso = true;
  estado.textContent = "Sincronizando automaticamente con el celular...";
  estado.style.color = "#536575";

  try {
    const ahora = new Date();
    const anio = ahora.getFullYear();

    if (!Number.isFinite(ahora.getTime()) || anio < 2025 || anio > 2099) {
      throw new Error("Comprueba la fecha del celular");
    }

    // Se envia fecha/hora LOCAL; no requiere Internet.
    const datos = new URLSearchParams({
      anio: String(anio),
      mes: String(ahora.getMonth() + 1),
      dia: String(ahora.getDate()),
      hora: String(ahora.getHours()),
      minuto: String(ahora.getMinutes()),
      segundo: String(ahora.getSeconds())
    });

    const respuesta = await fetch("/sincronizar-rtc", {
      method: "POST",
      headers: {"Content-Type": "application/x-www-form-urlencoded"},
      body: datos.toString(),
      cache: "no-store"
    });

    if (!respuesta.ok) {
      throw new Error("El ESP32 rechazo la sincronizacion");
    }

    const resultado = await respuesta.json();
    if (!resultado.ok) {
      throw new Error("No se pudo comprobar la fecha del RTC");
    }

    document.getElementById("fecha").textContent = resultado.fecha_hora;
    estado.textContent = "Hora sincronizada automaticamente con el celular";
    estado.style.color = "#166534";
    await actualizar();

  } catch (error) {
    estado.textContent = "Sincronizacion pendiente: " + error.message;
    estado.style.color = "#B91C1C";

  } finally {
    sincronizacionEnCurso = false;
  }
}

async function actualizar() {
 try {
  const respuesta = await fetch(
    "/datos", {cache:"no-store"}
  );

  const d = await respuesta.json();

  // Si el telefono perdio la conexion con el ESP32 y la recupero,
  // volver a tomar automaticamente la hora local del telefono.
  if (enlaceInterrumpido) {
    enlaceInterrumpido = false;
    sincronizarRTC();
  }

const temperatura = d.temperatura;

const tarjetaTemp =
  document.getElementById("tarjetaTemp");

const textoTemp =
  document.getElementById("temp");

let colorFondo;
let colorTexto;

if (temperatura === null ||
    !Number.isFinite(temperatura)) {

  textoTemp.textContent = "ERROR";

  colorFondo = "#F3F4F6";
  colorTexto = "#6B7280";

} else {

  // El color usa la temperatura original, sin redondearla.
  // Solo la presentacion utiliza una cifra decimal.
  const t = Number(temperatura);

  textoTemp.textContent = t.toFixed(1) + " °C";

  // Clasificacion por temperatura
  if (t < 18) {

    // MUY FRIO
    colorFondo = "#1D4ED8";
    colorTexto = "#FFFFFF";

  } else if (t < 22) {

    // FRESCO
    colorFondo = "#BAE6FD";
    colorTexto = "#075985";

  } else if (t < 26) {

    // TEMPLADO
    colorFondo = "#DCFCE7";
    colorTexto = "#166534";

  } else if (t <= 30) {

    // CALIDO
    colorFondo = "#FFEDD5";
    colorTexto = "#9A3412";

  } else {

    // MUY CALIDO
    colorFondo = "#FEE2E2";
    colorTexto = "#991B1B";

  }
}

// Aplicar colores a la tarjeta
tarjetaTemp.style.backgroundColor = colorFondo;
tarjetaTemp.style.color = colorTexto;

textoTemp.style.color = colorTexto;


  // HUMEDAD AMBIENTAL: conserva el valor en porcentaje.
  // Solo cambia el color de su tarjeta segun el rango indicado.
  const h = d.humedad_aire;
  const tarjetaHumedad = document.getElementById("tarjetaHumedad");
  const textoHumedad = document.getElementById("aire");
  let fondoHumedad;
  let textoColorHumedad;

  if (h === null || !Number.isFinite(h)) {
    textoHumedad.textContent = "ERROR";
    fondoHumedad = "#F3F4F6";
    textoColorHumedad = "#6B7280";
  } else {
    textoHumedad.textContent = Number(h).toFixed(1) + " %";

    if (h > 90) {
      // MUY HUMEDO
      fondoHumedad = "#367ba2";
      textoColorHumedad = "#FFFFFF";
    } else if (h > 60) {
      // HUMEDO (se corrige ##36a278 -> #36a278)
      fondoHumedad = "#36a278";
      textoColorHumedad = "#2e4964";
    } else if (h > 50) {
      // OPTIMO
      fondoHumedad = "#74c664";
      textoColorHumedad = "#3247d1";
    } else if (h > 40) {
      // SECO
      fondoHumedad = "#a29a36";
      textoColorHumedad = "#4d1b10";
    } else {
      // MUY SECO
      fondoHumedad = "#d68448";
      textoColorHumedad = "#000000";
    }
  }

  tarjetaHumedad.style.backgroundColor = fondoHumedad;
  tarjetaHumedad.style.color = textoColorHumedad;
  textoHumedad.style.color = textoColorHumedad;

  // HUMEDAD DEL SUELO: solo mostrar la categoria, nunca el ADC.
  const nivelSuelo = d.nivel_suelo;
  const tarjetaSuelo = document.getElementById("tarjetaSuelo");
  const textoSuelo = document.getElementById("suelo");
  const coloresSuelo = {
    "Demasiado Húmeda": ["#1D4ED8", "#FFFFFF"],
    "Húmeda":          ["#BFDBFE", "#1E3A8A"],
    "Neutro":          ["#DCFCE7", "#166534"],
    "Seca":            ["#FEF3C7", "#92400E"],
    "Demasiado Seca":  ["#FEE2E2", "#991B1B"]
  };

  const estiloSuelo = coloresSuelo[nivelSuelo] || ["#F3F4F6", "#6B7280"];
  textoSuelo.textContent = nivelSuelo || "SIN LECTURA";
  tarjetaSuelo.style.backgroundColor = estiloSuelo[0];
  tarjetaSuelo.style.color = estiloSuelo[1];
  textoSuelo.style.color = estiloSuelo[1];

  
const nivel = d.nivel_luz;

const colores = {
  "Demasiado oscuro": ["#1F2937", "#FFFFFF"],
  "Oscuro": ["#475569", "#FFFFFF"],
  "Óptimo": ["#DCFCE7", "#166534"],
  "Iluminado": ["#FEF3C7", "#92400E"],
  "Demasiado iluminado": ["#FEE2E2", "#991B1B"]
};

const estilo = colores[nivel] || ["#FFFFFF", "#176B46"];

document.getElementById("luz").textContent = nivel;

const tarjetaLuz = document.getElementById("tarjetaLuz");
tarjetaLuz.style.backgroundColor = estilo[0];
// Tambien cambia el color del titulo de la tarjeta
// para que sea legible sobre fondos oscuros.
tarjetaLuz.style.color = estilo[1];

document.getElementById("luz").style.color = estilo[1];

  document.getElementById("fecha").textContent =
    d.fecha_hora;

  document.getElementById("estado").textContent =
    d.sistema_ok ? "OPERATIVO" : "REVISAR";

 } catch(e) {
  enlaceInterrumpido = true;
  document.getElementById("estado").textContent =
    "SIN CONEXION";
 }
}

// Sin botones: sincroniza al abrir y cada 10 min mientras
// el navegador siga conectado a la red WiFi del ESP32.
actualizar();
sincronizarRTC();
setInterval(actualizar, 3000);
setInterval(sincronizarRTC, 10 * 60 * 1000);
</script>

</body>
</html>
)rawliteral";

// Declaraciones para usar estas funciones desde las rutas HTTP.
void leerRTC();
void actualizarEstado();
void mostrarOLED();

// Validacion de fecha recibida por el navegador.
bool fechaRecibidaValida(int anio, int mes, int dia,
                        int hora, int minuto, int segundo) {
  if (anio < 2025 || anio > 2099 ||
      mes < 1 || mes > 12 ||
      hora < 0 || hora > 23 ||
      minuto < 0 || minuto > 59 ||
      segundo < 0 || segundo > 59) {
    return false;
  }

  const int diasMes[12] = {31, 28, 31, 30, 31, 30,
                           31, 31, 30, 31, 30, 31};
  int maxDia = diasMes[mes - 1];
  bool bisiesto = ((anio % 4 == 0 && anio % 100 != 0) ||
                    (anio % 400 == 0));
  if (mes == 2 && bisiesto) maxDia = 29;
  return dia >= 1 && dia <= maxDia;
}

// =====================================
// INICIAR WIFI
// BASADO EN TU CODIGO FUNCIONAL
// =====================================

void iniciarWiFi() {

  WiFi.mode(WIFI_AP);

  wifiOK = WiFi.softAP(ssid, password);

  if (wifiOK) {
    Serial.println("WIFI ACTIVADO");
    Serial.println(WiFi.softAPIP());
  }
  else {
    Serial.println("ERROR WIFI");
  }

  server.on("/", HTTP_GET, []() {
    // Evitar que el celular conserve una pagina antigua en cache.
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send_P(
      200,
      "text/html; charset=utf-8",
      paginaHTML
    );
  });

  server.on("/datos", HTTP_GET, []() {

    String json = "{";

    json += "\"temperatura\":";
    // Enviar la lectura con dos decimales para conservar
    // posibles valores cercanos a los limites de color.
    json += isnan(temperatura)
      ? "null" : String(temperatura, 2);

    json += ",\"humedad_aire\":";
    json += isnan(humedadAire)
      ? "null" : String(humedadAire, 1);

    // El servidor entrega categorias, sin valores ADC.
    json += ",\"nivel_suelo\":\"";
    json += clasificarSuelo(sueloADC);
    json += "\"";

    json += ",\"nivel_luz\":\"";
    json += clasificarLuz(luzADC);
    json += "\"";


    json += ",\"fecha_hora\":\"";
    json += fechaHora;
    json += "\"";

    json += ",\"sistema_ok\":";
    json += sistemaOK ? "true" : "false";

    json += "}";

    server.sendHeader(
      "Cache-Control", "no-store"
    );

    server.send(
      200,
      "application/json; charset=utf-8",
      json
    );
  });

  // Recibe la fecha y hora local del celular al abrir la pagina.
  // Solo funciona desde un dispositivo conectado a la red del ESP32.
  server.on("/sincronizar-rtc", HTTP_POST, []() {
    const char* campos[] = {
      "anio", "mes", "dia", "hora", "minuto", "segundo"
    };

    for (const char* campo : campos) {
      if (!server.hasArg(campo)) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"Faltan datos\"}");
        return;
      }
    }

    int anio = server.arg("anio").toInt();
    int mes = server.arg("mes").toInt();
    int dia = server.arg("dia").toInt();
    int hora = server.arg("hora").toInt();
    int minuto = server.arg("minuto").toInt();
    int segundo = server.arg("segundo").toInt();

    if (!fechaRecibidaValida(anio, mes, dia, hora, minuto, segundo)) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Fecha invalida\"}");
      return;
    }

    // Actualiza el reloj real DS1302, NO solo el texto de la web.
    RtcDateTime fechaTelefono(anio, mes, dia, hora, minuto, segundo);
    rtc.SetDateTime(fechaTelefono);
    if (!rtc.GetIsRunning()) {
      rtc.SetIsRunning(true);
    }

    // Refrescar inmediatamente pantalla y estado del sistema.
    leerRTC();
    actualizarEstado();
    mostrarOLED();

    if (!rtcOK) {
      server.send(500, "application/json",
                  "{\"ok\":false,\"error\":\"RTC no responde\"}");
      Serial.println("ERROR: no se pudo verificar la hora del RTC");
      return;
    }

    Serial.print("RTC sincronizado desde el celular: ");
    Serial.println(fechaHora);

    String respuesta = "{\"ok\":true,\"fecha_hora\":\"";
    respuesta += fechaHora;
    respuesta += "\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", respuesta);
  });

  server.begin();
}

// =====================================
// COMPROBAR DISPOSITIVO I2C
// =====================================

bool comprobarI2C(uint8_t direccion) {

  Wire.beginTransmission(direccion);

  return Wire.endTransmission() == 0;
}

// =====================================
// INICIAR OLED
// =====================================

void iniciarOLED() {

  Wire.begin(SDA_OLED, SCL_OLED);
  Wire.setClock(100000);

  if (comprobarI2C(0x3C)) {
    direccionOLED = 0x3C;
  }
  else if (comprobarI2C(0x3D)) {
    direccionOLED = 0x3D;
  }
  else {
    Serial.println("OLED NO DETECTADA");
    return;
  }

  oledDisponible = oled.begin(
    SSD1306_SWITCHCAPVCC,
    direccionOLED
  );

  oledResponde = oledDisponible;

  if (oledDisponible) {

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println("Ecomundo");
    oled.println("Iniciando...");

    oled.display();

    Serial.println("OLED INICIADA");
  }
}

// =====================================
// INICIAR RTC DS1302
// =====================================

void iniciarRTC() {

  rtc.Begin();

  if (rtc.GetIsWriteProtected()) {
    rtc.SetIsWriteProtected(false);
  }

  // CR2032 no recargable: mantener deshabilitada la carga.
  rtc.SetTrickleChargeSettings(DS1302Tcr_Disabled);

  RtcDateTime ahora = rtc.GetDateTime();

  // IMPORTANTE: no corregir el reloj por ser "mas antiguo que
  // la fecha de compilacion". Esa comparacion puede sobrescribir
  // la hora conservada tras desconectar el ESP32.
  bool fechaValida = ahora.IsValid() &&
                     ahora.Year() >= 2025 &&
                     ahora.Year() <= 2050;

  if (!fechaValida) {
    // SOLO recuperacion inicial: NO es la hora actual de Internet.
    RtcDateTime fechaCompilacion(__DATE__, __TIME__);
    rtc.SetDateTime(fechaCompilacion);
    Serial.println("RTC: fecha invalida; ajuste provisional de compilacion.");
    Serial.println("RTC: revise pila CR2032 si vuelve a ocurrir.");
  } else {
    Serial.println("RTC: hora respaldada conservada sin cambios.");
  }

  if (!rtc.GetIsRunning()) {
    rtc.SetIsRunning(true);
    Serial.println("RTC: oscilador activado.");
  }

  RtcDateTime lectura = rtc.GetDateTime();
  Serial.printf("RTC al arrancar: %02u/%02u/%04u %02u:%02u:%02u\n",
                (unsigned)lectura.Day(), (unsigned)lectura.Month(),
                (unsigned)lectura.Year(), (unsigned)lectura.Hour(),
                (unsigned)lectura.Minute(), (unsigned)lectura.Second());

  Serial.println("RTC DS1302 INICIADO");
}

// =====================================
// LECTURA ANALOGICA PROMEDIADA
// =====================================

int leerADC(int pin) {

  long suma = 0;

  for (int i = 0; i < 8; i++) {
    suma += analogRead(pin);
    delayMicroseconds(150);
  }

  return suma / 8;
}

// =====================================
// FECHA Y HORA
// =====================================

void leerRTC() {

  RtcDateTime ahora = rtc.GetDateTime();

  rtcOK = ahora.IsValid() &&
          ahora.Year() >= 2025 &&
          ahora.Year() <= 2099 &&
          rtc.GetIsRunning();

  if (!rtcOK) {
    strcpy(fechaHora, "RTC SIN HORA");
    return;
  }

  snprintf(
    fechaHora,
    sizeof(fechaHora),
    "%02u/%02u/%04u %02u:%02u:%02u",
    (unsigned)ahora.Day(),
    (unsigned)ahora.Month(),
    (unsigned)ahora.Year(),
    (unsigned)ahora.Hour(),
    (unsigned)ahora.Minute(),
    (unsigned)ahora.Second()
  );
}

// =====================================
// MOSTRAR DATOS EN PANTALLA
// =====================================

void mostrarOLED() {

  if (!oledResponde) return;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.println(
    sistemaOK ? "Ecomundo - OK" : "Ecomundo - REVISAR"
);

  oled.setCursor(0, 8);
  oled.print("T:");
  if (isnan(temperatura)) oled.print("ERROR");
  else oled.print(temperatura, 1);
  oled.print("C  HR:");
  if (isnan(humedadAire)) oled.print("ERROR");
  else oled.print(humedadAire, 1);
  oled.print("%");

  oled.setCursor(0, 16);
  oled.print("Suelo:");

  oled.setCursor(0, 24);
  String estadoSuelo = clasificarSuelo(sueloADC);
  // Fuente OLED basica: quitar caracteres acentuados.
  estadoSuelo.replace("ú", "u");
  estadoSuelo.replace("Ú", "U");
  oled.print(estadoSuelo);

  oled.setCursor(0, 32);
  oled.print("Luminosidad:");

  oled.setCursor(0, 40);
  String estadoLuz = clasificarLuz(luzADC);
  estadoLuz.replace("Ó", "O");
  oled.print(estadoLuz);

  oled.setCursor(0, 48);

  if (rtcOK) {
    oled.print(String(fechaHora).substring(11));
  }
  else {
    oled.print("RTC SIN HORA");
  }

  oled.setCursor(0, 56);
  if (rtcOK) {
    oled.print(String(fechaHora).substring(0, 10));
  }

  oled.display();
}

// =====================================
// COMPROBAR ESTADO Y CONTROLAR LED
// =====================================

void actualizarEstado() {

  // Comprobar que OLED sigue respondiendo
  oledResponde = oledDisponible &&
                comprobarI2C(direccionOLED);

  bool dhtOK =
    !isnan(temperatura) &&
    !isnan(humedadAire);

  sistemaOK =
    wifiOK &&
    oledResponde &&
    rtcOK &&
    dhtOK;

  // LED AUTOMATICO
  digitalWrite(
    LED_PIN,
    sistemaOK ? HIGH : LOW
  );
}

// =====================================
// LEER TODOS LOS SENSORES
// =====================================

void leerSensores() {

  temperatura = dht.readTemperature();
  humedadAire = dht.readHumidity();

  sueloADC = leerADC(SUELO_PIN);
  luzADC = leerADC(LDR_PIN);

  leerRTC();
  actualizarEstado();

  mostrarOLED();

  Serial.println("----------------------");

  Serial.print("Temperatura: ");
  Serial.println(temperatura);

  Serial.print("Humedad aire: ");
  Serial.println(humedadAire);

  // El ADC se conserva unicamente en el Monitor serie
  // para calibracion. No se envia a la pagina web.
  Serial.print("Suelo ADC (calibracion): ");
  Serial.println(sueloADC);
  Serial.print("Estado del suelo: ");
  Serial.println(clasificarSuelo(sueloADC));

  Serial.print("Luminosidad: ");
  Serial.println(clasificarLuz(luzADC));

  Serial.print("Luz ADC (calibracion): ");
  Serial.println(luzADC);

  Serial.print("RTC: ");
  Serial.println(fechaHora);

  Serial.print("ESTADO: ");
  Serial.println(
    sistemaOK ? "OPERATIVO" : "REVISAR"
  );
}

// =====================================
// SETUP
// =====================================

void setup() {

  Serial.begin(115200);
  delay(300);

  // LED APAGADO AL INICIAR
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("ESTACION ECOMUNDO");
  Serial.println("Iniciando sistema");

  // CONEXION WIFI BASADA EN EL CODIGO YA PROBADO
  iniciarWiFi();

  // LUEGO LOS COMPONENTES
  pinMode(SUELO_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);

  analogReadResolution(12);

  dht.begin();

  iniciarOLED();
  iniciarRTC();

  // Esperar estabilizacion del DHT11
  delay(2500);

  leerSensores();
  ultimaLectura = millis();

  Serial.println("SISTEMA LISTO");
}

// =====================================
// LOOP
// =====================================

void loop() {

  server.handleClient();

  if (millis() - ultimaLectura >= INTERVALO) {

    ultimaLectura = millis();
    leerSensores();
  }

  delay(5);
}
