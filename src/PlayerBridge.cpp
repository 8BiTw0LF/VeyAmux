#include "PlayerBridge.h"
#include <QCoreApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QVariantMap>
#include <QSettings>

using veyamux::MediaEngine;

PlayerBridge::PlayerBridge(const QString& shaderDir, QObject* parent)
    : QObject(parent), engine_(shaderDir.toStdString()) {
    loadSettings();
    connect(&obs_, &ObsStream::viewersChanged, this, [this] { updateObsSource(); emit obsChanged(); });
    if (QSettings().value("obs/enabled", false).toBool()) setObsEnabled(true);
    MediaEngine::Callbacks cb;
    cb.onFrame = [this](const uint8_t* rgba, int w, int h, double aspect) {
        QImage img = QImage(rgba, w, h, w * 4, QImage::Format_RGBA8888).copy();   // detach from engine buffer
        if (obs_.wantsFrames() && !liveVulkanActive()) obs_.pushFrame(img);        // shared, not copied
        bool post = false;
        {
            QMutexLocker l(&imgMutex_);
            image_ = std::move(img);
            aspect_ = aspect;
            post = !framePending_;
            framePending_ = true;
        }
        if (post) QMetaObject::invokeMethod(this, [this] {
            { QMutexLocker l(&imgMutex_); framePending_ = false; }
            emit frameReady();
        }, Qt::QueuedConnection);
    };
    cb.onEnded = [this] { QMetaObject::invokeMethod(this, [this] { emit ended(); emit playingChanged(); }, Qt::QueuedConnection); };
    cb.onError = [this](const std::string& m) {
        const QString q = QString::fromStdString(m);
        QMetaObject::invokeMethod(this, [this, q] { setStatus(translateMessage(q)); }, Qt::QueuedConnection);
    };
    cb.onGpuMessage = [this](const std::string& m) {
        const QString q = QString::fromStdString(m);
        QMetaObject::invokeMethod(this, [this, q] {
            if (q.startsWith("GPU:")) gpuName_ = q.mid(5); else setStatus(translateMessage(q));
            emit statsChanged();
        }, Qt::QueuedConnection);
    };
    cb.onLiveFrame = [this](veyamux::RawFrame&& f) { if (liveVk_) liveVk_->submit(std::move(f)); };
    engine_.setCallbacks(cb);
    applyEnhance();
    tick_.setInterval(100);
    connect(&tick_, &QTimer::timeout, this, [this] {
        if (!engine_.isOpen()) return;
        emit positionChanged();
        if (engine_.playing() != lastPlaying_) { lastPlaying_ = engine_.playing(); emit playingChanged(); }
        const auto st = engine_.stats();
        updateOutputFps(liveVulkanActive() ? liveVk_->framesShown() : st.rendered);
        updateObsSource();
        if (engine_.isLive()) {   // measured content values change continuously: refresh the media line when its text does
            const QString m = mediaInfo();
            if (m != lastMediaInfo_) { lastMediaInfo_ = m; emit mediaChanged(); }
        }
        if (liveVulkanActive()) {
            const QSize sz = liveVk_->imageSize();
            perf_ = QString("%1×%2 · %3 ms · %4").arg(sz.width()).arg(sz.height()).arg(liveVk_->lastMs(), 0, 'f', 1)
                        .arg(L("GPU direct", "GPU direkte"));
            gpuName_ = L("Vulkan (shared with the UI)", "Vulkan (delt med brugerfladen)");
            upError_ = s_.upscaler == veyamux::Upscaler::AmdFsr3 ? translateMessage(liveVk_->lastUpError()) : QString();
            updateLsfgMultiplier();
            emit statsChanged();
            return;
        }
        upError_ = s_.upscaler == veyamux::Upscaler::AmdFsr3 ? translateMessage(QString::fromStdString(st.upError)) : QString();
        updateLsfgMultiplier();
        perf_ = QString("%1×%2 · %3 ms%4")
                    .arg(st.outW).arg(st.outH).arg(st.processMs, 0, 'f', 1)
                    .arg(st.dropped ? QString(" · %1 %2").arg(st.dropped).arg(L("dropped", "tabt")) : QString());
        if (!engine_.gpuName().empty()) gpuName_ = QString::fromStdString(engine_.gpuName());
        emit statsChanged();
    });
    tick_.start();
}

PlayerBridge::~PlayerBridge() {
    saveSettings();
    if (liveVk_) liveVk_->setStream(-1, nullptr);
    obs_.stop();
    engine_.close();
}

// ---------------------------------------------------------------- OBS browser source

QString PlayerBridge::obsUrl() const { return QString("http://127.0.0.1:%1/").arg(obsPort_); }

void PlayerBridge::setObsEnabled(bool v) {
    obsError_.clear();
    if (v) {
        QString e;
        if (!obs_.start(obsPort_, &e)) {
            obsError_ = L("Could not open port %1: ", "Kunne ikke åbne port %1: ").arg(obsPort_) + e;
            v = false;
        }
    } else {
        obs_.stop();
    }
    obsEnabled_ = v;
    QSettings().setValue("obs/enabled", v);
    updateObsSource();
    emit obsChanged();
}

void PlayerBridge::setObsHeight(int h) {
    h = std::clamp(h, 0, 4320);
    obs_.setMaxHeight(h);
    QSettings().setValue("obs/height", h);
    updateObsSource();
    emit obsChanged();
}

// The live (GPU-direct) path copies frames back from the GPU only while OBS is actually watching.
void PlayerBridge::updateObsSource() {
    if (!liveVk_) return;
    const int want = obs_.wantsFrames() ? obs_.maxHeight() : -1;
    if (want == obsStreamH_) return;
    obsStreamH_ = want;
    liveVk_->setStream(want, want < 0 ? std::function<void(QImage, int)>() : [this](QImage img, int delayMs) { obs_.pushFrame(std::move(img), delayMs); });
}

void PlayerBridge::saveObsFile() {
    const QString start = obsFile_.isEmpty() ? QDir::homePath() + "/VeyAmux-OBS.html" : obsFile_;
    QString path = QFileDialog::getSaveFileName(nullptr, L("Save the OBS browser file", "Gem browserfilen til OBS"), start,
                                                L("Web page (*.html)", "Webside (*.html)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".html", Qt::CaseInsensitive)) path += ".html";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        obsError_ = L("Could not save %1: ", "Kunne ikke gemme %1: ").arg(path) + f.errorString();
        emit obsChanged();
        return;
    }
    f.write(ObsStream::pageHtml(QString("http://127.0.0.1:%1/stream.mjpg").arg(obsPort_), "VeyAmux"));
    f.close();
    obsFile_ = path;
    QSettings().setValue("obs/file", path);
    if (!obsEnabled_) setObsEnabled(true);   // the file is only useful while the stream runs
    emit obsChanged();
}

void PlayerBridge::copyObsUrl() { QGuiApplication::clipboard()->setText(obsUrl()); }

void PlayerBridge::loadSettings() {
    QSettings q;
    obsPort_ = quint16(std::clamp(q.value("obs/port", 47920).toInt(), 1024, 65535));
    obs_.setMaxHeight(std::clamp(q.value("obs/height", 1080).toInt(), 0, 4320));
    obsFile_ = q.value("obs/file").toString();
    s_.enabled = q.value("enh/enabled", true).toBool();
    s_.scale = float(q.value("enh/scale", 1.0).toDouble());
    s_.upscaler = veyamux::Upscaler(std::clamp(q.value("enh/upscaler", 0).toInt(), 0, 1));
    s_.fxaa = q.value("enh/fxaa", false).toBool();
    s_.denoise = veyamux::Denoise(std::clamp(q.value("enh/denoise", 0).toInt(), 0, 3));
    s_.denoiseStrength = std::clamp(q.value("enh/denoiseStrength", 1).toInt(), 0, 2);
    s_.hdrPeak = float(std::clamp(q.value("enh/hdrPeak", 1000).toInt(), 203, 10000));
    s_.vrr = q.value("enh/vrr", false).toBool();
    // Default: Lossless Scaling 2x. The old in-app modes (1 AMD FSR / RIFE, 4 RIFE + Lossless Scaling) become 2x.
    {
        int fg = std::clamp(q.value("enh/frameGenMode", 2).toInt(), 0, 4);
        if (fg == 1 || fg == 4) fg = 2;
        s_.frameGen = veyamux::FrameGenMode(fg);
    }
    lsfgPerformance_ = q.value("lsfg/performance", false).toBool();
    s_.fxaaQuality = q.value("enh/fxaaQuality", 1).toInt();
    s_.sharpen = float(q.value("enh/sharpen", 0.0).toDouble());
    s_.exposure = float(q.value("enh/exposure", 0.0).toDouble());
    s_.contrast = float(q.value("enh/contrast", 1.0).toDouble());
    s_.saturation = float(q.value("enh/saturation", 1.0).toDouble());
    volume_ = q.value("audio/volume", 1.0).toDouble();
    muted_ = q.value("audio/muted", false).toBool();
}

void PlayerBridge::saveSettings() {
    QSettings q;
    q.setValue("enh/enabled", s_.enabled); q.setValue("enh/scale", double(s_.scale)); q.setValue("enh/upscaler", int(s_.upscaler));
    q.setValue("enh/fxaa", s_.fxaa); q.setValue("enh/denoise", int(s_.denoise)); q.setValue("enh/denoiseStrength", s_.denoiseStrength); q.setValue("enh/hdrPeak", hdrPeak()); q.setValue("enh/vrr", s_.vrr); q.setValue("enh/frameGenMode", int(s_.frameGen)); q.setValue("enh/fxaaQuality", s_.fxaaQuality);
    q.setValue("enh/sharpen", double(s_.sharpen)); q.setValue("enh/exposure", double(s_.exposure));
    q.setValue("enh/contrast", double(s_.contrast)); q.setValue("enh/saturation", double(s_.saturation));
    q.setValue("audio/volume", volume_); q.setValue("audio/muted", muted_);
}

// The rate frame generation starts from: for capture with frame generation on, repeated pictures are dropped, so it
// is the game's measured frame rate; otherwise the source frame rate.
double PlayerBridge::frameGenBaseFps() const {
    if (!engine_.isOpen()) return 60.0;
    if (engine_.isLive() && s_.frameGen != veyamux::FrameGenMode::Off) {
        const auto c = engine_.liveContent();
        if (c.contentFps > 1.0) return c.contentFps;
    }
    return engine_.info().fps > 0 ? engine_.info().fps : 60.0;
}

void PlayerBridge::updateOutputFps(uint64_t shownTotal) {
    if (!fpsClock_.isValid()) fpsClock_.start();
    const qint64 now = fpsClock_.elapsed();
    if (fpsT0_ == 0 || shownTotal < fpsCount_) { fpsT0_ = now; fpsCount_ = shownTotal; return; }
    const qint64 dt = now - fpsT0_;
    if (dt < 1000) return;
    const double fps = double(shownTotal - fpsCount_) * 1000.0 / double(dt);
    fpsT0_ = now; fpsCount_ = shownTotal;
    const double old = outFps_;
    outFps_ = engine_.playing() ? fps : 0.0;
    if (qEnvironmentVariableIsSet("VEYAMUX_DEBUG_FPS")) qInfo("output fps %.1f (playing %d)", fps, int(engine_.playing()));
    if (std::lround(old) != std::lround(outFps_)) emit mediaChanged();
}

// "→ 120 fps" after the source rate when frame generation raises it: source rate x multiplier for Lossless Scaling
// (lsfg-vk generates its frames after VeyAmux, in the driver layer).
QString PlayerBridge::outputFpsText(double srcFps) const {
    const bool ls = s_.lsfgFg();
    if (ls && lsfgActive_ && lsfgMultiplier_ > 1 && srcFps > 0)
        return QString(" → %1 fps (Lossless Scaling %2×)").arg(std::lround(frameGenBaseFps() * lsfgMultiplier_)).arg(lsfgMultiplier_);
    return {};
}

QString PlayerBridge::mediaInfo() const {
    const auto& i = engine_.info();
    if (!engine_.isOpen()) return {};
    QStringList p;
    if (i.hasVideo && engine_.isLive()) {
        // A capture card always sends its fixed signal (e.g. 3840x2160 @ 60): lead with what the game actually renders
        // (measured frame rate, estimated resolution with "~"), the signal after it.
        const auto c = engine_.liveContent();
        const QString signal = QString("signal %1×%2 %3%4").arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec))
                                   .arg(i.fps > 0 ? QString(" @ %1 fps").arg(i.fps, 0, 'f', 2) : QString());
        if (c.contentFps > 0 || c.estW > 0) {
            QString res;
            if (c.estW > 0 && c.estW != i.width) res = QString("≈%1×%2").arg(c.estW).arg(c.estH);
            else res = QString("%1×%2").arg(i.width).arg(i.height);
            QString fps;
            if (c.contentFps > 0) fps = QString(" @ %1 fps").arg(std::lround(c.contentFps));
            if (c.staticPicture) fps += L(" (still picture)", " (stillestående billede)");
            p << res + fps + outputFpsText(i.fps) << signal;
        } else {
            p << QString("%1×%2 %3%4").arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec))
                     .arg(i.fps > 0 ? QString(" @ %1 fps").arg(i.fps, 0, 'f', 2) + outputFpsText(i.fps) : QString());
        }
    } else if (i.hasVideo) {
        p << QString("%1×%2 %3%4").arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec))
                 .arg(i.fps > 0 ? QString(" @ %1 fps").arg(i.fps, 0, 'f', 2) + outputFpsText(i.fps) : QString());
    }
    if (i.hasAudio) p << QString::fromStdString(i.audioCodec);
    if (engine_.hwDecodeActive()) p << "VAAPI";
    return p.join(" · ");
}

QImage PlayerBridge::currentImage() { QMutexLocker l(&imgMutex_); return image_; }

void PlayerBridge::setStatus(const QString& s) { status_ = s; emit statusChanged(); }

bool PlayerBridge::openUrl(const QUrl& url) { return openPath(url.isLocalFile() ? url.toLocalFile() : url.toString()); }

bool PlayerBridge::openPath(const QString& path) {
    std::string err;
    if (liveVk_) liveVk_->setActive(false);
    { QMutexLocker l(&imgMutex_); image_ = QImage(); }
    if (!engine_.open(path.toStdString(), err)) {
        setStatus(translateMessage(QString::fromStdString(err)));
        emit mediaChanged();
        return false;
    }
    updateLsfgMultiplier();
    engine_.setVolume(float(volume_));
    engine_.setMuted(muted_);
    applyEnhance();
    setStatus({});
    emit mediaChanged();
    engine_.play();
    lastPlaying_ = true;
    emit playingChanged();
    return true;
}

void PlayerBridge::openDialog() {
    const QString f = QFileDialog::getOpenFileName(nullptr, L("Open video", "Åbn video"), QString(),
        L("Media (*.mp4 *.mkv *.webm *.mov *.avi *.m4v *.ts *.m2ts *.wmv *.flv *.mp3 *.flac *.ogg *.opus *.m4a *.wav);;All files (*)",
          "Medier (*.mp4 *.mkv *.webm *.mov *.avi *.m4v *.ts *.m2ts *.wmv *.flv *.mp3 *.flac *.ogg *.opus *.m4a *.wav);;Alle filer (*)"));
    if (!f.isEmpty()) openPath(f);
}

void PlayerBridge::togglePlay() { engine_.playing() ? pause() : play(); }
void PlayerBridge::play() { engine_.play(); lastPlaying_ = engine_.playing(); emit playingChanged(); }
void PlayerBridge::pause() { engine_.pause(); lastPlaying_ = false; emit playingChanged(); }
void PlayerBridge::seek(double s) { engine_.seek(s); emit positionChanged(); }
void PlayerBridge::seekRelative(double d) { seek(position() + d); }

void PlayerBridge::setVolume(double v) { volume_ = qBound(0.0, v, 1.5); engine_.setVolume(float(volume_)); emit audioChanged(); }
void PlayerBridge::setMuted(bool m) { muted_ = m; engine_.setMuted(m); emit audioChanged(); }

void PlayerBridge::applyEnhance() { engine_.setEnhance(s_); if (liveVk_) liveVk_->setSettings(s_); emit enhanceChanged(); }
void PlayerBridge::setEnhance(bool v) { s_.enabled = v; applyEnhance(); }
void PlayerBridge::setScale(double v) { s_.scale = float(qBound(1.0, v, 4.0)); applyEnhance(); }
void PlayerBridge::setUpscaler(int v) {
    v = std::clamp(v, 0, 1);
    if (int(s_.upscaler) == v) return;
    s_.upscaler = veyamux::Upscaler(v);
    applyEnhance();
}
void PlayerBridge::setFxaa(bool v) { s_.fxaa = v; applyEnhance(); }
void PlayerBridge::setFrameGenMode(int v) {
    v = std::clamp(v, 0, 3);
    if (v == 1) v = 2;
    const bool wasLs = s_.lsfgFg();
    s_.frameGen = veyamux::FrameGenMode(v);
    const bool isLs = v >= 2;
    { QSettings q; q.setValue("enh/frameGenMode", v); }
    if (lsfgActive_) {
        // lsfg-vk hot-reloads the multiplier: 1 = joined but no generated frames.
        lsfgMultiplier_ = isLs ? (v == 3 ? 3 : 2) : 1;
        lsfg::writeConfig(lsfg_, lsfgMultiplier_, lsfgPerformance_, lsfgFlowScale());
    }
    applyEnhance();
    emit mediaChanged();   // the "→ N fps" part of the media line
    if (wasLs != isLs) emit lsfgChanged();
    emit lsfgChanged();
}
void PlayerBridge::setFxaaQuality(int v) { s_.fxaaQuality = qBound(0, v, 2); applyEnhance(); }
void PlayerBridge::setSharpen(double v) { s_.sharpen = float(qBound(0.0, v, 1.0)); applyEnhance(); }
void PlayerBridge::setExposure(double v) { s_.exposure = float(qBound(-3.0, v, 3.0)); applyEnhance(); }
void PlayerBridge::setContrast(double v) { s_.contrast = float(qBound(0.5, v, 1.5)); applyEnhance(); }
void PlayerBridge::setSaturation(double v) { s_.saturation = float(qBound(0.0, v, 2.0)); applyEnhance(); }
void PlayerBridge::resetEnhance() {
    const bool en = s_.enabled;
    s_ = veyamux::EnhanceSettings{};
    s_.enabled = en;
    applyEnhance();
}

// ------------------------------------------------------------------ capture card support

QVariantList PlayerBridge::captureDevices() {
    QVariantList out;
    for (const auto& d : veyamux::listCaptureDevices()) {
        QVariantMap m;
        m["path"] = QString::fromStdString(d.path);
        m["name"] = QString::fromStdString(d.name);
        out << m;
    }
    return out;
}

QVariantList PlayerBridge::captureModes(const QString& path) {
    lastModes_ = veyamux::listCaptureModes(path.toStdString());
    QVariantList out;
    for (const auto& m : lastModes_) out << QString::fromStdString(m.label);
    return out;
}

QVariantList PlayerBridge::audioSources() {
    // PulseAudio/PipeWire input sources (capture cards expose their HDMI audio as one). Uses pactl if present.
    QVariantList out;
    QProcess p;
    p.start("pactl", {"list", "sources"});
    if (!p.waitForFinished(2500)) { p.kill(); return out; }
    QString name, desc;
    auto flush = [&] {
        if (!name.isEmpty() && !name.endsWith(".monitor")) {
            QVariantMap m; m["name"] = name; m["desc"] = desc.isEmpty() ? name : desc; out << m;
        }
        name.clear(); desc.clear();
    };
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n');
    for (const QString& raw : lines) {
        const QString l = raw.trimmed();
        if (l.startsWith("Source #")) flush();
        else if (l.startsWith("Name:")) name = l.mid(5).trimmed();
        else if (l.startsWith("Description:")) desc = l.mid(12).trimmed();
    }
    flush();
    return out;
}

QString PlayerBridge::guessAudioSource(const QString& cardName) {
    static const QStringList skip = {"usb", "video", "capture", "hdmi", "audio", "device", "card", "the"};
    QStringList toks;
    for (const QString& t : cardName.toLower().split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts))
        if (t.size() >= 3 && !skip.contains(t)) toks << t;
    if (toks.isEmpty()) return {};
    int best = 0; QString bestName;
    for (const QVariant& v : audioSources()) {
        const QVariantMap m = v.toMap();
        const QString hay = (m["name"].toString() + " " + m["desc"].toString()).toLower();
        int score = 0;
        for (const QString& t : toks) if (hay.contains(t)) ++score;
        if (score > best) { best = score; bestName = m["name"].toString(); }
    }
    return bestName;
}

QString PlayerBridge::lastCaptureDevice() const { return QSettings().value("cap/device").toString(); }
QString PlayerBridge::lastCaptureAudio() const { return QSettings().value("cap/audio").toString(); }

bool PlayerBridge::openCapture(const QString& path, int modeIndex, const QString& audioSource) {
    if (modeIndex < 0 || modeIndex >= int(lastModes_.size())) { setStatus(L("No capture mode selected", "Ingen capture-tilstand valgt")); return false; }
    std::string err;
    { QMutexLocker l(&imgMutex_); image_ = QImage(); }
    if (liveVk_) liveVk_->setActive(false);
    const bool zeroCopy = liveVk_ != nullptr;
    if (!engine_.openCapture(path.toStdString(), lastModes_[size_t(modeIndex)], audioSource.toStdString(), err, zeroCopy)) {
        setStatus(translateMessage(QString::fromStdString(err)));
        emit mediaChanged();
        return false;
    }
    QSettings q; q.setValue("cap/device", path); q.setValue("cap/audio", audioSource);
    updateLsfgMultiplier();
    if (engine_.info().height > 0) aspect_ = double(engine_.info().width) / engine_.info().height;
    if (zeroCopy) { liveVk_->setSettings(s_); liveVk_->setActive(true); }
    engine_.setVolume(float(volume_));
    engine_.setMuted(muted_);
    applyEnhance();
    setStatus({});
    emit mediaChanged();
    lastPlaying_ = true;
    emit playingChanged();
    return true;
}

bool PlayerBridge::openCaptureAuto(const QString& path) {
    QString card;
    for (const auto& d : veyamux::listCaptureDevices()) if (QString::fromStdString(d.path) == path) card = QString::fromStdString(d.name);
    captureModes(path);
    return openCapture(path, 0, guessAudioSource(card));
}

// ------------------------------------------------------------------ frame generation / Lossless Scaling

void PlayerBridge::setRunContext(bool vulkanUi, bool lsfgActive, bool lsfgCrashed, bool danish) {
    vulkanUi_ = vulkanUi;
    lsfgActive_ = lsfgActive;
    lsfgCrashed_ = lsfgCrashed;
    danish_ = danish;
    refreshLsfg();
    if (lsfgActive_) {
        lsfgMultiplier_ = s_.frameGen == veyamux::FrameGenMode::LsfgFixed3 ? 3 : (s_.lsfgFg() ? 2 : 1);
        runClock_.start();
        auto* t = new QTimer(this);   // lsfg-vk writes its log once the window's Vulkan device exists
        connect(t, &QTimer::timeout, this, &PlayerBridge::updateLsfgLog);
        t->start(1500);
    }
}

void PlayerBridge::refreshLsfg() {
    lsfg_ = lsfg::detect(QSettings().value("lsfg/folder").toString());
    emit lsfgChanged();
}

void PlayerBridge::chooseLsfgFolder() {
    const QString start = lsfg_.installDir.isEmpty() ? QDir::homePath() : lsfg_.installDir;
    const QString dir = QFileDialog::getExistingDirectory(nullptr, L("Select the Lossless Scaling folder", "Vælg mappen med Lossless Scaling"), start);
    if (dir.isEmpty()) return;
    QSettings().setValue("lsfg/folder", dir);
    refreshLsfg();
}

QString PlayerBridge::lsfgState() const {
    if (lsfgCrashed_) return "crashed";
    if (!lsfg_.installed()) return "noInstall";
    if (lsfg_.layerManifest.isEmpty()) return "noLayer";
    if (!lsfg_.layerV2) return "oldLayer";
    if (lsfg_.dllPath.isEmpty()) return "noDll";
    if (!vulkanUi_) return "noVulkanUi";
    return lsfgActive_ ? "active" : "ready";
}

bool PlayerBridge::restartNeeded() const {
    const bool ls = s_.lsfgFg();
    return ls && !lsfgActive_ && lsfgState() == "ready";
}

double PlayerBridge::displayHz() const {
    QScreen* sc = window_ ? window_->screen() : QGuiApplication::primaryScreen();
    return sc ? sc->refreshRate() : 60.0;
}

double PlayerBridge::lsfgFlowScale() const {
    // Chosen from the screen, not the window: VeyAmux is mostly used fullscreen, and every change makes lsfg-vk rebuild
    // its whole pipeline (seen in user logs when the window was resized/maximised).
    const QScreen* sc = window_ && window_->screen() ? window_->screen() : QGuiApplication::primaryScreen();
    const int h = sc ? int(sc->size().height() * sc->devicePixelRatio()) : 1080;
    return lsfg::recommendedFlowScale(h > 0 ? h : 1080);
}

void PlayerBridge::setLsfgPerformance(bool v) {
    lsfgPerformance_ = v;
    QSettings().setValue("lsfg/performance", v);
    if (lsfgActive_) lsfg::writeConfig(lsfg_, lsfgMultiplier_, v, lsfgFlowScale());   // hot-reloaded by lsfg-vk
    emit lsfgChanged();
}

void PlayerBridge::updateLsfgMultiplier() {
    if (!lsfgActive_) return;
    int m = lsfgMultiplier_;
    if (s_.frameGen == veyamux::FrameGenMode::LsfgFixed3) m = 3;
    else if (s_.lsfgFg()) m = 2;
    else m = 1;
    const double flow = lsfgFlowScale();
    if (m == lsfgMultiplier_ && flow == lastFlow_ && QFileInfo::exists(lsfg::configPath())) return;
    lastFlow_ = flow;
    lsfgMultiplier_ = m;
    lsfg::writeConfig(lsfg_, m, lsfgPerformance_, lsfgFlowScale());
    emit lsfgChanged();
    emit mediaChanged();
}

void PlayerBridge::updateLsfgLog() {
    QString t;
    if (lsfgActive_) {
        if (!runClock_.isValid()) runClock_.start();
        bool loaded = false;
        const QString line = lsfg::logSummary(&loaded);
        if (loaded) {
            const bool bad = line.contains("[ERROR]", Qt::CaseInsensitive) || line.contains("[WARNING]", Qt::CaseInsensitive);
            t = (bad ? L("lsfg-vk reports: ", "lsfg-vk melder: ") : L("lsfg-vk is loaded: ", "lsfg-vk er indlæst: ")) + line;
        } else if (runClock_.elapsed() > 6000) {
            t = L("lsfg-vk did not load into VeyAmux (it wrote no log to %1). Check the installation with "
                  "\"lsfg-vk-cli healthcheck\" in a terminal.",
                  "lsfg-vk blev ikke indlæst i VeyAmux (ingen log i %1). Tjek installationen med "
                  "\"lsfg-vk-cli healthcheck\" i en terminal.").arg(lsfg::logPath());
        }
    }
    if (t != lsfgLog_) {
        lsfgLog_ = t;
        if (!t.isEmpty()) qInfo("Lossless Scaling: %s", qPrintable(t));
        emit lsfgChanged();
    }
}

void PlayerBridge::retryLsfg() {
    QSettings().remove("lsfg/disabledAfterCrash");
    restartApp();
}

void PlayerBridge::restartApp() {
    saveSettings();
    // Inside an AppImage the binary lives in a temporary mount that disappears on exit: relaunch the AppImage itself.
    const QString target = qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : QCoreApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments().mid(1);
    args.removeAll("--probe-ui");
    // Release the capture card (and audio) BEFORE the new instance starts, or it gets "device busy".
    engine_.close();
    QSettings().sync();
    if (QProcess::startDetached(target, args)) QCoreApplication::quit();
}

QString PlayerBridge::translateMessage(const QString& msg) const {
    if (!danish_ || msg.isEmpty()) return msg;
    static const std::pair<const char*, const char*> table[] = {
        {"Audio unavailable: ", "Lyd utilgængelig: "},
        {"Capture audio unavailable: ", "Capture-lyd utilgængelig: "},
        {"Audio output unavailable: ", "Lydudgang utilgængelig: "},
        {"No signal from the capture card", "Intet signal fra capture-kortet"},
        {"Capture device disconnected", "Capture-enheden blev frakoblet"},
        {"Vulkan unavailable: ", "Vulkan utilgængelig: "},
        {"Could not set up the enhancement chain: ", "Kunne ikke starte forbedringskæden: "},
        {"GPU error, falling back to passthrough: ", "GPU-fejl, viser uden forbedring: "},
        {"GPU enhancement disabled (VEYAMUX_NO_GPU=1)", "GPU-forbedring slået fra (VEYAMUX_NO_GPU=1)"},
        {"Read error: ", "Læsefejl: "},
        {"Could not open the file: ", "Kunne ikke åbne filen: "},
        {"Could not read stream info: ", "Kunne ikke læse streaminformation: "},
        {"The file contains no playable audio or video", "Filen indeholder ingen lyd eller video, der kan afspilles"},
        {"No decoder for the video stream", "Ingen dekoder til videostrømmen"},
        {"Could not open the video decoder: ", "Kunne ikke åbne videodekoderen: "},
        {"Could not open ", "Kunne ikke åbne "},
        {"The device refused pixel format ", "Enheden afviste pixelformatet "},
        {"The device does not support memory-mapped streaming", "Enheden understøtter ikke memory-mapped streaming"},
        {"Out of GPU memory for FSR upscaling", "Ikke nok GPU-hukommelse til FSR-opskalering"},
        {"Out of GPU memory", "Ikke nok GPU-hukommelse"},
        {"The capture device is in use by another program", "Capture-kortet bruges af et andet program"},
        {"Live path unavailable", "GPU-direkte visning utilgængelig"},
    };
    for (const auto& [en, da] : table) {
        const QString e = QString::fromUtf8(en);
        if (msg.startsWith(e)) return QString::fromUtf8(da) + msg.mid(e.size());
    }
    return msg;
}
