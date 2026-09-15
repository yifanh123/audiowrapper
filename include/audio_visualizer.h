#pragma once

// Starts the Bluetooth A2DP speaker, I2S output, and LED matrix.
// Safe to call once from setup().
bool beginAudioVisualizer();

// Processes one pending audio frame and redraws the matrix. Call frequently.
void updateAudioVisualizer();

// Lightweight state queries used by the network scheduler and diagnostics.
bool isBluetoothConnected();
bool isBluetoothAudioStreaming();

// Returns true once for one or more coalesced AVRCP metadata/playback events.
// The caller must perform any network work outside the Bluetooth callback.
bool consumeBluetoothSpotifyRefreshRequest();
