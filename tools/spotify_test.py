"""
Spotify test tool for the RFID turntable. Used to log in once, get the
refresh token for the ESP32 and test playback from the PC.

Needs only Python 3, no extra packages.

Usage (inside the "tools" folder):
    python spotify_test.py login                  log in to Spotify once
    python spotify_test.py devices                list playback devices
    python spotify_test.py play LINK [DEVICE]     start an album or playlist
    python spotify_test.py pause [DEVICE]         pause playback

LINK can be a normal Spotify link (https://open.spotify.com/album/...)
or a Spotify URI (spotify:album:...).
DEVICE is part of the device name, e.g. "echo", case insensitive.

Login uses "Authorization Code with PKCE", so no client secret is needed,
only the public client ID. The ESP32 does the same.

Files created next to this script (both are in .gitignore):
    spotify_config.json   your client ID
    token.json            access token and refresh token
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

# Has to match the redirect URI in the Spotify developer dashboard exactly
REDIRECT_URI = "http://127.0.0.1:8888/callback"
PORT = 8888

#   user-read-playback-state    read devices and playback state
#   user-modify-playback-state  start and pause playback
SCOPES = "user-read-playback-state user-modify-playback-state"

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
API_URL = "https://api.spotify.com/v1"

FOLDER = pathlib.Path(__file__).resolve().parent
CONFIG_FILE = FOLDER / "spotify_config.json"
TOKEN_FILE = FOLDER / "token.json"


def load_json(path):
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    return {}


def save_json(path, data):
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def get_client_id():
    config = load_json(CONFIG_FILE)
    if not config.get("client_id"):
        config["client_id"] = input("Client ID from the Spotify dashboard: ").strip()
        save_json(CONFIG_FILE, config)
        print("Saved in", CONFIG_FILE.name)
    return config["client_id"]


def send(method, url, data=None, headers=None):
    """Returns (status, response as dict). HTTP errors are returned, not
    raised, so Spotify's error message is visible."""
    request = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            status, text = response.status, response.read().decode("utf-8")
    except urllib.error.HTTPError as err:
        status, text = err.code, err.read().decode("utf-8")
    try:
        content = json.loads(text) if text.strip() else {}
    except json.JSONDecodeError:
        content = {"text": text}
    return status, content


def token_request(fields):
    data = urllib.parse.urlencode(fields).encode("utf-8")
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    status, content = send("POST", TOKEN_URL, data, headers)
    if status != 200:
        sys.exit(f"Token request failed ({status}): {content}")
    return content


def save_token(response, old_refresh_token=None):
    token = {
        "access_token": response["access_token"],
        # Spotify does not always send a new refresh token, then the old one stays valid
        "refresh_token": response.get("refresh_token", old_refresh_token),
        # access token is valid for about an hour, 60 s margin
        "expires_at": time.time() + response.get("expires_in", 3600) - 60,
    }
    save_json(TOKEN_FILE, token)
    return token


def get_access_token():
    token = load_json(TOKEN_FILE)
    if not token.get("refresh_token"):
        sys.exit("Not logged in yet. Run first:  python spotify_test.py login")

    if time.time() >= token.get("expires_at", 0):
        response = token_request({
            "grant_type": "refresh_token",
            "refresh_token": token["refresh_token"],
            "client_id": get_client_id(),
        })
        token = save_token(response, token["refresh_token"])
    return token["access_token"]


def api(method, path, params=None, body=None):
    url = API_URL + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    headers = {"Authorization": "Bearer " + get_access_token()}
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif method in ("PUT", "POST"):
        data = b""   # empty body so a Content-Length header is sent
    return send(method, url, data, headers)


# --- login -------------------------------------------------------------------

def cmd_login():
    client_id = get_client_id()

    # PKCE: send only the SHA-256 hash of a random verifier first and reveal
    # the verifier when exchanging the code, so an intercepted code is useless
    verifier = secrets.token_urlsafe(64)
    digest = hashlib.sha256(verifier.encode("ascii")).digest()
    challenge = base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")

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

    result = {}   # filled by the small web server below

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            parts = urllib.parse.urlparse(self.path)
            if parts.path != "/callback":      # ignore /favicon.ico etc.
                self.send_response(404)
                self.end_headers()
                return
            result.update({k: v[0] for k, v in urllib.parse.parse_qs(parts.query).items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write("<h2>Done. You can close this window.</h2>".encode("utf-8"))

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)

    print("Opening the Spotify login in your browser.")
    print("If it does not open, use this address:\n")
    print(login_url, "\n")
    webbrowser.open(login_url)

    while not result:
        server.handle_request()
    server.server_close()

    if "error" in result:
        sys.exit("Spotify returned an error: " + result["error"])
    if result.get("state") != state:
        sys.exit("State does not match. Login aborted.")

    response = token_request({
        "grant_type": "authorization_code",
        "code": result["code"],
        "redirect_uri": REDIRECT_URI,
        "client_id": client_id,
        "code_verifier": verifier,
    })
    save_token(response)
    print("Login successful. Token saved in", TOKEN_FILE.name)


# --- devices -----------------------------------------------------------------

def get_devices():
    status, content = api("GET", "/me/player/devices")
    if status != 200:
        sys.exit(f"Could not get devices ({status}): {content}")
    return content.get("devices", [])


def cmd_devices():
    devices = get_devices()
    if not devices:
        print("No devices found.")
        print("Tip: play something from Spotify on the Echo briefly, then try again.")
        return
    for d in devices:
        active = "active" if d["is_active"] else "      "
        print(f"  {active}  {d['name']:<30} {d['type']:<12} {d['id']}")


def find_device(search):
    """Finds a device by part of its name. Without search: the only device."""
    devices = get_devices()
    if search:
        matches = [d for d in devices if search.lower() in d["name"].lower()]
    else:
        matches = devices
    if len(matches) == 1:
        return matches[0]
    names = ", ".join(d["name"] for d in devices) or "none"
    sys.exit(f"Device not unique. Devices found: {names}")


# --- play / pause --------------------------------------------------------------

def to_uri(link):
    """https://open.spotify.com/album/ABC?si=xyz  ->  spotify:album:ABC"""
    if link.startswith("spotify:"):
        return link
    match = re.search(r"open\.spotify\.com/(?:intl-[a-z]+/)?(album|playlist|track)/([A-Za-z0-9]+)", link)
    if not match:
        sys.exit("Not a Spotify link to an album, playlist or track: " + link)
    return f"spotify:{match.group(1)}:{match.group(2)}"


def cmd_play(link, search):
    uri = to_uri(link)
    device = find_device(search)

    # albums and playlists are a "context", single tracks a list of URIs
    if uri.startswith("spotify:track:"):
        body = {"uris": [uri]}
    else:
        body = {"context_uri": uri}

    status, content = api("PUT", "/me/player/play", {"device_id": device["id"]}, body)
    if status in (200, 202, 204):
        print(f"Playing {uri} on {device['name']}")
    else:
        print(f"Error ({status}): {content}")


def cmd_pause(search):
    device = find_device(search)
    status, content = api("PUT", "/me/player/pause", {"device_id": device["id"]})
    if status in (200, 202, 204):
        print("Paused on", device["name"])
    else:
        print(f"Error ({status}): {content}")


def main():
    args = sys.argv[1:]
    cmd = args[0] if args else ""

    if cmd == "login":
        cmd_login()
    elif cmd == "devices":
        cmd_devices()
    elif cmd == "play" and len(args) >= 2:
        cmd_play(args[1], args[2] if len(args) > 2 else None)
    elif cmd == "pause":
        cmd_pause(args[1] if len(args) > 1 else None)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
