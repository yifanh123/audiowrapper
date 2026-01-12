/*
  Spotify Album Art Display (No Audio)
  
  REQUIRED LIBRARIES:
  1. TFT_eSPI
  2. TJpg_Decoder
  3. U8g2_for_TFT_eSPI
  4. SpotifyArduino
  5. ArduinoJson

  NOTE: 
  - Bluetooth/Audio removed. This is a display-only device.
  - Use the built-in BOOT button (Pin 0) to toggle Karaoke Mode.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>         
#include <SpotifyArduino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>       
#include <TJpg_Decoder.h>
#include <U8g2_for_TFT_eSPI.h> 
#include <vector>
#include "secrets.h"

// --- DISPLAY SETTINGS ---
TFT_eSPI tft = TFT_eSPI(); 
U8g2_for_TFT_eSPI u8f; 

#define FORCE_SCREEN_HEIGHT 320 

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

// --- MEMORY OPTIMIZATION ---
std::vector<uint8_t> jpgData; 

// --- GLOBAL VARIABLES ---
unsigned long lastCheck = 0;
unsigned long lastButtonPress = 0;
unsigned long lastProgressBarUpdate = 0;
int currentVolume = 30; // Volume is now controlled by phone, this tracks local display if needed
bool isSpotifyPlaying = false;
String lastTrackURI = ""; 
bool forceRedraw = false; 

// Mode State
bool isKaraokeMode = false; 

// Progress Tracking
long songDuration = 0;
long songProgress = 0;
unsigned long lastSongFetchTime = 0;
int lastBarWidth = 0; 

// --- COLORS ---
uint16_t dominantColor = 0x1DB9; 
uint16_t backgroundColor = TFT_BLACK;

// --- LAYOUT CONSTANTS ---
const int IMG_X = 180;
const int IMG_Y = 0; 
const int TEXT_X = 10;
const int TEXT_W = 160; 
int lyricY = 180; 

// --- PIN DEFINITIONS ---
// Use built-in BOOT button for Karaoke toggle.
#define BOOT_BUTTON 0 

//Helper to get correct height
int getScreenHeight() {
  if (FORCE_SCREEN_HEIGHT > 0) return FORCE_SCREEN_HEIGHT;
  return tft.height();
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
  Serial.println("\n\n--- ESP32 Spotify Display ---");

  // UPDATE: Reduced buffer slightly to 70KB to free up RAM for Network/SSL
  // This balances "High Detail Images" vs "Stable Connection"
  jpgData.reserve(70000); 

  // Setup Boot Button
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  // 2. Setup Display
  tft.init();
  tft.setRotation(1); 
  tft.fillScreen(TFT_BLACK);
  
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

  // 3. Setup WiFi
  WiFi.mode(WIFI_STA);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int temp = 0;
  while (WiFi.status() != WL_CONNECTED && temp <= 20) {
    delay(500);
    Serial.print(".");
    temp++;
  }
  if (temp > 20) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID_PUB);
    Serial.print("WEB");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
  }

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("WiFi Connected");
  
  // 4. Setup Spotify API
  client.setInsecure(); 
  tft.println("Auth Spotify...");
  if (spotify.refreshAccessToken()) {
    Serial.println("Token refreshed!");
    tft.println("Ready!");
    delay(1000);
    tft.fillScreen(TFT_BLACK);
  } else {
    tft.setTextColor(TFT_RED);
    tft.println("Auth Failed!");
  }
}

// =========================================================================
//   MAIN LOOP
// =========================================================================
void loop() {
  handleButtons();

  // 1. Refresh Data (Every 3 seconds OR immediately if forced)
  if ((millis() - lastCheck > 3000) || lastCheck == 0) { 
    lastCheck = millis();
    
    int status = spotify.getCurrentlyPlaying(printCurrentlyPlaying, SPOTIFY_MARKET);
    
    if (status == 200) {
      // Normal operation, data handled in callback
    } else if (status == 204) {
      if (isSpotifyPlaying) { 
        isSpotifyPlaying = false;
        tft.fillScreen(TFT_BLACK);
        u8f.setFont(u8g2_font_helvB14_tf); 
        u8f.setCursor(10, 100);
        u8f.print("Paused / Idle");
      }
    } else if (status == 401) {
      Serial.println("Token expired (401). Refreshing...");
      spotify.refreshAccessToken();
    } else {
      // DEBUG: Print other errors (e.g. -1 = connection fail, 429 = rate limit)
      Serial.print("Spotify Error: ");
      Serial.println(status);
    }
  }

  // 2. Update Progress Bar & Lyrics
  if (isSpotifyPlaying && songDuration > 0) {
    if (millis() - lastProgressBarUpdate > 100) {
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
String urlEncode(String str) {
    String encodedString = "";
    encodedString.reserve(str.length() * 3); 
    char c;
    char code0;
    char code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == ' ') encodedString += "%20";
        else if (isalnum(c)) encodedString += c;
        else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) code0 = c - 10 + 'A';
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

void parseLrc(String lrcContent) {
  currentLyrics.clear();
  if (currentLyrics.capacity() < 50) currentLyrics.reserve(50);
  
  int start = 0;
  while (start < lrcContent.length()) {
    int end = lrcContent.indexOf('\n', start);
    if (end == -1) end = lrcContent.length();
    
    String line = lrcContent.substring(start, end);
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
    start = end + 1;
  }
}

void fetchLyrics(String trackName, String artistName) {
  hasLyrics = false;
  currentLyricIndex = -1;
  currentLyrics.clear();
  
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://lrclib.net/api/get?artist_name=" + urlEncode(artistName) + "&track_name=" + urlEncode(trackName);
    http.begin(url);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
      String payload = http.getString();
      JsonDocument doc; 
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        if (doc.containsKey("syncedLyrics") && !doc["syncedLyrics"].isNull()) {
           String rawLrc = doc["syncedLyrics"].as<String>();
           parseLrc(rawLrc);
           if (currentLyrics.size() > 0) hasLyrics = true;
        }
      }
    }
    http.end();
  }
}

String truncateText(String text, int maxWidth) {
  if (u8f.getUTF8Width(text.c_str()) <= maxWidth) return text;
  String result = text;
  while (u8f.getUTF8Width((result + "...").c_str()) > maxWidth && result.length() > 0) {
      int len = result.length();
      int charStart = len - 1;
      while (charStart > 0 && (result[charStart] & 0xC0) == 0x80) charStart--;
      result = result.substring(0, charStart);
  }
  return result + "...";
}

int drawWrappedText(String text, int x, int y, int maxWidth, int lineHeight, int maxLines) {
  int currentY = y; 
  String currentLine = "";
  currentLine.reserve(text.length()); 
  int lineCount = 0;
  
  for (int i = 0; i < text.length(); i++) {
    char c = text[i];
    String glyph = "";
    glyph += c;
    
    if ((c & 0x80) != 0) { 
      if ((c & 0xE0) == 0xC0 && i+1 < text.length()) { glyph += text[++i]; }
      else if ((c & 0xF0) == 0xE0 && i+2 < text.length()) { glyph += text[++i]; glyph += text[++i]; }
      else if ((c & 0xF8) == 0xF0 && i+3 < text.length()) { glyph += text[++i]; glyph += text[++i]; glyph += text[++i]; }
    }
    
    int w = u8f.getUTF8Width((currentLine + glyph).c_str());
    
    if (w > maxWidth) {
      if (lineCount >= maxLines - 1) {
         String textToPrint = currentLine;
         while (u8f.getUTF8Width((textToPrint + "...").c_str()) > maxWidth && textToPrint.length() > 0) {
             int len = textToPrint.length();
             if (len == 0) break;
             int charStart = len - 1;
             while (charStart > 0 && (textToPrint[charStart] & 0xC0) == 0x80) charStart--;
             textToPrint = textToPrint.substring(0, charStart);
         }
         u8f.setCursor(x, currentY);
         u8f.print((textToPrint + "...").c_str());
         return currentY + lineHeight;
      }
      
      int lastSpace = currentLine.lastIndexOf(' ');
      if (lastSpace > 0 && lastSpace > currentLine.length() / 2) {
         String lineToPrint = currentLine.substring(0, lastSpace);
         u8f.setCursor(x, currentY);
         u8f.print(lineToPrint.c_str());
         currentLine = currentLine.substring(lastSpace + 1) + glyph;
      } else {
         u8f.setCursor(x, currentY);
         u8f.print(currentLine.c_str());
         currentLine = glyph;
      }
      currentY += lineHeight;
      lineCount++;
      if (currentY > getScreenHeight() - 25) return currentY; 
    } else {
      currentLine += glyph;
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
void drawSongInfo(CurrentlyPlaying currentlyPlaying) {
  u8f.setFont(u8g2_font_wqy16_t_gb2312); 
  u8f.setForegroundColor(TFT_WHITE);
  u8f.setBackgroundColor(TFT_BLACK);
  
  int cursorY = 25; 
  cursorY = drawWrappedText(String(currentlyPlaying.trackName), TEXT_X, cursorY, TEXT_W, 24, 3);
  cursorY += 5; 

  if (currentlyPlaying.numArtists > 0) {
    cursorY = drawWrappedText(String(currentlyPlaying.artists[0].artistName), TEXT_X, cursorY, TEXT_W, 24, 2);
  }
  cursorY += 5; 

  String albumStr = truncateText(String(currentlyPlaying.albumName), TEXT_W);
  u8f.setForegroundColor(TFT_WHITE); 
  u8f.setCursor(TEXT_X, cursorY);
  u8f.print(albumStr.c_str());
  cursorY += 24; 

  lyricY = cursorY + 30;
}

void updateLyrics() {
  if (!hasLyrics || currentLyrics.empty()) return;
  long currentMs = songProgress + (millis() - lastSongFetchTime);
  int activeIndex = -1;
  for (int i = 0; i < currentLyrics.size(); i++) {
    if (currentMs >= currentLyrics[i].timestamp) activeIndex = i; else break; 
  }
  
  if (activeIndex != -1 && activeIndex != currentLyricIndex) {
    currentLyricIndex = activeIndex;
    String text = currentLyrics[activeIndex].text;
    int fontHeight = 16; 
    int clearY = lyricY - fontHeight;
    int clearH = (getScreenHeight() - 10) - clearY;
    if (clearH > 0) {
      tft.fillRect(TEXT_X, clearY, TEXT_W, clearH, backgroundColor); 
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
void drawKaraokeHeader(CurrentlyPlaying currentlyPlaying) {
  u8f.setFont(u8g2_font_wqy16_t_gb2312); 
  u8f.setForegroundColor(TFT_WHITE);
  u8f.setBackgroundColor(TFT_BLACK); 
  
  int y = 30;
  String title = String(currentlyPlaying.trackName);
  String artist = "";
  if (currentlyPlaying.numArtists > 0) artist = String(currentlyPlaying.artists[0].artistName);
  
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
  
  long currentMs = songProgress + (millis() - lastSongFetchTime);
  int activeIndex = -1;
  for (int i = 0; i < currentLyrics.size(); i++) {
    if (currentMs >= currentLyrics[i].timestamp) activeIndex = i; else break; 
  }
  
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
      if (i >= 0 && i < currentLyrics.size()) {
        int yPos = centerY + ((i - activeIndex) * lineHeight);
        int lineTop = yPos - fontAscent;
        
        if (lineTop > lyricAreaTop && yPos < getScreenHeight() - 15) {
          String txt = currentLyrics[i].text;
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
    // Only ONE button remains: The BOOT button for toggling Mode
    if (digitalRead(BOOT_BUTTON) == LOW) {
      isKaraokeMode = !isKaraokeMode;
      Serial.println(isKaraokeMode ? "Karaoke Mode ON" : "Karaoke Mode OFF");
      forceRedraw = true; 
      lastCheck = 0;
      lastButtonPress = millis();
    }
  }
}

// =========================================================================
//   DISPLAY LOGIC
// =========================================================================
void printCurrentlyPlaying(CurrentlyPlaying currentlyPlaying) {
    isSpotifyPlaying = currentlyPlaying.isPlaying;
    songDuration = currentlyPlaying.durationMs;
    songProgress = currentlyPlaying.progressMs;
    lastSongFetchTime = millis();

    if (String(currentlyPlaying.trackUri) == lastTrackURI && !forceRedraw) return;
    
    bool isNewTrack = (String(currentlyPlaying.trackUri) != lastTrackURI);

    lastTrackURI = String(currentlyPlaying.trackUri);
    Serial.print("New Track: ");
    Serial.println(currentlyPlaying.trackName);

    dominantColor = 0x1DB9; 
    backgroundColor = TFT_BLACK; 
    
    tft.fillRect(0, 0, tft.width(), getScreenHeight() - 10, TFT_BLACK);
    
    if (isNewTrack) {
      lastBarWidth = 0; 
      tft.fillRect(0, getScreenHeight() - 10, tft.width(), 10, backgroundColor);
    }
    
    forceRedraw = false; 

    if (isKaraokeMode) {
      drawKaraokeHeader(currentlyPlaying);
      // UPDATE: Draw Progress Bar BEFORE fetching lyrics to ensure UI responsiveness
      updateProgressBar();
      fetchLyrics(currentlyPlaying.trackName, currentlyPlaying.artists[0].artistName);
    } else {
      drawSongInfo(currentlyPlaying); 
      
      String newAlbumArtUrl = "";
      if (currentlyPlaying.numImages > 0) {
         if (currentlyPlaying.numImages > 1) newAlbumArtUrl = currentlyPlaying.albumImages[1].url;
         else newAlbumArtUrl = currentlyPlaying.albumImages[0].url;
      }
      if (newAlbumArtUrl != "") {
        drawAlbumArt(newAlbumArtUrl, IMG_X, IMG_Y);
      }
      
      // UPDATE: Draw Progress Bar BEFORE fetching lyrics
      updateProgressBar();
      fetchLyrics(currentlyPlaying.trackName, currentlyPlaying.artists[0].artistName);
    }
}

void updateProgressBar() {
  if (songDuration == 0) return;
  long estimatedProgress = songProgress + (millis() - lastSongFetchTime);
  if (estimatedProgress > songDuration) estimatedProgress = songDuration;

  int barWidth = tft.width(); 
  int barHeight = 6;
  int barY = getScreenHeight() - 10; 
  
  // FIX: Use float math to prevent overflow on long songs (> 1 hour)
  int fillWidth = (int)(((float)estimatedProgress / (float)songDuration) * barWidth);
  
  if (fillWidth == lastBarWidth) return; 

  if (fillWidth > lastBarWidth) {
    tft.fillRect(lastBarWidth, barY, fillWidth - lastBarWidth, barHeight, dominantColor);
  } else {
    tft.fillRect(0, barY, fillWidth, barHeight, dominantColor); 
    tft.fillRect(fillWidth, barY, barWidth - fillWidth, barHeight, backgroundColor); 
  }
  lastBarWidth = fillWidth;
}

// =========================================================================
//   IMAGE DOWNLOADER (BUFFERED)
// =========================================================================
void drawAlbumArt(String url, int xPos, int yPos) {
  int splitIndex = url.indexOf("//") + 2;
  int pathIndex = url.indexOf("/", splitIndex);
  String host = url.substring(splitIndex, pathIndex);
  String path = url.substring(pathIndex);

  WiFiClientSecure imgClient;
  imgClient.setInsecure(); 

  if (imgClient.connect(host.c_str(), 443)) {
    imgClient.print(String("GET ") + path + " HTTP/1.1\r\n" +
                 "Host: " + host + "\r\n" +
                 "User-Agent: ESP32\r\n" +
                 "Connection: close\r\n\r\n");

    while (imgClient.connected()) {
      String line = imgClient.readStringUntil('\n');
      if (line == "\r") break; 
    }

    jpgData.clear(); 

    // FIX: Stack buffer size reduced to 512 bytes
    uint8_t buffer[512]; 
    
    while (imgClient.connected() || imgClient.available()) {
      size_t size = imgClient.available();
      if (size) {
        int c = imgClient.readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
        
        // UPDATE: Check safety limit (100KB is risky but usually okay if no BT)
        if (jpgData.size() + c <= 100000) {
           jpgData.insert(jpgData.end(), buffer, buffer + c);
        } else {
           break; // Stop if too big
        }
      }
      delay(1); 
    }
    imgClient.stop();
    TJpgDec.drawJpg(xPos, yPos, jpgData.data(), jpgData.size());
  } else {
    Serial.println("Failed to connect to image server");
  }
}