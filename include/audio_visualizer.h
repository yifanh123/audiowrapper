#pragma once

// Starts the Bluetooth A2DP speaker, I2S output, and LED matrix.
// Safe to call once from setup().
bool beginAudioVisualizer();

// Processes one pending audio frame and redraws the matrix. Call frequently.
void updateAudioVisualizer();

