"""
One-time setup for the Spotify keychain display.

Run:   python tools\\spotify_login.py

It asks for your WiFi and your Spotify app's keys, opens Spotify's login page
in your browser, and writes include/secrets.h. It can then upload the firmware.

Before running, create a Spotify app (free):
  1. Go to https://developer.spotify.com/dashboard and log in.
  2. Create app. Name and description can be anything.
  3. Redirect URI:  http://127.0.0.1:8888/callback   (exactly this)
  4. Tick "Web API", save.
  5. Open the app's Settings to find the Client ID and Client secret.
"""

import base64
import getpass
import http.server
import json
import os
import secrets
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = ("user-read-currently-playing user-read-playback-state "
          "user-modify-playback-state")
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRETS_PATH = os.path.join(PROJECT, "include", "secrets.h")


def read_saved():
    """Returns the values already in include/secrets.h, if any."""
    saved = {}
    try:
        with open(SECRETS_PATH, encoding="utf-8") as f:
            for line in f:
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[0] == "#define" and parts[2].startswith('"'):
                    end = parts[2].rfind('"')
                    value = parts[2][1:end].replace('\\"', '"').replace("\\\\", "\\")
                    saved[parts[1]] = value
    except OSError:
        pass
    return saved


def ask(prompt, required=True, hidden=False, current=""):
    """Asks for a value. With a saved value, pressing Enter keeps it."""
    if current:
        prompt = prompt.rstrip(": ") + " [Enter = keep the saved one]: "
    while True:
        value = (getpass.getpass(prompt) if hidden else input(prompt)).strip()
        if not value and current:
            return current
        if value or not required:
            return value
        print("  This one is needed.")


def c_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def wait_for_code(state):
    """Runs a tiny local web server until Spotify redirects back to it."""
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_response(404)
                self.end_headers()
                return
            query = urllib.parse.parse_qs(url.query)
            if query.get("state", [""])[0] != state:
                result["error"] = "the reply did not match this login attempt"
            elif "error" in query:
                result["error"] = query["error"][0]
            else:
                result["code"] = query.get("code", [""])[0]
            ok = "code" in result
            body = ("<h2>Done. You can close this tab.</h2>" if ok
                    else "<h2>Login failed: %s</h2>" % result.get("error", "unknown"))
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(body.encode("utf-8"))

        def log_message(self, *args):
            pass

    with http.server.HTTPServer(("127.0.0.1", 8888), Handler) as server:
        while not result:
            server.handle_request()
    return result


def exchange_code(client_id, client_secret, code):
    data = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI,
    }).encode()
    basic = base64.b64encode(("%s:%s" % (client_id, client_secret)).encode()).decode()
    request = urllib.request.Request(
        "https://accounts.spotify.com/api/token", data=data,
        headers={"Authorization": "Basic " + basic,
                 "Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def main():
    print(__doc__)

    print("WiFi the keychain should use (2.4 GHz networks only):")
    saved = read_saved()
    ssid1 = ask("  WiFi name: ", current=saved.get("WIFI_SSID_1", ""))
    pass1 = ask("  WiFi password (hidden while typing): ", required=False, hidden=True,
                current=saved.get("WIFI_PASS_1", ""))
    print("Optional second WiFi, e.g. your phone hotspot. Press Enter to skip.")
    ssid2 = ask("  Second WiFi name: ", required=False, current=saved.get("WIFI_SSID_2", ""))
    pass2 = ask("  Second WiFi password: ", required=False, hidden=True,
                current=saved.get("WIFI_PASS_2", "")) if ssid2 else ""

    print("\nFrom your Spotify app's Settings page:")
    client_id = ask("  Client ID: ", current=saved.get("SPOTIFY_CLIENT_ID", ""))
    client_secret = ask("  Client secret (hidden while typing): ", hidden=True,
                        current=saved.get("SPOTIFY_CLIENT_SECRET", ""))

    state = secrets.token_urlsafe(16)
    login_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "state": state,
    })
    print("\nOpening Spotify in your browser. Log in and press Agree.")
    print("If it does not open, paste this into your browser:\n  " + login_url + "\n")
    webbrowser.open(login_url)

    try:
        result = wait_for_code(state)
    except OSError as error:
        sys.exit("Could not listen on 127.0.0.1:8888 (%s). Close whatever is using "
                 "that port and run this again." % error)
    if "code" not in result:
        sys.exit("Spotify login failed: %s" % result.get("error", "unknown"))

    try:
        tokens = exchange_code(client_id, client_secret, result["code"])
    except urllib.error.HTTPError as error:
        sys.exit("Spotify refused the keys (%s): %s\nCheck the Client ID and secret."
                 % (error.code, error.read().decode(errors="replace")))
    refresh_token = tokens.get("refresh_token")
    if not refresh_token:
        sys.exit("Spotify did not return a refresh token: %s" % tokens)

    with open(SECRETS_PATH, "w", encoding="utf-8") as f:
        f.write("// Written by tools/spotify_login.py. Keep this file private.\n")
        f.write("#pragma once\n\n")
        f.write("#define WIFI_SSID_1            %s\n" % c_string(ssid1))
        f.write("#define WIFI_PASS_1            %s\n" % c_string(pass1))
        f.write("#define WIFI_SSID_2            %s\n" % c_string(ssid2))
        f.write("#define WIFI_PASS_2            %s\n\n" % c_string(pass2))
        f.write("#define SPOTIFY_CLIENT_ID      %s\n" % c_string(client_id))
        f.write("#define SPOTIFY_CLIENT_SECRET  %s\n" % c_string(client_secret))
        f.write("#define SPOTIFY_REFRESH_TOKEN  %s\n" % c_string(refresh_token))
    print("\nLogged in. Saved " + SECRETS_PATH)

    if ask("\nUpload to the board now? Plug it in first. [Y/n] ", required=False).lower() in ("", "y", "yes"):
        pio = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "pio.exe")
        if not os.path.exists(pio):
            pio = "pio"
        code = subprocess.call([pio, "run", "-e", "spotify", "-t", "upload"], cwd=PROJECT)
        print("\nUpload finished." if code == 0 else
              "\nUpload failed. Plug the board in and press Upload in PlatformIO "
              "(environment: spotify).")
    else:
        print("When ready, press Upload in PlatformIO (environment: spotify).")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("\nCancelled.")
