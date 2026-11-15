import QtQuick
import QtQuick.Window
import VeyAmux 1.0

Window {
    id: win
    visible: false          // shown from C++ once the graphics device is configured
    width: 1280
    height: 720
    minimumWidth: 720
    minimumHeight: 420
    color: "#0b0c10"
    title: player.hasMedia ? player.title + " — VeyAmux" : "VeyAmux"

    readonly property bool fs: visibility === Window.FullScreen
    property bool panelOpen: startPanel
    property bool controlsShown: true
    property bool scrubbing: false
    property real scrubValue: 0

    readonly property var strings: ({
        en: { open: "Open file", drop: "Drop a video here, or open a file", enhance: "Enhance",
              upscale: "Upscale", off: "Off", aa: "Anti-aliasing", aaq: ["Medium", "High", "Ultra"],
              sharpen: "Sharpness", exposure: "Exposure", contrast: "Contrast", saturation: "Saturation",
              hdr: "HDR capture (P010): source peak brightness",
              hdrHint: "Used when the capture card sends HDR (pick a P010 (HDR) mode under Capture card). The HDR picture is tone-mapped to your SDR display (BT.2390, as in Veyra); match the peak to the game's HDR calibration - lower = brighter.",
              vrr: "VRR: the game's own frame rate, evenly paced",
              vrrHint: "Shows only the game's new pictures, spaced evenly at its own rate (e.g. 45 fps), for a display with VRR/adaptive sync. Turn on Adaptive sync in the display settings and run VeyAmux fullscreen. Adds up to about half a frame of latency.",
              vrrRestart: "VeyAmux must restart to switch vsync for VRR.",
              denoise: "Noise reduction", denoiseModes: ["Off", "Spatial", "Temporal", "Both"], denoiseLevels: ["Light", "Medium", "Strong"],
              denoiseHint0: "Removes grain, dither and compression noise before upscaling and sharpening, so Sharpness can be turned up without sharpening the noise.",
              denoiseHint1: "Non-local means: averages each pixel with similar-looking pixels nearby. Frame by frame, never smears motion; strong settings soften fine texture a little.",
              denoiseHint2: "Averages each pixel over several frames along the measured motion: keeps the most detail. Fast, unpredictable motion falls back to the current frame.",
              denoiseHint3: "Temporal first, then a gentle spatial pass: the cleanest picture. Pick the level after how noisy the source is.",
              reset: "Reset", gpu: "GPU", cpuOnly: "No GPU — passthrough", hint: "Space play/pause · ←/→ seek · F fullscreen · E enhance · M mute · C capture card",
              native: "Native",
              method: "Method", lanczos: "Lanczos", fsr31: "AMD FSR 3.1",
              lanczosHint: "Sharp classic scaling, frame by frame. No added latency.",
              fsr31Hint: "AMD FidelityFX (open source), builds the picture over several frames from motion VeyAmux measures in the video. Experimental: FSR is made for games, and in VeyAmux's tests on video it came out softer than Lanczos.",
              fsrNeedsScale: "Choose 1.5×, 2× or 3× to use it.",
              fsrFallback: "FSR 3.1 could not start, using Lanczos: ",
              capture: "Capture card", device: "Device", mode: "Mode (resolution · rate · format)", audio: "Audio source", noAudio: "No audio", defaultInput: "Default input (system default microphone)",
              noDevices: "No capture devices found. Check that the card is connected (it must appear as /dev/video*).",
              refresh: "Refresh", cancel: "Cancel", start: "Start", live: "LIVE",
              fg: "Frame generation", fgOff: "Off",
              obs: "OBS (browser source)",
              obsOff: "Off. Turn on to stream the picture to OBS while you use VeyAmux.",
              obsWaiting: "Ready - waiting for OBS (%1).",
              obsLive: "OBS is receiving the picture (%1 connected).",
              obsRes: "Resolution sent to OBS",
              obsSave: "Save browser file…", obsCopy: "Copy address",
              obsSaved: "Browser file: %1",
              obsHelp: "In OBS: Sources → + → Browser → tick \"Local file\" → pick the saved file, and set width/height to your canvas (e.g. 1920 × 1080). Only the video is sent: add the sound with Application Audio Capture (VeyAmux) or straight from the capture card.",
              lsPerf: "Performance mode",
              lsPerfHint: "Lighter Lossless Scaling model: faster, slightly lower quality. Flow scale is set automatically for your resolution (now %1 %).",
              lsFixed: "Fixed 2× (doubles)", lsFixedHint: "Always doubles the frame rate (60 → 120 fps).",
              ls3x: "Fixed 3× (triples)", ls3xHint: "Always triples the frame rate (60 → 180 fps, 40 → 120 fps). Needs a display fast enough for the result.",
              lsActive: "Active through lsfg-vk. Lossless Scaling: %1", lsReady: "Lossless Scaling found: %1",
              lsRestart: "VeyAmux must restart to turn Lossless Scaling on.",
              lsNoInstall: "Lossless Scaling was not found in your Steam libraries. It has to be bought and installed through Steam.",
              lsNoDll: "Only Lossless.dll was found. lsfg-vk 2 needs lsfg-vk.dll: in Steam open Lossless Scaling → Properties → Betas and choose the lsfg-vk branch.",
              lsNoLayer: "lsfg-vk is not installed. Install it from lsfg-vk.dev; VeyAmux uses it to run Lossless Scaling on Linux.",
              lsOldLayer: "An older lsfg-vk (1.x) was found. VeyAmux needs lsfg-vk 2.0 or newer.",
              lsNoVulkan: "Needs the Vulkan display mode; VeyAmux is running in software mode.",
              lsCrashed: "VeyAmux closed unexpectedly the last time it started with Lossless Scaling, so it has been turned off.",
              restartNow: "Restart now", tryAgain: "Try again", chooseFolder: "Choose folder…", checkAgain: "Check again" },
        da: { open: "Åbn fil", drop: "Træk en video hertil, eller åbn en fil", enhance: "Forbedring",
              upscale: "Opskalering", off: "Fra", aa: "Anti-aliasing", aaq: ["Medium", "Høj", "Ultra"],
              sharpen: "Skarphed", exposure: "Eksponering", contrast: "Kontrast", saturation: "Farvemætning",
              hdr: "HDR-capture (P010): kildens maks. lysstyrke",
              hdrHint: "Bruges når capture-kortet sender HDR (vælg en P010 (HDR)-tilstand under Capture-kort). HDR-billedet tonemappes til din SDR-skærm (BT.2390, som i Veyra); vælg værdien efter spillets HDR-kalibrering – lavere = lysere.",
              vrr: "VRR: spillets egen billedfrekvens i jævn takt",
              vrrHint: "Viser kun spillets nye billeder, jævnt fordelt i dets egen takt (fx 45 fps), til en skærm med VRR/adaptiv synkronisering. Slå Adaptiv synkronisering til i skærmindstillingerne, og kør VeyAmux i fuldskærm. Giver op til ca. et halvt billedes ekstra forsinkelse.",
              vrrRestart: "VeyAmux skal genstarte for at skifte vsync til VRR.",
              denoise: "Støjfjerning", denoiseModes: ["Fra", "Rumlig", "Temporal", "Begge"], denoiseLevels: ["Let", "Middel", "Kraftig"],
              denoiseHint0: "Fjerner korn, dithering og komprimeringsstøj før opskalering og skarphed, så Skarphed kan skrues op uden at gøre støjen skarp.",
              denoiseHint1: "Non-local means: blander hver pixel med pixels i nærheden, der ligner den. Billede for billede, slører aldrig bevægelse; kraftig gør fin tekstur en anelse blødere.",
              denoiseHint2: "Midler hver pixel over flere billeder langs den målte bevægelse: bevarer flest detaljer. Ved hurtig, uforudsigelig bevægelse bruges det aktuelle billede.",
              denoiseHint3: "Først temporal, så en mild rumlig: det reneste billede. Vælg niveau efter hvor støjfyldt kilden er.",
              reset: "Nulstil", gpu: "GPU", cpuOnly: "Ingen GPU — uden forbedring", hint: "Mellemrum afspil/pause · ←/→ spol · F fuldskærm · E forbedring · M lyd fra · C capture-kort",
              native: "Original",
              method: "Metode", lanczos: "Lanczos", fsr31: "AMD FSR 3.1",
              lanczosHint: "Skarp klassisk skalering, billede for billede. Ingen ekstra forsinkelse.",
              fsr31Hint: "AMD FidelityFX (open source), opbygger billedet over flere billeder ud fra bevægelse, som VeyAmux måler i videoen. Eksperimentel: FSR er lavet til spil, og i VeyAmux' test på video blev den blødere end Lanczos.",
              fsrNeedsScale: "Vælg 1,5×, 2× eller 3× for at bruge den.",
              fsrFallback: "FSR 3.1 kunne ikke starte, bruger Lanczos: ",
              capture: "Capture-kort", device: "Enhed", mode: "Tilstand (opløsning · frekvens · format)", audio: "Lydkilde", noAudio: "Ingen lyd", defaultInput: "Standard-indgang (systemets standardmikrofon)",
              noDevices: "Ingen capture-enheder fundet. Tjek at kortet er tilsluttet (det skal vises som /dev/video*).",
              refresh: "Opdater", cancel: "Annuller", start: "Start", live: "LIVE",
              fg: "Frame generation", fgOff: "Fra",
              obs: "OBS (browserkilde)",
              obsOff: "Fra. Slå til for at sende billedet til OBS, mens du bruger VeyAmux.",
              obsWaiting: "Klar - venter på OBS (%1).",
              obsLive: "OBS modtager billedet (%1 tilsluttet).",
              obsRes: "Opløsning til OBS",
              obsSave: "Gem browserfil…", obsCopy: "Kopiér adresse",
              obsSaved: "Browserfil: %1",
              obsHelp: "I OBS: Kilder → + → Browser → sæt flueben i \"Lokal fil\" → vælg den gemte fil, og sæt bredde/højde til dit lærred (fx 1920 × 1080). Kun billedet sendes: tilføj lyden med programlyd-optagelse (Application Audio Capture → VeyAmux) eller direkte fra capture-kortet.",
              lsPerf: "Ydelsestilstand (performance mode)",
              lsPerfHint: "Lettere Lossless Scaling-model: hurtigere, lidt lavere kvalitet. Flow scale sættes automatisk efter din opløsning (nu %1 %).",
              lsFixed: "Fast 2× (fordobler)", lsFixedHint: "Fordobler altid billedfrekvensen (60 → 120 fps).",
              ls3x: "Fast 3× (tredobler)", ls3xHint: "Tredobler altid billedfrekvensen (60 → 180 fps, 40 → 120 fps). Kræver en skærm, der er hurtig nok til resultatet.",
              lsActive: "Aktiv via lsfg-vk. Lossless Scaling: %1", lsReady: "Lossless Scaling fundet: %1",
              lsRestart: "VeyAmux skal genstartes for at slå Lossless Scaling til.",
              lsNoInstall: "Lossless Scaling blev ikke fundet i dine Steam-biblioteker. Det skal købes og installeres via Steam.",
              lsNoDll: "Kun Lossless.dll blev fundet. lsfg-vk 2 kræver lsfg-vk.dll: åbn Lossless Scaling i Steam → Egenskaber → Betaer, og vælg lsfg-vk-grenen.",
              lsNoLayer: "lsfg-vk er ikke installeret. Installér det fra lsfg-vk.dev; VeyAmux bruger det til at køre Lossless Scaling på Linux.",
              lsOldLayer: "Der blev fundet en ældre lsfg-vk (1.x). VeyAmux kræver lsfg-vk 2.0 eller nyere.",
              lsNoVulkan: "Kræver Vulkan-visning; VeyAmux kører i software-tilstand.",
              lsCrashed: "VeyAmux lukkede uventet, sidste gang det startede med Lossless Scaling, så det er slået fra.",
              restartNow: "Genstart nu", tryAgain: "Prøv igen", chooseFolder: "Vælg mappe…", checkAgain: "Tjek igen" }
    })
    function t(k) { var s = strings[uiLanguage] || strings.en; return s[k] !== undefined ? s[k] : k }
    function fmt(sec) {
        sec = Math.max(0, Math.floor(sec))
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function wake() { controlsShown = true; hideTimer.restart() }
    readonly property var scales: [1.0, 1.5, 2.0, 3.0]

    Timer { id: hideTimer; interval: 2600; onTriggered: if (player.playing && !panelOpen && !scrubbing && !keepControls) controlsShown = false }

    // ------------------------------------------------------------- video surface
    VideoItem { id: video; anchors.fill: parent; bridge: player }

    // Empty state
    Column {
        visible: !player.hasMedia
        anchors.centerIn: parent
        spacing: 18
        Image { anchors.horizontalCenter: parent.horizontalCenter; source: "icons/logo.svg"; sourceSize: Qt.size(112, 112); width: 112; height: 112; smooth: true }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "VeyAmux"; color: "#ffffff"; font.pixelSize: 54; font.weight: Font.DemiBold; font.letterSpacing: 2 }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: t("drop"); color: "#8b90a0"; font.pixelSize: 15 }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 12
            Rectangle {
                width: 160; height: 44; radius: 22; color: openMa.containsMouse ? "#ff8f63" : "#ff7a45"
                Text { anchors.centerIn: parent; text: t("open"); color: "#14110f"; font.pixelSize: 15; font.weight: Font.DemiBold }
                MouseArea { id: openMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.openDialog() }
            }
            Rectangle {
                width: 160; height: 44; radius: 22; color: capMa.containsMouse ? "#2b2e38" : "#1d2028"; border.color: "#33ffffff"
                Text { anchors.centerIn: parent; text: t("capture"); color: "#e8eaf0"; font.pixelSize: 15; font.weight: Font.DemiBold }
                MouseArea { id: capMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: capDlg.openDialog() }
            }
        }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: t("hint"); color: "#555a69"; font.pixelSize: 12 }
    }

    // Click / hover / double-click on the video
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        enabled: player.hasMedia
        cursorShape: (win.controlsShown || !win.fs) ? Qt.ArrowCursor : Qt.BlankCursor
        onPositionChanged: wake()
        onClicked: { player.togglePlay(); wake() }
        onDoubleClicked: { player.togglePlay(); win.visibility = fs ? Window.Windowed : Window.FullScreen }
    }

    DropArea {
        anchors.fill: parent
        onDropped: (drop) => { if (drop.hasUrls) player.openUrl(drop.urls[0]) }
    }

    // ------------------------------------------------------------- status toast
    Rectangle {
        visible: player.status !== ""
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 18 }
        width: Math.min(parent.width - 40, toastText.implicitWidth + 32); height: 36; radius: 18; color: "#cc1b1d24"
        border.color: "#44ff7a45"
        Text { id: toastText; anchors.centerIn: parent; text: player.status; color: "#ffb89c"; font.pixelSize: 13; elide: Text.ElideRight; width: parent.width - 28; horizontalAlignment: Text.AlignHCenter }
    }

    // ------------------------------------------------------------- title (top)
    Rectangle {
        id: topBar
        visible: player.hasMedia
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 70
        opacity: controlsShown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        gradient: Gradient { GradientStop { position: 0; color: "#b0000000" } GradientStop { position: 1; color: "#00000000" } }
        Text { x: 22; y: 18; width: parent.width - 44; text: player.title; color: "#ffffff"; font.pixelSize: 16; font.weight: Font.Medium; elide: Text.ElideRight }
        Text { x: 22; y: 40; text: player.mediaInfo; color: "#9da2b3"; font.pixelSize: 12 }
    }

    // ------------------------------------------------------------- bottom controls
    Rectangle {
        id: bottomBar
        visible: player.hasMedia
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: 110
        opacity: controlsShown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        gradient: Gradient { GradientStop { position: 0; color: "#00000000" } GradientStop { position: 1; color: "#d0000000" } }

        // seek bar
        Item {
            id: seek
            visible: player.duration > 0
            anchors { left: parent.left; right: parent.right; bottom: controlsRow.top; leftMargin: 22; rightMargin: 22; bottomMargin: 4 }
            height: 22
            Text { id: tCur; anchors.verticalCenter: parent.verticalCenter; text: fmt(scrubbing ? scrubValue : player.position); color: "#e8eaf0"; font.pixelSize: 12; width: 52 }
            VSlider {
                anchors { left: tCur.right; right: tDur.left; verticalCenter: parent.verticalCenter; leftMargin: 6; rightMargin: 6 }
                from: 0; to: Math.max(player.duration, 0.001)
                value: scrubbing ? scrubValue : player.position
                onDraggingChanged: { if (dragging) { win.scrubbing = true } else { win.scrubbing = false; player.seek(win.scrubValue) } }
                onMoved: (v) => { win.scrubValue = v; wake() }
            }
            Text { id: tDur; anchors { right: parent.right; verticalCenter: parent.verticalCenter } text: fmt(player.duration); color: "#9da2b3"; font.pixelSize: 12; width: 52; horizontalAlignment: Text.AlignRight }
        }

        Rectangle {
            visible: player.live
            anchors { left: parent.left; bottom: controlsRow.top; leftMargin: 22; bottomMargin: 10 }
            width: 58; height: 22; radius: 11; color: "#e5484d"
            Text { anchors.centerIn: parent; text: t("live"); color: "#ffffff"; font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 1 }
        }
        Row {
            id: controlsRow
            anchors { left: parent.left; bottom: parent.bottom; leftMargin: 14; bottomMargin: 10 }
            spacing: 2
            IconButton { size: 40; source: "icons/folder.svg"; onClicked: player.openDialog() }
            IconButton { size: 40; source: "icons/capture.svg"; active: player.live; onClicked: capDlg.openDialog() }
            IconButton { size: 40; source: "icons/back10.svg"; onClicked: { player.seekRelative(-10); wake() } }
            IconButton { size: 46; source: player.playing ? "icons/pause.svg" : "icons/play.svg"; onClicked: { player.togglePlay(); wake() } }
            IconButton { size: 40; source: "icons/fwd10.svg"; onClicked: { player.seekRelative(10); wake() } }
            IconButton { size: 40; source: player.muted || player.volume < 0.01 ? "icons/mute.svg" : "icons/volume.svg"; onClicked: player.muted = !player.muted }
            VSlider {
                width: 90; anchors.verticalCenter: parent.verticalCenter
                from: 0; to: 1.0; value: player.muted ? 0 : player.volume
                onMoved: (v) => { player.muted = false; player.volume = v }
            }
            Item { width: 14; height: 1 }
            IconButton { size: 40; source: "icons/sparkles.svg"; active: win.panelOpen; onClicked: { win.panelOpen = !win.panelOpen; wake() } }
            IconButton { size: 40; source: win.fs ? "icons/exitfs.svg" : "icons/fullscreen.svg"; onClicked: win.visibility = win.fs ? Window.Windowed : Window.FullScreen }
        }
    }

    // ------------------------------------------------------------- enhancement panel
    // Half the window wide, everything visible at once (no scrolling): the sections sit in three columns (two in narrow
    // windows), and if they still do not fit the height, the whole content is scaled down to fit.
    Rectangle {
        id: panel
        width: Math.max(560, parent.width / 2 - 18)
        // as tall as the content needs, at most the window height (then the content is scaled to fit)
        height: Math.min(parent.height - 24, area.contentH + 36)
        y: 12
        x: parent.width - width - 12            // always anchored; never animates on window resize (fullscreen)
        opacity: panelOpen ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
        radius: 16
        color: "#ee14161c"
        border.color: "#22ffffff"
        MouseArea { anchors.fill: parent; hoverEnabled: true; onPositionChanged: wake() }   // keep controls awake & eat clicks

        component Param: Column {
            id: p
            property string label
            property real from; property real to; property real value; property real neutral: NaN
            property int decimals: 2
            signal moved(real v)
            width: parent.width; spacing: 4; opacity: player.enhance ? 1 : 0.45
            Row {
                width: parent.width
                Text { text: p.label; color: "#9da2b3"; font.pixelSize: 12; width: parent.width - 50 }
                Text { text: p.value.toFixed(p.decimals); color: "#e8eaf0"; font.pixelSize: 12; width: 50; horizontalAlignment: Text.AlignRight }
            }
            VSlider { width: parent.width; from: p.from; to: p.to; value: p.value; neutral: p.neutral; onMoved: (v) => p.moved(v) }
        }

        Item {
            id: area
            anchors { fill: parent; margins: 18 }
            readonly property int cols: width >= 820 ? 3 : 2
            readonly property real gap: 22
            readonly property real colW: (width - (cols - 1) * gap) / cols
            // natural size of the laid-out sections, then one scale factor so it all fits
            readonly property real contentW: cols === 3 ? 3 * colW + 2 * gap : 2 * colW + gap
            readonly property real contentH: cols === 3 ? Math.max(secA.height, secB.height, secC.height)
                                                        : Math.max(secA.height + gap + secB.height, secC.height)
            readonly property real fit: Math.min(1, (win.height - 24 - 36) / Math.max(1, contentH))

            Item {
                id: content
                width: area.contentW; height: area.contentH
                transformOrigin: Item.Top
                scale: area.fit
                x: (area.width - width) / 2    // centred; scaling keeps the top centre in place

                Column {
                    id: secA
                    x: 0; y: 0; width: area.colW
                    spacing: 16
                    Row {
                        width: parent.width
                        Text { text: t("enhance"); color: "#ffffff"; font.pixelSize: 17; font.weight: Font.DemiBold; width: parent.width - 44; anchors.verticalCenter: parent.verticalCenter }
                        ToggleSwitch { checked: player.enhance; onToggled: (v) => player.enhance = v; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Column {
                        width: parent.width; spacing: 8; opacity: player.enhance ? 1 : 0.45
                        Text { text: t("upscale"); color: "#9da2b3"; font.pixelSize: 12 }
                        Segmented {
                            width: parent.width; enabled: player.enhance
                            model: [t("native"), "1.5×", "2×", "3×"]
                            current: { var i = scales.indexOf(Math.round(player.scale * 10) / 10); return i < 0 ? 0 : i }
                            onPicked: (i) => player.scale = scales[i]
                        }
                        Text { text: t("method"); color: "#9da2b3"; font.pixelSize: 12; topPadding: 2 }
                        Segmented {
                            width: parent.width; enabled: player.enhance
                            model: [t("lanczos"), t("fsr31")]
                            current: player.upscaler
                            onPicked: (i) => player.upscaler = i
                        }
                        Text {
                            width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 11
                            color: player.upscalerError !== "" ? "#ffb89c" : "#8b90a0"
                            text: player.upscalerError !== "" ? t("fsrFallback") + player.upscalerError
                                  : (player.upscaler === 1 ? t("fsr31Hint") + (player.scale <= 1.001 ? " " + t("fsrNeedsScale") : "")
                                                           : t("lanczosHint"))
                        }
                    }
                    Column {
                        width: parent.width; spacing: 8; opacity: player.enhance ? 1 : 0.45
                        Text { text: t("denoise"); color: "#9da2b3"; font.pixelSize: 12 }
                        Segmented {
                            width: parent.width; model: t("denoiseModes"); enabled: player.enhance
                            current: player.denoise; onPicked: (i) => player.denoise = i
                        }
                        Segmented {
                            width: parent.width; model: t("denoiseLevels"); enabled: player.enhance && player.denoise > 0
                            current: player.denoiseStrength; onPicked: (i) => player.denoiseStrength = i
                        }
                        Text {
                            width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 11; color: "#8b90a0"
                            text: t("denoiseHint" + player.denoise)
                        }
                        Text { text: t("hdr"); color: "#9da2b3"; font.pixelSize: 12; topPadding: 6 }
                        Segmented {
                            width: parent.width; model: ["600", "1000", "2000", "4000 nits"]
                            readonly property var peaks: [600, 1000, 2000, 4000]
                            current: Math.max(0, peaks.indexOf(player.hdrPeak))
                            onPicked: (i) => player.hdrPeak = peaks[i]
                        }
                        Text { width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 11; color: "#8b90a0"; text: t("hdrHint") }
                    }
                    Column {
                        width: parent.width; spacing: 8; opacity: player.enhance ? 1 : 0.45
                        Row {
                            width: parent.width
                            Text { text: t("aa") + " (FXAA)"; color: "#9da2b3"; font.pixelSize: 12; width: parent.width - 44; anchors.verticalCenter: parent.verticalCenter }
                            ToggleSwitch { checked: player.fxaa; enabled: player.enhance; onToggled: (v) => player.fxaa = v; anchors.verticalCenter: parent.verticalCenter }
                        }
                        Segmented {
                            width: parent.width; model: t("aaq"); enabled: player.enhance && player.fxaa
                            current: player.fxaaQuality; onPicked: (i) => player.fxaaQuality = i
                        }
                    }
                }
                Column {
                    id: secB
                    x: area.cols === 3 ? area.colW + area.gap : 0
                    y: area.cols === 3 ? 0 : secA.height + area.gap
                    width: area.colW
                    spacing: 16
                    Param { label: t("sharpen"); from: 0; to: 1; value: player.sharpen; onMoved: (v) => player.sharpen = v }
                    Param { label: t("exposure"); from: -2; to: 2; value: player.exposure; neutral: 0; onMoved: (v) => player.exposure = v }
                    Param { label: t("contrast"); from: 0.7; to: 1.4; value: player.contrast; neutral: 1; onMoved: (v) => player.contrast = v }
                    Param { label: t("saturation"); from: 0; to: 2; value: player.saturation; neutral: 1; onMoved: (v) => player.saturation = v }

                    Rectangle {
                        width: parent.width; height: 34; radius: 10; color: rstMa.containsMouse ? "#2b2e38" : "#1d2028"
                        Text { anchors.centerIn: parent; text: t("reset"); color: "#c9ccd6"; font.pixelSize: 13 }
                        MouseArea { id: rstMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.resetEnhance() }
                    }
                    Column {
                        width: parent.width; spacing: 3
                        Text { width: parent.width; text: t("gpu") + ": " + (player.gpuName !== "" ? player.gpuName : t("cpuOnly")); color: "#8b90a0"; font.pixelSize: 11; wrapMode: Text.Wrap }
                        Text { width: parent.width; text: player.perf; color: "#6c7182"; font.pixelSize: 11 }
                    }
                }
                Column {
                    id: secC
                    x: area.cols === 3 ? 2 * (area.colW + area.gap) : area.colW + area.gap
                    y: 0; width: area.colW
                    spacing: 16
                    FrameGenSection { width: parent.width; tr: win.t }
                    Rectangle { width: parent.width; height: 1; color: "#262933" }
                    ObsSection { width: parent.width; tr: win.t }
                }
            }
        }
    }

    CaptureDialog { id: capDlg; anchors.fill: parent; z: 100; tr: win.t }

    // ------------------------------------------------------------- keyboard
    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: (e) => {
            wake()
            if (capDlg.visible) { if (e.key === Qt.Key_Escape) { capDlg.visible = false; e.accepted = true } return }
            switch (e.key) {
            case Qt.Key_Space: player.togglePlay(); break
            case Qt.Key_Left: player.seekRelative(e.modifiers & Qt.ShiftModifier ? -30 : -5); break
            case Qt.Key_Right: player.seekRelative(e.modifiers & Qt.ShiftModifier ? 30 : 5); break
            case Qt.Key_Up: player.volume = Math.min(1.5, player.volume + 0.05); break
            case Qt.Key_Down: player.volume = Math.max(0, player.volume - 0.05); break
            case Qt.Key_M: player.muted = !player.muted; break
            case Qt.Key_F: case Qt.Key_F11: win.visibility = win.fs ? Window.Windowed : Window.FullScreen; break
            case Qt.Key_Escape: if (win.fs) win.visibility = Window.Windowed; break
            case Qt.Key_E: player.enhance = !player.enhance; break
            case Qt.Key_P: win.panelOpen = !win.panelOpen; break
            case Qt.Key_O: player.openDialog(); break
            case Qt.Key_C: capDlg.openDialog(); break
            default: return
            }
            e.accepted = true
        }
    }
    Component.onCompleted: { hideTimer.start(); if (startDialog) capDlg.openDialog() }
}
