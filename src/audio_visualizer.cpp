#include "audio_visualizer.h"

#include <Arduino.h>
#include <BluetoothA2DPSink.h>
#include <FastLED.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint8_t MATRIX_DATA_PIN = 32;
constexpr uint8_t I2S_BCLK_PIN = 14;
constexpr uint8_t I2S_LRCLK_PIN = 13;
constexpr uint8_t I2S_DATA_PIN = 22;

constexpr uint8_t MATRIX_WIDTH = 16;
constexpr uint8_t MATRIX_HEIGHT = 16;
constexpr size_t NUM_LEDS = MATRIX_WIDTH * MATRIX_HEIGHT;
constexpr uint8_t MATRIX_BRIGHTNESS = 40;
constexpr uint16_t FFT_SAMPLES = 512;
constexpr float SAMPLE_RATE_HZ = 44100.0F;
constexpr uint8_t BAND_COUNT = 16;
constexpr float NOISE_GATE = 50.0F;
// A 256-pixel WS2812 frame occupies the timing peripheral for roughly 8 ms.
// 20 FPS remains fluid while leaving substantially more time for A2DP/Wi-Fi.
constexpr uint32_t MIN_FRAME_INTERVAL_MS = 50;

// Higher values reduce the height of a band. Bass is intentionally damped
// more heavily than treble to match the supplied visualizer tuning.
constexpr uint16_t SENSITIVITY[BAND_COUNT] = {
    4000, 3500, 3000, 2500,
    2000, 1500, 1000, 800,
    600, 500, 400, 300,
    250, 200, 150, 100,
};

CRGB leds[NUM_LEDS];
BluetoothA2DPSink a2dpSink;

// The A2DP callback and Arduino loop run in different FreeRTOS tasks. Keep a
// short critical section around the shared capture frame, then do all FFT work
// after releasing it so Bluetooth cannot be starved.
portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
int16_t* captureBuffer = nullptr;
int16_t* processingBuffer = nullptr;
volatile size_t captureCount = 0;
volatile bool frameReady = false;

float* vReal = nullptr;
float* vImag = nullptr;
ArduinoFFT<float>* fft = nullptr;
uint8_t bandValues[BAND_COUNT] = {};
uint32_t lastFrameAt = 0;
bool visualizerReady = false;

uint16_t xy(uint8_t x, uint8_t y) {
  return (y & 1U)
             ? (y * MATRIX_WIDTH) + (MATRIX_WIDTH - 1U - x)
             : (y * MATRIX_WIDTH) + x;
}

void onAudioData(const uint8_t* data, uint32_t length) {
  if (data == nullptr || captureBuffer == nullptr ||
      length < (2U * sizeof(int16_t))) return;

  const int16_t* stereoSamples = reinterpret_cast<const int16_t*>(data);
  const size_t sampleCount = length / sizeof(int16_t);

  // This callback is the only writer. The loop never swaps buffers until
  // frameReady becomes true, so filling the buffer does not need to block
  // Bluetooth interrupts with a long critical section.
  if (!frameReady) {
    // Bluetooth PCM is interleaved L/R. Capture the left channel only.
    for (size_t i = 0; i + 1U < sampleCount && captureCount < FFT_SAMPLES; i += 2U) {
      captureBuffer[captureCount++] = stereoSamples[i];
    }
    if (captureCount == FFT_SAMPLES) {
      portENTER_CRITICAL(&audioMux);
      frameReady = true;
      portEXIT_CRITICAL(&audioMux);
    }
  }
}

void onConnectionState(esp_a2d_connection_state_t state, void*) {
  Serial.printf("[Bluetooth] connection=%s\n", a2dpSink.to_str(state));
}

void onAudioState(esp_a2d_audio_state_t state, void*) {
  Serial.printf("[Bluetooth] audio=%s\n", a2dpSink.to_str(state));
}

void onSampleRate(uint16_t rate) {
  Serial.printf("[Audio] negotiated sample rate=%u Hz\n", rate);
}

void onVolumeChanged(int volume) {
  Serial.printf("[Audio] Bluetooth volume=%d/127\n", volume);
}

bool takeAudioFrame() {
  bool available = false;
  portENTER_CRITICAL(&audioMux);
  if (frameReady) {
    // Swap two PSRAM pointers instead of copying 1 KB while interrupts are
    // disabled. The audio callback immediately continues into the empty one.
    int16_t* completedFrame = captureBuffer;
    captureBuffer = processingBuffer;
    processingBuffer = completedFrame;
    captureCount = 0;
    frameReady = false;
    available = true;
  }
  portEXIT_CRITICAL(&audioMux);
  return available;
}

void calculateBands() {
  for (size_t i = 0; i < FFT_SAMPLES; ++i) {
    vReal[i] = static_cast<float>(processingBuffer[i]);
    vImag[i] = 0.0F;
  }

  fft->windowing(FFTWindow::Blackman_Harris, FFTDirection::Forward);
  fft->compute(FFTDirection::Forward);
  fft->complexToMagnitude();

  // Skip DC and the first bin, then divide every usable positive-frequency
  // bin among all 16 columns without reading past the Nyquist limit.
  constexpr size_t FIRST_BIN = 2;
  constexpr size_t BIN_LIMIT = FFT_SAMPLES / 2;
  constexpr size_t USABLE_BINS = BIN_LIMIT - FIRST_BIN;
  for (size_t band = 0; band < BAND_COUNT; ++band) {
    const size_t start = FIRST_BIN + (band * USABLE_BINS) / BAND_COUNT;
    const size_t end = FIRST_BIN + ((band + 1U) * USABLE_BINS) / BAND_COUNT;
    float sum = 0.0F;
    for (size_t bin = start; bin < end; ++bin) sum += vReal[bin];

    float average = sum / static_cast<float>(end - start);
    if (average < NOISE_GATE) average = 0.0F;
    const int height = static_cast<int>(average / SENSITIVITY[band]);
    bandValues[band] = static_cast<uint8_t>(
        std::max(0, std::min(height, static_cast<int>(MATRIX_HEIGHT))));
  }
}

void drawMatrix() {
  FastLED.clear();
  for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
    for (uint8_t y = 0; y < bandValues[x]; ++y) {
      // HSV hue 96 is green and hue 0 is red.
      const uint8_t hue = static_cast<uint8_t>(96U - (96U * y) / (MATRIX_HEIGHT - 1U));
      leds[xy(x, y)] = CHSV(hue, 255, 255);
    }
  }
  FastLED.show();
}

bool startClassicBluetoothController() {
  esp_bt_controller_status_t status = esp_bt_controller_get_status();
  if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) return true;

  if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
    // Arduino-ESP32 2.x normally starts BTDM (Classic + unused BLE). Starting
    // the controller ourselves lets us permanently return the BLE reservation
    // to the internal heap before Wi-Fi and TLS need it.
    const esp_err_t releaseResult = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    Serial.printf("[Bluetooth] release unused BLE memory: %s\n", esp_err_to_name(releaseResult));

    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    config.mode = ESP_BT_MODE_CLASSIC_BT;
    // A2DP and AVRCP share one Bluetooth ACL link to the same phone. The
    // framework default reserves room for seven simultaneous Classic peers,
    // which wastes scarce DMA-capable internal RAM in this single-speaker app.
    config.bt_max_acl_conn = 1;
    const esp_err_t initResult = esp_bt_controller_init(&config);
    if (initResult != ESP_OK) {
      Serial.printf("[Bluetooth] ERROR: controller init failed: %s\n", esp_err_to_name(initResult));
      return false;
    }
    status = esp_bt_controller_get_status();
  }

  if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
    const esp_err_t enableResult = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (enableResult != ESP_OK) {
      Serial.printf("[Bluetooth] ERROR: controller enable failed: %s\n", esp_err_to_name(enableResult));
      return false;
    }
  }
  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}

bool allocateFftWorkspace() {
  constexpr size_t FFT_BYTES = FFT_SAMPLES * sizeof(float);
  constexpr size_t PCM_BYTES = FFT_SAMPLES * sizeof(int16_t);
  captureBuffer = static_cast<int16_t*>(
      heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  processingBuffer = static_cast<int16_t*>(
      heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vReal = static_cast<float*>(heap_caps_malloc(FFT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vImag = static_cast<float*>(heap_caps_malloc(FFT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (captureBuffer == nullptr || processingBuffer == nullptr ||
      vReal == nullptr || vImag == nullptr) {
    if (captureBuffer != nullptr) heap_caps_free(captureBuffer);
    if (processingBuffer != nullptr) heap_caps_free(processingBuffer);
    if (vReal != nullptr) heap_caps_free(vReal);
    if (vImag != nullptr) heap_caps_free(vImag);
    captureBuffer = nullptr;
    processingBuffer = nullptr;
    vReal = nullptr;
    vImag = nullptr;
    Serial.println("[FFT] ERROR: PSRAM workspace allocation failed; audio will continue without visualization");
    return false;
  }

  fft = new ArduinoFFT<float>(vReal, vImag, FFT_SAMPLES, SAMPLE_RATE_HZ);
  if (fft == nullptr) {
    Serial.println("[FFT] ERROR: processor allocation failed; audio will continue without visualization");
    return false;
  }
  Serial.printf("[FFT] Workspace allocated in %s\n",
                esp_ptr_external_ram(vReal) ? "PSRAM" : "internal RAM");
  return true;
}

}  // namespace

bool beginAudioVisualizer() {
  FastLED.addLeds<WS2812B, MATRIX_DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);
  FastLED.setBrightness(MATRIX_BRIGHTNESS);
  FastLED.clear(true);

  visualizerReady = allocateFftWorkspace();
  if (!startClassicBluetoothController()) {
    Serial.println("[Bluetooth] ERROR: Classic Bluetooth controller did not start");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_LRCLK_PIN;
  pins.data_out_num = I2S_DATA_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = 44100;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = 0;
  // Eight buffers absorb short Wi-Fi/TLS scheduling delays and prevent I2S
  // underruns. The PSRAM changes elsewhere preserve enough internal DMA RAM.
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = 64;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.fixed_mclk = 0;

  a2dpSink.set_pin_config(pins);
  a2dpSink.set_i2s_config(i2sConfig);
  a2dpSink.set_volume(127);
  a2dpSink.set_stream_reader(onAudioData, true);
  a2dpSink.set_on_connection_state_changed(onConnectionState);
  a2dpSink.set_on_audio_state_changed_post(onAudioState);
  a2dpSink.set_sample_rate_callback(onSampleRate);
  a2dpSink.set_on_volumechange(onVolumeChanged);

  Serial.printf(
      "[Audio] I2S BCLK=%u LRCLK=%u DATA=%u; matrix DATA=%u; Bluetooth name=ESP32-Visualizer\n",
      I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DATA_PIN, MATRIX_DATA_PIN);
  a2dpSink.start("ESP32-Visualizer");
  Serial.println("[Bluetooth] A2DP speaker started and discoverable");
  return true;
}

void updateAudioVisualizer() {
  if (!visualizerReady || !frameReady || millis() - lastFrameAt < MIN_FRAME_INTERVAL_MS) return;
  if (!takeAudioFrame()) return;

  lastFrameAt = millis();
  calculateBands();
  drawMatrix();
}
