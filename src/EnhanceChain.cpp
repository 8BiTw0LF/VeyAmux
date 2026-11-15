#include "EnhanceChain.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace veyamux {
namespace {
constexpr VkFormat kLinearFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags kWork = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
uint32_t groups(int v, int g) { return uint32_t((v + g - 1) / g); }

// cbuffer mirrors (DX packing, see the HLSL files)
struct RgbParams {
    uint32_t width, height, transfer, limited;
    float reserved[2]; float primaries2020, padding;
    float colorRow0[4], colorRow1[4], colorRow2[4], colorControls[4], colorFlags[4];
};
static_assert(sizeof(RgbParams) == 112);
struct AaParams {
    uint32_t width, height; float invW, invH;
    float workingToWhite, threshold, strength, historyWeight;
    float subsample[4];
    float historyRejection, historyValid, r0, r1;
};
static_assert(sizeof(AaParams) == 64);
struct ScaleParams { uint32_t sw, sh, ow, oh; float encodeSrgb, ditherStep, p2, p3; };
struct LanczosParams { uint32_t sw, sh, dw, dh; };
struct EnhanceParams { uint32_t w, h; float sharpen, exposure, contrast, saturation, pad[2]; };
} // namespace

EnhanceChain::EnhanceChain(VkContext& ctx, std::string shaderDir)
    : ctx_(ctx), dir_(std::move(shaderDir)), fsrUp_(std::make_unique<FsrUpscaler>(ctx, dir_)) {}

EnhanceChain::~EnhanceChain() {
    liveMode_ = false;
    releaseStream();
    fsrUp_.reset();
    nrMotion_.reset();
    releaseResources();
    for (auto& r : retired_) ctx_.destroy(r.img);
    retired_.clear();
    for (auto& [n, p] : kernels_) ctx_.destroy(p);
}

bool EnhanceChain::loadKernel(const std::string& name, std::string& error) {
    const std::string path = dir_ + "/" + name + ".spv";
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { error = "Missing shader: " + path; return false; }
    const auto size = size_t(f.tellg());
    f.seekg(0);
    std::vector<uint32_t> code((size + 3) / 4);
    f.read(reinterpret_cast<char*>(code.data()), std::streamsize(size));
    std::string e;
    VkPipeline p = ctx_.createComputePipeline(code, e);
    if (!p) { error = name + ": " + e; return false; }
    kernels_[name] = p;
    return true;
}

bool EnhanceChain::init(std::string& error) {
    for (const char* n : {"RgbToLinear", "AaLuma", "AaFxaa3", "AaFxaa4", "AaFxaa5", "LxLanczos", "LxEnhance", "ScaleBlit", "LxYuv", "LxDownscale", "LxNlm", "LxTemporalNr", "LxP010"})
        if (!loadKernel(n, error)) return false;
    return true;
}

VkPipeline EnhanceChain::kernel(const char* name) const {
    auto it = kernels_.find(name);
    return it == kernels_.end() ? VK_NULL_HANDLE : it->second;
}

void EnhanceChain::outputSize(int w, int h, const EnhanceSettings& s, int& ow, int& oh) {
    if (!s.enabled || s.scale <= 1.001f) { ow = w; oh = h; return; }
    ow = std::max(2, int(std::lround(w * s.scale)) & ~1);
    oh = std::max(2, int(std::lround(h * s.scale)) & ~1);
}

void EnhanceChain::releaseResources() {
    for (ImageRes* i : {&src_, &lin_, &linB_, &luma_, &up_, &fin_, &rawPacked_, &rawY_, &rawUV_, &rawY16_, &rawUV16_, &nrOut_, &nrHist_[0], &nrHist_[1]}) ctx_.destroy(*i);
    nrHistory_ = false;
    if (liveMode_) retire(out8_); else ctx_.destroy(out8_);
    for (auto& b : liveStaging_) ctx_.destroy(b);
    liveStagingSize_ = 0;
    ctx_.destroy(staging_);
    ctx_.destroy(readback_);
    w_ = h_ = ow_ = oh_ = 0;
}

bool EnhanceChain::ensureResources(int w, int h, int ow, int oh, std::string& error) {
    if (w == w_ && h == h_ && ow == ow_ && oh == oh_ && src_.image) return true;
    vkDeviceWaitIdle(ctx_.device());
    releaseResources();
    const VkImageUsageFlags upl = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    src_ = ctx_.createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, upl);
    lin_ = ctx_.createImage(w, h, kLinearFmt, kWork);
    linB_ = ctx_.createImage(w, h, kLinearFmt, kWork);
    luma_ = ctx_.createImage(w, h, ctx_.lumaFormat(), kWork);
    up_ = ctx_.createImage(ow, oh, kLinearFmt, kWork);
    fin_ = ctx_.createImage(ow, oh, kLinearFmt, kWork);
    out8_ = ctx_.createImage(ow, oh, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ++liveGen_;
    staging_ = ctx_.createBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    readback_ = ctx_.createBuffer(VkDeviceSize(ow) * oh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!src_.image || !lin_.image || !linB_.image || !luma_.image || !up_.image || !fin_.image || !out8_.image ||
        !staging_.buffer || !readback_.buffer) {
        error = "Out of GPU memory while allocating the enhancement chain";
        releaseResources();
        return false;
    }
    w_ = w; h_ = h; ow_ = ow; oh_ = oh;
    return true;
}

bool EnhanceChain::process(const uint8_t* rgba, int w, int h, int stride, const EnhanceSettings& s,
                           std::vector<uint8_t>& out, int& outW, int& outH, std::string& error,
                           float frameMs) {
    int ow, oh;
    outputSize(w, h, s, ow, oh);
    if (!ensureResources(w, h, ow, oh, error)) return false;
    outW = ow; outH = oh;
    frameMs_ = frameMs;

    // Upload (row by row so any source stride works).
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(staging_.mapped) + size_t(y) * w * 4, rgba + size_t(y) * stride, size_t(w) * 4);

    ctx_.beginFrame();
    VkCommandBuffer cmd = ctx_.cmd();
    VkBufferImageCopy up{};
    up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    up.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyBufferToImage(cmd, staging_.buffer, src_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &up);
    ctx_.computeBarrier();

    RgbParams rp{};
    rp.width = w; rp.height = h; rp.transfer = 1; rp.limited = 0;   // sws already produced full-range sRGB-coded RGB
    run("RgbToLinear", {&src_}, {&lin_}, &rp, sizeof rp, w, h, 16);

    auto post = [&](const ImageRes* start, BufferRes& dstBuf) {
        recordPost(start, s);
        VkBufferImageCopy rb{};
        rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rb.imageExtent = {uint32_t(ow), uint32_t(oh), 1};
        vkCmdCopyImageToBuffer(cmd, out8_.image, VK_IMAGE_LAYOUT_GENERAL, dstBuf.buffer, 1, &rb);
        ctx_.computeBarrier();
    };
    post(&lin_, readback_);

    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);

    if (!ctx_.submitAndWait(error)) return false;
    out.resize(size_t(ow) * oh * 4);
    std::memcpy(out.data(), readback_.mapped, out.size());
    return true;
}


// ------------------------------------------------------------------ shared recording helpers

void EnhanceChain::run(const char* k, std::initializer_list<const ImageRes*> in, std::initializer_list<const ImageRes*> outs,
                       const void* ubo, uint32_t uboSize, int dw, int dh, int g) {
    std::vector<const ImageRes*> a(in), b(outs);
    ctx_.dispatch(kernel(k), a.data(), uint32_t(a.size()), b.data(), uint32_t(b.size()), ubo, uboSize, groups(dw, g),
                  groups(dh, g));
    ctx_.computeBarrier();
}

// [FXAA] -> [Lanczos] -> [sharpen/grade] -> sRGB encode into out8_.
void EnhanceChain::recordPost(const ImageRes* start, const EnhanceSettings& s) {
    const int w = w_, h = h_, ow = ow_, oh = oh_;
    const ImageRes* cur = start;
    if (s.enabled && s.denoise != Denoise::Off) cur = recordDenoise(cur, s);
    else nrHistory_ = false;
    if (s.enabled && s.fxaa) {
        AaParams ap{};
        ap.width = w; ap.height = h; ap.invW = 1.0f / w; ap.invH = 1.0f / h; ap.workingToWhite = 1.0f;
        ap.threshold = 0.1f; ap.strength = 1.0f;
        run("AaLuma", {cur}, {&luma_}, &ap, sizeof ap, w, h, 8);
        const char* fx = s.fxaaQuality <= 0 ? "AaFxaa3" : (s.fxaaQuality == 1 ? "AaFxaa4" : "AaFxaa5");
        run(fx, {cur, &luma_}, {&linB_}, &ap, sizeof ap, w, h, 8);
        cur = &linB_;
    }
    if (s.enabled && (ow != w || oh != h)) {
        bool done = false;
        // A setup failure is not retried every frame (context creation stalls the GPU): only after the size or the
        // upscaler choice changes.
        const uint64_t key = (uint64_t(w) << 48) ^ (uint64_t(h) << 32) ^ (uint64_t(ow) << 16) ^ uint64_t(oh);
        if (s.upscaler == Upscaler::AmdFsr3 && key != upFailedKey_) {
            std::string e;
            done = fsrUp_->record(ctx_.active(), *cur, up_, !upHistory_, frameMs_, e);
            upHistory_ = done;
            if (!done) { upError_ = e; upFailedKey_ = key; }
            else upError_.clear();
        } else if (s.upscaler != Upscaler::AmdFsr3) {
            upHistory_ = false;
            upError_.clear();
            upFailedKey_ = 0;
        }
        if (!done) {
            LanczosParams lp{uint32_t(w), uint32_t(h), uint32_t(ow), uint32_t(oh)};
            run("LxLanczos", {cur}, {&up_}, &lp, sizeof lp, ow, oh, 16);
        }
        cur = &up_;
    } else {
        upHistory_ = false;
    }
    if (s.enabled && (s.sharpen > 0.001f || s.exposure != 0.0f || s.contrast != 1.0f || s.saturation != 1.0f)) {
        EnhanceParams ep{uint32_t(ow), uint32_t(oh), s.sharpen, s.exposure, s.contrast, s.saturation, {0, 0}};
        run("LxEnhance", {cur}, {&fin_}, &ep, sizeof ep, ow, oh, 16);
        cur = &fin_;
    }
    ScaleParams sp{uint32_t(ow), uint32_t(oh), uint32_t(ow), uint32_t(oh), 1.0f, 0.0f, 0, 0};
    run("ScaleBlit", {cur}, {&out8_}, &sp, sizeof sp, ow, oh, 16);
}

// Noise levels (perceptual units, sqrt of linear light) for light / medium / strong.
static constexpr float kNrSigma[3] = {0.010f, 0.018f, 0.028f};
static constexpr float kNrBlend[3] = {0.6f, 0.8f, 0.9f};   // temporal: at most this much history per frame

const ImageRes* EnhanceChain::recordDenoise(const ImageRes* in, const EnhanceSettings& s) {
    const int w = w_, h = h_;
    if (!nrOut_.image) {
        nrOut_ = ctx_.createImage(w, h, kLinearFmt, kWork);
        nrHist_[0] = ctx_.createImage(w, h, kLinearFmt, kWork);
        nrHist_[1] = ctx_.createImage(w, h, kLinearFmt, kWork);
        nrHistory_ = false;
        if (!nrOut_.image || !nrHist_[0].image || !nrHist_[1].image) return in;
    }
    const int k = std::clamp(s.denoiseStrength, 0, 2);
    const ImageRes* cur = in;
    if (s.denoise == Denoise::Temporal || s.denoise == Denoise::Both) {
        if (!nrMotion_) nrMotion_ = std::make_unique<MotionEstimator>(ctx_, dir_);
        std::string e;
        static const bool zeroMv = std::getenv("VEYAMUX_NR_ZEROMV") != nullptr;   // test hook: no motion
        const ImageRes* mv = nrMotion_->record(ctx_.active(), *in, !nrHistory_ || zeroMv, e);
        if (mv) {
            const int prev = nrCur_;
            nrCur_ ^= 1;
            struct { uint32_t w, h; float sigma, blend; uint32_t reset, p0, p1, p2; } tp{
                uint32_t(w), uint32_t(h), kNrSigma[k], kNrBlend[k], nrHistory_ ? 0u : 1u, 0, 0, 0};
            run("LxTemporalNr", {in, &nrHist_[prev], mv}, {&nrHist_[nrCur_]}, &tp, sizeof tp, w, h, 16);
            cur = &nrHist_[nrCur_];
            nrHistory_ = true;
        } else {
            nrHistory_ = false;
        }
    } else {
        nrHistory_ = false;
    }
    if (s.denoise == Denoise::Spatial || s.denoise == Denoise::Both) {
        // after the temporal pass less noise is left: a gentler spatial pass keeps more texture
        static const float tuneS = std::getenv("VEYAMUX_NR_SIGMA") ? float(std::atof(std::getenv("VEYAMUX_NR_SIGMA"))) : 0.0f;
        static const float tuneH = std::getenv("VEYAMUX_NR_H") ? float(std::atof(std::getenv("VEYAMUX_NR_H"))) : 0.7f;
        const float sg = (tuneS > 0 ? tuneS : kNrSigma[k]) * (s.denoise == Denoise::Both ? 0.6f : 1.0f);
        struct { uint32_t w, h; float sigma, filterH; } np{uint32_t(w), uint32_t(h), sg, sg * tuneH};
        run("LxNlm", {cur}, {&nrOut_}, &np, sizeof np, w, h, 16);
        cur = &nrOut_;
    }
    return cur;
}

void EnhanceChain::retire(ImageRes& img) {
    if (img.image) retired_.push_back({img, 8});
    img = {};
}

void EnhanceChain::collectRetired() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (--it->framesLeft <= 0) { ctx_.destroy(it->img); it = retired_.erase(it); }
        else ++it;
    }
}

bool EnhanceChain::ensureLive(int w, int h, int ow, int oh, std::string& error) {
    const VkDeviceSize need = VkDeviceSize(w) * h * 4;
    if (liveW_ == w && liveH_ == h && w_ == w && h_ == h && ow_ == ow && oh_ == oh && out8_.image && liveStagingSize_ >= need)
        return true;
    liveMode_ = true;
    if (!ensureResources(w, h, ow, oh, error)) return false;     // also recreates out8_ (old one retired)
    rawPacked_ = ctx_.createImage(uint32_t(w / 2), uint32_t(h), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    rawY_ = ctx_.createImage(uint32_t(w), uint32_t(h), VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    rawUV_ = ctx_.createImage(uint32_t(w / 2), uint32_t(h / 2), VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    bool ok = rawPacked_.image && rawY_.image && rawUV_.image;
    for (auto& b : liveStaging_) {
        b = ctx_.createBuffer(need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
        ok = ok && b.buffer;
    }
    if (!ok) { error = "Out of GPU memory while allocating the live capture path"; return false; }
    liveStagingSize_ = need;
    liveW_ = w; liveH_ = h;
    return true;
}

bool EnhanceChain::recordLive(VkCommandBuffer cb, int slot, const LiveInput& in, const EnhanceSettings& s, std::string& error,
                              float frameMs) {
    frameMs_ = frameMs;
    int ow, oh;
    outputSize(in.w, in.h, s, ow, oh);
    if (!ensureLive(in.w, in.h, ow, oh, error)) return false;
    collectRetired();
    if (in.fmt == LiveInput::P010 && !rawY16_.image) {   // HDR capture: 16-bit planes, created on first use
        const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        rawY16_ = ctx_.createImage(uint32_t(in.w), uint32_t(in.h), VK_FORMAT_R16_UNORM, u);
        rawUV16_ = ctx_.createImage(uint32_t(in.w / 2), uint32_t(in.h / 2), VK_FORMAT_R16G16_UNORM, u);
        if (!rawY16_.image || !rawUV16_.image) { error = "Out of GPU memory for HDR capture"; return false; }
    }
    const int sl = ((slot % VkContext::kSlots) + VkContext::kSlots) % VkContext::kSlots;
    const size_t bytes = std::min<size_t>(in.size, size_t(liveStagingSize_));
    std::memcpy(liveStaging_[sl].mapped, in.data, bytes);

    ctx_.beginExternal(cb, sl);
    // Previous frames may still be reading our images (Qt's fragment pass) or the staging buffer: wait for them.
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = mb.srcAccessMask;
        // The image Qt displays is handed to Qt in SHADER_READ_ONLY_OPTIMAL and Qt leaves it there. It is completely
        // rewritten every frame, so take it back to GENERAL with UNDEFINED as the old layout (contents discarded) -
        // valid whatever layout Qt left it in.
        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = out8_.image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, nullptr, 1, &ib);
    }
    const BufferRes& st = liveStaging_[sl];
    auto copyTo = [&](const ImageRes& img, VkDeviceSize offset, uint32_t rowLenTexels, uint32_t w, uint32_t h) {
        VkBufferImageCopy c{};
        c.bufferOffset = offset;
        c.bufferRowLength = rowLenTexels;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, st.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    };
    const uint32_t W = uint32_t(in.w), H = uint32_t(in.h);
    switch (in.fmt) {
    case LiveInput::RGBA: copyTo(src_, 0, uint32_t(in.stride / 4), W, H); break;
    case LiveInput::YUYV:
    case LiveInput::UYVY: copyTo(rawPacked_, 0, uint32_t(in.stride / 4), W / 2, H); break;
    case LiveInput::NV12:
        copyTo(rawY_, 0, uint32_t(in.stride), W, H);
        copyTo(rawUV_, VkDeviceSize(in.stride) * H, uint32_t(in.stride / 2), W / 2, H / 2);
        break;
    case LiveInput::P010:
        copyTo(rawY16_, 0, uint32_t(in.stride / 2), W, H);
        copyTo(rawUV16_, VkDeviceSize(in.stride) * H, uint32_t(in.stride / 4), W / 2, H / 2);
        break;
    }
    ctx_.computeBarrier();
    if (in.fmt == LiveInput::P010) {
        // HDR: PQ/BT.2020 -> tone-mapped linear SDR straight into the working image (keeps all 10 bits)
        struct { uint32_t w, h, full, p0; float peak, white, p1, p2; } hp{W, H, in.fullRange ? 1u : 0u, 0,
                                                                         std::clamp(s.hdrPeak, 203.0f, 10000.0f), 203.0f, 0, 0};
        run("LxP010", {&rawY16_, &rawUV16_}, {&lin_}, &hp, sizeof hp, in.w, in.h, 16);
    } else if (in.fmt != LiveInput::RGBA) {
        struct { uint32_t w, h, mode, full, bt709, p0, p1, p2; } yp{W, H, in.fmt == LiveInput::YUYV ? 0u : (in.fmt == LiveInput::UYVY ? 1u : 2u),
                                                                      in.fullRange ? 1u : 0u, (in.h >= 720 || in.w >= 1280) ? 1u : 0u, 0, 0, 0};
        run("LxYuv", {&rawPacked_, &rawY_, &rawUV_}, {&src_}, &yp, sizeof yp, in.w, in.h, 16);
    }
    if (in.fmt != LiveInput::P010) {
        RgbParams rp{};
        rp.width = W; rp.height = H; rp.transfer = 1; rp.limited = 0;
        run("RgbToLinear", {&src_}, {&lin_}, &rp, sizeof rp, in.w, in.h, 16);
    }
    recordPost(&lin_, s);
    // OBS stream copy of the displayed image
    if (streamMaxH_.load() >= 0) recordStream(cb, out8_, 0);

    // Hand the display image to Qt's fragment shader in SHADER_READ_ONLY_OPTIMAL - the layout the QSGTexture
    // wrapper is created with (VideoItem), so Qt's own layout tracking matches reality.
    VkImageMemoryBarrier ob{};
    ob.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ob.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    ob.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ob.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ob.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ob.srcQueueFamilyIndex = ob.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ob.image = out8_.image;
    ob.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ob);
    return true;
}

void EnhanceChain::recordStream(VkCommandBuffer cb, const ImageRes& src, int delayMs) {
    if (!src.image || src.width == 0) return;
    int mh = streamMaxH_.load();
    int tw = int(src.width), th = int(src.height);
    if (mh > 0 && th > mh) { tw = int(std::lround(double(tw) * mh / th)); th = mh; }
    tw = std::max(2, tw & ~1); th = std::max(2, th & ~1);

    std::lock_guard l(streamM_);
    StreamSlot* slot = nullptr;
    for (auto& sl : stream_) if (!sl.pending) { slot = &sl; break; }
    if (!slot) return;   // the reader is behind: this frame is not streamed (the screen is unaffected)
    if (slot->img.width != uint32_t(tw) || slot->img.height != uint32_t(th)) {
        ctx_.destroy(slot->img);
        ctx_.destroy(slot->buf);
        slot->img = ctx_.createImage(uint32_t(tw), uint32_t(th), VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        slot->buf = ctx_.createBuffer(VkDeviceSize(tw) * th * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!slot->img.image || !slot->buf.buffer) { ctx_.destroy(slot->img); ctx_.destroy(slot->buf); return; }
    }
    if (!slot->ev) {
        VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (vkCreateEvent(ctx_.device(), &ei, nullptr, &slot->ev) != VK_SUCCESS) return;
    }
    struct { uint32_t sw, sh, dw, dh; } dp{src.width, src.height, uint32_t(tw), uint32_t(th)};
    run("LxDownscale", {&src}, {&slot->img}, &dp, sizeof dp, tw, th, 16);
    ctx_.computeBarrier();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {uint32_t(tw), uint32_t(th), 1};
    vkCmdCopyImageToBuffer(cb, slot->img.image, VK_IMAGE_LAYOUT_GENERAL, slot->buf.buffer, 1, &c);
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    vkCmdSetEvent(cb, slot->ev, VK_PIPELINE_STAGE_TRANSFER_BIT);
    slot->pending = true;
    slot->seq = ++streamSeq_;
    slot->delayMs = delayMs;
}

void EnhanceChain::pollStream(std::vector<StreamFrame>& out) {
    std::lock_guard l(streamM_);
    for (;;) {
        StreamSlot* oldest = nullptr;
        for (auto& sl : stream_)
            if (sl.pending && (!oldest || sl.seq < oldest->seq)) oldest = &sl;
        if (!oldest || vkGetEventStatus(ctx_.device(), oldest->ev) != VK_EVENT_SET) return;
        VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        r.memory = oldest->buf.memory;
        r.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(ctx_.device(), 1, &r);
        StreamFrame f;
        f.w = int(oldest->img.width); f.h = int(oldest->img.height); f.delayMs = oldest->delayMs;
        const auto* p = static_cast<const uint8_t*>(oldest->buf.mapped);
        f.rgba.assign(p, p + size_t(f.w) * f.h * 4);
        vkResetEvent(ctx_.device(), oldest->ev);
        oldest->pending = false;
        out.push_back(std::move(f));
    }
}

void EnhanceChain::releaseStream() {
    std::lock_guard l(streamM_);
    bool any = false;
    for (auto& sl : stream_) any = any || sl.img.image || sl.ev;
    if (!any) return;
    vkDeviceWaitIdle(ctx_.device());
    for (auto& sl : stream_) {
        ctx_.destroy(sl.img);
        ctx_.destroy(sl.buf);
        if (sl.ev) vkDestroyEvent(ctx_.device(), sl.ev, nullptr);
        sl = StreamSlot{};
    }
}

} // namespace veyamux
