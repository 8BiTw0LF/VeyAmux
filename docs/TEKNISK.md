# VeyAmux for Linux – teknisk dokumentation

Kort introduktion og installation: se [README](../README.md).

VeyAmux er en native Linux-version bygget på **Veyra-NRVideo 2.1.5** (Windows, GPL-3.0, https://github.com/Likely7/Veyra-NRVideo):
**Qt 6/QML UI + FFmpeg + Vulkan compute**. Genbruger Veyras originale HLSL-shaders
(`shaders/veyra/*.hlsl`) ved at kompilere dem til SPIR-V med Microsofts DXC – ingen omskrivning af shaderne.

## Byg AppImage
```
sudo apt install cmake ninja-build g++ pkg-config patchelf file python3 glslang-tools qt6-base-dev qt6-declarative-dev \
  qt6-svg-dev qt6-wayland libvulkan-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libpulse-dev libva-dev qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-layouts
./scripts/build-appimage.sh          # -> dist/VeyAmux-x86_64.AppImage
```
`scripts/fetch-tools.sh` henter DXC, linuxdeploy og appimagetool automatisk. Kør AppImage uden FUSE: `APPIMAGE_EXTRACT_AND_RUN=1`.

## Hvad er med
| Område | Status |
|---|---|
| Afspilning (FFmpeg), seek, pause, volumen, drag&drop, fuldskærm | ✅ |
| Hardware-dekodning på AMD/Intel (VAAPI), automatisk fallback til software | ✅ (ikke testet på rigtig GPU endnu) |
| Lyd via PulseAudio/PipeWire (lyd er master-clock) | ✅ |
| GPU-kæde: RgbToLinear → [støjfjerning: temporal/NLM] → FXAA (AaLuma+AaFxaa, originale shaders) → Lanczos-3 **eller AMD FSR 3.1** opskalering → sharpen/eksponering/kontrast/mætning → ScaleBlit (sRGB) | ✅ |
| **AMD FSR 3.1-opskalering** (eksperimentel): FidelityFX SDK 1.1.4 FSR 3.1 upscaler på Vulkan med VeyAmux' egen sub-pixel-bevægelsesmåling | ✅ (blødere end Lanczos i test, se nedenfor) |
| **Lossless Scaling frame generation** (Fast 2× / Fast 3×) via brugerens egen lsfg-vk + Lossless Scaling fra Steam | ✅ testet af brugeren på RX 9070 XT / Bazzite |
| Vulkan-device vælges automatisk (diskret GPU først); `VEYAMUX_VK_DEVICE=<n>` tvinger valg | ✅ |
| Capture-kort via V4L2 (UVC): YUYV/UYVY/NV12/I420/RGB/MJPEG, valg af opløsning/frekvens, lavlatens-visning, lyd fra PulseAudio/PipeWire-kilde | ✅ (testet uden hardware: konvertering verificeret, enheden selv ikke) |
| DLSS/NGX, NVOF, NVIDIA VFG-DLL, Reflex, NVAPI, AMD HIP-NR (RDNA3) | ❌ Windows/NVIDIA-specifikke |
| **HDR-capture (P010)** → SDR: BT.2020/PQ → BT.2390-tonemapping med Veyras egne `HdrColor.hlsli`/`HdrToSdr.hlsli` (`shaders/LxP010.hlsl`) | ✅ (testet med syntetisk P010-signal, ikke med kortet) |
| **VRR-tilstand**: spillets egne billeder i jævn takt + vsync | ✅ (algoritmen simuleret, ikke testet på VRR-skærm) |
| HDR-videofiler (tonemapping ved filafspilning), HDR-output til skærmen, PS5/Xbox/Moonlight-streaming, undertekster | ❌ ikke portet endnu |

## Hvad spillet faktisk kører (capture)
Et capture-kort sender altid sit faste signal (fx 3840×2160 @ 60), uanset hvad konsollen/pc'en renderer. Infolinjen
viser derfor først det målte indhold og signalet bagefter, fx `≈1920×1080 @ 30 fps → 60 fps (Lossless Scaling 2×) · signal 3840×2160 NV12 @ 60.00 fps`
(`src/ContentProbe.cpp`):
- **Billedfrekvens (målt):** billeder, der faktisk er nye, per sekund. Alle billeder fra kortet sammenlignes på et glidende
  gitter (også dem, der springes over for lav forsinkelse); kortets egen støj måles og trækkes fra. Står billedet helt
  stille (pause, menu), vises den sidste værdi med "(stillestående billede)".
- **Opløsning (skøn, "≈"):** et opskaleret billede har et spejlbillede af sit frekvensspektrum omkring den oprindelige
  opløsnings grænsefrekvens, uanset skaleringsmetode. Én gang i sekundet analyseres 48 linjer (FFT) på en baggrundstråd og
  medianen af de sidste 5 vises. Testet rigtigt på 720p/900p/1080p/1440p/dynamisk 1728 → 4K (bilineær, bikubisk, Lanczos,
  med skarphed, med HUD) og 720p → 1080p. Begrænsninger: spil med temporal opskalering (FSR 2/3, DLSS, TAAU,
  checkerboard) eller kornfilter efter opskaleringen genskaber fine detaljer og læses derfor som højere opløsning; meget
  flade scener giver intet skøn. `veyamux-probe-test` i build-mappen tester begge målinger på rå luma-filer.

## Støjfjerning
Menu → *Forbedring* → *Støjfjerning*: Fra / Rumlig / Temporal / Begge og Let / Middel / Kraftig. Kører først i kæden
(på kildebilledet før FXAA, opskalering og skarphed), så *Skarphed* kan skrues op uden at gøre støjen skarp.
- **Rumlig** (`shaders/LxNlm.hlsl`): non-local means – hver pixel blandes med pixels i et 7×7-område, hvis 3×3-omgivelser
  og farve ligner den. Billede for billede, ingen spøgelser ved bevægelse.
- **Temporal** (`shaders/LxTemporalNr.hlsl`): rekursivt gennemsnit over flere billeder langs bevægelsen fra VeyAmux'
  egen bevægelsesmåling (`MotionEstimator`), Catmull-Rom-sampling, historikken klemmes til det aktuelle 3×3-områdes
  værdier og vægtes ned, hvor den afviger mere end støjen kan forklare.
- **Begge**: temporal, derefter en mildere rumlig.
Målt på 960×540 med tilføjet støj (σ = 7/255, PSNR mod det rene billede; "detalje" = hvor meget af den ægte fine
detalje der er tilbage, 1,00 = alt): støjfyldt 31,2–31,3 dB. Kraftig: Rumlig 37,1–37,4 dB, Temporal 35,0–38,1 dB,
Begge 36,9–40,9 dB, alle med detalje 0,97–1,02. En rød boks, der flytter 40 px/billede over grå baggrund, gav ingen
spor eller glorier (ingen pixel over 30/255 fra det rene billede). Hastighed på et rigtigt grafikkort er ikke målt her.

## Bevægelsesmåling (FSR-opskalering)
Video har ingen bevægelsesvektorer fra en spilmotor, så VeyAmux måler dem selv til FSR 3.1-opskaleringen og den temporale støjfjerning
(`src/MotionEstimator.cpp`): luma-pyramide med op til 7 niveauer, regulariseret Lucas-Kanade fra groft til fint,
udfyldning af flade områder og til sidst skarpe bevægelseskanter (`LxMvSelect`). Over 2560 px bredde måles i halv opløsning.

## Gentagne billeder (30 fps-spil i 60 Hz-signal)
Når frame generation (Lossless Scaling) er slået til, sorteres capture-billeder, der er en gentagelse af det
forrige, fra *før* behandlingen (`FrameDiff` i `src/ContentProbe.cpp`; kortstøj måles og trækkes fra, og en gentagelse
sammenlignes med det sidst viste billede, så langsomme overtoninger stadig kommer igennem). Frame generation laver så
mellembilleder mellem to forskellige billeder: et 30 fps-spil bliver til jævne 60 fps i stedet for 60 fps med dobbelte
billeder.

Lossless Scaling: lsfg-vk skriver sin log (niveau debug) til `~/.config/VeyAmux/lsfg-vk.log`; VeyAmux viser i menuen
(og i terminalen), om laget blev indlæst, og hvilken fejl lsfg-vk eventuelt melder. *Ydelsestilstand* slår lsfg-vk's
performance mode til; flow scale vælges automatisk efter vinduets opløsning (4K 50 %, 1440p 75 %, ellers 100 %, som
Lossless Scaling anbefaler; valgt efter skærmen, så den ikke skifter – hvert skift får lsfg-vk til at bygge alt forfra).
Begge dele genindlæses af lsfg-vk, mens den kører. lsfg-vk 2 har ingen frame pacing (eneste tilstand er "vsync":
billeder vises, så snart de er klar), så resultatet er kun jævnt, når skærmens Hz er et helt multiplum af den nye
billedfrekvens (fx 60 fps × 2 = 120 fps: sæt skærmen til 120 Hz).

## OBS (browserkilde)
Menu → *OBS (browserkilde)*: slå til, vælg opløsning (720p/1080p/1440p/original) og tryk **Gem browserfil…** for at
gemme en HTML-fil, hvor du vil. I OBS: *Kilder → + → Browser → Lokal fil →* vælg filen, og sæt bredde/højde til dit lærred.
VeyAmux kører en lille HTTP-server, der kun lytter på `127.0.0.1:47920` (indstillingen `obs/port`), og sender det færdige
billede som MJPEG – efter alle forbedringer, i samme tempo som på skærmen (Lossless Scaling's mellembilleder laves
først bagefter i grafikdriverens lag og kommer ikke med i OBS) (`src/ObsStream.cpp`). I den direkte capture-vej skaleres billedet ned og kopieres fra GPU'en (`LxDownscale`,
afsluttet via en Vulkan-event, så det ikke venter på næste skærmopdatering); det sker kun, mens OBS faktisk er tilsluttet.
Siden er gennemsigtig, mens VeyAmux er lukket, og forbinder selv igen. Kun billedet sendes: lyd tages med OBS'
*Application Audio Capture* (VeyAmux) eller direkte fra capture-kortet. Adressen `http://127.0.0.1:47920/` virker også
som URL-browserkilde, og `/snapshot.jpg` giver det seneste billede.

## Opskalering
Menu → *Forbedring* → *Opskalering* (1,5× / 2× / 3×) og *Metode*:
- **Lanczos** (standard) – skarp klassisk skalering billede for billede, ingen ekstra forsinkelse.
- **AMD FSR 3.1** (eksperimentel) – AMD FidelityFX SDK 1.1.4 (open source). FSR 3.1 er en *temporal* opskalerer: den samler
  detaljer over flere billeder og skal vide, hvordan hver pixel har bevæget sig, ned til brøkdele af en pixel. Spil giver
  den præcise bevægelsesvektorer og rykker kameraet en brøkdel af en pixel hvert billede (jitter); video har ingen af delene.
  VeyAmux måler derfor bevægelsen selv (luma-pyramide + regulariseret Lucas-Kanade fra groft til fint, med udfyldning af flade
  områder; `shaders/LxLuma.hlsl`, `LxLkLevel.hlsl`, `LxMvFill.hlsl`) og kører AMD's RCAS-skarphed bagefter.
  FidelityFX optical flow (som frame generation bruger) blev forsøgt først, men giver kun hele pixels og ingen bevægelse de
  første 6 billeder efter hver nulstilling.
  **Målt resultat** (640×360 → 1280×720, PSNR mod de ægte 1280×720-billeder, panorering med skiftende sub-pixel-forskydning,
  software-GPU): naturligt motiv Lanczos 37,3 dB / FSR 3.1 30,0 dB (33,5 dB selv med perfekt kendt bevægelse); blødt motiv
  36,4 / 32,8 dB; meget detaljeret motiv 26,6 / 25,6 dB (27,2 dB med perfekt bevægelse – det eneste tilfælde, hvor FSR slår
  Lanczos). Konklusion: på video er FSR 3.1 blødere end Lanczos; derfor er Lanczos standard. FSR 4.1.2 findes kun som lukkede
  Windows-DLL'er og kan ikke bruges i et Linux-program.

## Frame generation
Menu → *Frame generation*:
- **Lossless Scaling – Fast 2× (fordobler, standard) / Fast 3× (tredobler)** – kræver at du har købt Lossless Scaling på Steam og har installeret
  [lsfg-vk](https://lsfg-vk.dev) 2.0+. VeyAmux finder selv Lossless Scaling i alle dine Steam-biblioteker ud fra `$HOME`
  (fx `/var/home/<bruger>/.local/share/Steam/steamapps/common/Lossless Scaling/` på Bazzite, også Flatpak-Steam og ekstra
  biblioteker fra `libraryfolders.vdf`), skriver sin egen profil til `~/.config/VeyAmux/lsfg-vk.toml` og starter lsfg-vk for
  sit eget vindue via `LSFGVK_CONFIG`/`LSFGVK_PROFILE`. Din egen lsfg-vk-konfiguration røres ikke.
  - lsfg-vk 2 bruger `lsfg-vk.dll` (fra Lossless Scalings *lsfg-vk*-gren i Steam, fanen Betaer), ikke `Lossless.dll`.
  - **Fast 3×**: tre gange billedfrekvensen (60 → 180 fps). Kræver en skærm med mindst den frekvens.
  - Til/fra-skift kræver genstart (knap i menuen), fordi laget indlæses ved opstart. Crasher VeyAmux under opstart med
    lsfg-vk, slås Lossless Scaling automatisk fra, indtil du trykker *Prøv igen*.
  - VeyAmux indeholder/kopierer ingen lsfg-vk- eller Lossless Scaling-kode (lsfg-vk er CC BY-NC-ND 4.0) og bundler dem ikke.

## HDR-capture (P010)
Live Gamer Ultra 2.1 (og andre kort) kan levere **P010**: 10-bit Y'CbCr 4:2:0 med HDR (BT.2020-farver, PQ/SMPTE ST 2084).
Linux' uvcvideo kender formatet; i VeyAmux' capture-dialog hedder tilstandene "P010 (HDR)". P010 vælges aldrig
automatisk (dobbelt USB-båndbredde; 4K60 P010 er ca. 12 Gbit/s og passer ikke i kortets USB 3.2 Gen 2 – vælg fx 4K30
eller 1440p60, alt efter hvad kortet tilbyder). De 16-bit planer uploades som R16/RG16 og `shaders/LxP010.hlsl` regner
direkte til VeyAmux' lineære arbejdsbillede (ingen 8-bit-mellemtrin): BT.2020 NCL → PQ-afkodning → BT.2390 EETF
(Hermite-skulder i PQ-domænet) og nuance-bevarende gamut-komprimering til BT.709, SDR-hvid = 203 nits (BT.2408) – Veyras
egne HDR-funktioner. *HDR-capture: kildens maks. lysstyrke* (600/1000/2000/4000 nits, standard 1000) skal passe til
spillets/konsollens HDR-kalibrering. Testet med et syntetisk P010-signal (`VEYAMUX_TEST_LIVE=p010`: PQ-gråskala
0–4000 nits, 203-nits farvefelter i BT.2020) i den direkte capture-vej uden Vulkan-valideringsfejl; ikke testet med selve
kortet. Visning i ægte HDR på skærmen er ikke lavet (AppImage'en bruger Qt 6.4, som ikke kan levere HDR-swapchains, og
VeyAmux kører gennem XWayland, som ikke har HDR).

## VRR-tilstand
Et capture-kort optager altid med fast frekvens (VRR på kortet er kun HDMI-gennemføring), så et 45 fps-spil kommer som
billeder med 1, 1 og 2 signalbilleders afstand (16,7 / 16,7 / 33,3 ms ved 60 Hz) – også på en VRR-skærm ser det hakkende ud.
*Frame generation → Fra → VRR* sorterer gentagne billeder fra og holder hvert nyt billede, til der er gået ét
indholdsinterval (målt billedfrekvens, ellers et løbende gennemsnit) siden det forrige, højst ¾ interval. Simuleret
(45/40/50 fps i 60 Hz, 1 ms USB-jitter): uregelmæssighed i billedafstand 6,7–8,3 ms → 0,8–1,2 ms, ekstra forsinkelse i
gennemsnit 4–5,5 ms. VRR-tilstanden kører med vsync (kræver genstart, knap i menuen), så skærmen viser hvert billede helt,
når det præsenteres. Kræver *Adaptiv synkronisering* i KDE's skærmindstillinger og VeyAmux i fuldskærm. Kan ikke
kombineres med Lossless Scaling (lsfg-vk kræver fast skærmfrekvens).

## Sprog
Engelsk som standard; dansk automatisk når sprog **eller** region er dansk (`da_DK`, `en_DK`, dansk `LC_TIME` …).

## Lav latens (capture)
Samme princip som Windows-versionen: billedet forlader aldrig GPU'en. Rå YUYV/UYVY/NV12-frames uploades, farvekonverteres og
forbedres i Vulkan *direkte i Qt's egen frame* (samme Vulkan-enhed, ingen readback, ingen CPU-konvertering), og V4L2-køen
tømmes så kun det nyeste billede bruges ("mailbox"). Vsync er slået fra som standard (`VEYAMUX_VSYNC=1` slår det til).
MJPEG dekodes stadig på CPU'en.
Start-logik: programmet afprøver Vulkan-UI i en barneproces og gemmer resultatet; på Wayland prøves XWayland, før det
falder tilbage til software. `VEYAMUX_UI=software|vulkan` tvinger valg, `--reset-ui` nulstiller det gemte valg.

## Capture-kort
Tryk **C** (eller kamera-ikonet). Vælg enhed, tilstand (★ = anbefalet, ≥50 fps foretrækkes) og lydkilde. Start direkte fra terminal:
`./VeyAmux-x86_64.AppImage --capture /dev/video0`. Kilden antages at være limited-range (typisk for HDMI-capture).

## Miljøvariabler
`VEYAMUX_UI=software|vulkan` · `VEYAMUX_VSYNC=1` · `--reset-ui` · `VEYAMUX_LSFG=0` (slå lsfg-vk fra) · `VEYAMUX_DEBUG_FG=1` (logger frame generation og opskalering i den direkte capture-vej) · `VEYAMUX_NO_GPU=1` (slå Vulkan-kæden fra) · `VEYAMUX_HWDEC=0` (slå VAAPI fra) · `VEYAMUX_VAAPI_DEVICE=/dev/dri/renderD128` · `VEYAMUX_VK_DEVICE=n` · `VEYAMUX_SHADER_DIR`

## Genvejstaster
Mellemrum afspil/pause · ←/→ ±5 s (Shift ±30 s) · ↑/↓ volumen · F fuldskærm · E forbedring til/fra · P panel · M lyd fra · O åbn

## Arkitektur
`VkContext` (Vulkan, ét universelt descriptor-layout t→0, u→16, b→32, s→48 via DXC `-fvk-*-shift`) → `EnhanceChain` →
`MediaEngine` (demux/video/lyd/render-tråde) → `PlayerBridge`/`VideoItem` (Qt/QML).
Licens: GPL-3.0 (som det oprindelige Veyra-NRVideo, som VeyAmux bygger på). Tredjepart: se `THIRD_PARTY_NOTICES_LINUX.md`.

Testværktøjer i build-mappen: `veyamux-upscale-test <shaders> <in.rgba> <w> <h> <frames> <out.rgba> lanczos|fsr [skala]` (hele forbedringskæden, til
PSNR-sammenligning af opskaleringsmetoderne; `VEYAMUX_TEST_NR=tilstand,styrke` slår støjfjerning til).
