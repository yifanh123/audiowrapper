"""Force esptool to patch the bootloader header for 20 MHz flash reads.

Arduino-ESP32 only ships a 40 MHz DIO bootloader ELF, so the project must be
built at 40 MHz. This older module boots reliably only when esptool patches the
flashed bootloader header to 20 MHz. The application remains otherwise
unchanged.
"""

Import("env")


flags = list(env.get("UPLOADERFLAGS", []))
for index, flag in enumerate(flags[:-1]):
    if str(flag) in ("--flash_freq", "-ff"):
        flags[index + 1] = "20m"

env.Replace(UPLOADERFLAGS=flags)
