#pragma once

// Copy this file to include/secrets.h and replace the placeholder values.
// include/secrets.h is ignored by Git so credentials are not committed.

#define WIFI_SSID_MAIN "your-main-wifi-name"
#define WIFI_PASSWORD_MAIN "your-main-wifi-password"

#define WIFI_SSID_1 "your-backup-wifi-name"
#define WIFI_PASSWORD_1 "your-backup-wifi-password"

// Leave the password out only for a genuinely open network.
#define WIFI_SSID_PUB "your-open-wifi-name"

#define SPOTIFY_CLIENT_ID "your-spotify-client-id"
#define SPOTIFY_CLIENT_SECRET "your-spotify-client-secret"
#define SPOTIFY_REFRESH_TOKEN "your-spotify-refresh-token"

// Spotify uses a two-letter ISO 3166-1 country code, such as "US".
#define SPOTIFY_MARKET "US"
