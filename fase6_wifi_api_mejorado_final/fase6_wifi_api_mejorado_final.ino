/*
  FASE 6 - Sensor HC-SR04 + dos motores + modos + WiFi + API REST HTTPS
  Placa: Arduino UNO R4 WiFi
  Librerias: WiFiS3 (ArduinoCore-renesas), ArduinoJson 7.x

  Comandos Serial (115200) y desde API:
    a = automatico; m = manual; d = derecha; i = izquierda; s = detener
    d/i/s solo operan en modo manual.
  Comando adicional por Serial: x = parada de emergencia y modo manual.

  Contrato con el backend (server.js):
    POST https://api-eai.onrender.com/api/device/sync
    Header X-API-Key: <API_KEY>
    JSON enviado:  {"distance": numero>=0, "motor":"left|right|stop"}
    JSON esperado: {"mode":"auto|manual", "command":"left|right|stop"}
    El MODO y el COMANDO los decide la web; el Arduino los obedece.
    "command" es un ESTADO persistente en el servidor (no un evento): solo se
    aplica cuando cambia, y el primer sync tras arrancar nunca mueve el motor.
    El backend rechaza distance=null (400), por eso sin eco se envia 400 cm
    (fuera de rango: en AUTO el motor queda detenido).

  IMPORTANTE: WiFi.begin() y WiFiSSLClient.connect() pueden ser bloqueantes por
  la implementacion de WiFiS3. Por ello se DETIENE FISICAMENTE el motor durante
  esas llamadas, y se reanuda solo despues de actualizar el sensor y las
  condiciones de seguridad. Recibir HTTP ya NO bloquea loop().

  Se deduplican comandos identicos consecutivos; un cambio de modo reinicia
  esa deduplicacion (el backend pone command=stop al cambiar de modo).
*/

#include <WiFiS3.h>
#include <WiFiSSLClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>

// ================ CONFIGURACION ORIGINAL ====================
const char WIFI_SSID[] = "WIFIMAURI";
const char WIFI_PASS[] = "23437648";
// 1 = servidor en tu PC (HTTP, misma red WiFi). 0 = Render (HTTPS).
#define USAR_LOCAL 1
#if USAR_LOCAL
const char API_HOST[] = "10.120.208.200";   // <-- IP de tu PC (ipconfig / ip a)
const uint16_t API_PUERTO = 3000;         // <-- PORT de tu .env
#else
const char API_HOST[] = "api-eai.onrender.com";
const uint16_t API_PUERTO = 443;
#endif
const char API_BASE_RUTA[] = "/api";
const char API_RUTA_ESTADO[] = "/device/sync";
const char API_HEADER_KEY[] = "X-API-Key";
const char API_KEY[] = "kp4Y*W6%2^<8!JN/\\QjaP+22/10/2026";

const uint8_t PIN_TRIG = 9;
const uint8_t PIN_ECHO = 10;
const uint8_t PIN_ENA = 5, PIN_IN1 = 2, PIN_IN2 = 4;
const uint8_t PIN_ENB = 6, PIN_IN3 = 7, PIN_IN4 = 8;
const uint8_t VELOCIDAD = 200;

// ================ TIEMPOS Y LIMITES =========================
const unsigned long INTERVALO_SENSOR_MS = 60UL;
const unsigned long INTERVALO_INFO_MS = 1000UL;
const unsigned long INTERVALO_API_MS = 2000UL;
const unsigned long INTERVALO_WIFI_MS = 10000UL;
const unsigned long TIMEOUT_WIFI_MS = 8000UL;
const unsigned long TIMEOUT_CONEXION_TLS_MS = 8000UL;  // handshake TLS + arranque en frio de Render
const unsigned long TIMEOUT_HTTP_TOTAL_MS = 15000UL;
const unsigned long TIMEOUT_HTTP_INACTIVO_MS = 8000UL;
const unsigned long TIMEOUT_MOV_MANUAL_MS = 15000UL;
const unsigned long MAX_EDAD_SENSOR_MS = 350UL;
const uint16_t MAX_RESPUESTA_BYTES = 512;
const uint16_t MAX_LINEA_HTTP = 160;
const uint8_t MAX_ENCABEZADOS_HTTP = 40;
const uint8_t MAX_TRAILERS_HTTP = 12;
const uint16_t MAX_BYTES_HTTP_POR_LOOP = 192;
const uint8_t N_MUESTRAS = 5;

// Umbrales del enunciado. Histeresis de 1 cm alrededor de 25 cm.
const float DER_MIN = 10.0f;
const float DER_MAX = 25.0f;
const float IZQ_MAX = 40.0f;
const float HISTERESIS_CM = 1.0f;
const float DISTANCIA_SIN_ECO_CM = 400.0f;  // el backend exige distance numerico >= 0

enum EstadoMotor : uint8_t { DETENIDO, DERECHA, IZQUIERDA };
enum Modo : uint8_t { MANUAL, AUTO };

// Prototipos explicitos: evitan que el preprocesador de Arduino los genere
// antes de que conozca los tipos definidos por estos enum.
void aplicarSalidas(EstadoMotor nuevo);
void mover(EstadoMotor nuevo);
EstadoMotor decidirAutomatico(float distancia);
void cambiarModo(Modo nuevo);
const char* nombreMotor(EstadoMotor estado);
bool wifiListo();

Modo modoActual = MANUAL;
EstadoMotor estadoDeseado = DETENIDO;
EstadoMotor estadoFisico = DETENIDO;
bool salidaSuspendida = false;

float muestras[N_MUESTRAS];
uint8_t indiceMuestra = 0;
float distanciaActual = -1.0f;
unsigned long ultimaMedicion = 0;
unsigned long ultimaMedicionValida = 0;
unsigned long ultimaInfo = 0;
unsigned long ultimoComandoMotor = 0;

#if USAR_LOCAL
WiFiClient clienteHttps;
#else
WiFiSSLClient clienteHttps;
#endif
bool wifiConectando = false;
bool wifiConectadoAntes = false;
bool wifiModuloDisponible = true;
unsigned long ultimoIntentoWiFi = 0;
unsigned long inicioConexionWiFi = 0;
unsigned long ultimaRevisionWiFi = 0;
unsigned long ultimaApi = 0;
uint16_t fallosApiSeguidos = 0;

// ================ CONTROL DE MOTORES ========================
void detenerSalidas() {
  analogWrite(PIN_ENA, 0);
  analogWrite(PIN_ENB, 0);
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, LOW);
  estadoFisico = DETENIDO;
}

void aplicarSalidas(EstadoMotor nuevo) {
  if (salidaSuspendida) return;
  if (nuevo == estadoFisico) return;

  // Nunca invertir un motor sin desenergizar antes las salidas PWM.
  if (estadoFisico != DETENIDO) {
    detenerSalidas();
    if (nuevo != DETENIDO) delay(50);
  }
  if (nuevo == DETENIDO) return;

  const bool horario = (nuevo == DERECHA);
  digitalWrite(PIN_IN1, horario ? HIGH : LOW);
  digitalWrite(PIN_IN2, horario ? LOW : HIGH);
  digitalWrite(PIN_IN3, horario ? HIGH : LOW);
  digitalWrite(PIN_IN4, horario ? LOW : HIGH);
  analogWrite(PIN_ENA, VELOCIDAD);
  analogWrite(PIN_ENB, VELOCIDAD);
  estadoFisico = nuevo;
}

void mover(EstadoMotor nuevo) {
  estadoDeseado = nuevo;
  aplicarSalidas(nuevo);
}

// Para inmediatamente antes de llamadas de red potencialmente bloqueantes.
void suspenderMotores() {
  salidaSuspendida = true;
  detenerSalidas();
}

// ================ SENSOR ====================================
float medirDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  const unsigned long duracion = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (duracion == 0) return -1.0f;
  const float d = (float)duracion * 0.0343f / 2.0f;
  if (d < 2.0f || d > 400.0f) return -1.0f;
  return d;
}

float mediana(const float *datos, uint8_t n) {
  float copia[N_MUESTRAS];
  for (uint8_t i = 0; i < n; i++) copia[i] = datos[i];
  for (uint8_t i = 1; i < n; i++) {
    const float clave = copia[i];
    int j = i - 1;
    while (j >= 0 && copia[j] > clave) {
      copia[j + 1] = copia[j];
      --j;
    }
    copia[j + 1] = clave;
  }
  return copia[n / 2];
}

EstadoMotor decidirAutomatico(float d) {
  if (d < DER_MIN || d > IZQ_MAX) return DETENIDO;
  // Evita invertir continuamente si la distancia oscila alrededor de 25 cm.
  if (estadoDeseado == DERECHA && d <= DER_MAX + HISTERESIS_CM) return DERECHA;
  if (estadoDeseado == IZQUIERDA && d > DER_MAX - HISTERESIS_CM) return IZQUIERDA;
  return (d <= DER_MAX) ? DERECHA : IZQUIERDA;
}

void actualizarSensor(bool forzar) {
  const unsigned long ahora = millis();
  if (!forzar && ahora - ultimaMedicion < INTERVALO_SENSOR_MS) return;
  ultimaMedicion = ahora;
  const float lectura = medirDistanciaCm();
  muestras[indiceMuestra] = lectura;
  indiceMuestra = (uint8_t)((indiceMuestra + 1U) % N_MUESTRAS);

  if (lectura < 0) {
    // Un eco ausente debe detener el modo automatico INMEDIATAMENTE.
    distanciaActual = -1.0f;
  } else {
    ultimaMedicionValida = millis();
    distanciaActual = mediana(muestras, N_MUESTRAS);
  }
  if (modoActual == AUTO) mover(decidirAutomatico(distanciaActual));
}

void comprobarSeguridad() {
  const unsigned long ahora = millis();
  if (modoActual == AUTO) {
    if (ahora - ultimaMedicion > MAX_EDAD_SENSOR_MS ||
        ahora - ultimaMedicionValida > MAX_EDAD_SENSOR_MS) {
      mover(DETENIDO);
    }
  } else if (estadoDeseado != DETENIDO &&
             ahora - ultimoComandoMotor >= TIMEOUT_MOV_MANUAL_MS) {
    mover(DETENIDO);
    Serial.println(F(">> Seguridad: motor detenido por tiempo limite manual"));
  }
}

void reanudarMotores() {
  // La conexion puede haber tardado; NO volver a mover automaticamente con
  // una medicion anterior a ella.
  if (modoActual == AUTO) actualizarSensor(true);
  comprobarSeguridad();
  salidaSuspendida = false;
  aplicarSalidas(estadoDeseado);
}

// ================ COMANDOS ================================
void cambiarModo(Modo nuevo) {
  if (nuevo == modoActual) return;
  modoActual = nuevo;
  mover(DETENIDO);
  Serial.print(F(">> Modo: "));
  Serial.println(nuevo == AUTO ? F("AUTOMATICO") : F("MANUAL"));
}

void procesarComando(char c) {
  switch (c) {
    case 'a': cambiarModo(AUTO); break;
    case 'm': cambiarModo(MANUAL); break;
    case 'd':
    case 'i':
    case 's':
      if (modoActual != MANUAL) {
        Serial.println(F(">> Ignorado: comando motor exclusivo del modo manual"));
        break;
      }
      ultimoComandoMotor = millis();
      mover(c == 'd' ? DERECHA : (c == 'i' ? IZQUIERDA : DETENIDO));
      break;
    case 'x':
      modoActual = MANUAL;
      mover(DETENIDO);
      Serial.println(F(">> PARADA DE EMERGENCIA: modo manual, motor detenido"));
      break;
    default: break;
  }
}

void atenderSerie() {
  uint8_t pendientes = 0;
  while (Serial.available() > 0 && pendientes++ < 20) {
    const char c = (char)Serial.read();
    // x solo se acepta localmente. No hay codigo para ejecutarlo desde la API.
    procesarComando(c);
  }
}

const char* textoModo() { return modoActual == AUTO ? "auto" : "manual"; }
const char* nombreMotor(EstadoMotor estado) {
  return estado == DERECHA ? "derecha" :
         estado == IZQUIERDA ? "izquierda" : "detenido";
}
const char* textoMotor() { return nombreMotor(estadoFisico); }
// Nombres que entiende el backend.
const char* nombreMotorApi(EstadoMotor estado) {
  return estado == DERECHA ? "right" :
         estado == IZQUIERDA ? "left" : "stop";
}

void imprimirEstado() {
  Serial.print(F("Dist: "));
  if (distanciaActual < 0) Serial.print(F("SIN ECO"));
  else Serial.print(distanciaActual, 1);
  Serial.print(F(" cm | Modo: "));
  Serial.print(textoModo());
  Serial.print(F(" | Motor: "));
  Serial.println(textoMotor());
}

char comandoALetra(const char* cmd) {
  if (!cmd) return 0;
  if (!strcmp(cmd, "right")) return 'd';
  if (!strcmp(cmd, "left")) return 'i';
  if (!strcmp(cmd, "stop")) return 's';
  return 0;
}

// ================ PROTOCOLO HTTP (SIN LECTURAS BLOQUEANTES) ===
enum FaseHttp : uint8_t {
  HTTP_INACTIVO, HTTP_STATUS, HTTP_CABECERAS,
  HTTP_CUERPO_LONGITUD, HTTP_CUERPO_CIERRE,
  HTTP_CHUNK_TAM, HTTP_CHUNK_DATOS, HTTP_CHUNK_CR, HTTP_CHUNK_LF,
  HTTP_CHUNK_TRAILERS
};

struct EstadoHttp {
  FaseHttp fase;
  unsigned long inicio;
  unsigned long ultimoByte;
  int codigo;
  bool hayLongitud;
  bool chunked;
  bool codificacionNoAdmitida;
  unsigned long longitudEsperada;
  unsigned long restantesChunk;
  unsigned int lineasCabecera;
  unsigned int lineasTrailer;
  uint16_t longitudLinea;
  char linea[MAX_LINEA_HTTP];
  uint16_t longitudCuerpo;
  char cuerpo[MAX_RESPUESTA_BYTES + 1];
};
EstadoHttp http;

char ultimoComandoAPI[24] = "";
bool hayUltimoComandoAPI = false;
bool primerSyncHecho = false;

// Comparacion ASCII de prefijos, ignorando mayusculas/minusculas.
bool iniciaConSinCaso(const char* texto, const char* prefijo) {
  while (*prefijo) {
    if (!*texto || tolower((unsigned char)*texto) != tolower((unsigned char)*prefijo)) return false;
    ++texto;
    ++prefijo;
  }
  return true;
}

bool igualSinCaso(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

const char* omitirEspacios(const char* s) {
  while (*s == ' ' || *s == '\t') ++s;
  return s;
}

// Solo permite "chunked" (sin otras codificaciones encadenadas).
bool encabezadoSoloChunked(const char* valor) {
  valor = omitirEspacios(valor);
  if (!iniciaConSinCaso(valor, "chunked")) return false;
  valor += 7;
  valor = omitirEspacios(valor);
  return *valor == '\0';
}

void falloApi(const char* razon) {
  if (fallosApiSeguidos < 65535) ++fallosApiSeguidos;
  Serial.print(F(">> API: error ("));
  Serial.print(razon);
  Serial.print(F(", HTTP "));
  Serial.print(http.codigo);
  Serial.print(F(", fallos: "));
  Serial.print(fallosApiSeguidos);
  Serial.println(')');
}

void aplicarRespuestaAPI() {
  if (http.codigo < 200 || http.codigo >= 300) {
    if (http.codigo == 401) Serial.println(F(">> API: clave X-API-Key rechazada (401)"));
    falloApi("respuesta no exitosa");
    return;
  }
  if (http.longitudCuerpo == 0) {
    falloApi("respuesta vacia");
    return;
  }

  JsonDocument documento;
  DeserializationError error = deserializeJson(documento, http.cuerpo);
  if (error || !documento.is<JsonObject>()) {
    falloApi("JSON invalido");
    return;
  }
  JsonVariantConst campoModo = documento["mode"];
  JsonVariantConst campoCmd = documento["command"];
  if (!campoModo.is<const char*>() || !campoCmd.is<const char*>()) {
    falloApi("faltan mode/command");
    return;
  }
  const char* modoTxt = campoModo.as<const char*>();
  const char* cmd = campoCmd.as<const char*>();
  Modo modoServidor;
  if (!strcmp(modoTxt, "auto")) modoServidor = AUTO;
  else if (!strcmp(modoTxt, "manual")) modoServidor = MANUAL;
  else { Serial.println(F(">> API: mode no admitido")); return; }
  const char letra = comandoALetra(cmd);
  if (!letra || strlen(cmd) >= sizeof(ultimoComandoAPI)) {
    Serial.println(F(">> API: command no admitido"));
    return;
  }

  if (fallosApiSeguidos) Serial.println(F(">> API: restablecida"));
  fallosApiSeguidos = 0;

  // 1) Modo: lo manda la web. Al cambiar, el backend pone command=stop.
  if (modoServidor != modoActual) {
    Serial.print(F(">> API: modo "));
    Serial.println(modoTxt);
    cambiarModo(modoServidor);
    hayUltimoComandoAPI = false;
  }

  // 2) Comando: solo en manual y solo cuando cambia.
  if (modoActual != MANUAL) {
    hayUltimoComandoAPI = false;
    primerSyncHecho = true;
    return;
  }
  const bool cambio = !hayUltimoComandoAPI || strcmp(ultimoComandoAPI, cmd) != 0;
  strncpy(ultimoComandoAPI, cmd, sizeof(ultimoComandoAPI) - 1);
  ultimoComandoAPI[sizeof(ultimoComandoAPI) - 1] = '\0';
  hayUltimoComandoAPI = true;

  if (!primerSyncHecho) {
    // Seguridad: un comando viejo guardado en el servidor no mueve el motor al arrancar.
    primerSyncHecho = true;
    return;
  }
  if (cambio) {
    Serial.print(F(">> API: comando "));
    Serial.println(cmd);
    procesarComando(letra);
  } else if (letra != 's' && estadoDeseado == (letra == 'd' ? DERECHA : IZQUIERDA)) {
    // Mismo comando aun vigente: el enlace sigue vivo, renueva el limite manual.
    ultimoComandoMotor = millis();
  }
}

void terminarHttp(bool correcto, const char* detalle) {
  clienteHttps.stop();
  http.fase = HTTP_INACTIVO;
  if (!correcto) falloApi(detalle);
  else aplicarRespuestaAPI();
}

bool anexarCuerpo(char c) {
  if (http.longitudCuerpo >= MAX_RESPUESTA_BYTES) return false;
  http.cuerpo[http.longitudCuerpo++] = c;
  http.cuerpo[http.longitudCuerpo] = '\0';
  return true;
}

// Procesa una linea ya completa de status, cabecera, tamano chunk o trailer.
void procesarLineaHttp() {
  char* linea = http.linea;
  if (http.fase == HTTP_STATUS) {
    if (strncmp(linea, "HTTP/1.", 7) != 0 || strlen(linea) < 12 || linea[8] != ' ' ||
        !isdigit((unsigned char)linea[9]) || !isdigit((unsigned char)linea[10]) ||
        !isdigit((unsigned char)linea[11])) {
      terminarHttp(false, "linea HTTP invalida");
      return;
    }
    http.codigo = (linea[9] - '0') * 100 + (linea[10] - '0') * 10 + (linea[11] - '0');
    if (http.codigo < 200) {
      terminarHttp(false, "respuesta provisional no admitida");
      return;
    }
    http.fase = HTTP_CABECERAS;
    return;
  }

  if (http.fase == HTTP_CABECERAS) {
    if (++http.lineasCabecera > MAX_ENCABEZADOS_HTTP) {
      terminarHttp(false, "demasiadas cabeceras");
      return;
    }
    if (!*linea) {
      if (http.codificacionNoAdmitida) {
        terminarHttp(false, "codificacion no admitida");
      } else if (http.codigo == 204 || http.codigo == 304) {
        terminarHttp(true, "sin cuerpo");
      } else if (http.chunked) {
        http.fase = HTTP_CHUNK_TAM;
      } else if (http.hayLongitud) {
        if (http.longitudEsperada > MAX_RESPUESTA_BYTES) {
          terminarHttp(false, "cuerpo demasiado grande");
        } else if (http.longitudEsperada == 0) {
          terminarHttp(true, "sin cuerpo");
        } else {
          http.fase = HTTP_CUERPO_LONGITUD;
        }
      } else {
        http.fase = HTTP_CUERPO_CIERRE;
      }
      return;
    }
    if (iniciaConSinCaso(linea, "Content-Length:")) {
      const char* valor = omitirEspacios(linea + 15);
      if (!isdigit((unsigned char)*valor)) {
        terminarHttp(false, "Content-Length invalido");
        return;
      }
      char* final = nullptr;
      const unsigned long longitud = strtoul(valor, &final, 10);
      if (final == valor || *omitirEspacios(final) != '\0' ||
          (http.hayLongitud && longitud != http.longitudEsperada)) {
        terminarHttp(false, "Content-Length ambiguo");
        return;
      }
      http.longitudEsperada = longitud;
      http.hayLongitud = true;
    } else if (iniciaConSinCaso(linea, "Transfer-Encoding:")) {
      const char* valor = omitirEspacios(linea + 18);
      if (http.chunked || !encabezadoSoloChunked(valor)) {
        http.codificacionNoAdmitida = true;
      } else {
        http.chunked = true;
      }
    } else if (iniciaConSinCaso(linea, "Content-Encoding:")) {
      const char* valor = omitirEspacios(linea + 17);
      if (!igualSinCaso(valor, "identity")) http.codificacionNoAdmitida = true;
    }
    if (http.chunked && http.hayLongitud) http.codificacionNoAdmitida = true;
    return;
  }

  if (http.fase == HTTP_CHUNK_TAM) {
    const char* valor = omitirEspacios(linea);
    if (!isxdigit((unsigned char)*valor)) {
      terminarHttp(false, "tamano chunk invalido");
      return;
    }
    char* final = nullptr;
    const unsigned long longitud = strtoul(valor, &final, 16);
    if (*final && *final != ';' && *omitirEspacios(final) != '\0') {
      terminarHttp(false, "formato chunk invalido");
    } else if (longitud > (unsigned long)(MAX_RESPUESTA_BYTES - http.longitudCuerpo)) {
      terminarHttp(false, "chunk demasiado grande");
    } else if (longitud == 0) {
      http.fase = HTTP_CHUNK_TRAILERS;
    } else {
      http.restantesChunk = longitud;
      http.fase = HTTP_CHUNK_DATOS;
    }
    return;
  }

  if (http.fase == HTTP_CHUNK_TRAILERS) {
    if (++http.lineasTrailer > MAX_TRAILERS_HTTP) {
      terminarHttp(false, "demasiados trailers");
    } else if (!*linea) {
      terminarHttp(true, "fin chunks");
    }
  }
}

// NOTA: devolver true al cerrar una linea; false cuando quedan mas caracteres.
void recibirByteHttp(char c) {
  if (http.fase == HTTP_CUERPO_LONGITUD || http.fase == HTTP_CUERPO_CIERRE ||
      http.fase == HTTP_CHUNK_DATOS) {
    if (!anexarCuerpo(c)) {
      terminarHttp(false, "respuesta supera el limite");
      return;
    }
    if (http.fase == HTTP_CUERPO_LONGITUD &&
        http.longitudCuerpo == http.longitudEsperada) {
      terminarHttp(true, "contenido completo");
    } else if (http.fase == HTTP_CHUNK_DATOS && --http.restantesChunk == 0) {
      http.fase = HTTP_CHUNK_CR;
    }
    return;
  }
  if (http.fase == HTTP_CHUNK_CR) {
    if (c != '\r') terminarHttp(false, "chunk sin CR");
    else http.fase = HTTP_CHUNK_LF;
    return;
  }
  if (http.fase == HTTP_CHUNK_LF) {
    if (c != '\n') terminarHttp(false, "chunk sin LF");
    else http.fase = HTTP_CHUNK_TAM;
    return;
  }

  // Status, cabeceras, chunk-size y trailers: lectura limitada por linea.
  if (c == '\n') {
    if (http.longitudLinea && http.linea[http.longitudLinea - 1] == '\r') {
      --http.longitudLinea;
    }
    http.linea[http.longitudLinea] = '\0';
    http.longitudLinea = 0;
    procesarLineaHttp();
  } else if (http.longitudLinea + 1 < MAX_LINEA_HTTP) {
    http.linea[http.longitudLinea++] = c;
  } else {
    terminarHttp(false, "linea HTTP demasiado larga");
  }
}

void atenderHttp() {
  if (http.fase == HTTP_INACTIVO) return;
  const unsigned long ahora = millis();
  if (ahora - http.inicio > TIMEOUT_HTTP_TOTAL_MS ||
      ahora - http.ultimoByte > TIMEOUT_HTTP_INACTIVO_MS) {
    terminarHttp(false, "timeout HTTP");
    return;
  }

  uint16_t cuenta = 0;
  while (http.fase != HTTP_INACTIVO && clienteHttps.available() > 0 &&
         cuenta++ < MAX_BYTES_HTTP_POR_LOOP) {
    const int valor = clienteHttps.read();
    if (valor < 0) break;
    http.ultimoByte = millis();
    recibirByteHttp((char)valor);
  }
  if (http.fase == HTTP_INACTIVO) return;
  if (!clienteHttps.connected() && clienteHttps.available() == 0) {
    if (http.fase == HTTP_CUERPO_CIERRE) terminarHttp(true, "fin por cierre");
    else terminarHttp(false, "respuesta HTTP incompleta");
  }
}

bool crearJsonEstado(char* destino, size_t capacidad, size_t& escritos) {
  JsonDocument documento;
  if (distanciaActual < 0) documento["distance"] = DISTANCIA_SIN_ECO_CM;
  else documento["distance"] = round(distanciaActual * 10.0f) / 10.0f;
  documento["motor"] = nombreMotorApi(estadoDeseado);
  const size_t longitud = measureJson(documento);
  if (longitud >= capacidad) return false;
  escritos = serializeJson(documento, destino, capacidad);
  return escritos == longitud;
}

void iniciarHttp() {
  if (http.fase != HTTP_INACTIVO || !wifiListo()) return;
  ultimaApi = millis();

  char cuerpoPeticion[96];
  size_t longitudPeticion = 0;
  if (!crearJsonEstado(cuerpoPeticion, sizeof(cuerpoPeticion), longitudPeticion)) {
    http.codigo = -1;
    falloApi("no se pudo construir el JSON");
    return;
  }

  // Las operaciones SSL y de escritura pueden bloquear internamente: parar
  // fisicamente hasta terminar de enviar. El estado deseado no se pierde.
  suspenderMotores();
  clienteHttps.stop();
  clienteHttps.setTimeout(TIMEOUT_HTTP_INACTIVO_MS);
  clienteHttps.setConnectionTimeout((int)TIMEOUT_CONEXION_TLS_MS);
  const bool conectado = clienteHttps.connect(API_HOST, API_PUERTO);
  atenderSerie();

  if (!conectado) {
    clienteHttps.stop();
    reanudarMotores();
    http.codigo = -1;
    Serial.print(F(">> TLS: IP "));
    Serial.print(WiFi.localIP());
    Serial.print(F(" RSSI "));
    Serial.println(WiFi.RSSI());
    falloApi("conexion TLS fallida");
    return;
  }

  // La medicion anterior puede ser vieja por la negociacion TLS.
  if (modoActual == AUTO) actualizarSensor(true);
  comprobarSeguridad();
  // El JSON lleva la orden actual, NO el estado temporal de parada por TLS.
  if (!crearJsonEstado(cuerpoPeticion, sizeof(cuerpoPeticion), longitudPeticion)) {
    clienteHttps.stop();
    reanudarMotores();
    http.codigo = -1;
    falloApi("JSON demasiado grande");
    return;
  }

  // Una sola cabecera compacta reduce viajes al modem ESP32-S3.
  char cabecera[320];
  const int n = snprintf(cabecera, sizeof(cabecera),
      "POST %s%s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "%s: %s\r\n"
      "Content-Type: application/json\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n\r\n",
      API_BASE_RUTA, API_RUTA_ESTADO, API_HOST,
      API_HEADER_KEY, API_KEY, (unsigned int)longitudPeticion);
  if (n < 0 || (size_t)n >= sizeof(cabecera)) {
    clienteHttps.stop();
    reanudarMotores();
    http.codigo = -1;
    falloApi("cabecera HTTP demasiado grande");
    return;
  }

  const size_t escritosCabecera = clienteHttps.write((const uint8_t*)cabecera, (size_t)n);
  const size_t escritosCuerpo = (escritosCabecera == (size_t)n)
      ? clienteHttps.write((const uint8_t*)cuerpoPeticion, longitudPeticion) : 0;
  reanudarMotores();
  if (escritosCabecera != (size_t)n || escritosCuerpo != longitudPeticion) {
    clienteHttps.stop();
    http.codigo = -1;
    falloApi("envio HTTP incompleto");
    return;
  }

  memset(&http, 0, sizeof(http));
  http.fase = HTTP_STATUS;
  http.codigo = -1;
  http.inicio = millis();
  http.ultimoByte = http.inicio;
}

// ================ WIFI ======================================
// En WiFiS3 status() puede ser WL_CONNECTED antes de que DHCP asigne IP
// (0.0.0.0): sin IP no hay DNS y la conexion TLS falla siempre.
bool wifiListo() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void mantenerWiFi() {
  const unsigned long ahora = millis();
  if (ahora - ultimaRevisionWiFi < 250UL) return;
  ultimaRevisionWiFi = ahora;

  const int estado = WiFi.status();
  if (estado == WL_NO_MODULE) {
    if (wifiModuloDisponible) Serial.println(F(">> WiFi: modulo no detectado"));
    wifiModuloDisponible = false;
    wifiConectando = false;
    wifiConectadoAntes = false;
    return;
  }
  wifiModuloDisponible = true;

  if (estado == WL_CONNECTED && !wifiListo()) {
    // Asociado al AP pero sin IP todavia: esperar; si tarda, reiniciar el intento.
    if (!wifiConectando) { wifiConectando = true; inicioConexionWiFi = ahora; }
    if (ahora - inicioConexionWiFi < TIMEOUT_WIFI_MS) return;
    Serial.println(F(">> WiFi: sin IP (DHCP); reiniciando conexion"));
    WiFi.disconnect();
    wifiConectando = false;
    ultimoIntentoWiFi = ahora;
    return;
  }

  if (estado == WL_CONNECTED) {
    if (!wifiConectadoAntes) {
      Serial.print(F(">> WiFi conectado. IP: "));
      Serial.println(WiFi.localIP());
    }
    wifiConectadoAntes = true;
    wifiConectando = false;
    return;
  }

  if (wifiConectadoAntes) {
    Serial.println(F(">> WiFi: se perdio conexion"));
    wifiConectadoAntes = false;
    // Se abortara la solicitud HTTPS incompleta en la siguiente vuelta.
    if (http.fase != HTTP_INACTIVO) terminarHttp(false, "WiFi desconectado");
  }

  if (wifiConectando) {
    if (ahora - inicioConexionWiFi < TIMEOUT_WIFI_MS) return;
    wifiConectando = false;
    Serial.println(F(">> WiFi: intento agotado; se reintentara"));
  }
  if (ahora - ultimoIntentoWiFi < INTERVALO_WIFI_MS) return;
  if (http.fase != HTTP_INACTIVO) return;

  ultimoIntentoWiFi = ahora;
  inicioConexionWiFi = ahora;
  wifiConectando = true;
  Serial.println(F(">> WiFi: intentando conexion"));
  suspenderMotores();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  atenderSerie();
  reanudarMotores();
}

// ================ INICIALIZACION Y CICLO =====================
void setup() {
  Serial.begin(115200);
  const unsigned long inicioSerial = millis();
  while (!Serial && millis() - inicioSerial < 2500UL) delay(10);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_ENA, OUTPUT);
  pinMode(PIN_ENB, OUTPUT);
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  detenerSalidas();
  for (uint8_t i = 0; i < N_MUESTRAS; ++i) muestras[i] = -1.0f;
  memset(&http, 0, sizeof(http));
  http.fase = HTTP_INACTIVO;
  http.codigo = -1;

  // Disparar los primeros chequeos al iniciar sin esperar todo el intervalo.
  ultimaMedicion = millis() - INTERVALO_SENSOR_MS;
  ultimaInfo = millis();
  ultimoIntentoWiFi = millis() - INTERVALO_WIFI_MS;
  ultimaRevisionWiFi = millis() - 250UL;
  ultimaApi = millis();

  Serial.println(F("FASE 6 lista. a=auto, m=manual, d/i/s=motor (manual), x=PARO"));
  Serial.println(F("WiFiS3 + ArduinoJson v7. API: /api/device/sync (mode/command)."));
}

void loop() {
  atenderSerie();
  actualizarSensor(false);
  comprobarSeguridad();

  const unsigned long ahora = millis();
  if (ahora - ultimaInfo >= INTERVALO_INFO_MS) {
    ultimaInfo = ahora;
    imprimirEstado();
  }

  // Polling de red por estados: sin readStringUntil() ni esperas activas.
  atenderHttp();
  mantenerWiFi();
  if (wifiConectadoAntes && http.fase == HTTP_INACTIVO &&
      millis() - ultimaApi >= INTERVALO_API_MS) {
    iniciarHttp();
  }
}
