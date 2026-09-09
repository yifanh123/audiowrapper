 # SpotifyWrapper

An ESP32-WROVER audio project that displays Spotify playback on a TFT while
acting as a Bluetooth A2DP speaker with a 16x16 audio-reactive LED matrix.

## Features

- Connects to Spotify using the Spotify Web API
- Displays playback information on a TFT display
- Receives Bluetooth stereo audio and sends it to an I2S amplifier/DAC
- Displays a 16-band FFT visualizer on a 16x16 WS2812B matrix
- Runs on an ESP32-WROVER
- Designed for a compact, dedicated audio status display

## Hardware

- ESP32-WROVER development board
- HSD-9190J-B7 480x320 SPI TFT (ST7796 controller)
- I2S amplifier/DAC and speaker
- 16x16 WS2812B matrix with a suitable external 5 V supply
- USB cable and stable power supply
- Optional buttons or controls, depending on the build

## Software

- [PlatformIO](https://platformio.org/)
- ESP32 Arduino framework
- A Spotify Developer application
- Spotify API credentials: Client ID, Client Secret, and redirect URI

## Setup

1. Clone or open this project in PlatformIO.
2. Create an application in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
3. Add the project's redirect URI to the Spotify application settings.
4. Add the required Wi-Fi and Spotify credentials to the project's configuration or secrets file. Do not commit private credentials.
5. Connect the TFT, I2S board, and matrix using the wiring table below. The
   authoritative TFT_eSPI configuration is `include/TFT_Setup.h`.
6. Build and upload the firmware:

	```bash
	pio run --target upload
	```

7. Open the serial monitor to complete authentication and review connection status:

	```bash
	pio device monitor
	```

## Configuration

Configure the following values before uploading:

- Wi-Fi SSID and password
- Spotify Client ID
- Spotify Client Secret
- Spotify redirect URI
- TFT driver, resolution, and wiring pins

The Spotify account must have the required playback access; some playback endpoints require Spotify Premium.

## TFT wiring (ESP32-WROVER-E)

| TFT signal | ESP32 GPIO |
| --- | ---: |
| SCLK | 18 |
| MOSI | 23 |
| MISO | 19 |
| CS | 27 |
| DC / RS | 26 |
| RST | 25 |

## Bluetooth audio and matrix wiring

| Device signal | ESP32 GPIO |
| --- | ---: |
| I2S BCLK / SCK | 14 |
| I2S LRCLK / WS / LRC | 13 |
| I2S DATA / DIN | 22 |
| WS2812B matrix DIN | 32 |
| Mode button (other side to GND) | 33 |

Connect all grounds together. Power the 256-pixel matrix from an adequately
rated external 5 V supply rather than the ESP32 board. A 3.3-to-5 V logic-level
shifter on the matrix data line is recommended, particularly with longer wires.
The firmware limits the matrix to 500 mA in software, but the power supply and
wiring must still be appropriately protected.

GPIO 18, 19, and 23 are the ESP32's default VSPI pins. GPIO 25, 26, and 27
are ordinary output-capable pins and are used for the display control lines.
Avoid GPIO 0, 2, 5, 12, and 15 for display control because they are sampled
during boot. Also avoid GPIO 6-11 (flash), GPIO 16-17 on WROVER modules
(PSRAM), and GPIO 34-39 for outputs (input-only).

The audio/matrix/button assignments use GPIO 13, 14, 22, 32, and 33. They do not overlap
the TFT, boot-strapping pins, flash, or WROVER PSRAM. GPIO 13 and 14 are JTAG
pins when hardware JTAG is enabled; this project uses them as normal outputs.

## Usage

After startup, pair a phone or computer with `ESP32-Visualizer` and play audio.
The TFT separately uses Wi-Fi and Spotify to show the active track, artist,
artwork, playback progress, and lyrics.

## License

Add a license for this project before distribution.
