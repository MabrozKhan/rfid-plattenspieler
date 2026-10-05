# Projekt: RFID-Plattenspieler (Arbeitstitel)

Dieses Dokument fasst alles zusammen, was bisher zum Projekt besprochen und entschieden wurde. Es dient als Übergabe an einen neuen Chat mit Zugriff auf den Projektordner, damit dort direkt weitergearbeitet werden kann.

## 1. Über mich und meine Umgebung

- Mabroz, Student Elektrotechnik an der THM (Campus Friedberg)
- Betriebssystem: Windows
- GitHub-Account: vorhanden, aber noch keine Erfahrung mit Git oder GitHub
- Einstieg mit **GitHub Desktop**, die Kommandozeile kommt später
- 3D-Drucker: Bambu Lab A2L, Düse 0,4 mm
- Ich diktiere oft per Sprache, Nachrichten können daher Tippfehler oder Satzbrüche enthalten
- Bitte keine Gedankenstriche als Satztrenner in Texten, die für mich geschrieben werden

## 2. Projektidee

Ein kleiner Fake-Plattenspieler (ca. 20 x 20 cm). Dazu gibt es selbst gedruckte Platten, in deren Mitte ein NFC-Sticker steckt. Legt man eine Platte auf, liest der Plattenspieler den Sticker aus und startet über Spotify die passende Playlist oder das Album auf einem Amazon Echo. Der Teller dreht sich, ein Tonarm senkt sich auf die Platte.

Kernidee: **Der Spotify-Link steckt in der Platte, nicht im Gerät.** Dadurch funktioniert jede Platte auf jedem Gerät. Eine Platte, die ich mache, läuft auch beim Kollegen, und umgekehrt.

## 3. Vorhandene Hardware

- ESP32
- RFID-Leser RC522 (13,56 MHz, ISO 14443A), dazu Karte und Schlüsselanhänger aus dem Starter-Set (vermutlich MIFARE Classic)
- 100 Stück NTAG213 NFC-Sticker (144 Byte Nutzdaten)
- Schrittmotoren: zwei 6 V, einer 12 V
- Servos
- Li-Ion-Zelle ICR18650, 1800 mAh, 3,7 V (aus einer alten Powerbank)
- Step-Up-Modul
- USB-C-Lademodul
- Solar-Power-Management-Modul
- Alter Arduino (wird für dieses Projekt nicht gebraucht, da kein WLAN)
- 2S-LiPo 1500 mAh von der Drohne (bewusst nicht verwendet, siehe Stromversorgung)

## 4. Bisherige Entscheidungen

**NFC statt UID-Zuordnung**
- Auf jeden NTAG213-Sticker wird per Handy (App z. B. „NFC Tools“) ein URL-Eintrag im NDEF-Format mit dem Spotify-Link geschrieben.
- Der ESP32 liest die Rohdaten ab Seite 4, sucht den URL-Eintrag und ergänzt das Präfix (0x04 steht für „https://“).
- Aus `https://open.spotify.com/album/ID` wird `spotify:album:ID`, das gleiche für Playlists.
- Bonus: Hält man die Platte ans Handy, öffnet sich der Link direkt in Spotify.
- Die MFRC522-Bibliothek liest nur Rohdaten, das NDEF-Auswerten wird selbst programmiert.

**Spotify**
- Spotify Premium ist vorhanden, daher Weg über die **Spotify Web API**.
- Der Echo wird als Spotify-Connect-Gerät angesteuert.
- Hinweis: Playlists müssen öffentlich oder geteilt sein, sonst kann ein anderes Konto sie nicht abspielen. Alben funktionieren immer.

**Mechanik**
- Platten auf der Unterseite wellig, Teller mit passender welliger Mulde, damit die Platte mitdreht und immer gleich ausgerichtet liegt.
- Zusätzlich ein Mittelstift zum Zentrieren.
- NFC-Sticker in der Mitte unter dem Label, RC522 fest unter der Tellermitte. So bleibt der Sticker beim Drehen immer über dem Leser.
- Reichweite des RC522 nur ca. 1 bis 3 cm: zwischen Leser und Sticker höchstens ein paar Millimeter Kunststoff, kein Metall in der Nähe.
- Schrittmotor treibt den Teller, Servo bewegt den Tonarm. Tonarm runter = Start, Tonarm hoch = Pause.

**Stromversorgung**
- Hauptversorgung über **USB-C (5 V, 2 A Netzteil)**.
- Optional: 18650-Zelle mit Lademodul und Step-Up als Puffer, damit das Gerät auch kurz ohne Kabel läuft.
- Verbrauch beim Abspielen grob 1,5 bis 2 W (ESP32 mit WLAN ca. 0,5 W, Schrittmotor ca. 1 bis 1,2 W).
- Rechnung 18650: 3,7 V x 1,8 Ah = ca. 6,7 Wh, nach Verlusten ca. 5 Wh, also etwa 2,5 bis 3 Stunden Abspielzeit.
- Vor dem Einbau Zellspannung messen. Unter ca. 2,5 V die Zelle nicht verwenden.
- Das Lademodul muss gleichzeitig laden und versorgen können (Load Sharing bzw. Pass-Through), sonst startet der ESP beim Einstecken neu.
- Der 2S-LiPo wurde verworfen: braucht Balancer-Lader, kein eingebauter Tiefentladeschutz, nicht gut für unbeaufsichtigtes Laden im Gehäuse.

**Späteres Ausbauziel: Einrichtung per Captive Portal**
- Mit der Bibliothek WiFiManager: Findet der ESP kein bekanntes WLAN, öffnet er ein eigenes (z. B. „Plattenspieler-Setup“). Dort wählt man sein WLAN aus.
- Danach Weboberfläche im Heimnetz, z. B. unter `plattenspieler.local`.
- Spotify-Login über die Weboberfläche ist kniffliger: Spotify erlaubt Redirect-Adressen ohne HTTPS nur noch für 127.0.0.1. Lösung voraussichtlich eine kleine HTTPS-Hilfsseite (z. B. auf GitHub Pages).
- Spotify-Apps im Entwicklermodus können nur von freigeschalteten Nutzern verwendet werden. Für mich und ein paar Freunde reicht das.

## 5. Ablauf in Phasen

Status: noch nichts umgesetzt, alle Phasen offen.

**Phase 1: RFID am ESP32**
1. RC522 anschließen: SDA an GPIO 5, SCK an 18, MOSI an 23, MISO an 19, RST an 22, GND an GND, 3,3 V an 3V3. Der RC522 verträgt keine 5 V.
2. Bibliothek MFRC522 installieren, Beispiel zum Auslesen der UID flashen.
3. Karte, Anhänger und einen NTAG213-Sticker testen. Reichweite und Lesbarkeit durch Kunststoff prüfen.
4. Einen Sticker per Handy mit einem Spotify-Link beschreiben und die Rohdaten am ESP ausgeben.

**Phase 2: Spotify am PC testen**
1. App auf developer.spotify.com anlegen, Client-ID und Secret notieren. Redirect-URI z. B. `http://127.0.0.1:8888/callback`.
2. Mit einem kleinen Python-Skript einmalig einloggen und den Refresh Token holen.
3. Geräte abfragen (`/v1/me/player/devices`), ID des Echo notieren. Der Echo taucht manchmal erst auf, wenn er kurz vorher Spotify gespielt hat.
4. Playlist auf dem Echo starten (`/v1/me/player/play` mit Geräte-ID und URI).

**Phase 3: Beides zusammenführen**
1. Refresh Token auf dem ESP32 hinterlegen (z. B. Bibliothek SpotifyArduino).
2. NDEF auswerten, Link in Spotify-URI umwandeln, abspielen.
3. Platte drauf = Start, Platte weg = Pause. Leser regelmäßig abfragen und entprellen.

**Phase 4: Bewegung**
1. Schrittmotor mit Treiber, Strom direkt von den 5 V, nicht vom ESP-Pin.
2. Servo als Tonarm.

**Phase 5: Mechanik und Gehäuse**
Teller, Platten mit welliger Unterseite, Mittelstift, Halterung für den RC522, Gehäuse. Alles auf dem A2L drucken.

**Phase 6: Stromversorgung**
USB-C, optional 18650 mit Load-Sharing-Modul.

**Phase 7: Captive Portal und Spotify-Login über Weboberfläche**

## 6. Geplante Ordner- und Dateistruktur

Der Projektordner wird gleichzeitig das GitHub-Repository.

```
rfid-plattenspieler/
  README.md                Kurze Projektbeschreibung, Fotos, Status
  .gitignore               Unter anderem secrets.h und Python-Token-Dateien
  docs/
    projektplan.md         Dieses Dokument
    hardware.md            Bauteilliste mit Details
    verkabelung.md         Pinbelegung und Schaltplan
    spotify-setup.md       Anleitung für die Spotify-App und den Token
    stromversorgung.md     Rechnungen und Modulauswahl
  firmware/                ESP32-Code
  tools/                   Python-Skript für den Spotify-Token
  cad/                     STL-, 3MF- und Quelldateien der Druckteile
  fotos/                   Bilder vom Aufbau
```

**Wichtig:** Zugangsdaten (Spotify Client Secret, Refresh Token, WLAN-Passwort) kommen in eine eigene Datei wie `firmware/secrets.h`, die in der `.gitignore` steht. Sie darf nie ins Repo hochgeladen werden. Stattdessen eine `secrets.example.h` mit Platzhaltern einchecken.

## 7. GitHub-Vorhaben

Ziel: Git und GitHub direkt an diesem Projekt lernen, Schritt für Schritt, mit Erklärung der Begriffe. Erst GitHub Desktop, später die wichtigsten Befehle auf der Kommandozeile.

Geplante Lernschritte:
1. Begriffe verstehen: Repository, Commit, Push, Pull, Branch, `.gitignore`
2. GitHub Desktop installieren und mit dem Account anmelden
3. Den Projektordner als neues Repository anlegen
4. README und `.gitignore` erstellen, erster Commit
5. Repository auf GitHub veröffentlichen (zunächst privat)
6. Gewohnheit aufbauen: Nach jedem funktionierenden Schritt ein Commit mit aussagekräftiger Nachricht
7. Später: Branches für Experimente, Repo öffentlich machen als Portfolio für die Praktikumssuche
8. Später: Die gleichen Abläufe auf der Kommandozeile

## 8. Aufgaben für den nächsten Chat

1. Die Ordnerstruktur aus Abschnitt 6 im Projektordner anlegen und dieses Dokument als `docs/projektplan.md` ablegen.
2. README.md und .gitignore erstellen.
3. Mich Schritt für Schritt durch GitHub Desktop führen (Abschnitt 7). Nach jedem Schritt auf meine Bestätigung warten, bevor es weitergeht. Begriffe einfach erklären, ich habe noch keine Vorkenntnisse.
4. Danach mit Phase 1 starten.
