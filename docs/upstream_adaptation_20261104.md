# Selected upstream fixes (2026-11-04)

These changes adapt selected LizardByte/Sunshine fixes to StarCore's existing Windows architecture.
They retain the web login, branding, SudoVDA modes, client microphone uplink and HIDMaestro integration.
No libvirtualhid dependency or Vue SPA migration is included.

| Upstream change | Local adaptation |
| --- | --- |
| 9051ae84 | Keep AV1 probe results from overwriting HEVC results. |
| 1582fffb | Advertise 10-bit codec modes only after a successful matching probe. |
| c59469b7 | Fall back to SDR when the selected encoder lacks HDR support. |
| f0ad7cde | Optional AMD H.264/HEVC maximum access-unit size; unset retains encoder defaults. |
| 31da0d8d | Optional FP16 SDR gamma correction, disabled by default; configuration stays in the existing WebUI. |
| c1012508 | Separate pending audio/video UDP sender endpoints; retain microphone reception. |
| 8952d44b | Rotate surround mappings over the actual advertised channel count. |
| f111d6a4 | Honor explicit audio sink selection and distinguish default-device changes. |
| 2c8825ad | Case-insensitive boolean configuration values. |
| f86c4c9c | Bound hexadecimal decoding to the destination size. |
| 9124e77f | Respect the configured network bind address. |
| bc188c2c | Handle disconnected IGD results from newer MiniUPnPc. |
| 820700cb | Reload changed application configuration without replacing running process state. |
| c33f3b46 | Replace abandoned pairing handshakes while retaining sessions awaiting PIN approval. |
| 0daeb0e3 | Replace records with identical X.509 certificates on successful pairing. |
| e9b24114 | Preserve saved display settings and retry restoration when disconnected displays return. |
| 4f39fc11, 4dadc7d4 | Correct installer registry arguments and preserve PATH trailing backslashes. |

Display restoration uses the upstream-required libdisplaydevice revision
`a2afc0430d44223c25a2ebc598000f78b338aa52`. Nested local submodule checkouts are not reset.

Hardware acceptance remains necessary for real HDR/SDR capture, surround speaker assignment,
physical-display disconnect/reconnect, SudoVDA extend/exclusive teardown, and explicit audio sinks.

## Verification

- UCRT64 build: `sunshine` and `test_sunshine` succeeded in `cmake-build-hidmaestro`.
- WebUI Vite production build succeeded as part of the application build.
- Full gtest run: 499 tests, 490 passed, 8 environment-dependent skips, and one documentation
  consistency failure for pre-existing microphone options. After documenting both options, all five
  configuration consistency tests passed on retest. No failing test remains.
- Installer PATH: seven cases passed using a script copy redirected to an isolated temporary HKCU
  key; the system PATH was not changed.
- Existing coverage files produced checksum warnings after recompilation; these results do not
  represent a fresh coverage measurement.
