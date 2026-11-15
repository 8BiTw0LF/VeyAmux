#include "LiveVulkan.h"
#include <cstring>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <algorithm>
#include <QDebug>
#include <chrono>

using namespace veyamux;

LiveVulkan::LiveVulkan(QQuickWindow* window, VkContext* ctx, const QString& shaderDir, QObject* parent)
    : QObject(parent), win_(window), ctx_(ctx), chain_(std::make_unique<EnhanceChain>(*ctx, shaderDir.toStdString())) {
    connect(window, &QQuickWindow::beforeRendering, this, &LiveVulkan::onBeforeRendering, Qt::DirectConnection);
}

LiveVulkan::~LiveVulkan() {
    streamQuit_ = true;
    if (streamThread_.joinable()) streamThread_.join();
}

bool LiveVulkan::init(std::string& error) {
    if (!chain_->init(error)) return false;
    streamThread_ = std::thread([this] { streamLoop(); });
    return true;
}

void LiveVulkan::setStream(int maxHeight, std::function<void(QImage, int)> sink) {
    { std::lock_guard l(sinkM_); sink_ = std::move(sink); }
    chain_->setStreamMaxHeight(maxHeight);
}

// Collects the OBS stream copies as soon as the GPU has written them (independent of when Qt renders next).
void LiveVulkan::streamLoop() {
    std::vector<EnhanceChain::StreamFrame> frames;
    while (!streamQuit_.load()) {
        frames.clear();
        chain_->pollStream(frames);
        if (!frames.empty()) {
            std::function<void(QImage, int)> sink;
            { std::lock_guard l(sinkM_); sink = sink_; }
            for (auto& f : frames) {
                if (!sink) continue;
                QImage img(f.w, f.h, QImage::Format_RGBA8888);
                for (int y = 0; y < f.h; ++y) std::memcpy(img.scanLine(y), f.rgba.data() + size_t(y) * f.w * 4, size_t(f.w) * 4);
                sink(std::move(img), f.delayMs);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

QString LiveVulkan::lastUpError() const {
    std::lock_guard l(m_);
    return QString::fromStdString(upError_);
}

QString LiveVulkan::lastError() const {
    std::lock_guard l(m_);
    return QString::fromStdString(error_);
}

void LiveVulkan::setSettings(const EnhanceSettings& s) {
    {
        std::lock_guard l(m_);
        settings_ = s;
        redraw_ = true;           // re-run the chain on the current frame (matters while paused)
    }
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::setActive(bool on) {
    active_ = on;
    emit outputChanged();
    if (!on) {
        std::lock_guard l(m_);
        hasNew_ = haveCurrent_ = redraw_ = false;
    }
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::submit(RawFrame&& frame) {
    {
        std::lock_guard l(m_);
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double, std::milli>(now - lastArrival_).count();
        lastArrival_ = now;
        if (dt > 4.0 && dt < 100.0) frameMs_ = 0.85 * frameMs_ + 0.15 * dt;
        mailbox_ = std::move(frame);        // the newest frame always wins; an unrendered older one is simply replaced
        hasNew_ = true;
    }
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::onBeforeRendering() {
    if (!active_.load()) return;
    bool fresh = false;
    EnhanceSettings s;
    {
        std::lock_guard l(m_);
        s = settings_;
        if (hasNew_) {
            std::swap(current_, mailbox_);
            hasNew_ = false;
            haveCurrent_ = true;
            redraw_ = false;
            fresh = true;
        } else if (redraw_ && haveCurrent_) {
            redraw_ = false;
            fresh = true;
        }
    }
    if (!fresh) return;

    const auto t0 = std::chrono::steady_clock::now();
    LiveInput in;
    switch (current_.fmt) {
    case RawFrame::YUYV: in.fmt = LiveInput::YUYV; break;
    case RawFrame::UYVY: in.fmt = LiveInput::UYVY; break;
    case RawFrame::NV12: in.fmt = LiveInput::NV12; break;
    case RawFrame::P010: in.fmt = LiveInput::P010; break;
    default: in.fmt = LiveInput::RGBA; break;
    }
    in.data = current_.data.data();
    in.size = current_.data.size();
    in.w = current_.w; in.h = current_.h; in.stride = current_.stride;

    win_->beginExternalCommands();
    auto* rif = win_->rendererInterface();
    auto* cb = static_cast<VkCommandBuffer*>(rif->getResource(win_, QSGRendererInterface::CommandListResource));
    std::string err;
    double frameMs;
    { std::lock_guard l(m_); frameMs = frameMs_; }
    bool ok = cb && *cb && chain_->recordLive(*cb, win_->graphicsStateInfo().currentFrameSlot, in, s, err, float(frameMs));
    win_->endExternalCommands();
    if (!ok) {
        std::lock_guard l(m_);
        error_ = err.empty() ? "Live path unavailable" : err;
        static int warned = 0;
        if (warned++ < 3) qWarning() << "LiveVulkan: record failed:" << QString::fromStdString(error_);
        return;
    }
    {
        std::lock_guard l(m_);
        error_.clear();
        upError_ = chain_->upError();
    }
    const ImageRes& real = chain_->liveOutput();
    lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bool recreated = chain_->liveGeneration() != seenGen_;   // images were recreated (size change)
    seenGen_ = chain_->liveGeneration();
    ++seq_;
    const uint64_t realHandle = uint64_t(reinterpret_cast<uintptr_t>(real.image));
    if (qEnvironmentVariableIsSet("VEYAMUX_DEBUG_FG")) {
        static int real = 0;
        ++real;
        if (real % 10 == 0) qInfo("LiveVulkan: %d frames, capture interval %.1f ms, output %ux%u%s%s", real, frameMs,
                                     chain_->liveOutput().width, chain_->liveOutput().height,
                                     chain_->upError().empty() ? "" : ", upscaling error: ", chain_->upError().c_str());
    }
    publish(realHandle, int(real.width), int(real.height), recreated);
}

void LiveVulkan::publish(uint64_t handle, int w, int h, bool recreated) {
    ++shown_;
    imgW_ = w;
    imgH_ = h;
    image_ = handle;
    if (handle != lastHandle_.load() || recreated) {
        lastHandle_ = handle;
        gen_ = gen_.load() + 1;                     // VideoItem re-wraps the image on its next sync
        emit outputChanged();
    }
}

