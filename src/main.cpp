/*
  Spotify Album Art Display and Bluetooth Audio Visualizer
  
  REQUIRED LIBRARIES:
  1. TFT_eSPI
  2. TJpg_Decoder
  3. U8g2_for_TFT_eSPI
  4. SpotifyArduino
  5. ArduinoJson

  Connect a momentary button from GPIO 33 to GND to toggle Karaoke Mode.

  */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SpotifyArduino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>       
#include <TJpg_Decoder.h>
#include <U8g2_for_TFT_eSPI.h> 
#include <esp_heap_caps.h>
#include <vector>
#include "audio_visualizer.h"
#include "secrets.h"

// --- DISPLAY SETTINGS ---
TFT_eSPI tft = TFT_eSPI(); 
U8g2_for_TFT_eSPI u8f; 

constexpr int SCREEN_HEIGHT = 320;
constexpr size_t JPG_BUFFER_CAPACITY = 100000;
constexpr uint32_t SPOTIFY_POLL_INTERVAL_MS = 3000;
constexpr uint32_t UI_UPDATE_INTERVAL_MS = 100;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t NETWORK_TIMEOUT_SECONDS = 8;
constexpr uint32_t IMAGE_TIMEOUT_MS = 5000;
constexpr uint32_t IMAGE_TIMEOUT_SECONDS = 5;
constexpr uint32_t LYRICS_TIMEOUT_MS = 5000;
constexpr uint32_t LYRICS_TIMEOUT_SECONDS = 5;
constexpr uint8_t MAX_ALBUM_ART_ATTEMPTS = 3;
constexpr size_t PSRAM_MALLOC_THRESHOLD_BYTES = 512;
constexpr uint32_t SCREENSAVER_FRAME_INTERVAL_MS = 150;
constexpr int16_t SCREENSAVER_WIDTH = 164;
constexpr int16_t SCREENSAVER_HEIGHT = 42;

// --- NETWORK & SPOTIFY ---
WiFiClientSecure client;
SpotifyArduino spotify(client, SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);

// --- LYRICS STRUCTURE ---
struct LyricLine {
  long timestamp; // in milliseconds
  String text;
};
std::vector<LyricLine> currentLyrics;
int currentLyricIndex = -1;
bool hasLyrics = false;

// Album art lives in PSRAM so TLS and JSON can keep the internal heap.
uint8_t* jpgData = nullptr;
size_t jpgDataSize = 0;

// --- GLOBAL VARIABLES ---
unsigned long lastCheck = 0;
unsigned long lastButtonPress = 0;
unsigned long lastProgressBarUpdate = 0;
unsigned long lastReconnectAttempt = 0;
bool isSpotifyPlaying = false;
bool spotifyAuthenticated = false;
String lastTrackURI = ""; 
bool forceRedraw = false; 
bool screensaverActive = false;
int16_t screensaverX = 16;
int16_t screensaverY = 16;
int8_t screensaverDx = 3;
int8_t screensaverDy = 2;
uint32_t lastScreensaverFrameAt = 0;
uint8_t consecutiveSpotifyNetworkFailures = 0;
bool networkErrorVisible = false;

// SpotifyArduino invokes its callback while its HTTPS connection and JSON
// document are still alive. Copy the small fields we need and wait until the
// callback returns before opening the artwork or lyrics HTTPS connections.
String pendingAlbumArtUrl;
String pendingLyricsTrack;
String pendingLyricsArtist;
bool pendingTrackResources = false;
bool pendingAlbumArtDraw = false;
String queuedAlbumArtUrl;
String queuedLyricsTrack;
String queuedLyricsArtist;
bool albumArtRequestPending = false;
bool lyricsRequestPending = false;
uint8_t albumArtAttempt = 0;
uint32_t nextAlbumArtAttemptAt = 0;
uint32_t nextLyricsRequestAt = 0;

// Mode State
bool isKaraokeMode = false; 

// Progress Tracking
long songDuration = 0;
long songProgress = 0;
unsigned long lastSongFetchTime = 0;
int lastBarWidth = 0; 

// --- COLORS ---
constexpr uint16_t DOMINANT_COLOR = 0x1DB9;
constexpr uint16_t BACKGROUND_COLOR = TFT_BLACK;

// --- LAYOUT CONSTANTS ---
constexpr int IMG_X = 180;
constexpr int IMG_Y = 0;
constexpr int TEXT_X = 10;
constexpr int TEXT_W = 160;
int lyricY = 180; 

// --- PIN DEFINITIONS ---
// GPIO33 is not a boot-strapping pin. The onboard BOOT button on GPIO0 is
// deliberately unused so pressing the mode button cannot affect startup.
constexpr uint8_t MODE_BUTTON_PIN = 33;

// Function prototypes are required in a .cpp source file. Arduino only
// generates these automatically for .ino sketches.
void handleButtons();
void printCurrentlyPlaying(CurrentlyPlaying currentlyPlaying);
void updateProgressBar();
void updateKaraokeScroll();
void updateLyrics();
void setScreensaverActive(bool active);
void updateScreensaver();
bool drawAlbumArt(const String& url, int xPos, int yPos);
void processPendingTrackResources();
bool serviceDeferredNetworkTasks();
bool connectWiFi(const char* ssid, const char* password, const char* label);
void logMemory(const char* context);
void logTftSetup();
bool refreshSpotifyToken(const char* reason);
void onAllocationFailure(size_t size, uint32_t caps, const char* functionName);
void noteSpotifyNetworkSuccess();
void handleSpotifyNetworkFailure(int status);

//Helper to get correct height
int getScreenHeight() {
  return SCREEN_HEIGHT;
}

void logMemory(const char* context) {
  Serial.printf(
      "[MEM] %s | heap=%u, internal=%u, internal-largest=%u, dma=%u, dma-largest=%u, psram=%u\n",
      context,
      ESP.getFreeHeap(),
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
      ESP.getFreePsram());
}

void onAllocationFailure(size_t size, uint32_t caps, const char* functionName) {
  Serial.printf(
      "[MEM] ALLOCATION FAILED: function=%s bytes=%u caps=0x%08lX internal-free=%u largest=%u psram=%u\n",
      functionName != nullptr ? functionName : "unknown",
      static_cast<unsigned>(size),
      static_cast<unsigned long>(caps),
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      ESP.getFreePsram());
}

void logTftSetup() {
  setup_t setup;
  tft.getSetup(setup);
  Serial.printf(
      "[Display] TFT_eSPI=%s setup=\"%s\" id=%lu driver=0x%04X serial=%s\n",
      setup.version.c_str(),
      setup.setup_info.c_str(),
      static_cast<unsigned long>(setup.setup_id),
      setup.tft_driver,
      setup.serial ? "yes" : "no");
  Serial.printf(
      "[Display] compiled pins: SCLK=%d MOSI=%d MISO=%d CS=%d DC=%d RST=%d; SPI=%lu Hz\n",
      setup.pin_tft_clk,
      setup.pin_tft_mosi,
      setup.pin_tft_miso,
      setup.pin_tft_cs,
      setup.pin_tft_dc,
      setup.pin_tft_rst,
      static_cast<unsigned long>(setup.tft_spi_freq) * 100000UL);
}

bool refreshSpotifyToken(const char* reason) {
  const size_t clientIdLength = strlen(SPOTIFY_CLIENT_ID);
  const size_t clientSecretLength = strlen(SPOTIFY_CLIENT_SECRET);
  const size_t refreshTokenLength = strlen(SPOTIFY_REFRESH_TOKEN);
  Serial.printf(
      "[Spotify] Token refresh (%s): client-id=%u chars, client-secret=%u chars, refresh-token=%u chars, RSSI=%d dBm\n",
      reason,
      static_cast<unsigned>(clientIdLength),
      static_cast<unsigned>(clientSecretLength),
      static_cast<unsigned>(refreshTokenLength),
      WiFi.RSSI());

  if (clientIdLength != 32 || clientSecretLength != 32 || refreshTokenLength < 80) {
    Serial.println("[Spotify] ERROR: credential lengths are invalid; check include/secrets.h");
    return false;
  }

  const uint32_t started = millis();
  const bool refreshed = spotify.refreshAccessToken();
  Serial.printf(
      "[Spotify] Token refresh result=%s after %lu ms\n",
      refreshed ? "success" : "FAILED",
      millis() - started);
  logMemory("after Spotify token refresh");
  return refreshed;
}

bool connectWiFi(const char* ssid, const char* password, const char* label) {
  if (ssid == nullptr || ssid[0] == '\0') {
    Serial.printf("[WiFi] Skipping %s: SSID is empty\n", label);
    return false;
  }

  WiFi.disconnect();
  delay(100);
  Serial.printf("[WiFi] Connecting to %s (%s)\n", label, ssid);
  if (password != nullptr && password[0] != '\0') WiFi.begin(ssid, password);
  else WiFi.begin(ssid);

  const uint32_t started = millis();
  uint32_t lastDot = started;
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(50);
    if (millis() - lastDot >= 500) {
      Serial.print('.');
      lastDot = millis();
    }
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] %s failed, status=%d\n", label, static_cast<int>(WiFi.status()));
    return false;
  }

  Serial.printf(
      "[WiFi] Connected to %s | IP=%s RSSI=%d dBm\n",
      label,
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI());
  return true;
}

void noteSpotifyNetworkSuccess() {
  consecutiveSpotifyNetworkFailures = 0;
  if (networkErrorVisible) {
    networkErrorVisible = false;
    // The next successful callback redraws the area used by the warning.
    forceRedraw = true;
    lastCheck = 0;
    Serial.println("[Network] Spotify connectivity restored");
  }
}

void handleSpotifyNetworkFailure(int status) {
  ++consecutiveSpotifyNetworkFailures;
  networkErrorVisible = true;
  forceRedraw = true;

  // Leave the last useful track information visible, but make it obvious that
  // it is stale rather than making the screen appear frozen.
  tft.fillRect(0, 278, 180, 30, TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 286);
  tft.printf("Network error %d - retrying", status);

  Serial.printf("[Network] Spotify transport failure %u/2 (status=%d, WiFi=%d, DNS=%s)\n",
                consecutiveSpotifyNetworkFailures,
                status,
                static_cast<int>(WiFi.status()),
                WiFi.dnsIP().toString().c_str());

  if (consecutiveSpotifyNetworkFailures < 2) return;

  Serial.println("[Network] Forcing WiFi reconnect to renew DHCP/DNS state");
  consecutiveSpotifyNetworkFailures = 0;
  client.stop();
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.reconnect();
  lastReconnectAttempt = millis();
}

// =========================================================================
//   JPEG DECODER CALLBACK
// =========================================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= getScreenHeight()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

// =========================================================================
//   SETUP
// =========================================================================
void setup() {
  // 1. PERFORMANCE: Max CPU Speed
  setCpuFrequencyMhz(240); 

  Serial.begin(115200);
  Serial.println("\n\n[BOOT] ESP32 Spotify Display");
  Serial.printf("[BOOT] CPU=%u MHz, SDK=%s\n", ESP.getCpuFreqMHz(), ESP.getSdkVersion());
  Serial.printf("[BOOT] PSRAM detected: %s\n", psramFound() ? "yes" : "no");
  heap_caps_register_failed_alloc_callback(onAllocationFailure);

  if (psramFound()) {
    // Arduino's 4096-byte default keeps Spotify's ~3 KB JSON document and
    // many TLS allocations in scarce internal RAM. Keep small objects fast,
    // but route larger ordinary malloc/calloc requests to PSRAM. Explicit DMA
    // allocations still remain internal as required by Wi-Fi and I2S.
    heap_caps_malloc_extmem_enable(PSRAM_MALLOC_THRESHOLD_BYTES);
    Serial.printf("[MEM] Ordinary allocations >= %u bytes prefer PSRAM\n",
                  static_cast<unsigned>(PSRAM_MALLOC_THRESHOLD_BYTES));
  }

  if (psramFound()) {
    jpgData = static_cast<uint8_t*>(ps_malloc(JPG_BUFFER_CAPACITY));
  }
  if (jpgData == nullptr) {
    Serial.println("[JPEG] ERROR: PSRAM album-art buffer unavailable; artwork disabled to preserve Bluetooth/Wi-Fi memory");
  } else {
    Serial.printf("[JPEG] Allocated %u-byte album-art buffer\n", JPG_BUFFER_CAPACITY);
  }
  logMemory("after JPEG buffer allocation");

  // Setup Boot Button
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);

  // 2. Setup Display
  tft.init();
  tft.setRotation(1); 
  tft.fillScreen(TFT_BLACK);
  logTftSetup();
  Serial.printf("[Display] initialized at %dx%d, rotation=1\n", tft.width(), tft.height());
  
  u8f.begin(tft);                 
  u8f.setFontMode(1); 
  u8f.setFontDirection(0);        
  u8f.setForegroundColor(TFT_WHITE);
  u8f.setBackgroundColor(TFT_BLACK); 

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Connecting...");
  
  TJpgDec.setJpgScale(1);      
  TJpgDec.setSwapBytes(true);  
  TJpgDec.setCallback(tft_output);

  // Bluetooth audio and the matrix use only non-strapping GPIOs and do not
  // overlap the TFT SPI bus, WROVER PSRAM, or flash pins.
  beginAudioVisualizer();
  logMemory("after Bluetooth audio setup");

  // Spotify and artwork endpoints use HTTPS. This project intentionally uses
  // an insecure TLS client because it does not bundle root CA certificates.
  client.setInsecure();
  // WiFiClientSecure expects seconds; HTTPClient and Stream timeouts use ms.
  client.setTimeout(NETWORK_TIMEOUT_SECONDS);
  client.setHandshakeTimeout(NETWORK_TIMEOUT_SECONDS);

  // 3. Setup WiFi
  WiFi.mode(WIFI_STA);
  bool connected = connectWiFi(WIFI_SSID_MAIN, WIFI_PASSWORD_MAIN, "primary");
  if (!connected) connected = connectWiFi(WIFI_SSID_1, WIFI_PASSWORD_1, "backup");
  if (!connected) connected = connectWiFi(WIFI_SSID_PUB, nullptr, "public");

  if (!connected) {
    Serial.println("[WiFi] No configured network connected.");
    Serial.println("[WiFi] Send an SSID followed by Enter within 30 seconds, or reset to retry.");
    Serial.setTimeout(30000);
    String wifi = Serial.readStringUntil('\n');
    wifi.trim();
    if (!wifi.isEmpty()) {
      Serial.println("[WiFi] Send the password followed by Enter (blank for open WiFi).");
      String pass = Serial.readStringUntil('\n');
      pass.trim();
      connected = connectWiFi(wifi.c_str(), pass.c_str(), "serial entry");
    }
    Serial.setTimeout(1000);
  }

  if (!connected) {
    Serial.println("[WiFi] ERROR: continuing offline; automatic reconnect will retry.");
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 10);
    tft.println("WiFi Failed");
    return;
  }

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("WiFi Connected");
  
  // 4. Setup Spotify API
  tft.println("Auth Spotify...");
  spotifyAuthenticated = refreshSpotifyToken("startup");
  if (spotifyAuthenticated) {
    Serial.println("[Spotify] Access token refreshed");
    tft.println("Ready!");
    delay(1000);
    tft.fillScreen(TFT_BLACK);
  } else {
    Serial.println("[Spotify] ERROR: access-token refresh failed");
    tft.setTextColor(TFT_RED);
    tft.println("Auth Failed!");
  }
  logMemory("setup complete");
}

// =========================================================================
//   MAIN LOOP
// =========================================================================
void loop() {
  updateAudioVisualizer();
  handleButtons();
  updateScreensaver();

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnectAttempt >= 10000) {
      lastReconnectAttempt = millis();
      Serial.printf("[WiFi] Disconnected, reconnecting (status=%d)\n", static_cast<int>(WiFi.status()));
      WiFi.reconnect();
    }
    delay(10);
    return;
  }

  if (!spotifyAuthenticated) {
    Serial.println("[Spotify] Retrying access-token refresh");
    spotifyAuthenticated = refreshSpotifyToken("retry after failure/reconnect");
    if (!spotifyAuthenticated) {
      Serial.println("[Spotify] Token refresh still failing; retrying in 3 seconds");
      delay(3000);
      return;
    }
    Serial.println("[Spotify] Access token refreshed after reconnect");
    lastCheck = 0;
  }

  if (serviceDeferredNetworkTasks()) return;

  // 1. Refresh Data (Every 3 seconds OR immediately if forced)
  if (!albumArtRequestPending && !lyricsRequestPending &&
      ((millis() - lastCheck > SPOTIFY_POLL_INTERVAL_MS) || lastCheck == 0)) {
    lastCheck = millis();
    
    Serial.println("[Spotify] Polling currently-playing endpoint");
    int status = spotify.getCurrentlyPlaying(printCurrentlyPlaying, SPOTIFY_MARKET);
    Serial.printf("[Spotify] Poll complete, status=%d (library client cleanup may print 'Closing client')\n", status);
    
    if (status == 200) {
      noteSpotifyNetworkSuccess();
      // The Spotify TLS connection and response JSON are now out of the
      // callback path, so the image/lyrics requests can safely use the heap.
      processPendingTrackResources();
    } else if (status == 204) {
      noteSpotifyNetworkSuccess();
      isSpotifyPlaying = false;
      setScreensaverActive(true);
    } else if (status == 401) {
      Serial.println("[Spotify] Token expired (401); refreshing");
      spotifyAuthenticated = refreshSpotifyToken("API returned 401");
    } else {
      Serial.printf("[Spotify] Request failed, HTTP/status=%d\n", status);
      if (status < 0) handleSpotifyNetworkFailure(status);
    }
  }

  // 2. Update Progress Bar & Lyrics
  if (isSpotifyPlaying && songDuration > 0) {
    if (millis() - lastProgressBarUpdate > UI_UPDATE_INTERVAL_MS) {
      lastProgressBarUpdate = millis();
      updateProgressBar();
      if (isKaraokeMode) updateKaraokeScroll();
      else updateLyrics();
    }
  }
}

// =========================================================================
//   LYRICS & TEXT HELPERS
// =========================================================================
String urlEncode(const String& str) {
    String encodedString = "";
    encodedString.reserve(str.length() * 3); 
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = static_cast<unsigned char>(str.charAt(i));
        if (c == ' ') encodedString += "%20";
        else if (isalnum(c)) encodedString += static_cast<char>(c);
        else {
            char code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
            c = (c >> 4) & 0xf;
            char code0 = c + '0';
            if (c > 9) code0 = c - 10 + 'A';
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

void parseLrc(const char* lrcContent) {
  currentLyrics.clear();
  if (currentLyrics.capacity() < 50) currentLyrics.reserve(50);

  if (lrcContent == nullptr) return;
  const char* cursor = lrcContent;
  while (*cursor != '\0') {
    const char* lineEnd = strchr(cursor, '\n');
    const size_t lineLength = lineEnd == nullptr ? strlen(cursor) : static_cast<size_t>(lineEnd - cursor);
    String line;
    line.reserve(lineLength);
    line.concat(cursor, static_cast<unsigned int>(lineLength));
    line.trim();
    
    if (line.startsWith("[") && line.indexOf("]") > 0) {
      int bracketEnd = line.indexOf("]");
      String timePart = line.substring(1, bracketEnd);
      String textPart = line.substring(bracketEnd + 1);
      textPart.trim();
      
      int colonIndex = timePart.indexOf(":");
      int dotIndex = timePart.indexOf(".");
      
      if (colonIndex > 0) {
        long min = timePart.substring(0, colonIndex).toInt();
        long sec = 0;
        long ms = 0;
        if (dotIndex > 0) {
           sec = timePart.substring(colonIndex + 1, dotIndex).toInt();
           String msPart = timePart.substring(dotIndex + 1);
           if (msPart.length() == 2) ms = msPart.toInt() * 10;
           else if (msPart.length() == 3) ms = msPart.toInt();
        } else {
           sec = timePart.substring(colonIndex + 1).toInt();
        }
        long totalMs = (min * 60000) + (sec * 1000) + ms;
        currentLyrics.push_back({totalMs, textPart});
      }
    }
    if (lineEnd == nullptr) break;
    cursor = lineEnd + 1;
  }

  Serial.printf("[Lyrics] Parsed %u synchronized lines\n", static_cast<unsigned>(currentLyrics.size()));
}

void fetchLyrics(const String& trackName, const String& artistName) {
  hasLyrics = false;
  currentLyricIndex = -1;
  currentLyrics.clear();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Lyrics] Skipped: WiFi is disconnected");
    return;
  }

  String url;
  url.reserve(trackName.length() * 3 + artistName.length() * 3 + 64);
  url = "https://lrclib.net/api/get?artist_name=";
  url += urlEncode(artistName);
  url += "&track_name=";
  url += urlEncode(trackName);

  WiFiClientSecure lyricsClient;
  lyricsClient.setInsecure();
  lyricsClient.setTimeout(LYRICS_TIMEOUT_SECONDS);
  lyricsClient.setHandshakeTimeout(LYRICS_TIMEOUT_SECONDS);
  HTTPClient http;
  http.setConnectTimeout(LYRICS_TIMEOUT_MS);
  http.setTimeout(LYRICS_TIMEOUT_MS);

  Serial.printf("[Lyrics] Requesting lyrics for \"%s\" by \"%s\"\n", trackName.c_str(), artistName.c_str());
  if (!http.begin(lyricsClient, url)) {
    Serial.println("[Lyrics] ERROR: unable to initialize HTTPS request");
    return;
  }

  const int httpCode = http.GET();
  Serial.printf("[Lyrics] HTTP status=%d, content-length=%d\n", httpCode, http.getSize());
  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    filter["syncedLyrics"] = true;
    JsonDocument doc;
    const DeserializationError error = deserializeJson(
        doc,
        http.getStream(),
        DeserializationOption::Filter(filter));
    if (error) {
      Serial.printf("[Lyrics] ERROR: JSON parsing failed: %s\n", error.c_str());
    } else {
      const char* rawLrc = doc["syncedLyrics"].as<const char*>();
      if (rawLrc != nullptr && rawLrc[0] != '\0') {
        parseLrc(rawLrc);
        hasLyrics = !currentLyrics.empty();
      } else {
        Serial.println("[Lyrics] No synchronized lyrics in response");
      }
    }
  } else if (httpCode == HTTP_CODE_NOT_FOUND) {
    Serial.println("[Lyrics] No lyrics found");
  } else {
    Serial.printf("[Lyrics] ERROR: request failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
  logMemory("after lyrics request");
}

String truncateText(const String& text, int maxWidth) {
  if (u8f.getUTF8Width(text.c_str()) <= maxWidth) return text;
  String result = text;
  const int ellipsisWidth = u8f.getUTF8Width("...");
  while (result.length() > 0 && u8f.getUTF8Width(result.c_str()) + ellipsisWidth > maxWidth) {
    int charStart = result.length() - 1;
    while (charStart > 0 && (static_cast<uint8_t>(result[charStart]) & 0xC0) == 0x80) charStart--;
    result.remove(charStart);
  }
  result += "...";
  return result;
}

int drawWrappedText(const String& text, int x, int y, int maxWidth, int lineHeight, int maxLines) {
  int currentY = y; 
  String currentLine;
  currentLine.reserve(text.length()); 
  int lineCount = 0;
  int currentWidth = 0;
  
  for (size_t i = 0; i < text.length();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    size_t glyphLength = 1;
    if ((first & 0xE0) == 0xC0) glyphLength = 2;
    else if ((first & 0xF0) == 0xE0) glyphLength = 3;
    else if ((first & 0xF8) == 0xF0) glyphLength = 4;
    if (i + glyphLength > text.length()) glyphLength = 1;

    char glyph[5] = {0};
    for (size_t j = 0; j < glyphLength; j++) glyph[j] = text[i + j];
    i += glyphLength;
    const int glyphWidth = u8f.getUTF8Width(glyph);

    if (currentWidth + glyphWidth > maxWidth && !currentLine.isEmpty()) {
      if (lineCount >= maxLines - 1) {
        const String textToPrint = truncateText(currentLine, maxWidth);
        u8f.setCursor(x, currentY);
        u8f.print(textToPrint.c_str());
        return currentY + lineHeight;
      }

      const int lastSpace = currentLine.lastIndexOf(' ');
      if (lastSpace > 0 && lastSpace > static_cast<int>(currentLine.length() / 2)) {
        const String lineToPrint = currentLine.substring(0, lastSpace);
        u8f.setCursor(x, currentY);
        u8f.print(lineToPrint.c_str());
        currentLine.remove(0, lastSpace + 1);
        currentLine.concat(glyph, glyphLength);
        currentWidth = u8f.getUTF8Width(currentLine.c_str());
      } else {
        u8f.setCursor(x, currentY);
        u8f.print(currentLine.c_str());
        currentLine = glyph;
        currentWidth = glyphWidth;
      }
      currentY += lineHeight;
      lineCount++;
      if (currentY > getScreenHeight() - 25) return currentY;
    } else {
      currentLine.concat(glyph, glyphLength);
      currentWidth += glyphWidth;
    }
  }
  
  if (currentLine.length() > 0 && lineCount < maxLines) {
    u8f.setCursor(x, currentY);
    u8f.print(currentLine.c_str());
    return currentY + lineHeight;
  }
  return currentY;
}

// =========================================================================
//   STANDARD VIEW DRAWING
// =========================================================================
void drawSongInfo(const CurrentlyPlaying& currentlyPlaying) {
  u8f.setFont(u8g2_font_wqy16_t_gb2312); 
  u8f.setForegroundColor(TFT_WHITE);
  u8f.setBackgroundColor(TFT_BLACK);
  
  int cursorY = 25; 
  const String title(currentlyPlaying.trackName == nullptr ? "Unknown track" : currentlyPlaying.trackName);
  cursorY = drawWrappedText(title, TEXT_X, cursorY, TEXT_W, 24, 3);
  cursorY += 5; 

  if (currentlyPlaying.numArtists > 0 && currentlyPlaying.artists[0].artistName != nullptr) {
    const String artist(currentlyPlaying.artists[0].artistName);
    cursorY = drawWrappedText(artist, TEXT_X, cursorY, TEXT_W, 24, 2);
  }
  cursorY += 5; 

  const String album(currentlyPlaying.albumName == nullptr ? "Unknown album" : currentlyPlaying.albumName);
  const String albumStr = truncateText(album, TEXT_W);
  u8f.setForegroundColor(TFT_WHITE); 
  u8f.setCursor(TEXT_X, cursorY);
  u8f.print(albumStr.c_str());
  cursorY += 24; 

  lyricY = cursorY + 30;
}

int findActiveLyricIndex(long currentMs) {
  int low = 0;
  int high = static_cast<int>(currentLyrics.size()) - 1;
  int result = -1;
  while (low <= high) {
    const int middle = low + (high - low) / 2;
    if (currentLyrics[middle].timestamp <= currentMs) {
      result = middle;
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }
  return result;
}

void updateLyrics() {
  if (!hasLyrics || currentLyrics.empty()) return;
  const long currentMs = songProgress + (millis() - lastSongFetchTime);
  const int activeIndex = findActiveLyricIndex(currentMs);
  
  if (activeIndex != -1 && activeIndex != currentLyricIndex) {
    currentLyricIndex = activeIndex;
    const String& text = currentLyrics[activeIndex].text;
    int fontHeight = 16; 
    int clearY = lyricY - fontHeight;
    int clearH = (getScreenHeight() - 10) - clearY;
    if (clearH > 0) {
      tft.fillRect(TEXT_X, clearY, TEXT_W, clearH, BACKGROUND_COLOR); 
      if (text.length() > 0) {
         u8f.setFont(u8g2_font_wqy16_t_gb2312); 
         u8f.setForegroundColor(TFT_WHITE);
         u8f.setBackgroundColor(TFT_BLACK); 
         drawWrappedText(text, TEXT_X, lyricY, TEXT_W, 24, 5); 
      }
    }
  }
}

// =========================================================================
//   KARAOKE VIEW DRAWING
// =========================================================================
void drawKaraokeHeader(const CurrentlyPlaying& currentlyPlaying) {
  u8f.setFont(u8g2_font_wqy16_t_gb2312); 
  u8f.setForegroundColor(TFT_WHITE);
  u8f.setBackgroundColor(TFT_BLACK); 
  
  int y = 30;
  const String title(currentlyPlaying.trackName == nullptr ? "Unknown track" : currentlyPlaying.trackName);
  String artist;
  if (currentlyPlaying.numArtists > 0 && currentlyPlaying.artists[0].artistName != nullptr) {
    artist = currentlyPlaying.artists[0].artistName;
  }
  
  int titleW = u8f.getUTF8Width(title.c_str());
  int titleX = (tft.width() - titleW) / 2;
  if (titleX < 0) titleX = 0; 
  u8f.setCursor(titleX, y);
  u8f.print(title.c_str());
  
  y += 25;
  
  u8f.setForegroundColor(0xDDDD); 
  int artW = u8f.getUTF8Width(artist.c_str());
  int artX = (tft.width() - artW) / 2;
  if (artX < 0) artX = 0; 
  u8f.setCursor(artX, y);
  u8f.print(artist.c_str());
  
  tft.drawFastHLine(20, y + 15, tft.width() - 40, TFT_DARKGREY);
}

void updateKaraokeScroll() {
  if (!hasLyrics || currentLyrics.empty()) return;
  
  const long currentMs = songProgress + (millis() - lastSongFetchTime);
  const int activeIndex = findActiveLyricIndex(currentMs);
  
  if (activeIndex != currentLyricIndex) {
    currentLyricIndex = activeIndex;
    
    int lyricAreaTop = 71; 
    tft.fillRect(0, lyricAreaTop, tft.width(), getScreenHeight() - lyricAreaTop - 10, TFT_BLACK);
    
    int lineHeight = 30; 
    int centerY = 180;   
    int fontAscent = 20; 
    
    u8f.setFont(u8g2_font_wqy16_t_gb2312);
    u8f.setBackgroundColor(TFT_BLACK); 
    
    for (int i = activeIndex - 4; i <= activeIndex + 4; i++) {
      if (i >= 0 && i < static_cast<int>(currentLyrics.size())) {
        int yPos = centerY + ((i - activeIndex) * lineHeight);
        int lineTop = yPos - fontAscent;
        
        if (lineTop > lyricAreaTop && yPos < getScreenHeight() - 15) {
          const String& txt = currentLyrics[i].text;
          int txtW = u8f.getUTF8Width(txt.c_str());
          int txtX = (tft.width() - txtW) / 2;
          if (txtX < 10) txtX = 10;
          
          if (i == activeIndex) {
            u8f.setForegroundColor(TFT_YELLOW); 
          } else {
            u8f.setForegroundColor(TFT_LIGHTGREY); 
          }
          
          u8f.setCursor(txtX, yPos);
          u8f.print(txt.c_str());
        }
      }
    }
  }
}

// =========================================================================
//   BUTTON LOGIC
// =========================================================================
void handleButtons() {
  if (millis() - lastButtonPress > 200) {
    // External GPIO33-to-GND button toggles the display mode.
    if (digitalRead(MODE_BUTTON_PIN) == LOW) {
      isKaraokeMode = !isKaraokeMode;
      Serial.printf("[UI] GPIO33 mode button: switched to %s mode\n", isKaraokeMode ? "karaoke" : "standard");
      forceRedraw = true; 
      lastCheck = 0;
      lastButtonPress = millis();
    }
  }
}

// =========================================================================
//   DISPLAY LOGIC
// =========================================================================
void setScreensaverActive(bool active) {
  if (screensaverActive == active) return;
  screensaverActive = active;
  tft.fillScreen(TFT_BLACK);

  if (active) {
    // Do not let a cover or lyrics request repaint the display after Spotify
    // has become idle. The next playing callback queues fresh resources.
    pendingTrackResources = false;
    albumArtRequestPending = false;
    lyricsRequestPending = false;
    queuedAlbumArtUrl = "";
    queuedLyricsTrack = "";
    queuedLyricsArtist = "";
    screensaverX = 16;
    screensaverY = 16;
    screensaverDx = 3;
    screensaverDy = 2;
    lastScreensaverFrameAt = 0;
    Serial.println("[Display] Spotify idle; screensaver started");
  } else {
    Serial.println("[Display] Spotify playback resumed; screensaver stopped");
  }
}

void updateScreensaver() {
  if (!screensaverActive ||
      millis() - lastScreensaverFrameAt < SCREENSAVER_FRAME_INTERVAL_MS) return;
  lastScreensaverFrameAt = millis();

  // Erase only the previous badge instead of repainting the whole display.
  tft.fillRoundRect(screensaverX, screensaverY,
                    SCREENSAVER_WIDTH, SCREENSAVER_HEIGHT, 8, TFT_BLACK);

  screensaverX += screensaverDx;
  screensaverY += screensaverDy;
  const int16_t maxX = tft.width() - SCREENSAVER_WIDTH;
  const int16_t maxY = tft.height() - SCREENSAVER_HEIGHT;
  if (screensaverX <= 0 || screensaverX >= maxX) {
    screensaverX = constrain(screensaverX, 0, maxX);
    screensaverDx = -screensaverDx;
  }
  if (screensaverY <= 0 || screensaverY >= maxY) {
    screensaverY = constrain(screensaverY, 0, maxY);
    screensaverDy = -screensaverDy;
  }

  tft.fillRoundRect(screensaverX, screensaverY,
                    SCREENSAVER_WIDTH, SCREENSAVER_HEIGHT, 8, TFT_DARKGREEN);
  tft.drawRoundRect(screensaverX, screensaverY,
                    SCREENSAVER_WIDTH, SCREENSAVER_HEIGHT, 8, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.setTextSize(2);
  tft.setCursor(screensaverX + 10, screensaverY + 13);
  tft.print("Spotify idle");
}

void printCurrentlyPlaying(CurrentlyPlaying currentlyPlaying) {
  const bool wasSpotifyPlaying = isSpotifyPlaying;
  isSpotifyPlaying = currentlyPlaying.isPlaying;
  songDuration = currentlyPlaying.durationMs;
  songProgress = currentlyPlaying.progressMs;
  lastSongFetchTime = millis();

  const char* trackUri = currentlyPlaying.trackUri == nullptr ? "" : currentlyPlaying.trackUri;
  const char* trackName = currentlyPlaying.trackName == nullptr ? "Unknown track" : currentlyPlaying.trackName;
  const char* albumName = currentlyPlaying.albumName == nullptr ? "Unknown album" : currentlyPlaying.albumName;
  const char* artistName = "";
  if (currentlyPlaying.numArtists > 0 && currentlyPlaying.artists[0].artistName != nullptr) {
    artistName = currentlyPlaying.artists[0].artistName;
  }

  const bool isNewTrack = !lastTrackURI.equals(trackUri);
  if (!isSpotifyPlaying) {
    // Remember the paused track so resuming it is treated as a redraw rather
    // than an unnecessary new-track cover download.
    lastTrackURI = trackUri;
    setScreensaverActive(true);
    return;
  }

  if (!wasSpotifyPlaying || screensaverActive) forceRedraw = true;
  setScreensaverActive(false);
  if (!isNewTrack && !forceRedraw) return;

  lastTrackURI = trackUri;
  currentLyricIndex = -1;
  Serial.printf(
      "[Spotify] %s | track=\"%s\" artist=\"%s\" album=\"%s\" playing=%s progress=%ld/%ld ms mode=%s\n",
      isNewTrack ? "New track" : "Redraw",
      trackName,
      artistName[0] == '\0' ? "Unknown artist" : artistName,
      albumName,
      currentlyPlaying.isPlaying ? "yes" : "no",
      songProgress,
      songDuration,
      isKaraokeMode ? "karaoke" : "standard");

  tft.fillRect(0, 0, tft.width(), getScreenHeight() - 10, BACKGROUND_COLOR);

  if (isNewTrack) {
    lastBarWidth = 0;
    tft.fillRect(0, getScreenHeight() - 10, tft.width(), 10, BACKGROUND_COLOR);
  }

  forceRedraw = false;

  if (isKaraokeMode) {
    drawKaraokeHeader(currentlyPlaying);
  } else {
    drawSongInfo(currentlyPlaying);
    tft.fillRect(IMG_X, IMG_Y, 300, 300, TFT_DARKGREY);
    tft.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(225, 140);
    tft.print("Loading cover...");
  }

  const char* albumArtUrl = nullptr;
  if (currentlyPlaying.numImages > 1) albumArtUrl = currentlyPlaying.albumImages[1].url;
  else if (currentlyPlaying.numImages > 0) albumArtUrl = currentlyPlaying.albumImages[0].url;
  pendingAlbumArtUrl = albumArtUrl == nullptr ? "" : albumArtUrl;
  pendingLyricsTrack = trackName;
  pendingLyricsArtist = artistName;
  pendingAlbumArtDraw = !isKaraokeMode;
  pendingTrackResources = true;

  // Keep the interface responsive while the callback completes.
  updateProgressBar();
}

void processPendingTrackResources() {
  if (!pendingTrackResources) return;
  pendingTrackResources = false;

  queuedAlbumArtUrl = pendingAlbumArtUrl;
  queuedLyricsTrack = pendingLyricsTrack;
  queuedLyricsArtist = pendingLyricsArtist;
  pendingAlbumArtUrl = "";
  pendingLyricsTrack = "";
  pendingLyricsArtist = "";

  albumArtAttempt = 0;
  albumArtRequestPending = pendingAlbumArtDraw && !queuedAlbumArtUrl.isEmpty();
  lyricsRequestPending = !queuedLyricsArtist.isEmpty();
  nextAlbumArtAttemptAt = millis();
  nextLyricsRequestAt = albumArtRequestPending ? UINT32_MAX : millis() + 250;

  if (pendingAlbumArtDraw && queuedAlbumArtUrl.isEmpty()) {
    Serial.println("[JPEG] No album-art URL in Spotify response");
  }
  if (queuedLyricsArtist.isEmpty()) {
    hasLyrics = false;
    currentLyrics.clear();
    Serial.println("[Lyrics] Skipped: Spotify response has no artist");
  }
  Serial.printf("[Network] Queued cover=%s lyrics=%s\n",
                albumArtRequestPending ? "yes" : "no",
                lyricsRequestPending ? "yes" : "no");
}

bool serviceDeferredNetworkTasks() {
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint32_t now = millis();

  if (albumArtRequestPending && static_cast<int32_t>(now - nextAlbumArtAttemptAt) >= 0) {
    ++albumArtAttempt;
    Serial.printf("[JPEG] Attempt %u/%u\n", albumArtAttempt, MAX_ALBUM_ART_ATTEMPTS);
    logMemory("before album-art attempt");
    if (drawAlbumArt(queuedAlbumArtUrl, IMG_X, IMG_Y)) {
      albumArtRequestPending = false;
      queuedAlbumArtUrl = "";
      nextLyricsRequestAt = millis() + 250;
    } else if (albumArtAttempt < MAX_ALBUM_ART_ATTEMPTS) {
      const uint32_t retryDelay = albumArtAttempt == 1 ? 2000 : 5000;
      nextAlbumArtAttemptAt = millis() + retryDelay;
      Serial.printf("[JPEG] Retry scheduled in %lu ms\n", retryDelay);
    } else {
      albumArtRequestPending = false;
      queuedAlbumArtUrl = "";
      nextLyricsRequestAt = millis() + 250;
      tft.fillRect(IMG_X, IMG_Y, 300, 300, TFT_DARKGREY);
      tft.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
      tft.setTextSize(2);
      tft.setCursor(220, 140);
      tft.print("Cover unavailable");
      Serial.println("[JPEG] All cover attempts failed");
    }
    return true;
  }

  if (lyricsRequestPending && !albumArtRequestPending &&
      static_cast<int32_t>(now - nextLyricsRequestAt) >= 0) {
    lyricsRequestPending = false;
    fetchLyrics(queuedLyricsTrack, queuedLyricsArtist);
    queuedLyricsTrack = "";
    queuedLyricsArtist = "";
    logMemory("track resources complete");
    return true;
  }
  return false;
}

void updateProgressBar() {
  if (songDuration <= 0) return;
  long estimatedProgress = songProgress + (millis() - lastSongFetchTime);
  if (estimatedProgress < 0) estimatedProgress = 0;
  if (estimatedProgress > songDuration) estimatedProgress = songDuration;

  const int barWidth = tft.width();
  constexpr int barHeight = 6;
  const int barY = getScreenHeight() - 10;
  const int fillWidth = static_cast<int>(
      (static_cast<uint64_t>(estimatedProgress) * barWidth) / static_cast<uint64_t>(songDuration));
  
  if (fillWidth == lastBarWidth) return; 

  if (fillWidth > lastBarWidth) {
    tft.fillRect(lastBarWidth, barY, fillWidth - lastBarWidth, barHeight, DOMINANT_COLOR);
  } else {
    tft.fillRect(0, barY, fillWidth, barHeight, DOMINANT_COLOR);
    tft.fillRect(fillWidth, barY, barWidth - fillWidth, barHeight, BACKGROUND_COLOR);
  }
  lastBarWidth = fillWidth;
}

// =========================================================================
//   IMAGE DOWNLOADER (BUFFERED)
// =========================================================================
bool drawAlbumArt(const String& url, int xPos, int yPos) {
  if (jpgData == nullptr) {
    Serial.println("[JPEG] ERROR: buffer is unavailable");
    return false;
  }

  const int schemeEnd = url.indexOf("://");
  const int hostStart = schemeEnd < 0 ? 0 : schemeEnd + 3;
  const int pathIndex = url.indexOf('/', hostStart);
  if (pathIndex < 0 || pathIndex == hostStart) {
    Serial.printf("[JPEG] ERROR: invalid album-art URL: %s\n", url.c_str());
    return false;
  }
  const String host = url.substring(hostStart, pathIndex);
  const String path = url.substring(pathIndex);

  WiFiClientSecure imgClient;
  imgClient.setInsecure();
  imgClient.setTimeout(IMAGE_TIMEOUT_SECONDS);
  imgClient.setHandshakeTimeout(IMAGE_TIMEOUT_SECONDS);
  const uint32_t started = millis();
  Serial.printf("[JPEG] Connecting to %s\n", host.c_str());
  if (!imgClient.connect(host.c_str(), 443)) {
    Serial.printf("[JPEG] ERROR: TLS connection to %s failed after %lu ms\n", host.c_str(), millis() - started);
    return false;
  }

  imgClient.printf(
      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ESP32-Spotify-Display\r\nConnection: close\r\n\r\n",
      path.c_str(),
      host.c_str());

  const String statusLine = imgClient.readStringUntil('\n');
  int httpStatus = 0;
  if (statusLine.length() >= 12 && statusLine.startsWith("HTTP/")) {
    httpStatus = atoi(statusLine.c_str() + 9);
  }
  int contentLength = -1;
  bool chunked = false;
  while (imgClient.connected() || imgClient.available()) {
    String line = imgClient.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    if (line.startsWith("Content-Length:")) {
      line.remove(0, 15);
      line.trim();
      contentLength = line.toInt();
    } else if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }

  Serial.printf("[JPEG] HTTP status=%d, content-length=%d, chunked=%s\n", httpStatus, contentLength, chunked ? "yes" : "no");
  if (httpStatus != 200) {
    Serial.println("[JPEG] ERROR: image server returned a non-200 response");
    imgClient.stop();
    return false;
  }
  if (chunked) {
    Serial.println("[JPEG] ERROR: chunked image responses are not supported");
    imgClient.stop();
    return false;
  }
  if (contentLength > static_cast<int>(JPG_BUFFER_CAPACITY)) {
    Serial.printf("[JPEG] ERROR: image is larger than the %u-byte buffer\n", JPG_BUFFER_CAPACITY);
    imgClient.stop();
    return false;
  }

  jpgDataSize = 0;
  uint32_t lastDataAt = millis();
  while ((imgClient.connected() || imgClient.available()) &&
         jpgDataSize < JPG_BUFFER_CAPACITY &&
         (contentLength < 0 || jpgDataSize < static_cast<size_t>(contentLength))) {
    const int available = imgClient.available();
    if (available > 0) {
      size_t wanted = min(static_cast<size_t>(available), JPG_BUFFER_CAPACITY - jpgDataSize);
      if (contentLength >= 0) {
        wanted = min(wanted, static_cast<size_t>(contentLength) - jpgDataSize);
      }
      const size_t received = imgClient.readBytes(jpgData + jpgDataSize, wanted);
      jpgDataSize += received;
      lastDataAt = millis();
    } else {
      if (millis() - lastDataAt >= IMAGE_TIMEOUT_MS) {
        Serial.println("[JPEG] ERROR: download timed out");
        break;
      }
      delay(1);
    }
  }
  imgClient.stop();

  if (contentLength >= 0 && jpgDataSize != static_cast<size_t>(contentLength)) {
    Serial.printf("[JPEG] ERROR: incomplete download (%u/%d bytes)\n", jpgDataSize, contentLength);
    return false;
  }
  if (jpgDataSize >= JPG_BUFFER_CAPACITY && contentLength < 0) {
    Serial.printf("[JPEG] ERROR: download reached the %u-byte safety limit\n", JPG_BUFFER_CAPACITY);
    return false;
  }
  if (jpgDataSize < 2 || jpgData[0] != 0xFF || jpgData[1] != 0xD8) {
    Serial.printf("[JPEG] ERROR: response is not a valid JPEG (%u bytes)\n", jpgDataSize);
    return false;
  }

  Serial.printf("[JPEG] Downloaded %u bytes in %lu ms; decoding\n", jpgDataSize, millis() - started);
  const JRESULT result = TJpgDec.drawJpg(xPos, yPos, jpgData, jpgDataSize);
  Serial.printf("[JPEG] Decode result=%d\n", static_cast<int>(result));
  logMemory("after album art");
  return result == JDR_OK;
}
