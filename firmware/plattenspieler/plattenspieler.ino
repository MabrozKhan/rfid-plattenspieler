/*
  RFID-Plattenspieler, Phase 3: RFID und Spotify zusammen

  Ablauf:
    Platte (NFC-Sticker) auflegen  ->  Link auslesen  ->  Album/Playlist startet
    Platte wegnehmen               ->  Pause
    Dieselbe Platte wieder drauf   ->  es geht an der alten Stelle weiter

  Board in der Arduino IDE:  ESP32 Dev Module
  Bibliothek:                MFRC522 (von GithubCommunity / miguelbalboa)
  Serieller Monitor:         115200 Baud

  Zugangsdaten stehen in secrets.h (liegt neben dieser Datei, wird nicht
  hochgeladen). Vorlage: secrets.example.h

  Verkabelung RC522 -> ESP32:
    SDA 5, SCK 18, MOSI 23, MISO 19, RST 22, GND, 3.3V an 3V3 (niemals 5 V)
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
String   letzteUri;                // zuletzt gestartetes Album bzw. Playlist
uint32_t letzteAbfrage   = 0;

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

// Schickt einen Befehl an den Spotify-Player (play oder pause).
bool playerBefehl(const char* befehl, const String& koerper) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Kein WLAN, Befehl nicht gesendet.");
    return false;
  }
  if (!tokenBereit()) return false;

  String url = String("https://api.spotify.com/v1/me/player/") + befehl +
               "?device_id=" + SPOTIFY_DEVICE_ID;
  String antwort;
  int status = sendeHttps("PUT", url, koerper, "application/json", true, antwort);

  // 401 heißt: Token doch schon abgelaufen. Einmal erneuern und wiederholen.
  if (status == 401 && erneuereToken()) {
    status = sendeHttps("PUT", url, koerper, "application/json", true, antwort);
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

// Startet ein Album, eine Playlist oder einen Titel.
bool spiele(const String& uri) {
  String koerper;
  if (uri.startsWith("spotify:track:")) {
    koerper = "{\"uris\":[\"" + uri + "\"]}";        // einzelner Titel
  } else {
    koerper = "{\"context_uri\":\"" + uri + "\"}";   // Album oder Playlist
  }
  return playerBefehl("play", koerper);
}

// Setzt die Wiedergabe an der alten Stelle fort (leerer Auftrag = weiter).
bool weiter() { return playerBefehl("play", "{}"); }

bool pausiere() { return playerBefehl("pause", "{}"); }

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
  if (uri == letzteUri) {
    // Dieselbe Platte wie vorher: an der alten Stelle weiterspielen.
    Serial.println("Gleiche Platte, Wiedergabe wird fortgesetzt.");
    if (!weiter()) {
      spiele(uri);   // falls Fortsetzen nicht geht, von vorne starten
    }
  } else {
    Serial.print("Starte ");
    Serial.println(uri);
    if (spiele(uri)) {
      letzteUri = uri;
    }
  }
}

void platteWeggenommen() {
  Serial.println("Platte weg, Pause.");
  pausiere();
}

// ===========================================================================
// setup(): läuft einmal beim Start
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== RFID-Plattenspieler ===");

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
  // Nur alle 250 ms nachschauen, dazwischen sofort zurückkehren.
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
