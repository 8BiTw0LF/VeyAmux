// VeyAmux for Linux: VeyAmux Debug's tools - the full report, the benchmark and the debug switches.
//
// The report is one Markdown file with everything a bug needs across hardware: the machine, the graphics driver, the
// displays, the source and capture card, sound, Lossless Scaling, frame timing, the benchmark (if run), the settings
// (secrets hidden), the log and earlier crashes. It is written to ~/Documents/VeyAmux reports (or the home folder).
//
// The benchmark runs the live source through a fixed list of settings for a few seconds each and measures the GPU
// time of every stage, frame rate, jitter, delay, CPU use and memory. It answers "what does each setting cost on this
// machine" (MarkStokes-outlook/VeyAmux#13) and is written as a CSV next to the reports.
#include "PlayerBridge.h"
#include "CaptureSource.h"
#include "Diagnostics.h"
#include "LosslessScaling.h"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <vulkan/vulkan.h>

using veyamux::EnhanceSettings;
namespace diag = veyamux::diag;

namespace {

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

QString reportDir() {
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (docs.isEmpty()) docs = QDir::homePath();
    const QString d = docs + "/VeyAmux reports";
    QDir().mkpath(d);
    return d;
}

long long cpuTicks() {   // this process's user + system time, in clock ticks
    QFile f("/proc/self/stat");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QString s = QString::fromLatin1(f.readAll());
    const QStringList p = s.mid(s.lastIndexOf(')') + 2).split(' ');   // fields from 3 on
    return p.size() > 13 ? p[11].toLongLong() + p[12].toLongLong() : 0;
}

double rssMb() {
    QFile f("/proc/self/statm");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QStringList p = QString::fromLatin1(f.readAll()).split(' ');
    return p.size() > 1 ? p[1].toDouble() * double(sysconf(_SC_PAGESIZE)) / 1048576.0 : 0;
}

struct Spread { double mean = 0, sd = 0, p50 = 0, p99 = 0, max = 0; };
Spread spread(std::vector<double> v) {
    Spread r;
    if (v.empty()) return r;
    for (double x : v) r.mean += x;
    r.mean /= double(v.size());
    for (double x : v) r.sd += (x - r.mean) * (x - r.mean);
    r.sd = std::sqrt(r.sd / double(v.size()));
    std::sort(v.begin(), v.end());
    r.p50 = v[v.size() / 2];
    r.p99 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.99))];
    r.max = v.back();
    return r;
}

QString vulkanDriver(veyamux::VkContext* ctx) {
    if (!ctx || !ctx->physical()) return "unknown";
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    if (ctx->apiVersion() >= VK_API_VERSION_1_2) p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(ctx->physical(), &p2);
    const auto& p = p2.properties;
    const char* types[] = {"other", "integrated", "discrete", "virtual", "CPU"};
    QString s = QStringLiteral("%1 (%2, Vulkan %3.%4.%5, vendor 0x%6 device 0x%7)")
                    .arg(QString::fromUtf8(p.deviceName)).arg(p.deviceType <= 4 ? types[p.deviceType] : "?")
                    .arg(VK_API_VERSION_MAJOR(p.apiVersion)).arg(VK_API_VERSION_MINOR(p.apiVersion)).arg(VK_API_VERSION_PATCH(p.apiVersion))
                    .arg(p.vendorID, 4, 16, QChar('0')).arg(p.deviceID, 4, 16, QChar('0'));
    if (p2.pNext) s += QStringLiteral(" · driver %1 %2").arg(QString::fromUtf8(drv.driverName), QString::fromUtf8(drv.driverInfo));
    return s;
}

// switches for VeyAmux Debug, applied as environment variables at the next start (main.cpp)
struct Switch { const char* env; const char* value; const char* title; const char* detail; };
const Switch kSwitches[] = {
    {"VEYAMUX_DEBUG_PACE", "1", "Log every picture's timing",
     "capture → pacer → renderer → drawing, in ms, into the log (the report summarises it either way)"},
    {"VEYAMUX_QT_UPDATE", "1", "Draw through Qt's update() (before 1.8)",
     "waits for the compositor's frame callback; to compare jitter with the 1.8 way"},
    {"VEYAMUX_CAPTURE_COPY", "1", "Always copy capture pictures", "instead of reading them straight from the card's buffers"},
    {"VEYAMUX_DEDUP_SPARSE", "1", "Older repeat detection", "compares a sparse grid of pixels instead of the whole picture"},
    {"VEYAMUX_DEBUG_FG", "1", "Log frame generation", "FSR frame generation: every decision, into the log"},
    {"VEYAMUX_NO_FIT", "1", "No fit-to-window resampling", "the finished picture is stretched by Qt instead"},
    {"VEYAMUX_FILE_GPU", "0", "Video files on the CPU path", "instead of the GPU (live) path"},
    {"VEYAMUX_PS_SWDECODE", "1", "Remote Play: decode on the CPU", "instead of VA-API"},
    {"VEYAMUX_GAMESCOPE_KEEPALIVE", "0", "gamescope: no pointer keep-alive", "to test whether frame generation stops without it"},
    {"VEYAMUX_VSYNC", "1", "VSync on", "the UI waits for the display (VeyAmux runs without it by default)"},
};

} // namespace

bool PlayerBridge::debugBuild() const { return diag::kDebugBuild; }

QString PlayerBridge::buildInfo() const {
    return QStringLiteral("VeyAmux%1 %2 · %3").arg(diag::kDebugBuild ? " Debug" : "", appVersion(), diag::gitSha());
}

QVariantList PlayerBridge::debugSwitches() const {
    QVariantList out;
    QSettings q;
    for (const auto& s : kSwitches) {
        QVariantMap m;
        m["env"] = s.env;
        m["title"] = s.title;
        m["detail"] = QString(s.detail) + "  (" + s.env + "=" + s.value + ")";
        m["on"] = q.contains(QString("debug/env/") + s.env);
        out << m;
    }
    return out;
}

void PlayerBridge::setDebugSwitch(const QString& env, bool on) {
    QSettings q;
    for (const auto& s : kSwitches)
        if (env == s.env) {
            if (on) q.setValue("debug/env/" + env, QString(s.value));   // main.cpp sets it at the next start
            else q.remove("debug/env/" + env);
        }
    debugRestart_ = true;
    emit benchChanged();
}

QString PlayerBridge::paceSummary() {
    if (!liveVk_) return "No live path.";
    const auto ps = liveVk_->paceSamples();
    if (ps.size() < 10) return "Not enough pictures drawn yet.";
    std::vector<double> due, handed, draw, total, gap;
    for (const auto& p : ps) {
        due.push_back(p.due); handed.push_back(p.handed); draw.push_back(p.draw); total.push_back(p.total);
        if (p.gap > 0) gap.push_back(p.gap);
    }
    QStringList r;
    r << QStringLiteral("Last %1 pictures, ms:").arg(ps.size()) << ""
      << "| | mean | sd | median | 99 % | max |" << "|---|---:|---:|---:|---:|---:|";
    auto row = [&](const char* name, const std::vector<double>& v) {
        const Spread s = spread(v);
        r << QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 |").arg(name).arg(s.mean, 0, 'f', 2).arg(s.sd, 0, 'f', 2)
                 .arg(s.p50, 0, 'f', 2).arg(s.p99, 0, 'f', 2).arg(s.max, 0, 'f', 2);
    };
    row("capture → pacer due", due);
    row("due → renderer (or capture → renderer)", handed);
    row("renderer → drawing starts", draw);
    row("capture → drawing (total)", total);
    row("gap between drawings", gap);
    r << "" << QStringLiteral("Pacing repair: %1 · hold %2 ms · share repaired %3 %")
                   .arg(s_.pacingRepair ? "on" : "off").arg(engine_.pacingHoldMs(), 0, 'f', 1)
                   .arg(std::max(0.0, engine_.pacingRepairShare()) * 100.0, 0, 'f', 0);
    return r.join('\n');
}

QString PlayerBridge::saveReport(const QString& note) {
    updateOverlay();
    QString md;
    QTextStream o(&md);
    const QDateTime now = QDateTime::currentDateTime();
    o << "# VeyAmux report\n\n";
    o << "- **Build:** " << buildInfo() << "\n";
    o << "- **Time:** " << now.toString(Qt::ISODate) << " · language " << lang_ << "\n";
    if (!note.trimmed().isEmpty()) o << "\n## What happened\n\n" << note.trimmed() << "\n";

    o << "\n## System\n\n```\n" << diag::systemInfo() << "\n```\n";

    o << "\n## Graphics\n\n";
    veyamux::VkContext* ctx = liveVk_ ? liveVk_->context() : nullptr;
    o << "- **Vulkan device used by VeyAmux:** " << vulkanDriver(ctx) << "\n";
    o << "- **Live path:** " << (liveVulkanActive() ? "active" : "not active") << "\n";
    QString vki = diag::tool("vulkaninfo", {"--summary"}, 6000, 400);
    if (const int a = vki.indexOf("Instance Extensions"), b = vki.indexOf("Instance Layers"); a >= 0 && b > a) vki.remove(a, b - a);
    o << "\n<details><summary>vulkaninfo --summary</summary>\n\n```\n" << vki
      << "\n```\n</details>\n";
    o << "\n<details><summary>vainfo (video decode / encode on the GPU)</summary>\n\n```\n" << diag::tool("vainfo", {}, 6000, 80)
      << "\n```\n</details>\n";

    o << "\n## Displays\n\n";
    for (QScreen* sc : QGuiApplication::screens())
        o << "- " << sc->name() << " · " << sc->manufacturer() << " " << sc->model() << " · " << sc->size().width() << "×"
          << sc->size().height() << " @ " << QString::number(sc->refreshRate(), 'f', 2) << " Hz · scale "
          << sc->devicePixelRatio() << (sc == QGuiApplication::primaryScreen() ? " · primary" : "") << "\n";
    o << "- VRR on the desktop: " << (desktopVrr_ ? "on" : "off") << (vrrScreens_.isEmpty() ? "" : " (" + vrrScreens_ + ")") << "\n";
    o << "- HDR output: " << (hdrOutputActive() ? "on" : "off") << "\n";

    o << "\n## Source and settings\n\n```\n" << perfReport() << "\n```\n";

    o << "\n## Capture cards\n\n";
    const auto devices = veyamux::listCaptureDevices();
    if (devices.empty()) o << "None found.\n";
    for (const auto& d : devices) {
        o << "- **" << QString::fromStdString(d.name) << "** (" << QString::fromStdString(d.path) << ")";
        if (engine_.isLive() && QString::fromStdString(d.path) == curDevice_) o << " · in use";
        o << "\n";
        int n = 0;
        for (const auto& m : veyamux::listCaptureModes(d.path)) {
            if (++n > 40) { o << "  - …\n"; break; }
            o << "  - " << QString::fromStdString(m.label) << "\n";
        }
    }

    o << "\n## Frame timing\n\n" << paceSummary() << "\n";

    if (!bench_.rows.empty()) o << "\n## Benchmark\n\n" << benchTable() << "\n";

    o << "\n## Sound\n\n```\n" << diag::tool("pactl", {"info"}, 3000, 30) << "\n```\n";

    o << "\n## Lossless Scaling\n\n";
    o << "- State: " << lsfgState() << "\n";
    o << "- Lossless Scaling folder: " << (lsfg_.installDir.isEmpty() ? "not found" : lsfg_.installDir) << "\n";
    o << "- lsfg-vk.dll: " << (lsfg_.dllPath.isEmpty() ? "not found" : lsfg_.dllPath) << " · Lossless.dll: "
      << (lsfg_.losslessDll.isEmpty() ? "not found" : lsfg_.losslessDll) << "\n";
    o << "- Layer: " << (lsfg_.layerManifest.isEmpty() ? "not found" : lsfg_.layerManifest) << (lsfg_.layerV2 ? " (lsfg-vk 2)" : "") << "\n";
    {
        bool loaded = false;
        const QString sum = lsfg::logSummary(&loaded);
        o << "- lsfg-vk log: " << (loaded ? sum : QStringLiteral("not loaded in this run")) << "\n";
        const QString cfg = diag::readText(lsfg::configPath(), 8192);
        if (!cfg.isEmpty()) o << "\n<details><summary>VeyAmux's lsfg-vk profile</summary>\n\n```toml\n" << cfg << "\n```\n</details>\n";
    }

    o << "\n## All settings\n\n<details><summary>" << QSettings().fileName() << " (secrets hidden)</summary>\n\n```\n";
    {
        QSettings q;
        for (const QString& k : q.allKeys())
            o << k << " = " << (diag::secretKey(k) ? QStringLiteral("(hidden)") : q.value(k).toString()) << "\n";
    }
    o << "```\n</details>\n";

    const QStringList crashes = diag::crashFiles();
    if (!crashes.isEmpty()) {
        o << "\n## Crashes\n\n";
        for (int i = 0; i < std::min<int>(3, crashes.size()); ++i)
            o << "### " << QFileInfo(crashes[i]).fileName() << " (" << QFileInfo(crashes[i]).lastModified().toString(Qt::ISODate)
              << ")\n\n```\n" << diag::readText(crashes[i], 32 * 1024) << "\n```\n";
    }

    o << "\n## Log (newest " << 500 << " lines)\n\n<details><summary>log</summary>\n\n```\n" << diag::recentLog(500).join('\n')
      << "\n```\n</details>\n";
    if (!diag::logFile().isEmpty()) o << "\nFull log of this run: `" << diag::logFile() << "`\n";
    o.flush();

    const QString path = reportDir() + "/veyamux-report-" + now.toString("yyyyMMdd-HHmmss") + ".md";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(md.toUtf8()) < 0) {
        showNotice(L("Could not write the report", "Kunne ikke skrive rapporten"));
        return {};
    }
    f.close();
    lastReport_ = path;
    emit benchChanged();
    showNotice(L("Report saved: %1", "Rapport gemt: %1").arg(QFileInfo(path).fileName()));
    return path;
}

void PlayerBridge::openReportFolder() { QDesktopServices::openUrl(QUrl::fromLocalFile(reportDir())); }

void PlayerBridge::copyReportPath() {
    if (lastReport_.isEmpty()) return;
    QGuiApplication::clipboard()->setText(lastReport_);
    showNotice(L("Path copied", "Sti kopieret"));
}

void PlayerBridge::reportOnGitHub(const QString& note) {
    if (lastReport_.isEmpty()) saveReport(note);
    QString body = "**What happened**\n\n" + (note.trimmed().isEmpty() ? QStringLiteral("(describe it here)") : note.trimmed()) +
                   "\n\n**Report**\n\nDrag the report file here: `" + QFileInfo(lastReport_).fileName() + "`\n\n" + buildInfo();
    if (liveVk_ && liveVk_->context()) body += " · " + vulkanDriver(liveVk_->context()).section(" (", 0, 0);
    QUrlQuery q;
    q.addQueryItem("title", "[" + appVersion() + "] ");
    q.addQueryItem("body", body);
    QUrl u("https://github.com/8BiTw0LF/VeyAmux/issues/new");
    u.setQuery(q);
    QDesktopServices::openUrl(u);
    openReportFolder();
}

// ---------------------------------------------------------------------------------------------------------------
// Benchmark

void PlayerBridge::startBenchmark() {
    if (bench_.running) return;
    if (!hasMedia() || !liveVulkanActive()) {
        bench_.text = L("Start a capture card or the demo first: the benchmark measures the live picture.",
                        "Start et capture-kort eller demoen først: benchmarken måler det levende billede.");
        emit benchChanged();
        return;
    }
    bench_ = Bench{};
    bench_.saved = s_;
    bench_.source = title();
    const int srcH = engine_.info().height > 0 ? engine_.info().height : 1080;
    const bool canUpscale = srcH <= 1440;       // 2× of 4K would be 8K
    const bool fsrFgAllowed = !s_.lsfgFg() && !lsfgActive_;
    auto base = [](EnhanceSettings& e) {
        e.enabled = true; e.scale = 1.0f; e.upscaler = veyamux::Upscaler::Sgsr; e.fxaa = false; e.smaa = false;
        e.deflicker = false; e.denoise = veyamux::Denoise::Off; e.sharpen = 0; e.texture = e.clarity = e.dehaze = 0;
        e.retroTv = false; e.pixelPerfect = false; e.hudLock = false;
        if (e.frameGen == veyamux::FrameGenMode::FsrFree) e.frameGen = veyamux::FrameGenMode::Off;
    };
    auto add = [&](const QString& name, std::function<void(EnhanceSettings&)> f) {
        bench_.steps.push_back({name, [base, f](EnhanceSettings& e) { base(e); if (f) f(e); }});
    };
    add("Enhance off", [](EnhanceSettings& e) { e.enabled = false; });
    add("Enhance on, nothing else", nullptr);
    const char* str[] = {"light", "medium", "strong"};
    for (int k = 0; k < 3; ++k)
        add(QString("Noise reduction spatial %1").arg(str[k]), [k](EnhanceSettings& e) { e.denoise = veyamux::Denoise::Spatial; e.denoiseStrength = k; });
    add("Noise reduction temporal medium", [](EnhanceSettings& e) { e.denoise = veyamux::Denoise::Temporal; e.denoiseStrength = 1; });
    add("Noise reduction both medium", [](EnhanceSettings& e) { e.denoise = veyamux::Denoise::Both; e.denoiseStrength = 1; });
    add("Anti-flicker", [](EnhanceSettings& e) { e.deflicker = true; });
    add("FXAA high", [](EnhanceSettings& e) { e.fxaa = true; e.fxaaQuality = 1; });
    add("SMAA high", [](EnhanceSettings& e) { e.fxaa = true; e.smaa = true; e.fxaaQuality = 1; });
    add("Sharpness 0.5", [](EnhanceSettings& e) { e.sharpen = 0.5f; });
    add("Texture, clarity, dehaze", [](EnhanceSettings& e) { e.texture = 0.3f; e.clarity = 0.3f; e.dehaze = 0.2f; });
    add("FSR 3.1 native (TAA)", [](EnhanceSettings& e) { e.upscaler = veyamux::Upscaler::AmdFsr3; });
    if (canUpscale) {
        add("Upscale 2× SGSR", [](EnhanceSettings& e) { e.scale = 2; e.upscaler = veyamux::Upscaler::Sgsr; });
        add("Upscale 2× FSR 1", [](EnhanceSettings& e) { e.scale = 2; e.upscaler = veyamux::Upscaler::AmdFsr1; });
        add("Upscale 2× NIS", [](EnhanceSettings& e) { e.scale = 2; e.upscaler = veyamux::Upscaler::Nis; });
        add("Upscale 2× FSR 3.1", [](EnhanceSettings& e) { e.scale = 2; e.upscaler = veyamux::Upscaler::AmdFsr3; });
    }
    if (fsrFgAllowed) add("FSR 3.1.6 frame generation", [](EnhanceSettings& e) { e.frameGen = veyamux::FrameGenMode::FsrFree; });
    add(canUpscale ? "Heavy: both medium + anti-flicker + SMAA + 2× FSR 3.1" : "Heavy: both medium + anti-flicker + SMAA + FSR 3.1 native",
        [canUpscale](EnhanceSettings& e) {
            e.denoise = veyamux::Denoise::Both; e.denoiseStrength = 1; e.deflicker = true; e.fxaa = true; e.smaa = true;
            e.upscaler = veyamux::Upscaler::AmdFsr3; e.scale = canUpscale ? 2.0f : 1.0f;
        });
    bench_.running = true;
    bench_.timer = new QTimer(this);
    connect(bench_.timer, &QTimer::timeout, this, [this] { benchTick(); });
    bench_.timer->start(250);
    benchApplyStep();
    qInfo("Benchmark: %d steps on %s", int(bench_.steps.size()), qPrintable(bench_.source));
}

void PlayerBridge::benchApplyStep() {
    EnhanceSettings e = bench_.saved;
    bench_.steps[bench_.step].set(e);
    s_ = e;
    applyEnhance();
    bench_.phase = 0;
    bench_.t0 = nowMs();
    bench_.cur = BenchRow{};
    bench_.cur.name = bench_.steps[bench_.step].name;
    bench_.text = L("Benchmark %1/%2: %3", "Benchmark %1/%2: %3").arg(bench_.step + 1).arg(bench_.steps.size()).arg(bench_.cur.name);
    emit benchChanged();
}

void PlayerBridge::benchTick() {
    if (!bench_.running) return;
    if (!hasMedia() || !liveVulkanActive()) { stopBenchmark(); return; }
    const double t = nowMs();
    static const bool fast = qEnvironmentVariableIsSet("VEYAMUX_BENCH_FAST");   // tests: 0.5 + 1 s per step
    const double kSettle = fast ? 500 : 2500, kMeasure = fast ? 1000 : 5000;
    if (bench_.phase == 0) {
        if (t - bench_.t0 < kSettle) return;
        bench_.phase = 1;
        bench_.t0 = t;
        bench_.cpuTicks0 = cpuTicks();
        liveVk_->resetGpuSums();
        return;
    }
    auto& r = bench_.cur;
    r.delay += liveVk_->pipelineMs();
    ++r.samples;
    if (t - bench_.t0 < kMeasure) return;
    if (r.samples) r.delay /= r.samples;
    // GPU time: the average of every frame drawn while measuring (a stage counts only in the frames it ran in)
    const auto g = liveVk_->gpuSums();
    r.gpu = g.frames ? g.total / g.frames : -1;
    for (size_t k = 0; k < r.stages.size() && k < g.stage.size(); ++k) r.stages[k] = g.ran[k] ? g.stage[k] / g.ran[k] : -1;
    // frame times of the measuring window
    std::vector<double> shown = liveVk_->shownTimes(), iv;
    for (size_t i = 1; i < shown.size(); ++i)
        if (shown[i] >= bench_.t0) iv.push_back(shown[i] - shown[i - 1]);
    if (iv.size() >= 4) {
        const Spread s = spread(iv);
        r.frameMs = s.mean; r.fps = 1000.0 / s.mean; r.jitter = s.sd; r.p99 = s.p99;
    }
    r.cpu = double(cpuTicks() - bench_.cpuTicks0) / double(sysconf(_SC_CLK_TCK)) / ((t - bench_.t0) / 1000.0) * 100.0;
    r.rssMb = rssMb();
    bench_.rows.push_back(r);
    if (++bench_.step < bench_.steps.size()) { benchApplyStep(); return; }

    // done: the user's settings back, results to a CSV
    stopBenchmark();
    const QString path = reportDir() + "/veyamux-benchmark-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".csv";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        QTextStream o(&f);
        o << "# " << buildInfo() << " · " << bench_.source << " · "
          << (liveVk_->context() ? vulkanDriver(liveVk_->context()) : QString()) << "\n";
        o << "step,fps,frame_ms,jitter_ms,p99_ms,delay_ms,gpu_ms,gpu_input,gpu_denoise,gpu_aa,gpu_upscale,gpu_finish,gpu_output,cpu_pct,rss_mb\n";
        for (const auto& b : bench_.rows) {
            o << '"' << b.name << '"';
            for (double v : {b.fps, b.frameMs, b.jitter, b.p99, b.delay, b.gpu}) o << ',' << QString::number(v, 'f', 3);
            for (double v : b.stages) o << ',' << (v < 0 ? QString() : QString::number(v, 'f', 3));
            o << ',' << QString::number(b.cpu, 'f', 1) << ',' << QString::number(b.rssMb, 'f', 0) << '\n';
        }
        bench_.csv = path;
    }
    bench_.text = L("Benchmark done (%1 steps). Saved: %2. It is also in the next report.",
                    "Benchmark færdig (%1 trin). Gemt: %2. Den kommer også med i næste rapport.")
                      .arg(bench_.rows.size()).arg(QFileInfo(path).fileName());
    emit benchChanged();
}

void PlayerBridge::stopBenchmark() {
    if (bench_.timer) { bench_.timer->stop(); bench_.timer->deleteLater(); bench_.timer = nullptr; }
    if (bench_.running) {
        bench_.running = false;
        s_ = bench_.saved;
        applyEnhance();
        if (bench_.step < bench_.steps.size())
            bench_.text = L("Benchmark stopped after %1 steps.", "Benchmark stoppet efter %1 trin.").arg(bench_.rows.size());
    }
    emit benchChanged();
}

QString PlayerBridge::benchTable() const {
    QStringList r;
    r << QStringLiteral("Source: %1%2").arg(bench_.source, bench_.csv.isEmpty() ? "" : " · CSV: `" + bench_.csv + "`") << ""
      << "| Step | fps | jitter ms | 99 % ms | delay ms | GPU ms | input | denoise/AF | AA | upscale | finish | output | CPU % |"
      << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|";
    for (const auto& b : bench_.rows) {
        QString line = QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6").arg(b.name).arg(b.fps, 0, 'f', 1).arg(b.jitter, 0, 'f', 2)
                           .arg(b.p99, 0, 'f', 1).arg(b.delay, 0, 'f', 1).arg(b.gpu, 0, 'f', 2);
        for (double v : b.stages) line += v < 0 ? QStringLiteral(" | –") : QStringLiteral(" | %1").arg(v, 0, 'f', 2);
        line += QStringLiteral(" | %1 |").arg(b.cpu, 0, 'f', 0);
        r << line;
    }
    return r.join('\n');
}
