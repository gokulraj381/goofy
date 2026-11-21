// Template for include/secrets.h.
//
// Easiest: run  python tools/spotify_login.py  and it writes secrets.h for you.
// By hand: copy this file to secrets.h and fill in the values.
// secrets.h is in .gitignore. Never commit it.
#pragma once

#define WIFI_SSID_1            "your-wifi-name"
#define WIFI_PASS_1            "your-wifi-password"
#define WIFI_SSID_2            ""   // optional second network, e.g. a phone hotspot
#define WIFI_PASS_2            ""

#define SPOTIFY_CLIENT_ID      ""
#define SPOTIFY_CLIENT_SECRET  ""
#define SPOTIFY_REFRESH_TOKEN  ""   // written by tools/spotify_login.py
