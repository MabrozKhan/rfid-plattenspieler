/*
  RFID-Plattenspieler, Phase 3: RFID und Spotify zusammen

  Ablauf:
    Platte (NFC-Sticker) auflegen  ->  Link auslesen  ->  Album/Playlist startet
    Platte wegnehmen               ->  Pause
    Platte wieder auflegen         ->  startet immer von vorne, beim ersten Titel
    Poti drehen                    ->  Lautstärke
    Taster "Weiter" / "Zurück"     ->  nächster / vorheriger Titel

  Board in der Arduino IDE:  ESP32 Dev Module
  Bibliothek:                MFRC522 (von GithubCommunity / miguelbalboa)
  Serieller Monitor:         115200 Baud

  Zugangsdaten stehen in secrets.h (liegt neben dieser Datei, wird nicht
  hochgeladen). Vorlage: secrets.example.h

  Verkabelung RC522 -> ESP32:
    SDA 5, SCK 18, MOSI 23, MISO 19, RST 22, GND, 3.3V an 3V3 (niemals 5 V)

  Bedienelemente:
    Poti (B5K):     linker Pin an GND, mittlerer Pin an GPIO 34, rechter Pin an 3V3
                    (niemals an 5 V, die Eingänge des ESP32 vertragen nur 3,3 V)
    Taster Weiter:  zwischen GPIO 32 und GND
    Taster Zurück:  zwischen GPIO 33 und GND
*/

#include <SPI.h>               // Bus zum RC522
#include <MFRC522.h>           // RFID-Leser
#include <WiFi.h>              // WLAN
#include <WiFiClientSecure.h>  // verschlüsselte Verbindung (HTTPS)
#include <HTTPClient.h>        // HTTP-Anfragen
#include <Preferences.h>       // dauerhafter Speicher im Flash des ESP32

#include "secrets.h"           // WLAN, Client-ID, Refresh Token, Geräte-ID

// ---------------------------------------------------------------------------
// Einstellungen
// ---------------------------------------------------------------------------
const uint8_t PIN_SS  = 5;     // am RC522 mit SDA beschriftet
const uint8_t PIN_RST = 22;

// Wie oft nach einer Platte geschaut wird (in Millisekunden).
const uint32_t ABFRAGE_INTERVALL_MS = 250;

// Entprellen: Erst wenn die Platte so oft hintereinander NICHT gefunden wurde,
// gilt sie als weggenommen. 4 x 250 ms = etwa eine Sekunde. Ohne das würde
// ein einzelner Lesefehler beim Drehen sofort die Musik anhalten.
const uint8_t FEHLVERSUCHE_BIS_WEG = 4;

// NTAG213: Die Nutzdaten liegen auf Seite 4 bis 39, das sind 36 x 4 = 144 Byte.
const uint8_t  ERSTE_SEITE   = 4;
const uint16_t NUTZDATEN_LEN = 144;

// --- Bedienelemente ---
// Das Poti muss an einen Pin von ADC1 (GPIO 32 bis 39). Die Pins von ADC2
// funktionieren nicht, solange das WLAN läuft.
const uint8_t PIN_POTI   = 34;
// Die Taster schalten gegen GND. Der ESP32 zieht den Pin intern auf 3,3 V
// (INPUT_PULLUP), gedrückt liegt er auf 0 V. So braucht man keinen Widerstand.
// GPIO 34 bis 39 haben keinen internen Pull-up, deshalb 32 und 33.
const uint8_t PIN_WEITER = 32;
const uint8_t PIN_ZURUECK = 33;

// Lautstärke erst senden, wenn sie sich um mindestens so viele Prozentpunkte
// geändert hat. Der Analogeingang des ESP32 rauscht, ohne diese Schwelle
// würden ständig Befehle rausgehen.
const uint8_t  LAUTSTAERKE_SCHWELLE   = 3;
// Höchstens so oft einen Lautstärkebefehl senden (Spotify begrenzt die Anzahl
// der Anfragen, und jede Anfrage blockiert das Programm kurz).
const uint32_t LAUTSTAERKE_ABSTAND_MS = 400;
// Entprellzeit der Taster: Mechanische Kontakte "prellen" beim Drücken und
// liefern für einige Millisekunden mehrere Flanken.
const uint32_t TASTER_ENTPRELL_MS     = 50;

// ---------------------------------------------------------------------------
// Globale Objekte und Zustände
// ---------------------------------------------------------------------------
MFRC522 rfid(PIN_SS, PIN_RST);
Preferences speicher;

String   accessToken;              // kurzlebig, gilt etwa eine Stunde
String   refreshToken;             // langlebig, damit holen wir neue Access Tokens
uint32_t tokenGueltigBis = 0;      // Zeitpunkt (millis), ab dem erneuert wird

bool     platteLiegtAuf  = false;  // aktueller Zustand
bool     platteGelesen   = false;  // wurde der Link dieser Platte schon gelesen?
uint8_t  fehlversuche    = 0;      // Zähler fürs Entprellen
uint32_t letzteAbfrage   = 0;

float    potiGeglaettet     = -1;  // geglätteter Messwert, -1 = noch keiner
int      lautstaerkeGesendet = -1; // zuletzt gesendeter Wert in Prozent
uint32_t letzteLautstaerke  = 0;   // Zeitpunkt des letzten Lautstärkebefehls
uint32_t letztePotiMessung  = 0;

// Zustand eines Tasters fürs Entprellen
struct Taster {
  uint8_t  pin;
  bool     gedrueckt;       // entprellter Zustand
  bool     letzterRohwert;  // letzter direkt gelesener Wert
  uint32_t letzteAenderung; // wann sich der Rohwert zuletzt geändert hat
};
Taster tasterWeiter  = { PIN_WEITER,  false, false, 0 };
Taster tasterZurueck = { PIN_ZURUECK, false, false, 0 };

// ===========================================================================
// Teil 1: WLAN
// ===========================================================================
void verbindeWlan() {
  Serial.print("Verbinde mit WLAN ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);             // normaler Teilnehmer, kein eigener Hotspot
  WiFi.setAutoReconnect(true);     // bei Abbruch selbst neu verbinden
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.print(" verbunden, IP: ");
  Serial.println(WiFi.localIP());
}

// ===========================================================================
// Teil 2: Spotify
// ===========================================================================

// Holt einen Wert aus einer JSON-Antwort, z. B. "access_token".
// Das ist bewusst einfach gehalten und ersetzt keine echte JSON-Bibliothek,
// reicht aber für die flachen Antworten des Token-Servers.
String jsonWert(const String& json, const char* schluessel) {
  String suche = String('"') + schluessel + '"';
  int pos = json.indexOf(suche);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos + suche.length());
  if (pos < 0) return "";
  pos++;
  while (pos < (int)json.length() && json[pos] == ' ') pos++;   // Leerzeichen überspringen

  if (json[pos] == '"') {                       // Text in Anführungszeichen
    int ende = json.indexOf('"', pos + 1);
    return json.substring(pos + 1, ende);
  }
  int ende = pos;                               // Zahl: bis Komma oder Klammer
  while (ende < (int)json.length() && json[ende] != ',' && json[ende] != '}') ende++;
  return json.substring(pos, ende);
}

// Schickt eine HTTPS-Anfrage. Gibt den Statuscode zurück, die Antwort landet
// in "antwort". Negative Codes bedeuten: Verbindung kam nicht zustande.
int sendeHttps(const char* methode, const String& url, const String& koerper,
               const char* contentType, bool mitToken, String& antwort) {
  WiFiClientSecure client;
  // Vereinfachung für den Prototyp: Das Zertifikat von Spotify wird nicht
  // geprüft. Die Verbindung ist trotzdem verschlüsselt. Für die fertige
  // Version tragen wir hier das Root-Zertifikat ein.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  // Verbindung nach jeder Anfrage schließen ("Connection: close"). Sonst hält
  // Spotify sie offen, und das Lesen der Antwort wartet endlos auf weitere
  // Daten. Dann bleibt das ganze Programm stehen und scannt nicht mehr.
  http.setReuse(false);
  if (!http.begin(client, url)) return -1;

  http.addHeader("Content-Type", contentType);
  if (mitToken) {
    http.addHeader("Authorization", "Bearer " + accessToken);
  }

  int status = http.sendRequest(methode, koerper);
  // Den Antworttext nur lesen, wenn es einen gibt. Bei Erfolg schickt Spotify
  // für play und pause meist gar keinen Inhalt (Status 204 oder Länge 0).
  antwort = "";
  if (status > 0 && status != 204 && http.getSize() != 0) {
    antwort = http.getString();
  }
  http.end();
  return status;
}

// Holt mit dem Refresh Token einen neuen Access Token.
bool erneuereToken() {
  String koerper = "grant_type=refresh_token&refresh_token=" + refreshToken +
                   "&client_id=" + SPOTIFY_CLIENT_ID;
  String antwort;
  int status = sendeHttps("POST", "https://accounts.spotify.com/api/token", koerper,
                          "application/x-www-form-urlencoded", false, antwort);
  if (status != 200) {
    Serial.printf("Token erneuern fehlgeschlagen (%d): %s\n", status, antwort.c_str());
    return false;
  }

  accessToken = jsonWert(antwort, "access_token");
  long sekunden = jsonWert(antwort, "expires_in").toInt();
  if (sekunden <= 0) sekunden = 3600;
  // Eine Minute früher erneuern, damit er nie mitten in einer Anfrage abläuft.
  tokenGueltigBis = millis() + (uint32_t)(sekunden - 60) * 1000UL;

  // Spotify kann bei jeder Erneuerung einen NEUEN Refresh Token mitschicken.
  // Dann gilt der alte nicht mehr sicher. Deshalb speichern wir den neuen
  // dauerhaft im Flash, sonst wäre nach dem nächsten Neustart Schluss.
  String neu = jsonWert(antwort, "refresh_token");
  if (neu.length() > 0 && neu != refreshToken) {
    refreshToken = neu;
    speicher.putString("refresh", refreshToken);
  }
  Serial.println("Access Token erneuert.");
  return true;
}

// Stellt sicher, dass ein gültiger Access Token vorhanden ist.
bool tokenBereit() {
  // (int32_t) macht den Vergleich sicher, auch wenn millis() nach 49 Tagen überläuft.
  if (accessToken.length() > 0 && (int32_t)(tokenGueltigBis - millis()) > 0) {
    return true;
  }
  return erneuereToken();
}

// Schickt einen Befehl an den Spotify-Player.
//   methode    "PUT" oder "POST", je nach Befehl
//   befehl     play, pause, volume, next oder previous
//   parameter  zusätzliche Angaben für die Adresse, z. B. "&volume_percent=40"
//   koerper    Inhalt der Anfrage als JSON
bool playerBefehl(const char* methode, const char* befehl,
                  const String& parameter, const String& koerper) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Kein WLAN, Befehl nicht gesendet.");
    return false;
  }
  if (!tokenBereit()) return false;

  String url = String("https://api.spotify.com/v1/me/player/") + befehl +
               "?device_id=" + SPOTIFY_DEVICE_ID + parameter;
  String antwort;
  int status = sendeHttps(methode, url, koerper, "application/json", true, antwort);

  // 401 heißt: Token doch schon abgelaufen. Einmal erneuern und wiederholen.
  if (status == 401 && erneuereToken()) {
    status = sendeHttps(methode, url, koerper, "application/json", true, antwort);
  }

  if (status == 200 || status == 202 || status == 204) {
    Serial.printf("Spotify: %s ausgeführt (Status %d).\n", befehl, status);
    return true;
  }

  Serial.printf("Spotify-Fehler bei %s (%d): %s\n", befehl, status, antwort.c_str());
  if (status == 404) {
    Serial.println("Hinweis: Gerät nicht gefunden. Ist der Echo an und die Geräte-ID richtig?");
  }
  return false;
}

// Startet ein Album, eine Playlist oder einen Titel, und zwar immer von vorne.
// Wie bei einer echten Platte: Auflegen heißt, es beginnt mit dem ersten Lied.
bool spiele(const String& uri) {
  String koerper;
  if (uri.startsWith("spotify:track:")) {
    // Einzelner Titel, ab Sekunde 0.
    koerper = "{\"uris\":[\"" + uri + "\"],\"position_ms\":0}";
  } else {
    // Album oder Playlist.
    //   offset position 0  = mit dem ersten Titel beginnen
    //   position_ms 0      = diesen Titel ab Sekunde 0 spielen
    // Ohne diese Angaben macht Spotify manchmal dort weiter, wo man in
    // diesem Album zuletzt aufgehört hat.
    koerper = "{\"context_uri\":\"" + uri +
              "\",\"offset\":{\"position\":0},\"position_ms\":0}";
  }
  return playerBefehl("PUT", "play", "", koerper);
}

// Zufallswiedergabe ausschalten, damit die Titel in der richtigen Reihenfolge
// kommen. Sonst wäre nach dem ersten Lied Schluss mit der Plattenreihenfolge.
bool zufallAus() { return playerBefehl("PUT", "shuffle", "&state=false", "{}"); }

bool pausiere() { return playerBefehl("PUT", "pause", "", "{}"); }

// Lautstärke in Prozent (0 bis 100) setzen.
bool setzeLautstaerke(int prozent) {
  return playerBefehl("PUT", "volume", "&volume_percent=" + String(prozent), "{}");
}

// Nächster und vorheriger Titel. Diese beiden Befehle erwartet Spotify als POST.
bool naechsterTitel()  { return playerBefehl("POST", "next", "", "{}"); }
bool vorherigerTitel() { return playerBefehl("POST", "previous", "", "{}"); }

// ===========================================================================
// Teil 3: NFC-Sticker lesen und Link auswerten
// ===========================================================================

// Prüft, ob gerade ein Sticker im Feld liegt.
// Wir benutzen den Weckbefehl (WUPA) statt PICC_IsNewCardPresent(). Der Grund:
// Nach dem Lesen legen wir den Sticker schlafen. IsNewCardPresent() findet nur
// wache Sticker, der Weckbefehl findet auch schlafende. So merken wir
// zuverlässig, ob die Platte NOCH da ist.
bool stickerImFeld() {
  uint8_t antwort[2];
  uint8_t groesse = sizeof(antwort);
  MFRC522::StatusCode status = rfid.PICC_WakeupA(antwort, &groesse);
  return status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION;
}

// Liest die 144 Byte Nutzdaten (Seite 4 bis 39) in den Puffer.
bool leseNutzdaten(uint8_t* ziel) {
  for (uint16_t offset = 0; offset < NUTZDATEN_LEN; offset += 16) {
    uint8_t puffer[18];                    // 16 Byte Daten + 2 Byte Prüfsumme
    uint8_t groesse = sizeof(puffer);
    uint8_t seite = ERSTE_SEITE + offset / 4;   // ein Lesebefehl liefert 4 Seiten
    if (rfid.MIFARE_Read(seite, puffer, &groesse) != MFRC522::STATUS_OK) {
      return false;
    }
    memcpy(ziel + offset, puffer, 16);
  }
  return true;
}

// Sucht in den Nutzdaten den ersten URL-Eintrag und gibt die Adresse zurück.
//
// Aufbau der Daten (zwei Ebenen):
//   1. TLV-Blöcke: [Typ][Länge][Inhalt]. Typ 0x03 enthält die NDEF-Nachricht,
//      0xFE ist das Ende, 0x00 ist Füllmaterial.
//   2. In der NDEF-Nachricht stehen Datensätze: [Kopf][Typlänge][Inhaltslänge]
//      [Typ][Inhalt]. Typ 'U' bedeutet URL. Das erste Byte des Inhalts ist ein
//      Kürzel für den Anfang der Adresse (0x04 = "https://").
String findeUrl(const uint8_t* daten, uint16_t laenge) {
  uint16_t i = 0;

  // --- Ebene 1: den NDEF-Block (Typ 0x03) suchen ---
  while (i < laenge) {
    uint8_t typ = daten[i];
    if (typ == 0x00) { i++; continue; }          // Füllbyte
    if (typ == 0xFE) return "";                  // Ende erreicht, nichts gefunden
    if (i + 1 >= laenge) return "";
    uint16_t blockLen = daten[i + 1];
    if (blockLen == 0xFF) return "";             // Langformat, kommt beim NTAG213 nicht vor
    uint16_t start = i + 2;
    if (start + blockLen > laenge) return "";    // Längenangabe passt nicht zum Speicher

    if (typ != 0x03) { i = start + blockLen; continue; }   // anderer Block: überspringen

    // --- Ebene 2: Datensätze in der NDEF-Nachricht durchgehen ---
    uint16_t p = start;
    uint16_t ende = start + blockLen;
    while (p < ende) {
      uint8_t kopf = daten[p++];
      bool kurz    = kopf & 0x10;                // SR: Inhaltslänge passt in 1 Byte
      bool hatId   = kopf & 0x08;                // IL: Datensatz hat ein ID-Feld
      bool letzter = kopf & 0x40;                // ME: letzter Datensatz
      uint8_t tnf  = kopf & 0x07;                // 1 = "bekannter Typ"

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

      // Ist das ein URL-Datensatz?
      if (tnf == 0x01 && typLen == 1 && typFeld[0] == 'U' && inhaltLen >= 1) {
        // Kürzel für den Adressanfang, spart Platz auf dem Sticker.
        const char* anfang = "";
        switch (inhalt[0]) {
          case 0x01: anfang = "http://www.";  break;
          case 0x02: anfang = "https://www."; break;
          case 0x03: anfang = "http://";      break;
          case 0x04: anfang = "https://";     break;
        }
        String url = anfang;
        // Wir nehmen genau "inhaltLen" Zeichen und nicht mehr. Deshalb landet
        // das Kopfbyte eines folgenden Datensatzes nicht versehentlich im Link.
        for (uint32_t k = 1; k < inhaltLen; k++) {
          url += (char)inhalt[k];
        }
        return url;
      }

      p += typLen + idLen + inhaltLen;           // zum nächsten Datensatz
      if (letzter) break;
    }
    return "";                                   // NDEF-Block ohne URL
  }
  return "";
}

// Wandelt einen Spotify-Link in eine Spotify-URI um.
//   https://open.spotify.com/album/ABC?si=xyz   ->   spotify:album:ABC
String linkZuUri(const String& link) {
  if (link.startsWith("spotify:")) return link;  // ist schon eine URI

  const char* marke = "open.spotify.com/";
  int pos = link.indexOf(marke);
  if (pos < 0) return "";
  String rest = link.substring(pos + strlen(marke));

  // Manche Links enthalten ein Sprachkürzel, z. B. "intl-de/". Das entfernen wir.
  if (rest.startsWith("intl-")) {
    int schraeg = rest.indexOf('/');
    if (schraeg < 0) return "";
    rest = rest.substring(schraeg + 1);
  }

  int schraeg = rest.indexOf('/');
  if (schraeg < 0) return "";
  String art = rest.substring(0, schraeg);       // album, playlist oder track
  String id  = rest.substring(schraeg + 1);

  if (art != "album" && art != "playlist" && art != "track") return "";

  // Alles ab '?' oder '/' abschneiden (z. B. "?si=...").
  int ende = id.indexOf('?');
  if (ende >= 0) id = id.substring(0, ende);
  ende = id.indexOf('/');
  if (ende >= 0) id = id.substring(0, ende);
  if (id.length() == 0) return "";

  return "spotify:" + art + ":" + id;
}

// Liest den aufliegenden Sticker komplett aus und gibt die Spotify-URI zurück.
// Leerer Text bedeutet: Lesen fehlgeschlagen oder kein Spotify-Link.
String leseUriVomSticker(bool& leseFehler) {
  leseFehler = true;

  // Sticker "auswählen". Erst danach nimmt er Lesebefehle an.
  if (!rfid.PICC_ReadCardSerial()) return "";

  uint8_t daten[NUTZDATEN_LEN];
  bool ok = leseNutzdaten(daten);
  rfid.PICC_HaltA();                             // Sticker wieder schlafen legen
  if (!ok) return "";

  leseFehler = false;                            // technisch hat das Lesen geklappt
  String url = findeUrl(daten, NUTZDATEN_LEN);
  if (url.length() == 0) {
    Serial.println("Auf dem Sticker steht kein Link.");
    return "";
  }
  Serial.print("Link auf der Platte: ");
  Serial.println(url);
  return linkZuUri(url);
}

// ===========================================================================
// Teil 4: Was passiert beim Auflegen und Wegnehmen
// ===========================================================================
void platteAufgelegt(const String& uri) {
  // Jedes Auflegen startet die Platte von vorne, egal was vorher lief.
  Serial.print("Starte von vorne: ");
  Serial.println(uri);
  if (spiele(uri)) {
    // Erst nach dem Start, damit die Musik ohne Verzögerung losgeht.
    zufallAus();
  }
}

void platteWeggenommen() {
  Serial.println("Platte weg, Pause.");
  pausiere();
}

// ===========================================================================
// Teil 5: Poti und Taster
// ===========================================================================

// Liest das Poti und gibt die Stellung in Prozent zurück (0 bis 100).
int lesePotiProzent() {
  // 16 Messungen mitteln, das nimmt das gröbste Rauschen heraus.
  uint32_t summe = 0;
  for (uint8_t i = 0; i < 16; i++) {
    summe += analogRead(PIN_POTI);          // Wert von 0 bis 4095 (12 Bit)
  }
  float messwert = summe / 16.0f;

  // Zusätzlich gleitend glätten: Der neue Wert zählt nur zu 30 Prozent,
  // der bisherige zu 70 Prozent. Das beruhigt die Anzeige weiter.
  if (potiGeglaettet < 0) {
    potiGeglaettet = messwert;              // erster Durchlauf
  } else {
    potiGeglaettet = 0.7f * potiGeglaettet + 0.3f * messwert;
  }

  // Der Analogeingang des ESP32 ist an den Rändern ungenau. Deshalb werten
  // wir nur den Bereich von 150 bis 3950 aus. So erreicht man 0 und 100
  // Prozent sicher, auch wenn das Poti nicht ganz bis zum Anschlag misst.
  long prozent = map((long)potiGeglaettet, 150, 3950, 0, 100);
  return constrain(prozent, 0, 100);
}

// Prüft das Poti und schickt bei einer Änderung die neue Lautstärke.
void pruefePoti() {
  if (millis() - letztePotiMessung < 100) return;   // zehnmal pro Sekunde reicht
  letztePotiMessung = millis();

  int prozent = lesePotiProzent();

  // Beim Einschalten nur merken, nicht senden. Sonst würde die Lautstärke
  // bei jedem Start ungefragt auf die Poti-Stellung springen.
  if (lautstaerkeGesendet < 0) {
    lautstaerkeGesendet = prozent;
    return;
  }

  bool deutlichGeaendert = abs(prozent - lautstaerkeGesendet) >= LAUTSTAERKE_SCHWELLE;
  // Die Anschläge 0 und 100 immer übernehmen, auch bei kleiner Änderung.
  bool anschlagErreicht  = (prozent == 0 || prozent == 100) && prozent != lautstaerkeGesendet;
  bool abstandOk         = millis() - letzteLautstaerke >= LAUTSTAERKE_ABSTAND_MS;

  if ((deutlichGeaendert || anschlagErreicht) && abstandOk) {
    letzteLautstaerke = millis();
    lautstaerkeGesendet = prozent;   // auch bei Fehler merken, sonst Dauerfeuer
    Serial.printf("Lautstärke: %d %%\n", prozent);
    setzeLautstaerke(prozent);
  }
}

// Gibt genau einmal "true" zurück, wenn der Taster neu gedrückt wurde.
bool tasterGedrueckt(Taster& t) {
  bool rohwert = (digitalRead(t.pin) == LOW);       // LOW = gedrückt (schaltet gegen GND)

  if (rohwert != t.letzterRohwert) {                // Wert hat sich geändert:
    t.letzterRohwert = rohwert;                     // merken und Stoppuhr starten
    t.letzteAenderung = millis();
  }

  // Erst wenn der Wert länger als die Entprellzeit stabil ist, gilt er.
  if (millis() - t.letzteAenderung > TASTER_ENTPRELL_MS && rohwert != t.gedrueckt) {
    t.gedrueckt = rohwert;
    return t.gedrueckt;                             // true nur beim Drücken, nicht beim Loslassen
  }
  return false;
}

void pruefeTaster() {
  if (tasterGedrueckt(tasterWeiter)) {
    Serial.println("Taster: nächster Titel");
    naechsterTitel();
  }
  if (tasterGedrueckt(tasterZurueck)) {
    Serial.println("Taster: vorheriger Titel");
    vorherigerTitel();
  }
}

// ===========================================================================
// setup(): läuft einmal beim Start
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== RFID-Plattenspieler ===");

  // --- Bedienelemente ---
  pinMode(PIN_WEITER, INPUT_PULLUP);
  pinMode(PIN_ZURUECK, INPUT_PULLUP);
  // GPIO 34 ist ein reiner Eingang, für analogRead ist kein pinMode nötig.

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
  // Beim allerersten Start nehmen wir den Token aus secrets.h. Danach gilt der
  // gespeicherte, weil Spotify ihn zwischendurch austauschen kann. Trägst du in
  // secrets.h einen neuen ein, erkennen wir das am gemerkten Startwert.
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
  verbindeWlan();
  if (erneuereToken()) {
    Serial.println("Bereit. Platte auflegen.");
  } else {
    Serial.println("Spotify-Anmeldung fehlgeschlagen. Stimmen Client-ID und Refresh Token?");
  }
}

// ===========================================================================
// loop(): läuft in Dauerschleife
// ===========================================================================
void loop() {
  // Taster und Poti bei jedem Durchlauf prüfen, damit sie schnell reagieren.
  pruefeTaster();
  pruefePoti();

  // Nach der Platte nur alle 250 ms schauen, dazwischen sofort zurückkehren.
  if (millis() - letzteAbfrage < ABFRAGE_INTERVALL_MS) return;
  letzteAbfrage = millis();

  if (stickerImFeld()) {
    fehlversuche = 0;

    if (!platteGelesen) {
      // Neue Platte (oder der letzte Leseversuch ging schief): Link lesen.
      bool leseFehler;
      String uri = leseUriVomSticker(leseFehler);
      if (!leseFehler) {
        platteGelesen = true;        // nicht bei jedem Durchlauf neu lesen
        platteLiegtAuf = true;
        if (uri.length() > 0) {
          platteAufgelegt(uri);
        } else {
          Serial.println("Kein Spotify-Link erkannt.");
        }
      }
      // Bei einem Lesefehler bleibt platteGelesen false. Dann versuchen wir es
      // beim nächsten Durchlauf einfach noch einmal.
    } else {
      // Platte liegt weiter auf: Sticker wieder schlafen legen, nichts tun.
      rfid.PICC_HaltA();
    }
  } else {
    // Kein Sticker gefunden. Erst nach mehreren Fehlversuchen reagieren.
    if (fehlversuche < 255) fehlversuche++;
    if (platteLiegtAuf && fehlversuche >= FEHLVERSUCHE_BIS_WEG) {
      platteLiegtAuf = false;
      platteGelesen = false;
      platteWeggenommen();
    } else if (!platteLiegtAuf) {
      platteGelesen = false;
    }
  }
}
