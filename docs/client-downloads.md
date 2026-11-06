# Home-page client downloads

`ClientDownloads.vue` appears before the reference resources on the WebUI home page.
It mirrors the iPhone/iPad, Android and HarmonyOS destinations in macOS StarCore's
`StarCoreClientDownloadsView.swift`. The links are defined in `client_downloads.js`.

Each card includes a local SVG QR code, a download link and a clipboard action with
accessible success/failure feedback. QR codes use the exact destination URL with a
four-module quiet zone and black modules on white, independent of the active theme.
The images live under `public/assets/client-downloads` so the WebUI serves them
through its `/assets/` route.
When changing a destination, regenerate the corresponding SVG with a QR encoder
at error-correction level M and update the destination in the same change.

The connection guide directs users to install a client, discover or manually add this
Windows host, and submit the client's PIN on the existing pairing page. Cards stack
on narrow screens. English and Simplified Chinese messages are included.

The featured-app navigation entry is removed; its existing page remains available by URL.
