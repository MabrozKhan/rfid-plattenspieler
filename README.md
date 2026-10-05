# RFID-Plattenspieler

Ein kleiner Fake-Plattenspieler (ca. 20 x 20 cm) mit selbst gedruckten Platten. In jeder Platte steckt ein NFC-Sticker mit einem Spotify-Link. Legt man eine Platte auf, liest ein ESP32 den Sticker aus und startet das passende Album oder die Playlist auf einem Amazon Echo. Der Teller dreht sich, der Tonarm senkt sich auf die Platte.

Der Spotify-Link steckt in der Platte, nicht im Gerät. Dadurch läuft jede Platte auf jedem Gerät.

## Status

Projektstart. Noch nichts umgesetzt.

- [ ] Phase 1: RFID am ESP32
- [ ] Phase 2: Spotify am PC testen
- [ ] Phase 3: Beides zusammenführen
- [ ] Phase 4: Bewegung (Schrittmotor und Servo)
- [ ] Phase 5: Mechanik und Gehäuse
- [ ] Phase 6: Stromversorgung
- [ ] Phase 7: Captive Portal und Spotify-Login über Weboberfläche

## Hardware

- ESP32
- RFID-Leser RC522 (13,56 MHz)
- NTAG213 NFC-Sticker
- Schrittmotor für den Teller, Servo für den Tonarm
- Versorgung über USB-C (5 V), optional 18650-Zelle als Puffer

## Ordner

| Ordner | Inhalt |
| --- | --- |
| `docs/` | Projektplan und Dokumentation |
| `firmware/` | ESP32-Code |
| `tools/` | Python-Skript für den Spotify-Token |
| `cad/` | STL-, 3MF- und Quelldateien der Druckteile |
| `fotos/` | Bilder vom Aufbau |

Der ausführliche Plan steht in [docs/projektplan.md](docs/projektplan.md).

## Zugangsdaten

WLAN-Passwort, Spotify Client Secret und Refresh Token gehören in `firmware/secrets.h`. Diese Datei steht in der `.gitignore` und wird nicht hochgeladen. Als Vorlage dient `firmware/secrets.example.h`.
