# Third-Party Notices

This repository includes and references third-party open-source components.
Their original licenses and notices remain in effect for the corresponding
files/components.

## Bundled in this repository

- `balboa-spa/` (git submodule)
  - Source: [shomanjk/balboa-spa](https://github.com/shomanjk/balboa-spa)
  - License: Apache-2.0
  - License file: `balboa-spa/LICENSE`

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
