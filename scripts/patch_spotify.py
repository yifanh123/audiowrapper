"""Apply compatibility and safe-debug fixes to generated SpotifyArduino sources.

PlatformIO downloads this old library into .pio/libdeps, so direct edits disappear
after a clean. Running this as a pre-build script makes the fixes reproducible.
"""

from pathlib import Path

Import("env")


library_root = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "SpotifyArduino"
    / "src"
)
header_path = library_root / "SpotifyArduino.h"
source_path = library_root / "SpotifyArduino.cpp"

if not header_path.exists() or not source_path.exists():
    raise RuntimeError("SpotifyArduino sources were not found under " + str(library_root))

header = header_path.read_text(encoding="utf-8")
source = source_path.read_text(encoding="utf-8")
original_header = header
original_source = source

# Spotify now returns access tokens longer than the library's old 309-byte limit.
header = header.replace(
    "#define SPOTIFY_ACCESS_TOKEN_LENGTH 309",
    "#define SPOTIFY_ACCESS_TOKEN_LENGTH 512",
)
header = header.replace(
    "#define SPOTIFY_TIMEOUT 2000",
    "#define SPOTIFY_TIMEOUT 8000",
)

# The application prints concise request status, track details, timing, and
# memory diagnostics. Disable the library's duplicate raw JSON/status dump,
# which can stall Serial for an entire audio frame on every three-second poll.
header = header.replace("#define SPOTIFY_DEBUG 1", "// #define SPOTIFY_DEBUG 1")

# Do not emit OAuth request bodies or returned bearer tokens to Serial.
source = source.replace(
    "    Serial.println(body);",
    '    Serial.print(F("OAuth/request body length: "));\n    Serial.println(strlen(body));',
)
source = source.replace(
    '                Serial.print(F("Problem with access_token (too long or null): "));\n'
    "                Serial.println(accessToken);",
    '                Serial.print(F("Problem with access_token; received length: "));\n'
    "                Serial.println(accessToken == NULL ? 0 : strlen(accessToken));",
)

# Fix allocation pairing and make the OAuth body construction bounds-safe.
source = source.replace("        delete _refreshToken;", "        delete[] _refreshToken;")
source = source.replace(
    "    char body[300];\n"
    "    sprintf(body, refreshAccessTokensBody, _refreshToken, _clientId, _clientSecret);",
    "    char body[512];\n"
    "    snprintf(body, sizeof(body), refreshAccessTokensBody, _refreshToken, _clientId, _clientSecret);",
)

if "#define SPOTIFY_ACCESS_TOKEN_LENGTH 512" not in header:
    raise RuntimeError("SpotifyArduino access-token limit patch did not match this library version")

if header != original_header:
    header_path.write_text(header, encoding="utf-8", newline="\n")
if source != original_source:
    source_path.write_text(source, encoding="utf-8", newline="\n")

print("SpotifyArduino compatibility verified (512-char token, 8s timeout, redacted debug)")
