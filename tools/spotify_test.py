"""
RFID-Plattenspieler, Phase 2: Spotify am PC testen

Dieses Skript braucht nur Python 3, keine zusätzlichen Pakete.

Aufruf (im Ordner "tools"):
    python spotify_test.py login                 einmalig bei Spotify anmelden
    python spotify_test.py devices               Abspielgeräte auflisten
    python spotify_test.py play LINK [GERÄT]     Album oder Playlist starten
    python spotify_test.py pause [GERÄT]         Wiedergabe anhalten

LINK darf ein normaler Spotify-Link sein (https://open.spotify.com/album/...)
oder eine Spotify-URI (spotify:album:...).
GERÄT ist ein Teil des Gerätenamens, z. B. "echo". Groß/klein ist egal.

Anmeldeverfahren: "Authorization Code mit PKCE". Dabei wird kein Client Secret
gebraucht, nur die öffentliche Client-ID. Genau so macht es später der ESP32.

Dateien, die das Skript anlegt (beide stehen in der .gitignore):
    spotify_config.json   deine Client-ID
    token.json            Access Token und Refresh Token
"""

import base64
import hashlib
import http.server
import json
import pathlib
import re
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

# --- Feste Einstellungen ---------------------------------------------------

# Diese Adresse muss exakt so im Spotify-Dashboard als Redirect URI stehen.
REDIRECT_URI = "http://127.0.0.1:8888/callback"
PORT = 8888

# Berechtigungen, die wir von Spotify anfordern:
#   user-read-playback-state    Geräte und Wiedergabestatus lesen
#   user-modify-playback-state  Wiedergabe starten und anhalten
SCOPES = "user-read-playback-state user-modify-playback-state"

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
API_URL = "https://api.spotify.com/v1"

# Die Dateien liegen immer neben diesem Skript, egal von wo es gestartet wird.
ORDNER = pathlib.Path(__file__).resolve().parent
CONFIG_DATEI = ORDNER / "spotify_config.json"
TOKEN_DATEI = ORDNER / "token.json"


# --- Kleine Helfer zum Lesen und Schreiben der JSON-Dateien ------------------

def lade_json(datei):
    """Liest eine JSON-Datei. Gibt ein leeres Dictionary zurück, wenn sie fehlt."""
    if datei.exists():
        return json.loads(datei.read_text(encoding="utf-8"))
    return {}


def speichere_json(datei, daten):
    datei.write_text(json.dumps(daten, indent=2), encoding="utf-8")


def hole_client_id():
    """Liest die Client-ID aus der Konfiguration oder fragt einmalig danach."""
    config = lade_json(CONFIG_DATEI)
    if not config.get("client_id"):
        config["client_id"] = input("Client-ID aus dem Spotify-Dashboard: ").strip()
        speichere_json(CONFIG_DATEI, config)
        print("Gespeichert in", CONFIG_DATEI.name)
    return config["client_id"]


# --- HTTP-Anfragen -----------------------------------------------------------

def sende(methode, url, daten=None, header=None):
    """
    Schickt eine HTTP-Anfrage und gibt (Statuscode, Antwort als Dictionary) zurück.
    Bei einem Fehler (4xx, 5xx) wird nicht abgebrochen, sondern der Fehlertext
    von Spotify zurückgegeben. So sieht man, was Spotify zu bemängeln hat.
    """
    anfrage = urllib.request.Request(url, data=daten, method=methode, headers=header or {})
    try:
        with urllib.request.urlopen(anfrage, timeout=15) as antwort:
            status, text = antwort.status, antwort.read().decode("utf-8")
    except urllib.error.HTTPError as fehler:
        status, text = fehler.code, fehler.read().decode("utf-8")
    try:
        inhalt = json.loads(text) if text.strip() else {}
    except json.JSONDecodeError:
        inhalt = {"text": text}
    return status, inhalt


def token_anfrage(felder):
    """Anfrage an den Token-Server von Spotify (Formulardaten, kein JSON)."""
    daten = urllib.parse.urlencode(felder).encode("utf-8")
    header = {"Content-Type": "application/x-www-form-urlencoded"}
    status, inhalt = sende("POST", TOKEN_URL, daten, header)
    if status != 200:
        sys.exit(f"Token-Anfrage fehlgeschlagen ({status}): {inhalt}")
    return inhalt


def speichere_token(antwort, alter_refresh_token=None):
    """Speichert die Token-Antwort zusammen mit dem Ablaufzeitpunkt."""
    token = {
        "access_token": antwort["access_token"],
        # Beim Erneuern schickt Spotify nicht immer einen neuen Refresh Token.
        # Dann gilt der alte weiter.
        "refresh_token": antwort.get("refresh_token", alter_refresh_token),
        # Der Access Token gilt nur etwa eine Stunde. 60 s Sicherheitsabstand.
        "laeuft_ab": time.time() + antwort.get("expires_in", 3600) - 60,
    }
    speichere_json(TOKEN_DATEI, token)
    return token


def hole_access_token():
    """Gibt einen gültigen Access Token zurück und erneuert ihn bei Bedarf."""
    token = lade_json(TOKEN_DATEI)
    if not token.get("refresh_token"):
        sys.exit("Noch nicht angemeldet. Bitte zuerst:  python spotify_test.py login")

    if time.time() >= token.get("laeuft_ab", 0):
        # Abgelaufen: Mit dem Refresh Token einen neuen Access Token holen.
        antwort = token_anfrage({
            "grant_type": "refresh_token",
            "refresh_token": token["refresh_token"],
            "client_id": hole_client_id(),
        })
        token = speichere_token(antwort, token["refresh_token"])
    return token["access_token"]


def api(methode, pfad, parameter=None, koerper=None):
    """Anfrage an die Spotify Web API mit dem Access Token im Header."""
    url = API_URL + pfad
    if parameter:
        url += "?" + urllib.parse.urlencode(parameter)
    header = {"Authorization": "Bearer " + hole_access_token()}
    daten = None
    if koerper is not None:
        daten = json.dumps(koerper).encode("utf-8")
        header["Content-Type"] = "application/json"
    elif methode in ("PUT", "POST"):
        daten = b""   # leerer Körper, damit eine Content-Length mitgeschickt wird
    return sende(methode, url, daten, header)


# --- Befehl: login -----------------------------------------------------------

def befehl_login():
    client_id = hole_client_id()

    # PKCE: Wir denken uns ein Geheimnis aus (Verifier) und schicken Spotify
    # zunächst nur dessen SHA-256-Hash (Challenge). Beim Tausch des Codes gegen
    # den Token legen wir den Verifier offen. So kann niemand, der den Code
    # abfängt, etwas damit anfangen.
    verifier = secrets.token_urlsafe(64)
    hash_wert = hashlib.sha256(verifier.encode("ascii")).digest()
    challenge = base64.urlsafe_b64encode(hash_wert).decode("ascii").rstrip("=")

    # Zufallswert, den Spotify unverändert zurückschickt. Schutz vor Fälschung.
    state = secrets.token_urlsafe(16)

    login_url = AUTH_URL + "?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "scope": SCOPES,
        "state": state,
    })

    ergebnis = {}   # hier legt der kleine Webserver die Antwort von Spotify ab

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            teile = urllib.parse.urlparse(self.path)
            if teile.path != "/callback":      # z. B. /favicon.ico ignorieren
                self.send_response(404)
                self.end_headers()
                return
            ergebnis.update({k: v[0] for k, v in urllib.parse.parse_qs(teile.query).items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write("<h2>Fertig. Du kannst dieses Fenster schließen.</h2>".encode("utf-8"))

        def log_message(self, *args):          # keine Protokollzeilen ausgeben
            pass

    # Kleiner Webserver auf dem eigenen Rechner, der auf die Rückleitung wartet.
    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)

    print("Der Browser öffnet sich jetzt mit der Spotify-Anmeldung.")
    print("Falls nicht, diese Adresse von Hand öffnen:\n")
    print(login_url, "\n")
    webbrowser.open(login_url)

    while not ergebnis:                        # warten, bis /callback aufgerufen wurde
        server.handle_request()
    server.server_close()

    if "error" in ergebnis:
        sys.exit("Spotify meldet einen Fehler: " + ergebnis["error"])
    if ergebnis.get("state") != state:
        sys.exit("State stimmt nicht überein. Anmeldung abgebrochen.")

    # Den einmaligen Code gegen Access Token und Refresh Token tauschen.
    antwort = token_anfrage({
        "grant_type": "authorization_code",
        "code": ergebnis["code"],
        "redirect_uri": REDIRECT_URI,
        "client_id": client_id,
        "code_verifier": verifier,
    })
    speichere_token(antwort)
    print("Anmeldung erfolgreich. Token gespeichert in", TOKEN_DATEI.name)


# --- Befehl: devices ---------------------------------------------------------

def hole_geraete():
    status, inhalt = api("GET", "/me/player/devices")
    if status != 200:
        sys.exit(f"Geräte konnten nicht abgefragt werden ({status}): {inhalt}")
    return inhalt.get("devices", [])


def befehl_devices():
    geraete = hole_geraete()
    if not geraete:
        print("Keine Geräte gefunden.")
        print("Tipp: Auf dem Echo kurz etwas über Spotify abspielen und erneut versuchen.")
        return
    for g in geraete:
        aktiv = "aktiv" if g["is_active"] else "     "
        print(f"  {aktiv}  {g['name']:<30} {g['type']:<12} {g['id']}")


def finde_geraet(suchtext):
    """Sucht ein Gerät über einen Teil des Namens. Ohne Suchtext: das einzige Gerät."""
    geraete = hole_geraete()
    if suchtext:
        treffer = [g for g in geraete if suchtext.lower() in g["name"].lower()]
    else:
        treffer = geraete
    if len(treffer) == 1:
        return treffer[0]
    namen = ", ".join(g["name"] for g in geraete) or "keine"
    sys.exit(f"Gerät nicht eindeutig. Gefundene Geräte: {namen}")


# --- Befehl: play ------------------------------------------------------------

def zu_uri(link):
    """
    Wandelt einen Spotify-Link in eine Spotify-URI um.
      https://open.spotify.com/album/ABC?si=xyz   ->   spotify:album:ABC
    Dieselbe Umwandlung macht später der ESP32 mit dem Link vom NFC-Sticker.
    """
    if link.startswith("spotify:"):
        return link
    treffer = re.search(r"open\.spotify\.com/(?:intl-[a-z]+/)?(album|playlist|track)/([A-Za-z0-9]+)", link)
    if not treffer:
        sys.exit("Das ist kein Spotify-Link zu Album, Playlist oder Titel: " + link)
    return f"spotify:{treffer.group(1)}:{treffer.group(2)}"


def befehl_play(link, suchtext):
    uri = zu_uri(link)
    geraet = finde_geraet(suchtext)

    # Alben und Playlists sind ein "Kontext", einzelne Titel eine Liste von URIs.
    if uri.startswith("spotify:track:"):
        koerper = {"uris": [uri]}
    else:
        koerper = {"context_uri": uri}

    status, inhalt = api("PUT", "/me/player/play", {"device_id": geraet["id"]}, koerper)
    if status in (200, 202, 204):
        print(f"Spiele {uri} auf {geraet['name']}")
    else:
        print(f"Fehler ({status}): {inhalt}")


# --- Befehl: pause -----------------------------------------------------------

def befehl_pause(suchtext):
    geraet = finde_geraet(suchtext)
    status, inhalt = api("PUT", "/me/player/pause", {"device_id": geraet["id"]})
    if status in (200, 202, 204):
        print("Pausiert auf", geraet["name"])
    else:
        print(f"Fehler ({status}): {inhalt}")


# --- Hauptprogramm -----------------------------------------------------------

def main():
    argumente = sys.argv[1:]
    befehl = argumente[0] if argumente else ""

    if befehl == "login":
        befehl_login()
    elif befehl == "devices":
        befehl_devices()
    elif befehl == "play" and len(argumente) >= 2:
        befehl_play(argumente[1], argumente[2] if len(argumente) > 2 else None)
    elif befehl == "pause":
        befehl_pause(argumente[1] if len(argumente) > 1 else None)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
