# Third-party software

Builds pin and verify SHA256 checksums of nlohmann/json 3.12.0 (MIT),
libarchive 3.8.11 (BSD and permissive component licenses), and, on Linux,
curl 8.23.0 (curl license). Linux static builds also link Alpine's OpenSSL
(Apache-2.0) and zlib (zlib license). macOS uses Apple's system libcurl,
CommonCrypto, zlib and C++ runtime.

Release archives include the upstream license texts in `licenses/`.
Source: https://github.com/nlohmann/json, https://github.com/libarchive/libarchive,
https://curl.se, https://openssl.org, https://zlib.net.
