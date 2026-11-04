# StarCore星核 1.2.0

The user-visible product name is StarCore星核. Version 1.2.0 is defined by the root CMake project and flows into the server version, Windows executable resources, and installation packages.

The original artwork is copied from `StarCoreAndroid/app/src/main/res/mipmap-xhdpi/icon_app.png`. `tools/update-starcore-icons.ps1` creates the WebUI favicon, application ICO, sized PNGs, and embedded-image SVG tray assets. Playback, pause, and pairing badges distinguish tray states. The root `sunshine.png` is an exact copy of the Android source artwork.

Web translations apply the shared product name after translation, preserving technical identifiers and URLs; non-English translation files remain unchanged. Tray labels and Windows file properties use the same visible name. Windows setup display names and shortcuts use StarCore星核, with a `StarCore-1.2.0-Windows` package basename.

The internal Sunshine project name, executable name, service identifier, configuration paths, install directory, registry key, update repository, and WiX upgrade GUID remain compatible with existing installations. Upstream attribution and license URLs remain intact.

Validation: `node --test tests/branding.test.mjs tests/web_auth.test.mjs`, Vite production build, UCRT64 resource compilation, and targeted C++ compilation checks. An installed build must be replaced before the new branding appears there.
