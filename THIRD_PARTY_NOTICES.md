# Third-Party Notices

This repository includes and references third-party open-source components.
Their original licenses and notices remain in effect for the corresponding
files/components.

## Firmware license scope

Firmware authored in this repository (under `src/`, most of `lib/`, build
config, scripts, docs, and tooling) is licensed under
[PolyForm Noncommercial 1.0.0](LICENSE-firmware) unless a file carries its
own license header or is listed below.

The `balboa-spa/` submodule is **not** covered by that license; it remains
**Apache-2.0** (see [Bundled in this repository](#bundled-in-this-repository)).

## Bundled in this repository

- `balboa-spa/` (git submodule)
  - Source: [shomanjk/balboa-spa](https://github.com/shomanjk/balboa-spa)
  - License: Apache-2.0
  - License file: `balboa-spa/LICENSE`

- `lib/Analytics/Analytics.h`, `lib/Analytics/Analytics.cpp`
  - Copyright: Stefan Staub (2018)
  - License: MIT (embedded header notice)

- `lib/tinyXml2/tinyxml2.h`, `lib/tinyXml2/tinyxml2.cpp`
  - Source: TinyXML2 (Lee Thomason and contributors)
  - License: zlib-style permissive license (embedded header notice)
  - Notice location: top-of-file comments in `tinyxml2.h` and `tinyxml2.cpp`

- `lib/spaEpaper/to_jpg.cpp`, `lib/spaEpaper/yuv.h`
  - Source: Espressif Systems image conversion code
  - License: Apache-2.0 (embedded header notice)
  - Notice location: top-of-file comments in `to_jpg.cpp` and `yuv.h`

- `lib/spaEpaper/jpge.h`
  - Source: jpge by Rich Geldreich (with later contributions)
  - License: Public domain (as declared in file header)
  - Notice location: top-of-file comment in `jpge.h`

## PlatformIO library dependencies

Firmware builds link PlatformIO libraries declared in `platformio.ini`
(for example WiFiManager, PubSubClient, ArduinoJson, ESPAsyncWebServer).
Those packages are **not** vendored in this tree; their license terms apply
to the corresponding upstream packages (typically MIT, Apache-2.0, or LGPL).
Check each package's repository or PlatformIO registry metadata when
redistributing binaries.

## Emulator npm dependencies

The local emulator under `emulator/` has its own `package.json` and dependency
tree. Package license metadata is available in `emulator/package-lock.json`.

- Top-level emulator package license: ISC
- Example transitive dependency licenses (from lockfile metadata): MIT, BSD-3-Clause

## Notes

- This notices file is informational and does not replace or modify any
  underlying third-party license.
- Where a third-party component includes a required notice in source headers,
  that notice must be preserved when redistributing those files.
- Releases published before the PolyForm Noncommercial switch may have been
  distributed under Apache-2.0 for the full repository; those copies remain
  under the license they were received with.
