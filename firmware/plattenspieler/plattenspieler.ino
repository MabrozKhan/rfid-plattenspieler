/*
  RFID-Plattenspieler, Version 2

  Bedienung
    Platte auflegen            Link wird gelesen, noch keine Musik
    Tonarm runter              Musik startet beim ersten Titel, Teller dreht
    Tonarm hoch                Pause, Teller steht
    Tonarm wieder runter       Musik läuft an derselben Stelle weiter
    Platte wegnehmen           Pause und Reset, nächstes Auflegen beginnt von vorne
    Drehgeber drehen           Lautstärke in 5er-Schritten, geht auch in der Pause
    Drehgeber drücken          Stumm an und aus
    Taster Weiter / Zurück     nächster / vorheriger Titel, geht auch in der Pause

  Board in der Arduino IDE   ESP32 Dev Module
  Bibliotheken               MFRC522 (GithubCommunity), Adafruit SSD1306,
                             Adafruit GFX, ArduinoJson (Version 7, Benoit Blanchon)
  Serieller Monitor          115200 Baud
  Zugangsdaten               secrets.h (Vorlage secrets.example.h)

  Verkabelung
    RC522            SDA 5, SCK 18, MOSI 23, MISO 19, RST 22, GND, 3.3V an 3V3
    ULN2003 (Motor)  IN1 25, IN2 26, IN3 27, IN4 14, Plus an VIN (5 V), Minus an GND
    Drehgeber KY-040 CLK 34, DT 35, SW 13, Plus an 3V3 (nicht 5 V), GND an GND
    Taster Weiter    zwischen GPIO 32 und GND
    Taster Zurück    zwischen GPIO 33 und GND
    Tonarm-Schalter  Mikroschalter, COM an GND, NO an GPIO 16
    Display SSD1306  128 x 32, I2C: SDA an GPIO 21, SCL an GPIO 4, VCC an 3V3, GND

  Aufbau des Programms
    Das Programm besteht aus drei Aufgaben (Tasks), die nebeneinander laufen.
    1. loop() liest Platte, Tonarm, Taster und Drehgeber und entscheidet, was
       passieren soll. loop() wartet nie auf das Internet, deshalb reagiert
       das Gerät sofort.
    2. spotifyTask() schickt die Befehle an Spotify. Die Verbindung zu Spotify
       bleibt dabei offen, statt für jeden Befehl neu aufgebaut zu werden.
       Das spart pro Befehl etwa eine Sekunde.
    3. motorTask() dreht den Teller in einem gleichmäßigen Takt.

    loop() und spotifyTask() reden über eine gemeinsame "Auftragsliste"
    miteinander. Kommen mehrere Aufträge schneller, als Spotify sie abarbeiten
    kann, werden sie zusammengefasst. Beispiel: Fünf Rasten am Drehgeber
    während eines laufenden Befehls werden zu einem einzigen Befehl "+25 %".
*/

#include <SPI.h>               // Bus zum RC522
#include <MFRC522.h>           // RFID-Leser
#include <WiFi.h>              // WLAN
#include <WiFiClientSecure.h>  // verschlüsselte Verbindung (HTTPS)
#include <HTTPClient.h>        // nur für die Token-Anfrage
#include <Preferences.h>       // dauerhafter Speicher im Flash des ESP32
#include <Wire.h>              // I2C-Bus zum Display
#include <Adafruit_GFX.h>      // Grafikfunktionen (Text, Linien, Rechtecke)
#include <Adafruit_SSD1306.h>  // Treiber für das OLED-Display
#include <ArduinoJson.h>       // liest die Titelinfos aus der Spotify-Antwort

#include "secrets.h"           // WLAN, Client-ID, Refresh Token, Geräte-ID

// ===========================================================================
// Einstellungen
// ===========================================================================

// --- RC522 ---
const uint8_t PIN_SS  = 5;     // am RC522 mit SDA beschriftet
const uint8_t PIN_RST = 22;

// --- Schrittmotor 28BYJ-48 über ULN2003 ---
const uint8_t PIN_MOTOR[4] = { 25, 26, 27, 14 };   // IN1 bis IN4
// Zeit zwischen zwei Halbschritten. 4096 Halbschritte sind eine Umdrehung.
//   2 ms  ->  etwa 7 Umdrehungen pro Minute
//   1 ms  ->  etwa 15 Umdrehungen pro Minute (Obergrenze dieses Motors)
const uint32_t MOTOR_SCHRITT_MS  = 2;
const bool     MOTOR_RUECKWAERTS = false;           // Drehrichtung umkehren

// --- Drehgeber KY-040 ---
// GPIO 34 und 35 sind reine Eingänge ohne internen Pull-up. Das passt hier,
// weil die KY-040-Platine eigene Pull-up-Widerstände für CLK und DT hat.
const uint8_t PIN_DREH_CLK = 34;
const uint8_t PIN_DREH_DT  = 35;
const uint8_t PIN_DREH_SW  = 13;                    // Knopf im Drehgeber
// Wie viele Signalwechsel eine Raste ergibt. Beim KY-040 meist 4.
// Springt die Lautstärke pro Raste um zwei Schritte, hier 2 eintragen.
// Muss man zwei Rasten drehen für einen Schritt, hier 2 statt 4 testen.
const int8_t  DREH_WECHSEL_PRO_RASTE = 4;
const bool    DREH_UMKEHREN          = false;       // Richtung tauschen
const int8_t  LAUTSTAERKE_SCHRITT    = 5;           // Prozent pro Raste

// --- Taster ---
// Alle Taster und Schalter schalten gegen GND. Der ESP32 zieht den Pin intern
// auf 3,3 V (INPUT_PULLUP). Gedrückt liegt der Pin auf 0 V.
const uint8_t PIN_WEITER  = 32;
const uint8_t PIN_ZURUECK = 33;
const uint8_t PIN_TONARM  = 16;
// true: Schalter gedrückt heißt Tonarm liegt auf der Platte.
// Ist es bei deiner Mechanik umgekehrt, auf false stellen.
const bool TONARM_GEDRUECKT_HEISST_UNTEN = false;
const uint32_t TASTER_ENTPRELL_MS = 50;

// --- Platte ---
const uint32_t ABFRAGE_INTERVALL_MS = 250;   // so oft wird nach der Platte geschaut
// Erst nach so vielen Fehlversuchen hintereinander gilt die Platte als weg.
// 4 x 250 ms = etwa eine Sekunde. Schützt vor einzelnen Lesefehlern.
const uint8_t  FEHLVERSUCHE_BIS_WEG = 4;
// NTAG213: Nutzdaten auf Seite 4 bis 39, das sind 144 Byte.
const uint8_t  ERSTE_SEITE   = 4;
const uint16_t NUTZDATEN_LEN = 144;

// --- Spotify ---
const char*    API_HOST = "api.spotify.com";
// Findet Spotify den Echo unter der gespeicherten ID nicht, suchen wir ihn
// in der Geräteliste über einen Teil seines Namens (Groß/klein egal).
const char*    GERAET_NAME_TEIL = "echo";
// Nach so langer Funkstille bauen wir die Verbindung lieber frisch auf, weil
// Spotify unbenutzte Verbindungen irgendwann von sich aus schließt.
const uint32_t VERBINDUNG_LEERLAUF_MS = 20000;
const uint32_t ANTWORT_FRIST_MS       = 8000;    // so lange auf eine Antwort warten
// Selbstheilung: Nach so vielen Anfragen ohne Verbindung hintereinander wird
// das WLAN neu verbunden, nach doppelt so vielen startet der ESP32 neu.
const uint8_t  FEHLER_BIS_WLAN_NEU    = 2;
// Wurde die Lautstärke so lange nicht verändert, holen wir vor der nächsten
// Änderung den echten Wert von Spotify. Er könnte per Sprache geändert worden sein.
const uint32_t LAUTSTAERKE_NEU_LESEN_MS = 10000;
// So oft wird beim Abspielen nachgefragt, welcher Titel gerade läuft.
const uint32_t TITEL_ABFRAGE_MS = 5000;

// --- Display ---
// GPIO 22 ist schon vom RC522 belegt, deshalb SCL auf GPIO 4.
const uint8_t  PIN_OLED_SDA  = 21;
const uint8_t  PIN_OLED_SCL  = 4;
const uint8_t  OLED_ADRESSE  = 0x3C;
const uint32_t EINBLENDUNG_MS = 1500;   // so lange bleibt die große Lautstärke-Anzeige
const uint32_t HINWEIS_MS     = 4000;   // so lange bleibt ein Hinweis stehen

// ===========================================================================
// Typen und gemeinsame Daten
// ===========================================================================

// Was mit der Wiedergabe passieren soll.
enum Wiedergabe : uint8_t { W_NICHTS, W_START, W_WEITER, W_PAUSE };

// Die Auftragsliste zwischen loop() und spotifyTask().
struct Auftraege {
  Wiedergabe wiedergabe = W_NICHTS;   // es zählt nur der letzte Wunsch
  String     uri;                     // bei W_START: welches Album
  uint32_t   platteNr = 0;            // bei W_START: zu welchem Auflegen gehört er
  int8_t     spruenge = 0;            // +2 = zweimal weiter, -1 = einmal zurück
  int16_t    lautAenderung = 0;       // in Prozentpunkten, aufsummiert
  bool       stummUmschalten = false;
};

// Zustand eines Tasters fürs Entprellen
struct Taster {
  uint8_t  pin;
  bool     gedrueckt;       // entprellter Zustand
  bool     letzterRohwert;  // letzter direkt gelesener Wert
  uint32_t letzteAenderung; // wann sich der Rohwert zuletzt geändert hat
};

Auftraege         offeneAuftraege;
SemaphoreHandle_t auftragSperre;     // damit nie beide Tasks gleichzeitig schreiben

// Von loop() geschrieben, von den anderen Tasks gelesen. "volatile", weil
// mehrere Tasks gleichzeitig darauf zugreifen.
volatile bool     sollSpielen       = false;
volatile bool     motorSollDrehen   = false;
// Von spotifyTask() geschrieben: Für welches Auflegen läuft die Platte schon?
volatile uint32_t gestartetePlatteNr = 0;

// --- Nur von loop() benutzt ---
MFRC522  rfid(PIN_SS, PIN_RST);
bool     platteLiegtAuf  = false;
bool     platteGelesen   = false;
uint8_t  fehlversuche    = 0;
uint32_t platteNr        = 0;     // zählt jedes Auflegen hoch
String   aktuelleUri;
bool     armUnten        = false;
bool     zuletztSoll     = false;
uint32_t letzteAbfrage   = 0;

Taster tasterWeiter   = { PIN_WEITER,  false, false, 0 };
Taster tasterZurueck  = { PIN_ZURUECK, false, false, 0 };
Taster tasterTonarm   = { PIN_TONARM,  false, false, 0 };
Taster tasterDrehKnopf = { PIN_DREH_SW, false, false, 0 };

// --- Drehgeber, wird im Interrupt geändert ---
volatile int32_t drehZaehler = 0;
volatile uint8_t drehZustand = 0;
portMUX_TYPE     drehSperre  = portMUX_INITIALIZER_UNLOCKED;

// --- Nur von spotifyTask() benutzt (und einmal in setup) ---
Preferences      speicher;
WiFiClientSecure apiClient;          // bleibt zwischen den Befehlen offen
uint32_t         apiLetzteNutzung = 0;
String           accessToken;
String           refreshToken;
uint32_t         tokenGueltigBis = 0;
int              lautstaerke     = -1;   // -1 = noch unbekannt
int              lautVorStumm    = 30;
uint32_t         letzteLautAktion = 0;
String           geraetId = SPOTIFY_DEVICE_ID;   // kann sich zur Laufzeit ändern
uint32_t         naechsteTitelAbfrage = 0;
uint8_t          verbindungsFehlerInFolge = 0;
bool             anfrageProtokoll = true;         // false = Anfrage nicht im Monitor zeigen

// --- Display ---
// Breite 128, Höhe 32 Pixel, an Wire (I2C), kein eigener Reset-Pin.
Adafruit_SSD1306 display(128, 32, &Wire, -1);
bool             displayOk = false;
// Was angezeigt wird. Die Texte schreiben spotifyTask und loop(), lesen tut
// der Display-Task. Die Sperre verhindert, dass ein halber Text gelesen wird.
SemaphoreHandle_t anzeigeSperre;
String           anzeigeTitel;
String           anzeigeInterpret;
String           anzeigeAlbum;
String           anzeigeHinweis;
volatile uint32_t hinweisBis      = 0;
volatile int     anzeigeLautstaerke = -1;
volatile uint32_t einblendungBis  = 0;

// ===========================================================================
// Teil 1: WLAN
// ===========================================================================
void verbindeWlan() {
  Serial.print("Verbinde mit WLAN ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);     // bei Abbruch selbst neu verbinden
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  // Stromsparmodus des Funkmoduls aus. Sonst schläft es zwischendurch und
  // Verbindungen werden langsam oder scheitern.
  WiFi.setSleep(false);
  Serial.print(" verbunden, IP: ");
  Serial.print(WiFi.localIP());
  Serial.printf(", Signal %d dBm\n", (int)WiFi.RSSI());
}

// ===========================================================================
// Teil 2: Spotify
// ===========================================================================

// Holt einen Wert aus einer JSON-Antwort, z. B. "access_token".
// Bewusst einfach gehalten, reicht für die Werte, die wir brauchen.
String jsonWert(const String& json, const char* schluessel) {
  String suche = String('"') + schluessel + '"';
  int pos = json.indexOf(suche);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos + suche.length());
  if (pos < 0) return "";
  pos++;
  while (pos < (int)json.length() && json[pos] == ' ') pos++;

  if (json[pos] == '"') {                       // Text in Anführungszeichen
    int ende = json.indexOf('"', pos + 1);
    return json.substring(pos + 1, ende);
  }
  int ende = pos;                               // Zahl: bis Komma oder Klammer
  while (ende < (int)json.length() && json[ende] != ',' && json[ende] != '}') ende++;
  return json.substring(pos, ende);
}

// Holt mit dem Refresh Token einen neuen Access Token. Das passiert nur etwa
// einmal pro Stunde, deshalb hier mit einer eigenen, kurzen Verbindung.
bool erneuereToken() {
  WiFiClientSecure client;
  client.setInsecure();   // Prototyp: Zertifikat wird nicht geprüft, Verbindung ist trotzdem verschlüsselt
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String koerper = "grant_type=refresh_token&refresh_token=" + refreshToken +
                   "&client_id=" + SPOTIFY_CLIENT_ID;
  int status = http.POST(koerper);
  String antwort = (status > 0 && status != 204) ? http.getString() : "";
  http.end();

  if (status != 200) {
    Serial.printf("Token erneuern fehlgeschlagen (%d): %s\n", status, antwort.c_str());
    return false;
  }

  accessToken = jsonWert(antwort, "access_token");
  long sekunden = jsonWert(antwort, "expires_in").toInt();
  if (sekunden <= 0) sekunden = 3600;
  tokenGueltigBis = millis() + (uint32_t)(sekunden - 60) * 1000UL;   // 1 min Puffer

  // Spotify kann einen neuen Refresh Token mitschicken. Dann dauerhaft merken.
  String neu = jsonWert(antwort, "refresh_token");
  if (neu.length() > 0 && neu != refreshToken) {
    refreshToken = neu;
    speicher.putString("refresh", refreshToken);
  }
  Serial.println("Access Token erneuert.");
  return true;
}

bool tokenBereit() {
  // (int32_t) macht den Vergleich sicher, auch wenn millis() überläuft.
  if (accessToken.length() > 0 && (int32_t)(tokenGueltigBis - millis()) > 0) return true;
  return erneuereToken();
}

// --- Kleine HTTP-Bausteine für die offene Verbindung ---------------------
// Wir sprechen HTTP hier selbst, statt die Bibliothek HTTPClient zu nehmen.
// So haben wir genau im Griff, dass die Verbindung offen bleibt und dass nie
// auf Daten gewartet wird, die gar nicht kommen.

// Liest eine Zeile bis zum Zeilenende. false bei Zeitüberschreitung oder Abbruch.
bool leseZeile(String& zeile, uint32_t frist) {
  zeile = "";
  while ((int32_t)(frist - millis()) > 0) {
    while (apiClient.available() > 0) {
      char c = (char)apiClient.read();
      if (c == '\n') {
        zeile.trim();                  // entfernt das '\r' am Ende
        return true;
      }
      zeile += c;
    }
    if (!apiClient.connected()) return false;
    delay(1);
  }
  return false;
}

// Liest genau "anzahl" Byte und hängt sie an "ziel" an.
bool leseBytes(String& ziel, size_t anzahl, uint32_t frist) {
  uint8_t puffer[256];
  while (anzahl > 0) {
    if ((int32_t)(frist - millis()) <= 0) return false;
    if (apiClient.available() <= 0) {
      if (!apiClient.connected()) return false;
      delay(1);
      continue;
    }
    size_t wunsch = anzahl < sizeof(puffer) ? anzahl : sizeof(puffer);
    int n = apiClient.read(puffer, wunsch);
    if (n <= 0) { delay(1); continue; }
    for (int i = 0; i < n; i++) ziel += (char)puffer[i];
    anzahl -= n;
  }
  return true;
}

// Schickt EINE Anfrage an api.spotify.com. Gibt den HTTP-Status zurück,
// negative Werte bedeuten einen Fehler auf Verbindungsebene.
int apiAnfrageEinmal(const char* methode, const String& pfad,
                     const String& koerper, String& antwort) {
  antwort = "";

  // Lange unbenutzte Verbindung lieber frisch aufbauen.
  if (millis() - apiLetzteNutzung > VERBINDUNG_LEERLAUF_MS) apiClient.stop();

  if (!apiClient.connected()) {
    apiClient.stop();
    if (!apiClient.connect(API_HOST, 443, 5000)) {
      // Verbindung kam nicht zustande. Für die Fehlersuche ausgeben, woran es
      // liegen könnte: Namensauflösung (DNS), Arbeitsspeicher oder Funksignal.
      IPAddress ip;
      bool dnsOk = WiFi.hostByName(API_HOST, ip);
      Serial.printf("  Kein Verbindungsaufbau. DNS %s (%s), freier Speicher %u, "
                    "größter Block %u, WLAN %d dBm, Status %d\n",
                    dnsOk ? "ok" : "FEHLER", ip.toString().c_str(),
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                    (int)WiFi.RSSI(), (int)WiFi.status());
      return -1;
    }
  }
  while (apiClient.available() > 0) apiClient.read();          // Reste verwerfen

  // --- Anfrage zusammenbauen und in einem Stück senden ---
  String anfrage = String(methode) + " /v1" + pfad + " HTTP/1.1\r\n" +
                   "Host: " + API_HOST + "\r\n" +
                   "Authorization: Bearer " + accessToken + "\r\n" +
                   "Content-Type: application/json\r\n" +
                   "Content-Length: " + String(koerper.length()) + "\r\n" +
                   "Connection: keep-alive\r\n\r\n" +
                   koerper;
  if (apiClient.print(anfrage) != anfrage.length()) {
    apiClient.stop();
    return -3;                                                  // Senden fehlgeschlagen
  }
  apiLetzteNutzung = millis();

  // --- Statuszeile lesen, z. B. "HTTP/1.1 204 No Content" ---
  uint32_t frist = millis() + ANTWORT_FRIST_MS;
  String zeile;
  if (!leseZeile(zeile, frist)) { apiClient.stop(); return -5; }
  int leer = zeile.indexOf(' ');
  int status = (leer > 0) ? zeile.substring(leer + 1, leer + 4).toInt() : 0;
  if (status <= 0) { apiClient.stop(); return -5; }

  // --- Kopfzeilen lesen, bis eine Leerzeile kommt ---
  long laenge = -1;
  bool chunked = false;
  bool schliessen = false;
  for (;;) {
    if (!leseZeile(zeile, frist)) { apiClient.stop(); return -5; }
    if (zeile.length() == 0) break;
    zeile.toLowerCase();
    if (zeile.startsWith("content-length:")) {
      laenge = zeile.substring(15).toInt();
    } else if (zeile.startsWith("transfer-encoding:") && zeile.indexOf("chunked") > 0) {
      chunked = true;
    } else if (zeile.startsWith("connection:") && zeile.indexOf("close") > 0) {
      schliessen = true;
    }
  }

  // --- Inhalt lesen, je nachdem wie Spotify ihn schickt ---
  bool ok = true;
  if (status == 204 || status == 304) {
    // kein Inhalt
  } else if (chunked) {
    antwort.reserve(4096);                        // weniger Umkopieren, weniger Zerstückelung
    // Inhalt kommt in Blöcken: Größe (hex), Zeilenende, Daten, Zeilenende.
    // Ein Block der Größe 0 beendet die Antwort.
    for (;;) {
      if (!leseZeile(zeile, frist)) { ok = false; break; }
      long teil = strtol(zeile.c_str(), nullptr, 16);
      if (teil <= 0) { leseZeile(zeile, frist); break; }
      if (!leseBytes(antwort, (size_t)teil, frist)) { ok = false; break; }
      leseZeile(zeile, frist);
    }
  } else if (laenge >= 0) {
    antwort.reserve(laenge + 1);                  // Platz vorab reservieren, spart Zeit
    ok = leseBytes(antwort, (size_t)laenge, frist);
  } else {
    // Weder Länge noch Blöcke: Der Server schließt am Ende die Verbindung.
    while (apiClient.connected() || apiClient.available() > 0) {
      if ((int32_t)(frist - millis()) <= 0) break;
      if (apiClient.available() > 0) antwort += (char)apiClient.read();
      else delay(1);
    }
    schliessen = true;
  }

  if (!ok || schliessen) apiClient.stop();
  return status;
}

// Anfrage mit Zeitmessung und einem zweiten Versuch bei Verbindungsfehlern.
int apiAnfrage(const char* methode, const String& pfad,
               const String& koerper, String& antwort) {
  uint32_t start = millis();
  int status = apiAnfrageEinmal(methode, pfad, koerper, antwort);
  if (status < 0) {
    Serial.printf("  Verbindungsfehler %d, neuer Versuch mit frischer Verbindung\n", status);
    apiClient.stop();
    status = apiAnfrageEinmal(methode, pfad, koerper, antwort);
  }

  // --- Selbstheilung bei dauerhaften Verbindungsproblemen ---
  if (status < 0) {
    verbindungsFehlerInFolge++;
    zeigeHinweis("Keine Verbindung zu Spotify");
    if (verbindungsFehlerInFolge >= 2 * FEHLER_BIS_WLAN_NEU) {
      // Auch das neue WLAN hat nicht geholfen: kompletter Neustart.
      Serial.println("Verbindung bleibt weg. Starte den ESP32 neu...");
      zeigeHinweis("Neustart...");
      delay(1000);
      ESP.restart();
    } else if (verbindungsFehlerInFolge >= FEHLER_BIS_WLAN_NEU) {
      // WLAN trennen und neu verbinden. Das behebt hängende Namensauflösung
      // und Verbindungen, die nach langer Laufzeit nicht mehr zustande kommen.
      Serial.println("Mehrere Verbindungsfehler. Verbinde WLAN neu...");
      apiClient.stop();
      WiFi.disconnect();
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      uint32_t warteBis = millis() + 15000;
      while (WiFi.status() != WL_CONNECTED && (int32_t)(warteBis - millis()) > 0) delay(250);
      Serial.printf("WLAN %s\n", WiFi.status() == WL_CONNECTED ? "wieder verbunden" : "weiterhin weg");
      // Einmal neu versuchen
      status = apiAnfrageEinmal(methode, pfad, koerper, antwort);
      if (status >= 0) verbindungsFehlerInFolge = 0;
    }
  } else {
    verbindungsFehlerInFolge = 0;
  }

  // Für die Ausgabe nur den Teil vor dem '?' zeigen, die Geräte-ID ist lang.
  int frage = pfad.indexOf('?');
  String kurz = (frage >= 0) ? pfad.substring(0, frage) : pfad;
  if (anfrageProtokoll || status < 0 || status >= 300) {
    Serial.printf("  %s %s -> %d (%lu ms)\n", methode, kurz.c_str(), status,
                  (unsigned long)(millis() - start));
  }
  return status;
}

// Fragt die Geräteliste bei Spotify ab, gibt sie im Monitor aus und sucht den
// Echo über seinen Namen. true, wenn er gefunden wurde (geraetId ist dann aktuell).
bool findeGeraet() {
  String antwort;
  int status = apiAnfrage("GET", "/me/player/devices", "", antwort);
  if (status != 200) return false;

  Serial.println("Geräte, die Spotify gerade kennt:");
  bool gefunden = false;
  int pos = 0;
  // Jedes Gerät ist ein Objekt in geschweiften Klammern ohne weitere
  // Verschachtelung. Wir gehen sie der Reihe nach durch.
  for (;;) {
    int auf = antwort.indexOf('{', pos + 1);
    if (auf < 0) break;
    int zu = antwort.indexOf('}', auf);
    if (zu < 0) break;
    String objekt = antwort.substring(auf, zu + 1);
    pos = zu;
    String id   = jsonWert(objekt, "id");
    String name = jsonWert(objekt, "name");
    if (id.length() == 0) continue;
    Serial.printf("  %s  (%s)\n", name.c_str(), id.c_str());
    String nameKlein = name;
    nameKlein.toLowerCase();
    if (!gefunden && nameKlein.indexOf(GERAET_NAME_TEIL) >= 0) {
      gefunden = true;
      if (id != geraetId) {
        Serial.println("  -> neue ID für den Echo übernommen");
        geraetId = id;
      }
    }
  }
  if (!gefunden) {
    Serial.println("  Echo nicht in der Liste. Er ist für Spotify gerade nicht erreichbar.");
    zeigeHinweis("Echo nicht erreichbar");
    Serial.println("  Tipp: einmal \"Alexa, spiele Spotify\" sagen, dann erneut versuchen.");
  }
  return gefunden;
}

// Schickt einen Befehl an den Spotify-Player.
//   methode    "PUT" oder "POST"
//   befehl     play, pause, volume, next, previous, shuffle
//   parameter  zusätzliche Angaben, z. B. "&volume_percent=40"
bool playerBefehl(const char* methode, const char* befehl,
                  const String& parameter, const String& koerper) {
  if (!tokenBereit()) return false;
  String pfad = String("/me/player/") + befehl + "?device_id=" + geraetId + parameter;
  String antwort;
  int status = apiAnfrage(methode, pfad, koerper, antwort);
  if (status == 401 && erneuereToken()) {          // Token abgelaufen
    status = apiAnfrage(methode, pfad, koerper, antwort);
  }
  if (status == 404) {
    // Gerät unter dieser ID nicht gefunden. In der Geräteliste nachsehen,
    // ob der Echo eine neue ID hat, und dann einmal neu versuchen.
    Serial.println("Spotify kennt das Gerät unter dieser ID gerade nicht, suche den Echo...");
    if (findeGeraet()) {
      pfad = String("/me/player/") + befehl + "?device_id=" + geraetId + parameter;
      status = apiAnfrage(methode, pfad, koerper, antwort);
    }
  }
  if (status == 200 || status == 202 || status == 204) return true;

  Serial.printf("Spotify-Fehler bei %s (%d)\n", befehl, status);
  return false;
}

// Startet ein Album oder eine Playlist beim ersten Titel ab Sekunde 0.
bool spiele(const String& uri) {
  String koerper;
  if (uri.startsWith("spotify:track:")) {
    koerper = "{\"uris\":[\"" + uri + "\"],\"position_ms\":0}";
  } else {
    koerper = "{\"context_uri\":\"" + uri +
              "\",\"offset\":{\"position\":0},\"position_ms\":0}";
  }
  return playerBefehl("PUT", "play", "", koerper);
}

bool weiter()          { return playerBefehl("PUT",  "play",     "", "{}"); }
bool pausiere()        { return playerBefehl("PUT",  "pause",    "", "{}"); }
bool naechsterTitel()  { return playerBefehl("POST", "next",     "", "{}"); }
bool vorherigerTitel() { return playerBefehl("POST", "previous", "", "{}"); }
bool zufallAus()       { return playerBefehl("PUT",  "shuffle",  "&state=false", "{}"); }

bool setzeLautstaerke(int prozent) {
  return playerBefehl("PUT", "volume", "&volume_percent=" + String(prozent), "{}");
}

// Fragt die aktuelle Lautstärke des Echo bei Spotify ab. -1 bei Fehler.
int leseLautstaerke() {
  if (!tokenBereit()) return -1;
  String antwort;
  int status = apiAnfrage("GET", "/me/player", "", antwort);
  if (status == 401 && erneuereToken()) status = apiAnfrage("GET", "/me/player", "", antwort);
  if (status != 200) return -1;                     // 204 = gerade nichts aktiv
  String wert = jsonWert(antwort, "volume_percent");
  if (wert.length() == 0 || wert == "null") return -1;
  return wert.toInt();
}

// ---------------------------------------------------------------------------
// Die Spotify-Aufgabe: holt Aufträge ab und arbeitet sie nacheinander ab.
// ---------------------------------------------------------------------------
void spotifyTask(void* parameter) {
  apiClient.setInsecure();     // Prototyp: Zertifikat wird nicht geprüft

  for (;;) {
    // Aufträge abholen und die Liste leeren. Die Sperre sorgt dafür, dass
    // loop() nicht gleichzeitig hineinschreibt.
    xSemaphoreTake(auftragSperre, portMAX_DELAY);
    Auftraege a = offeneAuftraege;
    offeneAuftraege = Auftraege();
    xSemaphoreGive(auftragSperre);

    bool etwasZuTun = a.wiedergabe != W_NICHTS || a.spruenge != 0 ||
                      a.lautAenderung != 0 || a.stummUmschalten;
    if (!etwasZuTun) {
      // Nichts zu tun: Beim Abspielen ab und zu den aktuellen Titel holen.
      if (sollSpielen && WiFi.status() == WL_CONNECTED &&
          (int32_t)(millis() - naechsteTitelAbfrage) >= 0) {
        holeTitel();
        naechsteTitelAbfrage = millis() + TITEL_ABFRAGE_MS;
      }
      vTaskDelay(pdMS_TO_TICKS(20));                // kurz schlafen, dann wieder schauen
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Kein WLAN, Auftrag verworfen.");
      continue;
    }

    // --- 1. Wiedergabe ---
    switch (a.wiedergabe) {
      case W_START:
        Serial.printf("Spotify: starte von vorne %s\n", a.uri.c_str());
        if (spiele(a.uri)) {
          gestartetePlatteNr = a.platteNr;
          zufallAus();
          // Aktuelle Lautstärke holen, damit das Display sie gleich zeigt.
          int echt = leseLautstaerke();
          if (echt >= 0) {
            lautstaerke = echt;
            anzeigeLautstaerke = echt;
            letzteLautAktion = millis();
          }
        }
        break;
      case W_WEITER:
        Serial.println("Spotify: weiter");
        weiter();
        break;
      case W_PAUSE:
        Serial.println("Spotify: Pause");
        pausiere();
        break;
      default:
        break;
    }

    // --- 2. Titel springen ---
    bool gesprungen = a.spruenge != 0;
    while (a.spruenge > 0) { Serial.println("Spotify: nächster Titel");   naechsterTitel();  a.spruenge--; }
    while (a.spruenge < 0) { Serial.println("Spotify: vorheriger Titel"); vorherigerTitel(); a.spruenge++; }
    // Springen startet auf manchen Geräten die Wiedergabe. Liegt der Tonarm
    // oben, schicken wir deshalb gleich wieder Pause hinterher.
    if (gesprungen && !sollSpielen) pausiere();
    // Nach Start, Weiter oder Sprung den neuen Titel in einer Sekunde abfragen.
    // Der Echo braucht einen Moment, bis Spotify den Wechsel meldet.
    if (a.wiedergabe == W_START || a.wiedergabe == W_WEITER || gesprungen) {
      naechsteTitelAbfrage = millis() + 1000;
      if (gesprungen && !sollSpielen) holeTitel();   // in der Pause sofort zeigen
    }

    // --- 3. Lautstärke ---
    if (a.lautAenderung != 0 || a.stummUmschalten) {
      if (lautstaerke < 0 || millis() - letzteLautAktion > LAUTSTAERKE_NEU_LESEN_MS) {
        int echt = leseLautstaerke();
        if (echt >= 0) lautstaerke = echt;
        else if (lautstaerke < 0) lautstaerke = 30;  // Notlösung, falls unbekannt
      }
      int neu = lautstaerke;
      if (a.stummUmschalten) {
        if (lautstaerke > 0) { lautVorStumm = lautstaerke; neu = 0; }
        else                 { neu = lautVorStumm; }
      }
      neu = constrain(neu + a.lautAenderung, 0, 100);
      if (neu != lautstaerke && setzeLautstaerke(neu)) lautstaerke = neu;
      letzteLautAktion = millis();
      Serial.printf("Lautstärke jetzt %d %%\n", lautstaerke);
      anzeigeLautstaerke = lautstaerke;
      einblendungBis = millis() + EINBLENDUNG_MS;   // groß einblenden
    }
  }
}

// --- Aufträge eintragen (wird von loop() aufgerufen) ---
void auftragWiedergabe(Wiedergabe w, const String& uri, uint32_t nr) {
  xSemaphoreTake(auftragSperre, portMAX_DELAY);
  offeneAuftraege.wiedergabe = w;                   // der letzte Wunsch zählt
  offeneAuftraege.uri = uri;
  offeneAuftraege.platteNr = nr;
  xSemaphoreGive(auftragSperre);
}

void auftragSprung(int8_t richtung) {
  xSemaphoreTake(auftragSperre, portMAX_DELAY);
  int neu = offeneAuftraege.spruenge + richtung;
  offeneAuftraege.spruenge = constrain(neu, -5, 5);
  xSemaphoreGive(auftragSperre);
}

void auftragLautstaerke(int16_t aenderung) {
  xSemaphoreTake(auftragSperre, portMAX_DELAY);
  int neu = offeneAuftraege.lautAenderung + aenderung;
  offeneAuftraege.lautAenderung = constrain(neu, -100, 100);
  xSemaphoreGive(auftragSperre);
}

void auftragStumm() {
  xSemaphoreTake(auftragSperre, portMAX_DELAY);
  offeneAuftraege.stummUmschalten = !offeneAuftraege.stummUmschalten;
  xSemaphoreGive(auftragSperre);
}

// ===========================================================================
// Teil 3: NFC-Sticker lesen und Link auswerten
// ===========================================================================

// Liegt gerade ein Sticker im Feld? Der Weckbefehl (WUPA) findet auch
// Sticker, die wir nach dem Lesen schlafen gelegt haben.
bool stickerImFeld() {
  uint8_t antwort[2];
  uint8_t groesse = sizeof(antwort);
  MFRC522::StatusCode status = rfid.PICC_WakeupA(antwort, &groesse);
  return status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION;
}

// Liest die 144 Byte Nutzdaten (Seite 4 bis 39).
bool leseNutzdaten(uint8_t* ziel) {
  for (uint16_t offset = 0; offset < NUTZDATEN_LEN; offset += 16) {
    uint8_t puffer[18];                         // 16 Byte Daten + 2 Byte Prüfsumme
    uint8_t groesse = sizeof(puffer);
    uint8_t seite = ERSTE_SEITE + offset / 4;   // ein Lesebefehl liefert 4 Seiten
    if (rfid.MIFARE_Read(seite, puffer, &groesse) != MFRC522::STATUS_OK) return false;
    memcpy(ziel + offset, puffer, 16);
  }
  return true;
}

// Sucht in den Nutzdaten den ersten URL-Eintrag (NDEF) und gibt die Adresse zurück.
//   Ebene 1, TLV-Blöcke: [Typ][Länge][Inhalt]. 0x03 = NDEF, 0xFE = Ende, 0x00 = Füllbyte
//   Ebene 2, Datensätze: [Kopf][Typlänge][Inhaltslänge][Typ][Inhalt]. Typ 'U' = URL
String findeUrl(const uint8_t* daten, uint16_t laenge) {
  uint16_t i = 0;
  while (i < laenge) {
    uint8_t typ = daten[i];
    if (typ == 0x00) { i++; continue; }
    if (typ == 0xFE) return "";
    if (i + 1 >= laenge) return "";
    uint16_t blockLen = daten[i + 1];
    if (blockLen == 0xFF) return "";              // Langformat, kommt beim NTAG213 nicht vor
    uint16_t start = i + 2;
    if (start + blockLen > laenge) return "";
    if (typ != 0x03) { i = start + blockLen; continue; }

    uint16_t p = start;
    uint16_t ende = start + blockLen;
    while (p < ende) {
      uint8_t kopf = daten[p++];
      bool kurz    = kopf & 0x10;                 // Inhaltslänge passt in 1 Byte
      bool hatId   = kopf & 0x08;                 // Datensatz hat ein ID-Feld
      bool letzter = kopf & 0x40;                 // letzter Datensatz
      uint8_t tnf  = kopf & 0x07;                 // 1 = "bekannter Typ"

      if (p >= ende) return "";
      uint8_t typLen = daten[p++];
      uint32_t inhaltLen;
      if (kurz) {
        if (p >= ende) return "";
        inhaltLen = daten[p++];
      } else {
        if (p + 4 > ende) return "";
        inhaltLen = ((uint32_t)daten[p] << 24) | ((uint32_t)daten[p + 1] << 16) |
                    ((uint32_t)daten[p + 2] << 8) | daten[p + 3];
        p += 4;
      }
      uint8_t idLen = 0;
      if (hatId) {
        if (p >= ende) return "";
        idLen = daten[p++];
      }
      if ((uint32_t)p + typLen + idLen + inhaltLen > ende) return "";
      const uint8_t* typFeld = daten + p;
      const uint8_t* inhalt  = daten + p + typLen + idLen;

      if (tnf == 0x01 && typLen == 1 && typFeld[0] == 'U' && inhaltLen >= 1) {
        const char* anfang = "";                  // Kürzel für den Adressanfang
        switch (inhalt[0]) {
          case 0x01: anfang = "http://www.";  break;
          case 0x02: anfang = "https://www."; break;
          case 0x03: anfang = "http://";      break;
          case 0x04: anfang = "https://";     break;
        }
        String url = anfang;
        for (uint32_t k = 1; k < inhaltLen; k++) url += (char)inhalt[k];
        return url;
      }
      p += typLen + idLen + inhaltLen;
      if (letzter) break;
    }
    return "";
  }
  return "";
}

// Spotify-Link in Spotify-URI umwandeln.
//   https://open.spotify.com/album/ABC?si=xyz   ->   spotify:album:ABC
String linkZuUri(const String& link) {
  if (link.startsWith("spotify:")) return link;
  const char* marke = "open.spotify.com/";
  int pos = link.indexOf(marke);
  if (pos < 0) return "";
  String rest = link.substring(pos + strlen(marke));
  if (rest.startsWith("intl-")) {                 // Sprachkürzel wie "intl-de/" entfernen
    int schraeg = rest.indexOf('/');
    if (schraeg < 0) return "";
    rest = rest.substring(schraeg + 1);
  }
  int schraeg = rest.indexOf('/');
  if (schraeg < 0) return "";
  String art = rest.substring(0, schraeg);
  String id  = rest.substring(schraeg + 1);
  if (art != "album" && art != "playlist" && art != "track") return "";
  int ende = id.indexOf('?');
  if (ende >= 0) id = id.substring(0, ende);
  ende = id.indexOf('/');
  if (ende >= 0) id = id.substring(0, ende);
  if (id.length() == 0) return "";
  return "spotify:" + art + ":" + id;
}

// Liest den aufliegenden Sticker aus und gibt die Spotify-URI zurück.
String leseUriVomSticker(bool& leseFehler) {
  leseFehler = true;
  if (!rfid.PICC_ReadCardSerial()) return "";
  uint8_t daten[NUTZDATEN_LEN];
  bool ok = leseNutzdaten(daten);
  rfid.PICC_HaltA();                              // Sticker wieder schlafen legen
  if (!ok) return "";
  leseFehler = false;
  String url = findeUrl(daten, NUTZDATEN_LEN);
  if (url.length() == 0) return "";
  Serial.print("Link auf der Platte: ");
  Serial.println(url);
  return linkZuUri(url);
}

// ===========================================================================
// Teil 4: Eingänge (läuft in loop)
// ===========================================================================

// Entprellt einen Taster. Gibt genau einmal true zurück, wenn er neu gedrückt wird.
bool tasterGedrueckt(Taster& t) {
  bool rohwert = (digitalRead(t.pin) == LOW);     // LOW = gedrückt
  if (rohwert != t.letzterRohwert) {
    t.letzterRohwert = rohwert;
    t.letzteAenderung = millis();
  }
  if (millis() - t.letzteAenderung > TASTER_ENTPRELL_MS && rohwert != t.gedrueckt) {
    t.gedrueckt = rohwert;
    return t.gedrueckt;                           // nur beim Drücken, nicht beim Loslassen
  }
  return false;
}

// Interrupt für den Drehgeber. Läuft bei jedem Wechsel an CLK oder DT.
// Aus altem und neuem Zustand der beiden Leitungen ergibt die Tabelle, ob
// es einen Schritt vor (+1), zurück (-1) oder nichts (0, Störung) war.
void IRAM_ATTR drehgeberInterrupt() {
  static const int8_t TABELLE[16] = { 0, -1,  1,  0,
                                      1,  0,  0, -1,
                                     -1,  0,  0,  1,
                                      0,  1, -1,  0 };
  uint8_t ab = (digitalRead(PIN_DREH_CLK) << 1) | digitalRead(PIN_DREH_DT);
  portENTER_CRITICAL_ISR(&drehSperre);
  drehZustand = ((drehZustand << 2) | ab) & 0x0F;
  drehZaehler += TABELLE[drehZustand];
  portEXIT_CRITICAL_ISR(&drehSperre);
}

void pruefeDrehgeber() {
  // Volle Rasten aus dem Zähler nehmen, angefangene Rasten bleiben stehen.
  portENTER_CRITICAL(&drehSperre);
  int32_t rasten = drehZaehler / DREH_WECHSEL_PRO_RASTE;
  drehZaehler -= rasten * DREH_WECHSEL_PRO_RASTE;
  portEXIT_CRITICAL(&drehSperre);

  if (rasten != 0) {
    if (DREH_UMKEHREN) rasten = -rasten;
    Serial.printf("Drehgeber: %+ld\n", (long)rasten);
    auftragLautstaerke(rasten * LAUTSTAERKE_SCHRITT);
  }
  if (tasterGedrueckt(tasterDrehKnopf)) {
    Serial.println("Drehgeber gedrückt: stumm an/aus");
    auftragStumm();
  }
}

void pruefeTaster() {
  // Springen nur, wenn die aufliegende Platte schon gestartet wurde. Sonst
  // würde man in irgendeiner alten Wiedergabe springen.
  bool darfSpringen = platteLiegtAuf && gestartetePlatteNr == platteNr;
  if (tasterGedrueckt(tasterWeiter)) {
    if (darfSpringen) auftragSprung(+1);
    else Serial.println("Weiter ignoriert: Platte noch nicht gestartet.");
  }
  if (tasterGedrueckt(tasterZurueck)) {
    if (darfSpringen) auftragSprung(-1);
    else Serial.println("Zurück ignoriert: Platte noch nicht gestartet.");
  }
}

void pruefeTonarm() {
  tasterGedrueckt(tasterTonarm);                  // aktualisiert den entprellten Zustand
  bool unten = (tasterTonarm.gedrueckt == TONARM_GEDRUECKT_HEISST_UNTEN);
  if (unten != armUnten) {
    armUnten = unten;
    Serial.println(armUnten ? "Tonarm unten." : "Tonarm oben.");
  }
}

void pruefePlatte() {
  if (stickerImFeld()) {
    fehlversuche = 0;
    if (!platteGelesen) {
      bool leseFehler;
      String uri = leseUriVomSticker(leseFehler);
      if (leseFehler) {
        // Sticker ist im Feld, aber Auslesen hat nicht geklappt. Höchstens
        // jede Sekunde melden, damit der Monitor nicht vollläuft.
        static uint32_t letzteMeldung = 0;
        if (millis() - letzteMeldung > 1000) {
          letzteMeldung = millis();
          Serial.println("Sticker erkannt, Auslesen fehlgeschlagen. Neuer Versuch...");
        }
      }
      if (!leseFehler) {                          // bei Lesefehler: nächster Durchlauf versucht es neu
        platteGelesen  = true;
        platteLiegtAuf = true;
        platteNr++;                               // neues Auflegen
        aktuelleUri = uri;
        Serial.println("Platte erkannt.");
        if (uri.length() == 0) Serial.println("Platte ohne Spotify-Link.");
        else if (!armUnten)    Serial.println("Platte erkannt. Tonarm auflegen zum Starten.");
      }
    } else {
      rfid.PICC_HaltA();                          // liegt weiter auf, wieder schlafen legen
    }
  } else {
    if (fehlversuche < 255) fehlversuche++;
    if (platteLiegtAuf && fehlversuche >= FEHLVERSUCHE_BIS_WEG) {
      platteLiegtAuf = false;
      platteGelesen  = false;
      aktuelleUri    = "";
      Serial.println("Platte weg.");
      setzeTitel("", "", "");
    } else if (!platteLiegtAuf) {
      platteGelesen = false;
    }
  }
}

// Entscheidet aus Platte und Tonarm, ob Musik laufen soll.
void aktualisiereWiedergabe() {
  bool soll = platteLiegtAuf && aktuelleUri.length() > 0 && armUnten;
  sollSpielen = soll;
  motorSollDrehen = soll;                         // Teller reagiert sofort
  if (soll == zuletztSoll) return;
  zuletztSoll = soll;

  if (soll) {
    if (gestartetePlatteNr == platteNr) {
      auftragWiedergabe(W_WEITER, "", platteNr);  // Platte lief schon: fortsetzen
    } else {
      auftragWiedergabe(W_START, aktuelleUri, platteNr);   // neu aufgelegt: von vorne
    }
  } else {
    auftragWiedergabe(W_PAUSE, "", platteNr);
  }
}

// ===========================================================================
// Teil 4b: Titelinfos und Display
// ===========================================================================

// Fragt bei Spotify ab, welcher Titel gerade läuft, und gibt ihn ans Display.
void holeTitel() {
  if (!tokenBereit()) return;
  String antwort;
  anfrageProtokoll = false;                       // alle 5 s wäre zu viel im Monitor
  int status = apiAnfrage("GET", "/me/player/currently-playing", "", antwort);
  if (status == 401 && erneuereToken()) {
    status = apiAnfrage("GET", "/me/player/currently-playing", "", antwort);
  }
  anfrageProtokoll = true;
  if (status != 200) return;                      // 204 = gerade läuft nichts

  // Die Antwort ist groß. Mit dem Filter behält ArduinoJson nur die drei
  // Felder, die wir brauchen. Das spart viel Speicher.
  JsonDocument filter;
  filter["item"]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["artists"][0]["name"] = true;

  JsonDocument doc;
  DeserializationError fehler = deserializeJson(doc, antwort, DeserializationOption::Filter(filter));
  if (fehler) {
    Serial.printf("Titelinfo nicht lesbar: %s\n", fehler.c_str());
    return;
  }
  const char* titel     = doc["item"]["name"] | "";
  const char* interpret = doc["item"]["artists"][0]["name"] | "";
  const char* album     = doc["item"]["album"]["name"] | "";

  // Nur bei einem Wechsel im Monitor melden.
  xSemaphoreTake(anzeigeSperre, portMAX_DELAY);
  bool neu = (anzeigeTitel != titel);
  xSemaphoreGive(anzeigeSperre);
  if (neu && strlen(titel) > 0) Serial.printf("Läuft: %s - %s\n", interpret, titel);
  setzeTitel(titel, interpret, album);
}

void setzeTitel(const String& titel, const String& interpret, const String& album) {
  xSemaphoreTake(anzeigeSperre, portMAX_DELAY);
  anzeigeTitel = titel;
  anzeigeInterpret = interpret;
  anzeigeAlbum = album;
  xSemaphoreGive(anzeigeSperre);
}

void zeigeHinweis(const String& text) {
  xSemaphoreTake(anzeigeSperre, portMAX_DELAY);
  anzeigeHinweis = text;
  xSemaphoreGive(anzeigeSperre);
  hinweisBis = millis() + HINWEIS_MS;
}

// Spotify liefert Text als UTF-8. Die eingebaute Schrift des Displays kennt
// stattdessen den alten Zeichensatz CP437. Wir übersetzen die gängigen
// Sonderzeichen, alle anderen werden zu '?'.
String fuerDisplay(const String& text) {
  String r;
  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];
    if (c < 0x80) { r += (char)c; continue; }
    if (c == 0xC3 && i + 1 < text.length()) {
      uint8_t d = (uint8_t)text[++i];
      uint8_t z = '?';
      switch (d) {
        case 0xA4: z = 0x84; break;   // ä
        case 0xB6: z = 0x94; break;   // ö
        case 0xBC: z = 0x81; break;   // ü
        case 0x84: z = 0x8E; break;   // Ä
        case 0x96: z = 0x99; break;   // Ö
        case 0x9C: z = 0x9A; break;   // Ü
        case 0x9F: z = 0xE1; break;   // ß
        case 0xA9: z = 0x82; break;   // é
        case 0xA8: z = 0x8A; break;   // è
        case 0xA1: z = 0xA0; break;   // á
        case 0xB1: z = 0xA4; break;   // ñ
        case 0xA7: z = 0x87; break;   // ç
      }
      r += (char)z;
      continue;
    }
    // anderes Sonderzeichen aus mehreren Byte: durch '?' ersetzen
    r += '?';
    while (i + 1 < text.length() && (((uint8_t)text[i + 1]) & 0xC0) == 0x80) i++;
  }
  return r;
}

// Schreibt eine Textzeile. Passt sie auf das Display, steht sie still.
// Ist sie zu lang, läuft sie langsam von rechts nach links durch.
//   groesse 1: Zeichen 6 x 8 Pixel, 21 Zeichen pro Zeile
//   groesse 2: Zeichen 12 x 16 Pixel, 10 Zeichen pro Zeile
void zeichneZeile(int16_t y, const String& text, uint32_t jetzt, uint8_t groesse) {
  display.setTextSize(groesse);
  int16_t zeichenBreite = 6 * groesse;
  int16_t breite = text.length() * zeichenBreite;
  if (breite <= 128) {
    display.setCursor(0, y);
    display.print(text);
    display.setTextSize(1);
    return;
  }
  String lauf = text + "    ";                    // Abstand vor der Wiederholung
  int16_t gesamt = lauf.length() * zeichenBreite;
  // Erst 2 Sekunden stehen lassen, dann mit 25 Pixel pro Sekunde laufen.
  uint32_t t = jetzt % (2000 + (uint32_t)gesamt * 40);
  int16_t versatz = (t < 2000) ? 0 : (int16_t)((t - 2000) / 40);
  display.setCursor(-versatz, y);
  display.print(lauf);
  display.print(lauf);                            // zweite Kopie schließt die Lücke
  display.setTextSize(1);
}

void zeichneAnzeige() {
  uint32_t jetzt = millis();

  xSemaphoreTake(anzeigeSperre, portMAX_DELAY);
  String titel     = fuerDisplay(anzeigeTitel);
  String interpret = fuerDisplay(anzeigeInterpret);
  String album     = fuerDisplay(anzeigeAlbum);
  String hinweis   = fuerDisplay(anzeigeHinweis);
  xSemaphoreGive(anzeigeSperre);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 1. Lautstärke gerade geändert: groß einblenden mit Balken
  if ((int32_t)(einblendungBis - jetzt) > 0) {
    int laut = anzeigeLautstaerke;
    display.setTextSize(2);                       // doppelt so groß: 12 x 16 Pixel
    display.setCursor(0, 0);
    if (laut == 0) display.print("Stumm");
    else           display.printf("Laut %d%%", laut);
    display.drawRect(0, 22, 128, 10, SSD1306_WHITE);
    if (laut > 0) display.fillRect(2, 24, (124 * laut) / 100, 6, SSD1306_WHITE);
    display.display();
    return;
  }

  // 2. Hinweis, z. B. "Echo nicht erreichbar"
  if ((int32_t)(hinweisBis - jetzt) > 0 && hinweis.length() > 0) {
    display.setCursor(0, 0);
    display.print("Hinweis");
    zeichneZeile(12, hinweis, jetzt, 1);
    display.display();
    return;
  }

  // 3. Normale Anzeige je nach Zustand, alles in großer Schrift außer
  //    Album und Interpret.
  bool gestartet = platteLiegtAuf && gestartetePlatteNr == platteNr;
  if (!platteLiegtAuf) {
    zeichneZeile(0,  "Platte", jetzt, 2);
    zeichneZeile(16, "auflegen", jetzt, 2);
  } else if (!gestartet && !armUnten) {
    zeichneZeile(0,  "Tonarm", jetzt, 2);
    zeichneZeile(16, "runter", jetzt, 2);
  } else if (titel.length() == 0) {
    zeichneZeile(0, armUnten ? "Starte..." : "Pause", jetzt, 2);
  } else {
    // Oben der Titel groß (16 Pixel hoch)
    zeichneZeile(0, titel, jetzt, 2);
    if (anzeigeLautstaerke == 0) {
      // Stumm: statt Album und Interpret groß "Stumm"
      zeichneZeile(16, "Stumm", jetzt, 2);
    } else {
      // Darunter Album und Interpret klein (je 8 Pixel hoch)
      zeichneZeile(16, album, jetzt, 1);
      zeichneZeile(24, interpret, jetzt, 1);
    }
  }
  display.display();
}

// Einfache Startanzeige, solange die Tasks noch nicht laufen.
void zeigeStart(const char* zeile1, const char* zeile2) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(zeile1);
  display.setCursor(0, 12);
  display.print(zeile2);
  display.display();
}

// Eigene Aufgabe: zeichnet das Display etwa 20-mal pro Sekunde neu.
void anzeigeTask(void* parameter) {
  for (;;) {
    zeichneAnzeige();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ===========================================================================
// Teil 5: Schrittmotor (eigene Aufgabe)
// ===========================================================================
void motorTask(void* parameter) {
  // Halbschritt-Folge, je ein Bit pro Leitung, von links IN1 bis IN4.
  const uint8_t MUSTER[8] = { 0b1000, 0b1100, 0b0100, 0b0110,
                              0b0010, 0b0011, 0b0001, 0b1001 };
  uint8_t schritt = 0;
  bool spulenAn = false;
  TickType_t letzterSchritt = xTaskGetTickCount();

  for (;;) {
    if (motorSollDrehen) {
      for (uint8_t i = 0; i < 4; i++) {
        digitalWrite(PIN_MOTOR[i], (MUSTER[schritt] >> (3 - i)) & 1);
      }
      schritt = (schritt + (MOTOR_RUECKWAERTS ? 7 : 1)) & 7;
      spulenAn = true;
      vTaskDelayUntil(&letzterSchritt, pdMS_TO_TICKS(MOTOR_SCHRITT_MS));
    } else {
      if (spulenAn) {                             // im Stand Spulen stromlos schalten
        for (uint8_t i = 0; i < 4; i++) digitalWrite(PIN_MOTOR[i], LOW);
        spulenAn = false;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      letzterSchritt = xTaskGetTickCount();
    }
  }
}

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(2000);   // Zeit für den Seriellen Monitor, sonst gehen die ersten Zeilen verloren
  Serial.println();
  Serial.println("=== RFID-Plattenspieler, Version 2 ===");

  // --- Display ---
  anzeigeSperre = xSemaphoreCreateMutex();
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);                          // schneller I2C, flüssigere Anzeige
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADRESSE);
  if (displayOk) {
    display.cp437(true);                          // richtige Zeichentabelle für Umlaute
    display.setTextWrap(false);                   // lange Zeilen abschneiden statt umbrechen
    zeigeStart("Plattenspieler", "Starte...");
  } else {
    Serial.println("Display nicht gefunden. Verkabelung prüfen (SDA 21, SCL 4).");
  }

  // --- Motor ---
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(PIN_MOTOR[i], OUTPUT);
    digitalWrite(PIN_MOTOR[i], LOW);
  }

  // --- Taster, Tonarm, Drehgeber ---
  pinMode(PIN_WEITER,  INPUT_PULLUP);
  pinMode(PIN_ZURUECK, INPUT_PULLUP);
  pinMode(PIN_TONARM,  INPUT_PULLUP);
  pinMode(PIN_DREH_SW, INPUT_PULLUP);
  pinMode(PIN_DREH_CLK, INPUT);                   // Pull-ups sitzen auf der KY-040-Platine
  pinMode(PIN_DREH_DT,  INPUT);
  drehZustand = (digitalRead(PIN_DREH_CLK) << 1) | digitalRead(PIN_DREH_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_DREH_CLK), drehgeberInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_DREH_DT),  drehgeberInterrupt, CHANGE);

  // --- RFID-Leser ---
  SPI.begin();
  rfid.PCD_Init();
  delay(50);
  uint8_t version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("FEHLER: RC522 antwortet nicht, Verkabelung prüfen.");
  } else {
    Serial.printf("RC522 erkannt (Version 0x%02X).\n", version);
  }

  // --- Refresh Token aus dem Flash laden ---
  // Beim ersten Start kommt er aus secrets.h, danach aus dem Flash, weil
  // Spotify ihn zwischendurch austauschen kann.
  speicher.begin("spotify", false);
  String startwert = speicher.getString("startwert", "");
  if (startwert != SPOTIFY_REFRESH_TOKEN) {
    refreshToken = SPOTIFY_REFRESH_TOKEN;
    speicher.putString("startwert", SPOTIFY_REFRESH_TOKEN);
    speicher.putString("refresh", refreshToken);
    Serial.println("Refresh Token aus secrets.h übernommen.");
  } else {
    refreshToken = speicher.getString("refresh", SPOTIFY_REFRESH_TOKEN);
  }

  // --- WLAN und erster Token ---
  zeigeStart("Verbinde WLAN...", WIFI_SSID);
  verbindeWlan();
  zeigeStart("WLAN ok", "Anmeldung Spotify...");
  if (!erneuereToken()) {
    Serial.println("Spotify-Anmeldung fehlgeschlagen. Stimmen Client-ID und Refresh Token?");
    zeigeStart("Spotify-Anmeldung", "fehlgeschlagen");
    delay(3000);
  }

  // --- Aufgaben starten ---
  auftragSperre = xSemaphoreCreateMutex();
  // Spotify auf Kern 0 (dort läuft auch das WLAN), großer Stapel für die
  // Verschlüsselung. Motor auf Kern 1 mit höherer Priorität als loop().
  xTaskCreatePinnedToCore(spotifyTask, "spotify", 12288, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(motorTask,   "motor",   2048,  NULL, 2, NULL, 1);
  // Display auf Kern 1 mit derselben Priorität wie loop(). Größerer Stapel
  // wegen der Texte.
  if (displayOk) {
    xTaskCreatePinnedToCore(anzeigeTask, "anzeige", 4096, NULL, 1, NULL, 1);
  }

  uint8_t v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522-Version jetzt: 0x%02X%s\n", v,
                (v == 0x00 || v == 0xFF) ? "  (FEHLER: Leser antwortet nicht)" : "  (ok)");
  Serial.println("Bereit. Platte auflegen und Tonarm runter.");
}

// ===========================================================================
// loop(): liest die Eingänge, wartet nie auf das Internet
// ===========================================================================
void loop() {
  pruefeTaster();
  pruefeDrehgeber();
  pruefeTonarm();

  if (millis() - letzteAbfrage >= ABFRAGE_INTERVALL_MS) {
    letzteAbfrage = millis();
    pruefePlatte();
  }

  aktualisiereWiedergabe();
  delay(2);                                       // anderen Aufgaben Zeit lassen
}
