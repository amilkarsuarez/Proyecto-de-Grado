/*
  ESTACION METEOROLOGICA ECOMUNDO - RIEGO + INTERNET + DEEP SLEEP
  Version inicial de integracion - 2026-11

  HARDWARE:
  ESP32 Dev Module; DHT11 GPIO27; suelo AO GPIO34; LDR AO GPIO35;
  OLED SSD1306 I2C SDA21 SCL22; DS1302 DAT19 CLK18 RST33;
  L298N ENA GPIO32, IN1 GPIO25, IN2 GPIO26; valvula 12V DC NC.

  PRECAUCIONES:
  - Antes de validar la valvula, mantener ACTIVAR_SALIDA_VALVULA=false.
  - El ADC del suelo NO esta calibrado definitivamente. Desconexion o
    saturacion del sensor puede aparentar sequedad: se rechazan extremos.
  - La valvula NC convencional se energiza continuamente durante el riego.
  - El L298N produce caida de tension. Verificar tension REAL en la bobina.
  - La salida 12V no debe tocar el ESP32. GND comun por cable de potencia.
  - La proteccion electrica y la desconexion por bateria baja son externas.
  - El servidor LAN NO es accesible desde Internet sin servicio externo.
  - Adafruit IO requiere cuenta y clave; es un panel remoto distinto del local.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RtcDS1302.h>
#include <Preferences.h>
#include <AdafruitIO_WiFi.h>
#include <time.h>
#include <esp_sleep.h>

// ---------- CONFIGURACION USUARIO --------------------------------------
const char* WIFI_SSID = "FSUAMOR_INTERNAT";
const char* WIFI_CLAVE = "11121FSM";

// Adafruit IO: crear cuenta en https://io.adafruit.com
// Sustituir estos dos valores para habilitar el panel remoto seguro TLS.
#define IO_USERNAME "AmilkarSM"
#define IO_KEY      "Okm0Ijn9Uhb8"

// Para las pruebas, la electroválvula NO se activa físicamente.
// Cambiar a true SOLO tras comprobar cableado, 12 V bajo carga,
// NC, presion de agua, sentido del flujo y ADC del suelo.
const bool ACTIVAR_SALIDA_VALVULA = false;
const bool RIEGO_AUTOMATICO = true;
const bool REPOSO_NOCTURNO = true;

// Umbrales PROVISIONALES del sensor resistivo, no porcentajes reales.
const int ADC_INICIO_SECO = 2501;
const int ADC_FIN_HUMEDO = 1300;
const int ADC_MIN_VALIDO = 15;
const int ADC_MAX_VALIDO = 4050;

// Limites de seguridad de los primeros ensayos.
const uint32_t RIEGO_MAX_MS = 30000UL;     // 30 segundos como maximo
const uint32_t DESCANSO_RIEGO_S = 6UL * 3600UL;
const uint8_t LECTURAS_SECAS_CONSECUTIVAS = 5;
const uint32_t INTERVALO_SENSORES_MS = 3000UL;
const uint32_t INTERVALO_NUBE_MS = 120000UL; // 7 feeds / 2 min
const uint32_t REINTENTO_NUBE_MS = 180000UL;

// Reposo: 00:00 - 05:30, hora local de Ecuador (UTC-5).
const uint32_t FIN_REPOSO_SEG_DIA = 5UL * 3600UL + 30UL * 60UL;

// ---------- PINES -------------------------------------------------------
constexpr uint8_t PIN_DHT = 27;
constexpr uint8_t PIN_SUELO = 34;
constexpr uint8_t PIN_LDR = 35;
constexpr uint8_t PIN_LED = 2;
constexpr uint8_t PIN_ENA = 32;
constexpr uint8_t PIN_IN1 = 25;
constexpr uint8_t PIN_IN2 = 26;
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;
constexpr uint8_t PIN_RTC_CLK = 18;
constexpr uint8_t PIN_RTC_DAT = 19;
constexpr uint8_t PIN_RTC_RST = 33;

DHT dht(PIN_DHT, DHT11);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
ThreeWire rtcWire(PIN_RTC_DAT, PIN_RTC_CLK, PIN_RTC_RST);
RtcDS1302<ThreeWire> rtc(rtcWire);
WebServer server(80);
Preferences ajustes;

AdafruitIO_WiFi io(IO_USERNAME, IO_KEY, WIFI_SSID, WIFI_CLAVE);
AdafruitIO_Feed* feedTemp  = io.feed("temperatura");
AdafruitIO_Feed* feedHumed = io.feed("humedad");
AdafruitIO_Feed* feedSuelo = io.feed("suelo");
AdafruitIO_Feed* feedLuz   = io.feed("luminosidad");
AdafruitIO_Feed* feedRiego = io.feed("riego");
AdafruitIO_Feed* feedEstado= io.feed("estado");
AdafruitIO_Feed* feedHora  = io.feed("fecha-hora");

float temperatura = NAN;
float humedadAire = NAN;
int sueloADC = 4095;
int luzADC = 0;
bool oledOK = false;
bool rtcOK = false;
bool relojConfiable = false;
bool ntpOK = false;
bool sistemaOK = false;
bool riegoActivo = false;
bool estadoHumedadSueloOK = false;
bool nubeHabilitada = false;
uint8_t contadorSeco = 0;
uint32_t comienzoRiegoMs = 0;
uint32_t ultimoRiegoRtc = 0;
uint32_t ultimaLecturaMs = 0;
uint32_t ultimaNubeMs = 0;
uint32_t ultimoReintentoNubeMs = 0;
uint32_t ultimoNtpMs = 0;
uint32_t ultimoIntentoNtpMs = 0;
char fechaHora[24] = "RTC SIN HORA";

// ---------- FUNCIONES AUXILIARES ---------------------------------------
const char* categoriaSuelo(int adc) {
  if (!estadoHumedadSueloOK) return "Sensor sin verificar";
  if (adc <= 700) return "Demasiado Humeda";
  if (adc <= 900) return "Humeda";
  if (adc <= 1300) return "Neutro";
  if (adc <= 2500) return "Seca";
  return "Demasiado Seca";
}

const char* categoriaLuz(int adc) {
  if (adc >= 3000) return "Demasiado oscuro";
  if (adc >= 1900) return "Oscuro";
  if (adc >= 600) return "Optimo";
  if (adc >= 250) return "Iluminado";
  return "Demasiado iluminado";
}

bool rtcValido(const RtcDateTime &t) {
  return t.IsValid() && t.Year() >= 2025 && t.Year() <= 2050;
}

uint32_t segundosRtc() {
  if (!rtcOK) return 0;
  return rtc.GetDateTime().TotalSeconds();
}

void refrescarReloj() {
  RtcDateTime ahora = rtc.GetDateTime();
  rtcOK = rtcValido(ahora) && rtc.GetIsRunning();
  if (!rtcOK) {
    strncpy(fechaHora, "RTC SIN HORA", sizeof(fechaHora));
    fechaHora[sizeof(fechaHora)-1] = 0;
    return;
  }
  snprintf(fechaHora, sizeof(fechaHora), "%02u/%02u/%04u %02u:%02u:%02u",
           (unsigned)ahora.Day(), (unsigned)ahora.Month(), (unsigned)ahora.Year(),
           (unsigned)ahora.Hour(), (unsigned)ahora.Minute(), (unsigned)ahora.Second());
}

void prepararActuadorSeguro() {
  // ENA desactivado antes de configurar los otros pines.
  digitalWrite(PIN_ENA, LOW);
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  pinMode(PIN_ENA, OUTPUT);
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  digitalWrite(PIN_ENA, LOW);
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
}

void valvulaCerrar() {
  // En una valvula NC convencional, desenergizar = cerrar.
  digitalWrite(PIN_ENA, LOW);
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  riegoActivo = false;
}

void publicarEventoRiego(bool activacion) {
  // Publicar el inicio ANTES de energizar la bobina y el cierre DESPUES.
  // En ninguna circunstancia se espera por Internet con la bobina activa.
  if (!nubeHabilitada || WiFi.status()!=WL_CONNECTED) return;
  if (io.status() < AIO_CONNECTED) return;
  feedRiego->save(activacion ? 1 : 0);
}

void valvulaAbrir() {
  if (!ACTIVAR_SALIDA_VALVULA) return;
  publicarEventoRiego(true);
  // L298N canal A: corriente constante, NO inversion de pulsos.
  digitalWrite(PIN_ENA, LOW);
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_ENA, HIGH);
  riegoActivo = true;
  comienzoRiegoMs = millis();
  Serial.println("RIEGO: valvula energizada");
}

void terminarRiego(const char* motivo) {
  if (!riegoActivo) return;
  valvulaCerrar();
  publicarEventoRiego(false);
  if (relojConfiable && rtcOK) {
    ultimoRiegoRtc = segundosRtc();
    ajustes.putUInt("ultRiego", ultimoRiegoRtc);
  }
  Serial.print("RIEGO FINALIZADO: ");
  Serial.println(motivo);
}

bool pasoTiempoDescanso() {
  if (!rtcOK || !relojConfiable) return false;
  uint32_t ahora = segundosRtc();
  if (ultimoRiegoRtc == 0) return true;
  if (ahora <= ultimoRiegoRtc) return false;
  return (ahora - ultimoRiegoRtc >= DESCANSO_RIEGO_S);
}

bool enHorarioReposo() {
  if (!REPOSO_NOCTURNO || !rtcOK || !relojConfiable) return false;
  RtcDateTime ahora = rtc.GetDateTime();
  uint32_t s = (uint32_t)ahora.Hour()*3600UL +
               (uint32_t)ahora.Minute()*60UL + ahora.Second();
  return (s < FIN_REPOSO_SEG_DIA);
}

int promedioADC(uint8_t pin) {
  long s=0;
  for (int i=0;i<10;++i) {
    s += analogRead(pin);
    delayMicroseconds(150);
  }
  return (int)(s/10);
}

bool comprobarOLED(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission()==0;
}

void inicializarOLED() {
  Wire.begin(PIN_SDA,PIN_SCL);
  Wire.setClock(100000);
  if (comprobarOLED(0x3C)) oledOK = oled.begin(SSD1306_SWITCHCAPVCC,0x3C);
  else if (comprobarOLED(0x3D)) oledOK = oled.begin(SSD1306_SWITCHCAPVCC,0x3D);
  if (!oledOK) Serial.println("OLED: no detectada");
}

void iniciarRTC() {
  rtc.Begin();
  if (rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(false);
  rtc.SetTrickleChargeSettings(DS1302Tcr_Disabled); // CR2032 no recargable
  RtcDateTime t = rtc.GetDateTime();
  // No inventar fecha si la pila fallo. Solo NTP podra validar el horario.
  relojConfiable = rtcValido(t) && rtc.GetIsRunning();
  if (!rtc.GetIsRunning()) rtc.SetIsRunning(true);
  refrescarReloj();
}

void actualizarDesdeNTP() {
  if (WiFi.status()!=WL_CONNECTED) return;
  // Ecuador continental: UTC-5 sin horario de verano.
  ultimoIntentoNtpMs = millis();
  struct tm tmActual;
  if (!getLocalTime(&tmActual, 1500)) return;
  int anio = tmActual.tm_year + 1900;
  if (anio < 2025 || anio > 2050) return;
  rtc.SetDateTime(RtcDateTime(anio, tmActual.tm_mon+1, tmActual.tm_mday,
                              tmActual.tm_hour, tmActual.tm_min, tmActual.tm_sec));
  if (!rtc.GetIsRunning()) rtc.SetIsRunning(true);
  ntpOK = true;
  relojConfiable = true;
  ultimoNtpMs = millis();
  refrescarReloj();
  Serial.print("RTC actualizado via Internet: ");
  Serial.println(fechaHora);
}

// ---------- OLED --------------------------------------------------------
void mostrarOLED() {
  if (!oledOK) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0,0);
  oled.println(sistemaOK ? "Ecomundo - OK" : "Ecomundo - REVISAR");
  oled.setCursor(0,8);
  oled.print("T:");
  if (isnan(temperatura)) oled.print("ERR"); else oled.print(temperatura,1);
  oled.print("C HR:");
  if (isnan(humedadAire)) oled.print("ERR"); else oled.print(humedadAire,0);
  oled.print("%");
  oled.setCursor(0,16); oled.print("H-Suelo:");
  oled.setCursor(0,24); oled.print(categoriaSuelo(sueloADC));
  oled.setCursor(0,32); oled.print("Luminosidad:");
  oled.setCursor(0,40); oled.print(categoriaLuz(luzADC));
  oled.setCursor(0,48);
  oled.print(riegoActivo ? "RIEGO: ENCENDIDO" : "RIEGO: APAGADO");
  oled.setCursor(0,56);
  if (rtcOK) oled.print(String(fechaHora).substring(11));
  else oled.print("RTC SIN HORA");
  oled.display();
}

// ---------- LECTURA Y AUTOMATIZACION -----------------------------------
void leerSensores() {
  temperatura = dht.readTemperature();
  humedadAire = dht.readHumidity();
  sueloADC = promedioADC(PIN_SUELO);
  luzADC = promedioADC(PIN_LDR);
  estadoHumedadSueloOK = sueloADC >= ADC_MIN_VALIDO && sueloADC <= ADC_MAX_VALIDO;
  refrescarReloj();
  sistemaOK = WiFi.status()==WL_CONNECTED && rtcOK && oledOK &&
              !isnan(temperatura) && !isnan(humedadAire) && estadoHumedadSueloOK;
  digitalWrite(PIN_LED, sistemaOK ? HIGH : LOW);
  mostrarOLED();
  Serial.printf("T=%.1f C | HR=%.1f%% | Suelo ADC=%d (%s) | Luz ADC=%d (%s) | RTC=%s | Riego=%s\n",
                temperatura, humedadAire, sueloADC, categoriaSuelo(sueloADC),
                luzADC, categoriaLuz(luzADC), fechaHora,
                riegoActivo?"ABIERTO":"CERRADO");
}

void revisarRiego() {
  // Apagar tiene prioridad absoluta sobre cualquier apertura.
  if (riegoActivo) {
    if (millis()-comienzoRiegoMs >= RIEGO_MAX_MS) {
      terminarRiego("tiempo maximo");
    } else if (!estadoHumedadSueloOK || !rtcOK || !relojConfiable || enHorarioReposo()) {
      terminarRiego("sensor/reloj/reposo");
    } else if (sueloADC <= ADC_FIN_HUMEDO) {
      terminarRiego("suelo humedo");
    }
    return;
  }
  if (!ACTIVAR_SALIDA_VALVULA || !RIEGO_AUTOMATICO ||
      !estadoHumedadSueloOK || !rtcOK || !relojConfiable || enHorarioReposo()) {
    contadorSeco=0;
    return;
  }
  if (sueloADC >= ADC_INICIO_SECO) {
    if (contadorSeco < LECTURAS_SECAS_CONSECUTIVAS) ++contadorSeco;
  } else contadorSeco=0;

  if (contadorSeco >= LECTURAS_SECAS_CONSECUTIVAS && pasoTiempoDescanso()) {
    contadorSeco=0;
    valvulaAbrir();
  }
}

// ---------- PAGINA WEB LOCAL -------------------------------------------
const char PAGINA_HTML[] PROGMEM = R"ecoweb(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ecomundo | Estacion Meteorologica</title>
<style>
*{box-sizing:border-box}body{margin:0;padding:22px 12px 34px;background:#eef4f0;color:#24463a;font:15px Arial,sans-serif;text-align:center}
main{max-width:860px;margin:auto}.marca{font-weight:800;color:#16814c;font-size:34px;margin:0}.sub{color:#3c8163;margin:5px 0 22px;font-size:20px}
.cuadricula{display:grid;grid-template-columns:repeat(auto-fit,minmax(235px,1fr));gap:13px}.tarjeta{background:white;border:1px solid #dbe9df;border-radius:14px;padding:22px 12px;box-shadow:0 4px 12px #00000008}
.tarjeta h3{font-size:14px;margin:0 0 10px}.valor{font-size:25px;font-weight:750;overflow-wrap:anywhere}.nota{color:#60766b;font-size:12px;margin-top:7px}.tiempo{margin-top:14px}
</style></head><body><main><h1 class="marca">Ecomundo</h1><h2 class="sub">Estación Meteorológica</h2>
<div class="cuadricula">
<section class="tarjeta" id="ct"><h3>Temperatura ambiental</h3><div class="valor" id="t">--</div></section>
<section class="tarjeta" id="ch"><h3>Humedad ambiental</h3><div class="valor" id="h">--</div></section>
<section class="tarjeta" id="cs"><h3>Humedad del suelo</h3><div class="valor" id="s">--</div></section>
<section class="tarjeta" id="cl"><h3>Luminosidad ambiental</h3><div class="valor" id="l">--</div></section>
<section class="tarjeta" id="cr"><h3>Estado del aspersor</h3><div class="valor" id="r">--</div><div class="nota" id="m">--</div></section>
<section class="tarjeta"><h3>Estado del sistema</h3><div class="valor" id="e">--</div><div class="nota" id="w">--</div></section>
</div>
<section class="tarjeta tiempo"><h3>Fecha y hora (RTC DS1302)</h3><div class="valor" id="f">--</div><div class="nota">Sincronización automática por Internet cuando está disponible</div></section>
<div class="nota">Las categorías de humedad del suelo son provisionales hasta calibrar el sensor.</div>
</main><script>
function colorear(id,fondo,texto){let x=document.getElementById(id);x.style.backgroundColor=fondo;x.style.color=texto}
function nivelSuelo(s){return {'Demasiado Humeda':['#1d4ed8','#fff'],'Humeda':['#bfdbfe','#1e3a8a'],'Neutro':['#dcfce7','#166534'],'Seca':['#fef3c7','#92400e'],'Demasiado Seca':['#fee2e2','#991b1b']}[s]||['#f3f4f6','#6b7280']}
function nivelLuz(l){return {'Demasiado oscuro':['#1f2937','#fff'],'Oscuro':['#475569','#fff'],'Optimo':['#dcfce7','#166534'],'Iluminado':['#fef3c7','#92400e'],'Demasiado iluminado':['#fee2e2','#991b1b']}[l]||['#f3f4f6','#6b7280']}
async function refrescar(){try{const res=await fetch('/datos',{cache:'no-store'});if(!res.ok)throw Error('Sin conexion');let d=await res.json();
let t=d.temperatura;document.getElementById('t').textContent=t===null?'ERROR':Number(t).toFixed(1)+' °C';
let tc=t===null?['#f3f4f6','#6b7280']:t<18?['#1d4ed8','#fff']:t<22?['#bae6fd','#075985']:t<26?['#dcfce7','#166534']:t<=30?['#ffedd5','#9a3412']:['#fee2e2','#991b1b'];colorear('ct',...tc);
let h=d.humedad_aire;document.getElementById('h').textContent=h===null?'ERROR':Number(h).toFixed(1)+' %';let hc=h===null?['#f3f4f6','#6b7280']:h>90?['#367ba2','#fff']:h>60?['#36a278','#2e4964']:h>50?['#74c664','#3247d1']:h>40?['#a29a36','#4d1b10']:['#d68448','#000'];colorear('ch',...hc);
document.getElementById('s').textContent=d.nivel_suelo;colorear('cs',...nivelSuelo(d.nivel_suelo));
document.getElementById('l').textContent=d.nivel_luz;colorear('cl',...nivelLuz(d.nivel_luz));
document.getElementById('r').textContent=d.riego_activo?'REGANDO':'CERRADO';document.getElementById('m').textContent=d.riego_habilitado?'Control automático habilitado':'Actuador deshabilitado para pruebas';colorear('cr',d.riego_activo?'#dbeafe':'#f3f4f6',d.riego_activo?'#1d4ed8':'#334155');
document.getElementById('e').textContent=d.sistema_ok?'OPERATIVO':'REVISAR';document.getElementById('e').style.color=d.sistema_ok?'#15803d':'#b91c1c';
document.getElementById('w').textContent=d.wifi;document.getElementById('f').textContent=d.fecha_hora;
}catch(e){document.getElementById('e').textContent='SIN CONEXIÓN';document.getElementById('e').style.color='#b91c1c'}}
refrescar();setInterval(refrescar,3000);
</script></body></html>
)ecoweb";

void iniciarServidor() {
  server.on("/",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html; charset=utf-8",PAGINA_HTML);
  });
  server.on("/datos",HTTP_GET,[](){
    String j="{";
    j+="\"temperatura\":";
    j+=isnan(temperatura)?"null":String(temperatura,2);
    j+=",\"humedad_aire\":";
    j+=isnan(humedadAire)?"null":String(humedadAire,1);
    j+=",\"nivel_suelo\":\"";j+=categoriaSuelo(sueloADC);j+='"';
    j+=",\"nivel_luz\":\"";j+=categoriaLuz(luzADC);j+='"';
    j+=",\"riego_activo\":";j+=(riegoActivo?"true":"false");
    j+=",\"riego_habilitado\":";j+=(ACTIVAR_SALIDA_VALVULA?"true":"false");
    j+=",\"sistema_ok\":";j+=(sistemaOK?"true":"false");
    j+=",\"fecha_hora\":\"";j+=fechaHora;j+='"';
    j+=",\"wifi\":\"";j+=(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("WiFi sin conectar"));j+='"';
    j+='}';
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8",j);
  });
  server.begin();
}

// ---------- TELEMETRIA REMOTA ADAFRUIT IO -------------------------------
void enviarNube() {
  if (!nubeHabilitada || riegoActivo || WiFi.status()!=WL_CONNECTED) return;
  if (io.status() < AIO_CONNECTED) return;
  if (!isnan(temperatura)) feedTemp->save(temperatura);
  if (!isnan(humedadAire)) feedHumed->save(humedadAire);
  feedSuelo->save(String(categoriaSuelo(sueloADC)));
  feedLuz->save(String(categoriaLuz(luzADC)));
  feedRiego->save(riegoActivo ? 1 : 0);
  feedEstado->save(String(sistemaOK ? "OPERATIVO" : "REVISAR"));
  feedHora->save(String(fechaHora));
  ultimaNubeMs = millis();
}

// ---------- DEEP SLEEP --------------------------------------------------
void entrarEnReposo() {
  if (!enHorarioReposo()) return;
  terminarRiego("inicio de reposo nocturno");
  valvulaCerrar();
  digitalWrite(PIN_LED,LOW);
  if (oledOK) {
    oled.clearDisplay();
    oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
  }
  if (nubeHabilitada && WiFi.status()==WL_CONNECTED && io.status()>=AIO_CONNECTED) {
    feedRiego->save(0);
    feedEstado->save(String("DURMIENDO"));
  }
  RtcDateTime t=rtc.GetDateTime();
  uint32_t actual=(uint32_t)t.Hour()*3600UL+(uint32_t)t.Minute()*60UL+t.Second();
  uint32_t restantes=FIN_REPOSO_SEG_DIA-actual;
  if (restantes<5) restantes=5;
  Serial.printf("DEEP SLEEP: %lu segundos (hasta las 05:30)\n",(unsigned long)restantes);
  Serial.flush();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)restantes*1000000ULL);
  esp_deep_sleep_start();
}

// ---------- SETUP / LOOP ------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(PIN_LED,OUTPUT);
  digitalWrite(PIN_LED,LOW);
  // Importante: antes de arrancar WiFi o leer sensores, cierre seguro.
  prepararActuadorSeguro();
  valvulaCerrar();
  Serial.println("ECOMUNDO - RIEGO / WIFI / REPOSO");

  ajustes.begin("ecomundo",false);
  ultimoRiegoRtc = ajustes.getUInt("ultRiego",0);
  pinMode(PIN_SUELO,INPUT);
  pinMode(PIN_LDR,INPUT);
  analogReadResolution(12);
  dht.begin();
  inicializarOLED();
  iniciarRTC();

  // La red escolar debe ser WiFi 2.4 GHz WPA2/WPA3 personal.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_CLAVE);
  unsigned long inicio=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-inicio<15000UL) delay(250);
  if (WiFi.status()==WL_CONNECTED) {
    Serial.print("WiFi escuela, IP local: ");
    Serial.println(WiFi.localIP());
    configTime(-5*3600,0,"pool.ntp.org","time.google.com");
    actualizarDesdeNTP();
  } else {
    Serial.println("WiFi escolar no conectado: red de mantenimiento local");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP32_Ecomundo", "12345678");
    Serial.print("IP AP: ");Serial.println(WiFi.softAPIP());
  }
  iniciarServidor();

  // Solo se activa la nube cuando el usuario llena las credenciales.
  nubeHabilitada = strcmp(IO_USERNAME,"USUARIO_AQUI")!=0 &&
                   strcmp(IO_KEY,"CLAVE_AIO_AQUI")!=0;
  if (nubeHabilitada && WiFi.status()==WL_CONNECTED) {
    ultimoReintentoNubeMs=millis();
    io.connect();  // Adafruit IO usa MQTT TLS y certificado CA.
  } else if (!nubeHabilitada) {
    Serial.println("Adafruit IO pendiente de configurar");
  }

  delay(2200); // DHT11 requiere al menos 2 s tras alimentarlo.
  leerSensores();
  revisarRiego();
  if (enHorarioReposo()) entrarEnReposo();
  ultimaLecturaMs=millis();
  ultimaNubeMs=millis()-INTERVALO_NUBE_MS; // primer informe al conectar
  Serial.println("SISTEMA INICIADO");
}

void loop() {
  server.handleClient();

  // Seguridad: vigilar tiempo maximo aun entre lecturas de sensores.
  if (riegoActivo && millis()-comienzoRiegoMs>=RIEGO_MAX_MS)
    terminarRiego("limite de seguridad");

  if (millis()-ultimaLecturaMs>=INTERVALO_SENSORES_MS) {
    ultimaLecturaMs=millis();
    leerSensores();
    revisarRiego();
    if (enHorarioReposo()) entrarEnReposo();
  }

  if (WiFi.status()==WL_CONNECTED &&
      (ultimoIntentoNtpMs==0 || millis()-ultimoIntentoNtpMs > 6UL*3600UL*1000UL)) {
    actualizarDesdeNTP();
  }

  // No bloquear el control del actuador con conexiones de Internet.
  if (nubeHabilitada && !riegoActivo && WiFi.status()==WL_CONNECTED) {
    io.run(10,true);
    if (millis()-ultimaNubeMs>=INTERVALO_NUBE_MS) enviarNube();
    if (io.status()<AIO_CONNECTED &&
        millis()-ultimoReintentoNubeMs>=REINTENTO_NUBE_MS) {
      ultimoReintentoNubeMs=millis();
      io.connect();
    }
  }
  delay(5);
}
