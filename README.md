 # SpotifyWrapper

An ESP32-WROVER audio project that uses the Spotify Web API to display the currently playing track on a TFT screen.

## Features

- Connects to Spotify using the Spotify Web API
- Displays playback information on a TFT display
- Runs on an ESP32-WROVER
- Designed for a compact, dedicated audio status display

## Hardware

- ESP32-WROVER development board
- Compatible SPI TFT display
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
5. Connect the TFT display according to the pin definitions in the source code.
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

## Usage

After startup, connect the device to Wi-Fi and authorize it with Spotify when prompted. The TFT display will update with the active playback information, such as the track title, artist, and playback state.

## License

Add a license for this project before distribution.
