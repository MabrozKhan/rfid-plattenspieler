/*
  RFID-Plattenspieler, Phase 1: RC522 am ESP32 testen

  Was dieser Sketch macht:
    1. Prüft beim Start, ob der RC522 antwortet (Verkabelung in Ordnung?).
    2. Wartet auf eine Karte, einen Anhänger oder einen NFC-Sticker.
    3. Gibt die UID (Seriennummer) und den Kartentyp aus.
    4. Bei NTAG- und Ultralight-Stickern zusätzlich die Rohdaten ab Seite 4.
       Dort steht später der Spotify-Link im NDEF-Format.

  Board in der Arduino IDE:  ESP32 Dev Module
  Bibliothek:                MFRC522 (von GithubCommunity / miguelbalboa)
  Serieller Monitor:         115200 Baud

  Verkabelung RC522 -> ESP32:
    SDA (SS)  -> GPIO 5
    SCK       -> GPIO 18
    MOSI      -> GPIO 23
    MISO      -> GPIO 19
    RST       -> GPIO 22
    GND       -> GND
    3.3V      -> 3V3     (niemals 5 V, der RC522 verträgt nur 3,3 V)
    IRQ       -> bleibt frei
*/

#include <SPI.h>       // SPI-Bus, darüber spricht der ESP32 mit dem RC522
#include <MFRC522.h>   // Bibliothek für den RC522-Leser

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
// SCK (18), MISO (19) und MOSI (23) sind die Standard-SPI-Pins des ESP32.
// Die muss man nicht extra angeben. Frei wählbar sind nur diese beiden:
const uint8_t PIN_SS  = 5;    // "Slave Select", am RC522 mit SDA beschriftet
const uint8_t PIN_RST = 22;   // Reset-Leitung des RC522

// Objekt für den Leser anlegen. Über "rfid" rufen wir alle Funktionen auf.
MFRC522 rfid(PIN_SS, PIN_RST);

// ---------------------------------------------------------------------------
// Hilfsfunktion: ein Byte immer zweistellig als Hex ausgeben (z. B. 0A statt A)
// ---------------------------------------------------------------------------
void printHex(uint8_t wert) {
  if (wert < 0x10) {
    Serial.print('0');          // führende Null ergänzen
  }
  Serial.print(wert, HEX);
}

// ---------------------------------------------------------------------------
// Rohdaten eines NTAG-/Ultralight-Stickers ausgeben
// ---------------------------------------------------------------------------
// Ein NTAG213 ist in "Seiten" zu je 4 Byte aufgeteilt.
//   Seite 0 bis 3:   UID, Sperrbits, Kapazität (Verwaltungsdaten)
//   Seite 4 bis 39:  Nutzdaten (144 Byte), hier liegt die NDEF-Nachricht
// Der Lesebefehl liefert immer 4 Seiten auf einmal, also 16 Byte.
void zeigeRohdaten() {
  const uint8_t ERSTE_SEITE  = 4;    // ab hier beginnen die Nutzdaten
  const uint8_t LETZTE_SEITE = 39;   // letzte Nutzdaten-Seite beim NTAG213

  Serial.println("Rohdaten ab Seite 4 (je Zeile 4 Seiten = 16 Byte):");

  for (uint8_t seite = ERSTE_SEITE; seite <= LETZTE_SEITE; seite += 4) {
    uint8_t puffer[18];              // 16 Datenbyte + 2 Byte Prüfsumme (CRC)
    uint8_t groesse = sizeof(puffer);

    // Vier Seiten lesen. Der Rückgabewert sagt, ob es geklappt hat.
    MFRC522::StatusCode status = rfid.MIFARE_Read(seite, puffer, &groesse);

    if (status != MFRC522::STATUS_OK) {
      Serial.print("  Lesen ab Seite ");
      Serial.print(seite);
      Serial.print(" fehlgeschlagen: ");
      Serial.println(rfid.GetStatusCodeName(status));
      return;                        // abbrechen, Sticker ist vermutlich weg
    }

    // Zeilenanfang: Seitennummer
    Serial.print("  Seite ");
    if (seite < 10) Serial.print(' ');
    Serial.print(seite);
    Serial.print(":  ");

    // Die 16 Byte als Hex ausgeben
    for (uint8_t i = 0; i < 16; i++) {
      printHex(puffer[i]);
      Serial.print(' ');
    }

    // Dieselben 16 Byte als Text. So erkennt man den Spotify-Link direkt.
    // Nicht druckbare Zeichen werden als Punkt dargestellt.
    Serial.print(" |");
    for (uint8_t i = 0; i < 16; i++) {
      char zeichen = (char)puffer[i];
      Serial.print((zeichen >= 32 && zeichen <= 126) ? zeichen : '.');
    }
    Serial.println('|');
  }
}

// ---------------------------------------------------------------------------
// setup(): läuft einmal nach dem Einschalten oder Reset
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);   // Verbindung zum PC, gleiche Baudrate im Monitor wählen
  delay(500);             // kurz warten, bis der serielle Monitor bereit ist

  SPI.begin();            // SPI-Bus mit den Standard-Pins starten
  rfid.PCD_Init();        // RC522 initialisieren (PCD = der Leser)
  delay(50);

  Serial.println();
  Serial.println("=== RFID-Plattenspieler, Phase 1: RC522-Test ===");

  // Selbsttest: Wir lesen die Versionsnummer aus dem Chip.
  // Typische Werte sind 0x91 oder 0x92 (Original) und 0x88 oder 0xB2 (Nachbau).
  // 0x00 oder 0xFF bedeutet: keine Antwort, also Verkabelung prüfen.
  uint8_t version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("Chip-Version: 0x");
  printHex(version);
  Serial.println();

  if (version == 0x00 || version == 0xFF) {
    Serial.println("FEHLER: Der RC522 antwortet nicht.");
    Serial.println("Bitte prüfen: 3V3 und GND, SDA an GPIO 5, RST an GPIO 22,");
    Serial.println("SCK 18, MOSI 23, MISO 19, Stiftleiste angelötet?");
  } else {
    Serial.println("RC522 erkannt. Jetzt Karte, Anhänger oder Sticker auflegen.");
  }
  Serial.println();
}

// ---------------------------------------------------------------------------
// loop(): läuft danach in Dauerschleife
// ---------------------------------------------------------------------------
void loop() {
  // Liegt eine NEUE Karte im Feld? Wenn nicht, sofort von vorne anfangen.
  if (!rfid.PICC_IsNewCardPresent()) {   // PICC = die Karte bzw. der Sticker
    return;
  }

  // Karte gefunden. Jetzt ihre UID auslesen. Klappt das nicht, von vorne.
  if (!rfid.PICC_ReadCardSerial()) {
    return;
  }

  // --- UID ausgeben -------------------------------------------------------
  // Die UID ist die fest eingebrannte Seriennummer des Chips.
  // MIFARE Classic hat 4 Byte, NTAG213 hat 7 Byte.
  Serial.print("UID: ");
  for (uint8_t i = 0; i < rfid.uid.size; i++) {
    printHex(rfid.uid.uidByte[i]);
    Serial.print(' ');
  }
  Serial.print(" (");
  Serial.print(rfid.uid.size);
  Serial.println(" Byte)");

  // --- Kartentyp ausgeben -------------------------------------------------
  // Der Typ wird aus dem SAK-Byte bestimmt, das die Karte beim Anmelden schickt.
  MFRC522::PICC_Type typ = rfid.PICC_GetType(rfid.uid.sak);
  Serial.print("Typ: ");
  Serial.println(rfid.PICC_GetTypeName(typ));

  // --- Rohdaten nur bei NTAG / Ultralight ---------------------------------
  // NTAG213-Sticker melden sich als "MIFARE Ultralight". Nur diese lassen sich
  // ohne Schlüssel lesen. MIFARE Classic (Karte und Anhänger aus dem Set)
  // bräuchte eine Anmeldung mit Schlüssel, das brauchen wir im Projekt nicht.
  if (typ == MFRC522::PICC_TYPE_MIFARE_UL) {
    zeigeRohdaten();
  } else {
    Serial.println("Kein NTAG/Ultralight, daher keine Rohdaten.");
  }

  Serial.println("----------------------------------------");

  // Karte "schlafen legen". Sonst würde dieselbe Karte nicht als neu erkannt
  // und erst nach dem Wegnehmen und erneuten Auflegen wieder gelesen.
  rfid.PICC_HaltA();

  delay(1000);   // eine Sekunde Pause, damit die Ausgabe nicht durchrauscht
}
