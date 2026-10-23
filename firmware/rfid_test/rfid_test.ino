/*
  RC522 test sketch (first step of the project)

  Checks that the RC522 answers, waits for a card or NFC tag and prints its
  UID and type. For NTAG / Ultralight tags it also dumps the user memory from
  page 4, where the NDEF message with the Spotify link is stored.

  Board    ESP32 Dev Module
  Library  MFRC522 (GithubCommunity / miguelbalboa)
  Serial   115200 baud

  Wiring RC522 -> ESP32
    SDA (SS) -> GPIO 5
    SCK      -> GPIO 18
    MOSI     -> GPIO 23
    MISO     -> GPIO 19
    RST      -> GPIO 22
    GND      -> GND
    3.3V     -> 3V3   (never 5 V)
    IRQ      -> not connected
*/

#include <SPI.h>
#include <MFRC522.h>

// SCK, MISO and MOSI are the default VSPI pins and need no setting
const uint8_t PIN_SS  = 5;    // labelled SDA on the RC522
const uint8_t PIN_RST = 22;

MFRC522 rfid(PIN_SS, PIN_RST);

void printHex(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

// NTAG213 memory is split into 4 byte pages:
//   pages 0-3   UID, lock bits, capability container
//   pages 4-39  user memory (144 bytes), holds the NDEF message
// One read command returns 4 pages = 16 bytes.
void dumpUserMemory() {
  const uint8_t FIRST_PAGE = 4;
  const uint8_t LAST_PAGE  = 39;

  Serial.println("User memory from page 4 (4 pages = 16 bytes per line):");

  for (uint8_t page = FIRST_PAGE; page <= LAST_PAGE; page += 4) {
    uint8_t buf[18];                 // 16 data + 2 CRC
    uint8_t size = sizeof(buf);

    MFRC522::StatusCode status = rfid.MIFARE_Read(page, buf, &size);

    if (status != MFRC522::STATUS_OK) {
      Serial.print("  Read from page ");
      Serial.print(page);
      Serial.print(" failed: ");
      Serial.println(rfid.GetStatusCodeName(status));
      return;                        // tag probably removed
    }

    Serial.print("  Page ");
    if (page < 10) Serial.print(' ');
    Serial.print(page);
    Serial.print(":  ");

    for (uint8_t i = 0; i < 16; i++) {
      printHex(buf[i]);
      Serial.print(' ');
    }

    // same bytes as text, makes the link easy to spot
    Serial.print(" |");
    for (uint8_t i = 0; i < 16; i++) {
      char ch = (char)buf[i];
      Serial.print((ch >= 32 && ch <= 126) ? ch : '.');
    }
    Serial.println('|');
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  SPI.begin();
  rfid.PCD_Init();
  delay(50);

  Serial.println();
  Serial.println("=== RC522 test ===");

  // Version register: 0x91/0x92 original, 0x88/0xB2 clones,
  // 0x00/0xFF means no answer
  uint8_t version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("Chip version: 0x");
  printHex(version);
  Serial.println();

  if (version == 0x00 || version == 0xFF) {
    Serial.println("ERROR: RC522 does not answer.");
    Serial.println("Check 3V3 and GND, SDA on GPIO 5, RST on GPIO 22,");
    Serial.println("SCK 18, MOSI 23, MISO 19, header soldered?");
  } else {
    Serial.println("RC522 found. Hold a card, key fob or tag to the reader.");
  }
  Serial.println();
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  if (!rfid.PICC_ReadCardSerial()) {
    return;
  }

  // UID: 4 bytes on MIFARE Classic, 7 bytes on NTAG213
  Serial.print("UID: ");
  for (uint8_t i = 0; i < rfid.uid.size; i++) {
    printHex(rfid.uid.uidByte[i]);
    Serial.print(' ');
  }
  Serial.print(" (");
  Serial.print(rfid.uid.size);
  Serial.println(" bytes)");

  MFRC522::PICC_Type type = rfid.PICC_GetType(rfid.uid.sak);
  Serial.print("Type: ");
  Serial.println(rfid.PICC_GetTypeName(type));

  // NTAG213 reports as MIFARE Ultralight and can be read without a key.
  // MIFARE Classic would need authentication, not needed here.
  if (type == MFRC522::PICC_TYPE_MIFARE_UL) {
    dumpUserMemory();
  } else {
    Serial.println("Not NTAG/Ultralight, no memory dump.");
  }

  Serial.println("----------------------------------------");

  // halt the tag, otherwise it is not seen as new until removed
  rfid.PICC_HaltA();

  delay(1000);
}
