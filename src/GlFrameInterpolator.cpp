#include "GlFrameInterpolator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <dlfcn.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <link.h>
#endif

#include <pl/memory/Signature.hpp>

namespace framegen {
namespace {

using PFN_glBindVertexArray = void (*)(GLuint);
using PFN_glBindSampler = void (*)(GLuint, GLuint);
constexpr GLenum kVertexArrayBindingPname = 0x85B5;
constexpr GLenum kSamplerBindingPname = 0x8919;
constexpr GLenum kPixelUnpackBufferTarget = 0x88EC;
constexpr GLenum kPixelUnpackBufferBindingPname = 0x88EF;
constexpr EGLint kEglRenderBufferAttr = 0x3086;
constexpr EGLint kEglBackBufferValue = 0x3084;

PFN_glBindVertexArray gBindVertexArrayFn = nullptr;
PFN_glBindSampler gBindSamplerFn = nullptr;

constexpr float kFullscreenQuadVertices[16] = {
    // x,     y,    u,    v
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f
};

constexpr const char *kSynthesisVertexShaderSrc = R"glsl(
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

constexpr const char *kSynthesisFragmentShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;

uniform sampler2D uPrevTex;
uniform sampler2D uCurTex;
uniform vec2 uMotionVec;
uniform vec2 uPrevMotionVec;
uniform vec2 uTexelSize;
uniform vec2 uTanHalfFov;
uniform float uMotionConfidence;
uniform float uPhase;
uniform float uBlendWeight;
uniform float uHudProtection;
uniform float uTrailFreeGuard;
uniform float uOpaque;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// 3D Gnomonic (Perspective-Tangent) Camera Flow:
// Accounts for perspective projection curvature (1 + tan^2(fov/2) * ndc^2)
// so fast camera rotations do not stretch world geometry near screen edges.
vec2 perspectiveCameraFlow(vec2 uv, vec2 baseMv) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec2 t = ndc * uTanHalfFov;
    vec2 radialScale = clamp(1.0 + t * t * 0.36, 0.85, 1.42);
    float crossTerm = clamp(ndc.x * ndc.y * uTanHalfFov.x * uTanHalfFov.y * 0.14, -0.12, 0.12);
    return vec2(
        baseMv.x * radialScale.x + baseMv.y * crossTerm,
        baseMv.y * radialScale.y + baseMv.x * crossTerm
    );
}

void evalMotionCandidate(vec2 candVec, float priorPenalty, float phase,
                         inout vec2 bestVec, inout float bestCost) {
    vec2 uvA = clamp(vUV - phase * candVec, 0.0, 1.0);
    vec2 uvB = clamp(vUV + (1.0 - phase) * candVec, 0.0, 1.0);
    vec3 cA = texture2D(uPrevTex, uvA).rgb;
    vec3 cB = texture2D(uCurTex, uvB).rgb;
    float lDiff = abs(luma(cA) - luma(cB));
    vec3 rgbDiff = abs(cA - cB);
    float cDiff = (rgbDiff.r + rgbDiff.g + rgbDiff.b) * 0.3333;
    float cost = lDiff * 0.65 + cDiff * 0.35 + priorPenalty;
    if (cost < bestCost) {
        bestCost = cost;
        bestVec = candVec;
    }
}

// Contrast-Adaptive Sub-Texel Sharpening (CAS):
// Restores crisp block edges, item models, and pixel-art textures when
// bilinear warping samples between texel centers, strictly clamped to the
// local 5-tap min/max neighborhood so it never introduces ringing or halos.
vec3 sampleBidirectionalSharp(sampler2D tex, vec2 uv) {
    vec2 clampedUv = clamp(uv, 0.0, 1.0);
    vec3 c = texture2D(tex, clampedUv).rgb;
    vec2 ts = max(uTexelSize, vec2(0.0002, 0.0002));
    vec3 n = texture2D(tex, clamp(clampedUv + vec2(0.0, ts.y), 0.0, 1.0)).rgb;
    vec3 s = texture2D(tex, clamp(clampedUv - vec2(0.0, ts.y), 0.0, 1.0)).rgb;
    vec3 e = texture2D(tex, clamp(clampedUv + vec2(ts.x, 0.0), 0.0, 1.0)).rgb;
    vec3 w = texture2D(tex, clamp(clampedUv - vec2(ts.x, 0.0), 0.0, 1.0)).rgb;

    vec3 minCross = min(c, min(min(n, s), min(e, w)));
    vec3 maxCross = max(c, max(max(n, s), max(e, w)));
    vec3 avgCross = 0.25 * (n + s + e + w);

    vec2 f = fract(clampedUv / ts);
    float subpixel = max(4.0 * f.x * (1.0 - f.x), 4.0 * f.y * (1.0 - f.y));
    float localContrast = luma(maxCross - minCross);
    float casGain = (0.26 + 0.24 * subpixel) * (1.0 - smoothstep(0.45, 0.85, localContrast));
    vec3 sharpened = c + (c - avgCross) * casGain;
    return clamp(sharpened, minCross, maxCross);
}

// Synthesizes one true in-between frame at `phase` between REAL A and REAL B.
// `outAmbiguity` reports how unreliable the local warp is (0 = confident
// motion-compensated midpoint, 1 = both sources disagree with no clear winner).
vec3 synthesizePhaseSlice(float phase, vec2 camVec, vec2 prevCamVec,
                          vec3 prevUnshifted, vec3 curUnshifted,
                          inout float outAmbiguity, inout float outSliceConfidence) {
    outAmbiguity = 0.0;
    outSliceConfidence = 1.0;

    // 9-Candidate Local Motion & Depth-Parallax Refinement:
    // Tracks camera rotation, strafe depth parallax (near blocks vs far terrain),
    // temporal trajectory continuity, and local moving entities / particles.
    vec2 bestVec = camVec;
    float bestCost = 1000.0;
    evalMotionCandidate(camVec, 0.000, phase, bestVec, bestCost);
    evalMotionCandidate(camVec * 1.32, 0.005, phase, bestVec, bestCost);
    evalMotionCandidate(camVec * 0.58, 0.005, phase, bestVec, bestCost);
    evalMotionCandidate(prevCamVec, 0.007, phase, bestVec, bestCost);
    evalMotionCandidate(vec2(0.0, 0.0), 0.012, phase, bestVec, bestCost);

    vec2 ts = max(uTexelSize, vec2(0.0005, 0.0005));
    vec2 localRadius = clamp(abs(camVec) * 0.35 + ts * 3.0, ts * 2.0, vec2(0.035, 0.035));
    evalMotionCandidate(camVec + vec2(localRadius.x, 0.0), 0.009, phase, bestVec, bestCost);
    evalMotionCandidate(camVec - vec2(localRadius.x, 0.0), 0.009, phase, bestVec, bestCost);
    evalMotionCandidate(camVec + vec2(0.0, localRadius.y), 0.009, phase, bestVec, bestCost);
    evalMotionCandidate(camVec - vec2(0.0, localRadius.y), 0.009, phase, bestVec, bestCost);

    // Bidirectional Forward (REAL A -> midpoint) and Backward (REAL B -> midpoint) Warps
    vec2 rawUvA = vUV - phase * bestVec;
    vec2 rawUvB = vUV + (1.0 - phase) * bestVec;

    float inBoundsA = step(0.0, rawUvA.x) * step(rawUvA.x, 1.0) *
                      step(0.0, rawUvA.y) * step(rawUvA.y, 1.0);
    float inBoundsB = step(0.0, rawUvB.x) * step(rawUvB.x, 1.0) *
                      step(0.0, rawUvB.y) * step(rawUvB.y, 1.0);

    vec3 colA = sampleBidirectionalSharp(uPrevTex, rawUvA);
    vec3 colB = sampleBidirectionalSharp(uCurTex, rawUvB);

    // Anti-Ghosting Disocclusion Resolver:
    // When colA and colB agree (low diffAB), combine smoothly at phase.
    // When colA and colB disagree (crossing block edge, entity, player model,
    // or newly revealed background), select the single source most consistent
    // with the temporal reference instead of ghost-blending double edges.
    float diffAB = abs(luma(colA) - luma(colB)) + 0.35 * length(colA - colB);
    vec3 midRef = mix(prevUnshifted, curUnshifted, phase);
    float errA = abs(luma(colA) - luma(midRef)) + 0.30 * length(colA - midRef);
    float errB = abs(luma(colB) - luma(midRef)) + 0.30 * length(colB - midRef);

    float winnerB = smoothstep(-0.035, 0.035, (errA - errB) + (phase - 0.5) * 0.02);
    float disocclusionGate = smoothstep(0.06, 0.18, diffAB);
    float wB = mix(phase, winnerB, disocclusionGate);

    // Newly revealed terrain at screen borders: select the in-bounds frame cleanly
    bool borderWarp = false;
    if (inBoundsA < 0.5 && inBoundsB > 0.5) {
        wB = 1.0;
        borderWarp = true;
    } else if (inBoundsB < 0.5 && inBoundsA > 0.5) {
        wB = 0.0;
        borderWarp = true;
    } else if (inBoundsA < 0.5 && inBoundsB < 0.5) {
        // Neither warp lands on a real texel: fully unreliable.
        outAmbiguity = 1.0;
        outSliceConfidence = 0.0;
        return curUnshifted;
    }

    // Trail-Free Motion Guard metric:
    // High source disagreement with an ambiguous winner means the local flow is
    // unreliable (crossing edges, disocclusion, aliased textures). Reporting it
    // lets the caller keep the razor-sharp real pixel instead of leaving a
    // smeared motion trail / ghost.
    float ambiguity = disocclusionGate * (1.0 - abs(winnerB - 0.5) * 2.0);
    if (borderWarp) {
        ambiguity = max(ambiguity, 0.55);
    }
    outAmbiguity = clamp(ambiguity, 0.0, 1.0);

    // Winner-take-all selection confidence (residual cost of the chosen vector).
    outSliceConfidence = 1.0 - clamp((bestCost - 0.02) * 1.6, 0.0, 0.45);

    return mix(colA, colB, wB);
}

void main() {
    vec4 curUnshifted = texture2D(uCurTex, vUV);
    vec4 prevUnshifted = texture2D(uPrevTex, vUV);

    // Pure REAL pass (blendWeight == 0.0): anchors the temporal sequence
    // REAL A -> GENERATED -> REAL B 100% back to ground truth so interpolation
    // errors never accumulate. Also guarantees a pixel-perfect real frame.
    if (uBlendWeight <= 0.001) {
        gl_FragColor = vec4(curUnshifted.rgb, 1.0);
        return;
    }

    float phase = clamp(abs(uPhase), 0.0, 1.0);
    float conf = clamp(uMotionConfidence, 0.15, 1.0);
    // Full-magnitude warp: confidence only weights trust, not motion length.
    vec2 camVec = perspectiveCameraFlow(vUV, uMotionVec);
    vec2 prevCamVec = perspectiveCameraFlow(vUV, uPrevMotionVec);

    // Multi-Tap Static HUD / Crosshair / Hotbar / Text Protection:
    // Checks center + neighbor static stability AND geometric UI regions so
    // crosshairs, hotbar slots, item counts, and text remain 100% untouched.
    vec2 ts = max(uTexelSize, vec2(0.0005, 0.0005));
    float staticDiffCenter = abs(luma(curUnshifted.rgb) - luma(prevUnshifted.rgb)) +
                             0.30 * length(curUnshifted.rgb - prevUnshifted.rgb);
    vec3 curE = texture2D(uCurTex, clamp(vUV + vec2(ts.x * 2.0, 0.0), 0.0, 1.0)).rgb;
    vec3 prevE = texture2D(uPrevTex, clamp(vUV + vec2(ts.x * 2.0, 0.0), 0.0, 1.0)).rgb;
    float staticDiffNeigh = abs(luma(curE) - luma(prevE));
    float staticDiff = 0.65 * staticDiffCenter + 0.35 * staticDiffNeigh;

    vec3 prevShifted = texture2D(uPrevTex, clamp(vUV - phase * camVec, 0.0, 1.0)).rgb;
    vec3 curShifted  = texture2D(uCurTex, clamp(vUV + (1.0 - phase) * camVec, 0.0, 1.0)).rgb;
    float motionDiff = abs(luma(curShifted) - luma(prevShifted)) +
                       0.25 * length(curShifted - prevShifted);

    // Geometric HUD priority zones: center crosshair, bottom hotbar, top FPS/status bar
    vec2 centerDist = (vUV - vec2(0.5, 0.5)) * vec2(1.777, 1.0);
    float inCrosshairZone = 1.0 - smoothstep(0.028, 0.048, length(centerDist));
    float inHotbarZone = step(0.86, vUV.y) * step(abs(vUV.x - 0.5), 0.38);
    float inTopBarZone = (1.0 - step(0.065, vUV.y)) * (1.0 - step(0.35, vUV.x));
    float uiZoneBoost = max(inCrosshairZone, max(inHotbarZone, inTopBarZone));

    float staticUiLock = (1.0 - smoothstep(0.003, 0.018, staticDiff)) * uiZoneBoost;
    float dynamicHudConf = clamp((motionDiff - staticDiff * 2.2) * 6.0, 0.0, 1.0);
    float hudMask = clamp(max(dynamicHudConf, staticUiLock) * clamp(uHudProtection, 0.0, 1.0), 0.0, 1.0);

    // Single true in-between frame at the exact sub-frame phase. No multi-phase
    // averaging anywhere: averaging phase slices is literally a temporal box
    // filter and was the main cause of the perceived "motion blur" smear.
    float ambiguity = 0.0;
    float sliceConfidence = 1.0;
    vec3 synthesized = synthesizePhaseSlice(phase, camVec, prevCamVec,
                                            prevUnshifted.rgb, curUnshifted.rgb,
                                            ambiguity, sliceConfidence);

    // Zero-Dimming Photometric Guard:
    // 1. Ensure synthesized luminance never drops below the scene floor.
    // 2. If texture readback on a tiled GPU ever returns empty/black texels,
    //    validCapture drops to 0.0 so the real pixel is kept at full brightness.
    float curL = luma(curUnshifted.rgb);
    float prevL = luma(prevUnshifted.rgb);
    float minSceneL = min(curL, prevL);
    float synthL = luma(synthesized);
    if (synthL < minSceneL * 0.96 && synthL > 0.001) {
        synthesized = clamp(synthesized * clamp((minSceneL * 0.96) / synthL, 1.0, 1.35), 0.0, 1.0);
        synthL = luma(synthesized);
    }
    float validCapture = smoothstep(0.004, 0.018, minSceneL) *
                         smoothstep(0.004, 0.018, synthL);

    // Static / sub-pixel motion passthrough: with essentially no camera motion
    // the real pixel is already the perfect answer, so never resample it.
    vec2 invTexel = 1.0 / max(uTexelSize, vec2(1e-6, 1e-6));
    float mvPixels = length(uMotionVec * invTexel);
    float staticGate = 1.0 - smoothstep(0.20, 0.70, mvPixels);

    // Trail-Free Motion Guard (default ON):
    // Where the bidirectional sources are ambiguous the synthesized pixel is
    // unreliable -> suppress it and keep the razor-sharp real pixel instead of
    // producing a smeared trail, ghosted edge, or blurry band.
    float trailSuppress = clamp(ambiguity * clamp(uTrailFreeGuard, 0.0, 1.0), 0.0, 1.0);

    float userStrength = clamp(uBlendWeight / 0.65, 0.0, 1.0);
    float selection = clamp(
        userStrength * conf * sliceConfidence * (1.0 - hudMask) * validCapture *
        (1.0 - staticGate) * (1.0 - trailSuppress), 0.0, 1.0);

    if (uOpaque > 0.5) {
        // Own presented frame (REAL -> GENERATED -> REAL): fully defined output.
        // Trustworthy warps contribute synthesized midpoint pixels; everything
        // else falls back to the exact 1:1 real pixel (no blending, no smear).
        gl_FragColor = vec4(mix(curUnshifted.rgb, synthesized, selection), 1.0);
    } else {
        // Hardware alpha-composited fallback into the live backbuffer.
        gl_FragColor = vec4(synthesized, selection);
    }
}
)glsl";

constexpr const char *kBlitVertexShaderSrc = R"glsl(
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)glsl";

constexpr const char *kBlitFragmentShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;
uniform sampler2D uTex;
void main() { gl_FragColor = texture2D(uTex, vUV); }
)glsl";

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void clearGlErrors() noexcept {
    for (int i = 0; i < 8; ++i) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

GLuint compileGlShader(GLenum type, const char *source) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint createTexture2D(GLsizei width, GLsizei height) noexcept {
    clearGlErrors();
    if (gBindSamplerFn != nullptr) {
        // Ensure no GLES3 Pixel Unpack Buffer is bound when passing nullptr to glTexImage2D
        glBindBuffer(kPixelUnpackBufferTarget, 0);
        clearGlErrors();
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

} // namespace

GlStateGuard::GlStateGuard() noexcept {
    clearGlErrors();
    glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &mActiveTexture);
    glGetIntegerv(GL_VIEWPORT, mViewport);
    glGetIntegerv(GL_SCISSOR_BOX, mScissorBox);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mFramebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &mArrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &mElementArrayBuffer);
    if (gBindVertexArrayFn != nullptr) {
        glGetIntegerv(kVertexArrayBindingPname, &mVertexArray);
    }
    if (gBindSamplerFn != nullptr) {
        glGetIntegerv(kPixelUnpackBufferBindingPname, &mPixelUnpackBuffer);
    }
    glGetIntegerv(GL_BLEND_SRC_RGB, &mBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &mBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &mBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &mBlendDstAlpha);

    mDepthTest = glIsEnabled(GL_DEPTH_TEST);
    mBlend = glIsEnabled(GL_BLEND);
    mCullFace = glIsEnabled(GL_CULL_FACE);
    mScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    mStencilTest = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex0);
    if (gBindSamplerFn != nullptr) {
        glGetIntegerv(kSamplerBindingPname, &mSampler0);
    }
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex1);
    if (gBindSamplerFn != nullptr) {
        glGetIntegerv(kSamplerBindingPname, &mSampler1);
    }
    glActiveTexture(static_cast<GLenum>(mActiveTexture));
    clearGlErrors();
}

GlStateGuard::~GlStateGuard() noexcept {
    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(static_cast<GLuint>(mVertexArray));
    }
    if (gBindSamplerFn != nullptr) {
        glBindBuffer(kPixelUnpackBufferTarget,
                     static_cast<GLuint>(mPixelUnpackBuffer));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(mFramebuffer));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(mArrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLuint>(mElementArrayBuffer));
    glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
    glScissor(mScissorBox[0], mScissorBox[1], mScissorBox[2], mScissorBox[3]);
    glUseProgram(static_cast<GLuint>(mProgram));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex0));
    if (gBindSamplerFn != nullptr) {
        gBindSamplerFn(0, static_cast<GLuint>(mSampler0));
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex1));
    if (gBindSamplerFn != nullptr) {
        gBindSamplerFn(1, static_cast<GLuint>(mSampler1));
    }
    glActiveTexture(static_cast<GLenum>(mActiveTexture));

    glBlendFuncSeparate(static_cast<GLenum>(mBlendSrcRgb),
                        static_cast<GLenum>(mBlendDstRgb),
                        static_cast<GLenum>(mBlendSrcAlpha),
                        static_cast<GLenum>(mBlendDstAlpha));

    auto restoreCap = [](GLenum cap, GLboolean enabled) {
        if (enabled == GL_TRUE) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
    };
    restoreCap(GL_DEPTH_TEST, mDepthTest);
    restoreCap(GL_BLEND, mBlend);
    restoreCap(GL_CULL_FACE, mCullFace);
    restoreCap(GL_SCISSOR_TEST, mScissorTest);
    restoreCap(GL_STENCIL_TEST, mStencilTest);

    glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
    clearGlErrors();
}

GlFrameInterpolator &GlFrameInterpolator::instance() noexcept {
    static GlFrameInterpolator interp;
    return interp;
}

bool GlFrameInterpolator::installHook() noexcept {
    if (mHookInstalled.load(std::memory_order_relaxed)) {
        return true;
    }

    void *eglHandle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    void *swapSym = nullptr;
    if (eglHandle != nullptr) {
        swapSym = dlsym(eglHandle, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        swapSym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        return false;
    }

    mSwapHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(swapSym),
        reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour),
        reinterpret_cast<pl::memory::FuncPtr *>(&mOrigSwapBuffers),
        pl::memory::HookPriority::Normal);

    if (!mSwapHook.installed()) {
        return false;
    }

    // Resolve optional eglSwapInterval for VSync stall elimination
    auto swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
        eglGetProcAddress("eglSwapInterval"));
    if (swapIntFn == nullptr && eglHandle != nullptr) {
        swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
            dlsym(eglHandle, "eglSwapInterval"));
    }
    if (swapIntFn == nullptr) {
        swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
            dlsym(RTLD_DEFAULT, "eglSwapInterval"));
    }
    mSwapIntervalFn = swapIntFn;

    // Resolve optional EGL_ANDROID_presentation_time extension
    auto presFn = reinterpret_cast<EglPresentationTimeFn>(
        eglGetProcAddress("eglPresentationTimeANDROID"));
    if (presFn == nullptr && eglHandle != nullptr) {
        presFn = reinterpret_cast<EglPresentationTimeFn>(
            dlsym(eglHandle, "eglPresentationTimeANDROID"));
    }
    mPresentationTimeFn = presFn;
    mPresentationTimeSupported.store(presFn != nullptr,
                                     std::memory_order_relaxed);

    // Resolve optional VAO and GLES3 Sampler bind helpers for GlStateGuard
    gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
        eglGetProcAddress("glBindVertexArrayOES"));
    if (gBindVertexArrayFn == nullptr) {
        gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
            eglGetProcAddress("glBindVertexArray"));
    }
    gBindSamplerFn = reinterpret_cast<PFN_glBindSampler>(
        eglGetProcAddress("glBindSampler"));
    if (gBindSamplerFn == nullptr) {
        gBindSamplerFn = reinterpret_cast<PFN_glBindSampler>(
            dlsym(RTLD_DEFAULT, "glBindSampler"));
    }

    mHookInstalled.store(true, std::memory_order_relaxed);
    return true;
}

void GlFrameInterpolator::uninstallHook() noexcept {
    mSwapHook.reset();
    mHookInstalled.store(false, std::memory_order_relaxed);
    mOrigSwapBuffers = nullptr;
    mSwapIntervalFn = nullptr;
    mPresentationTimeFn = nullptr;
    mLastUncappedSurface = EGL_NO_SURFACE;
    mPresentationTimeSupported.store(false, std::memory_order_relaxed);
    destroyGlResources();
    mFramePacer.reset();
}

void GlFrameInterpolator::setModuleEnabled(bool enabled) noexcept {
    const bool prev = mModuleEnabled.exchange(enabled, std::memory_order_relaxed);
    if (prev != enabled) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
}

void GlFrameInterpolator::updateConfig(const FrameGenConfig &config) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    FrameGenConfig normalized = config;
    normalized.normalize();
    if (normalized.frameGenMode != mConfig.frameGenMode ||
        normalized.extraPresentedFrames != mConfig.extraPresentedFrames) {
        // Presentation topology changed (synthesis on/off, own presented frames
        // vs. composited): restart temporal history instead of interpolating
        // across the discontinuity. Cadence-only changes are handled by the
        // pacer re-anchoring its grid, keeping history intact.
        mResetRequested.store(true, std::memory_order_relaxed);
    }
    mConfig = normalized;
    mTrailFreeGuardEnabled.store(normalized.trailFreeGuard,
                                 std::memory_order_relaxed);
}

FrameGenConfig GlFrameInterpolator::currentConfig() const noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    return mConfig;
}

void GlFrameInterpolator::setPostFrameCallback(
    PostFrameCallback callback) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mPostFrameCallback = std::move(callback);
}

RuntimeTelemetrySnapshot GlFrameInterpolator::telemetrySnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mTelemetryMutex);
    return mTelemetry;
}

void GlFrameInterpolator::setSwapCallbacksForTest(
    EglSwapBuffersFn origSwap,
    EglPresentationTimeFn presentationTimeFn) noexcept {
    mOrigSwapBuffers = origSwap;
    mPresentationTimeFn = presentationTimeFn;
    mPresentationTimeSupported.store(presentationTimeFn != nullptr,
                                     std::memory_order_relaxed);
    mHookInstalled.store(origSwap != nullptr, std::memory_order_relaxed);
}

void GlFrameInterpolator::setInbuiltFpsCounterForTest(
    std::atomic<int> *frameCountPtr,
    std::atomic<int> *currentFpsPtr) noexcept {
    mInbuiltFpsFrameCount = frameCountPtr;
    mInbuiltCurrentFps = currentFpsPtr;
    mInbuiltFpsResolveCooldown = (frameCountPtr != nullptr) ? 1000000U : 0U;
}

std::atomic<int> *GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(
    const void *nativeGetFpsAddr) noexcept {
    if (nativeGetFpsAddr == nullptr) {
        return nullptr;
    }
    const auto *ins = static_cast<const std::uint32_t *>(nativeGetFpsAddr);
    const std::uint32_t ins0 = ins[0];
    const std::uint32_t ins1 = ins[1];
    const std::uint32_t ins2 = ins[2];

    // Match LeviLaunchroid libinbuiltmods.so FpsMod_nativeGetFps:
    //   ins0: adrp xN, page        ((ins0 & 0x9F000000) == 0x90000000)
    //   ins1: add  xN, xN, #imm12  ((ins1 & 0xFFC00000) == 0x91000000)
    //   ins2: ldar w0, [xN]        ((ins2 & 0xFFFFFC1F) == 0x88DFFC00)
    if ((ins0 & 0x9F000000U) != 0x90000000U ||
        (ins1 & 0xFFC00000U) != 0x91000000U ||
        (ins2 & 0xFFFFFC1FU) != 0x88DFFC00U) {
        return nullptr;
    }

    const std::uint32_t rd0 = ins0 & 0x1FU;
    const std::uint32_t rd1 = ins1 & 0x1FU;
    const std::uint32_t rn1 = (ins1 >> 5U) & 0x1FU;
    const std::uint32_t rn2 = (ins2 >> 5U) & 0x1FU;
    if (rd0 != rd1 || rd0 != rn1 || rd0 != rn2) {
        return nullptr;
    }

    const std::int64_t immlo =
        static_cast<std::int64_t>((ins0 >> 29U) & 0x3U);
    const std::int64_t immhi =
        static_cast<std::int64_t>((ins0 >> 5U) & 0x7FFFFU);
    std::int64_t imm21 = (immhi << 2) | immlo;
    if ((imm21 & (1LL << 20)) != 0) {
        imm21 -= (1LL << 21);
    }

    const std::uintptr_t pcPage =
        reinterpret_cast<std::uintptr_t>(nativeGetFpsAddr) &
        ~static_cast<std::uintptr_t>(0xFFFU);
    const std::uintptr_t targetPage = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(pcPage) + (imm21 << 12));
    const std::uintptr_t imm12 =
        static_cast<std::uintptr_t>((ins1 >> 10U) & 0xFFFU);

    // sCurrentFps is at targetPage + imm12 (0x178ac);
    // sFrameCount is the preceding 32-bit atomic int at targetPage + imm12 - 4 (0x178a8).
    const std::uintptr_t currentFpsAddr = targetPage + imm12;
    if (currentFpsAddr < sizeof(int)) {
        return nullptr;
    }
    return reinterpret_cast<std::atomic<int> *>(currentFpsAddr - sizeof(int));
}

void GlFrameInterpolator::tryResolveInbuiltFpsCounter() noexcept {
    if (mInbuiltFpsFrameCount != nullptr) {
        return;
    }
    if (mInbuiltFpsResolveCooldown > 0) {
        --mInbuiltFpsResolveCooldown;
        return;
    }
#if defined(__ANDROID__)
    const void *sym = nullptr;

    // Strategy 1: Walk loaded ELF segments via dl_iterate_phdr (bypasses Android
    // classloader linker namespace isolation between cache/native_mods and base.apk).
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, size_t /*size*/, void *data) -> int {
            if (info == nullptr || info->dlpi_name == nullptr ||
                std::strstr(info->dlpi_name, "libinbuiltmods.so") == nullptr) {
                return 0;
            }
            const auto **outSym = static_cast<const void **>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto &phdr = info->dlpi_phdr[i];
                if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 ||
                    phdr.p_filesz < 16) {
                    continue;
                }
                const auto segStart =
                    static_cast<std::uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
                const auto segEnd = segStart + phdr.p_filesz - 16;
                for (std::uintptr_t pc = (segStart + 3U) & ~static_cast<std::uintptr_t>(3U);
                     pc <= segEnd; pc += 4U) {
                    const auto *ins = reinterpret_cast<const std::uint32_t *>(pc);
                    // Fast filter for ldar w0, [xN] followed by ret (0xd65f03c0)
                    if ((ins[2] & 0xFFFFFC1FU) == 0x88DFFC00U &&
                        ins[3] == 0xD65F03C0U &&
                        GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(ins) !=
                            nullptr) {
                        *outSym = ins;
                        return 1;
                    }
                }
            }
            return 0;
        },
        &sym);

    // Strategy 2: Use preloader's /proc/self/maps signature scanner
    if (sym == nullptr) {
        std::uintptr_t sigAddr = pl::memory::resolveSignature(
            "68 00 00 B0 08 B1 22 91 00 FD DF 88 C0 03 5F D6",
            "libinbuiltmods.so");
        if (sigAddr == 0) {
            sigAddr = pl::memory::resolveSignature(
                "? ? ? B0 ? ? ? 91 00 FD DF 88 C0 03 5F D6",
                "libinbuiltmods.so");
        }
        if (sigAddr != 0) {
            sym = reinterpret_cast<const void *>(sigAddr);
        }
    }

    // Strategy 3: Fallback to dlsym / dlopen
    if (sym == nullptr) {
        sym = dlsym(
            RTLD_DEFAULT,
            "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
    }
    if (sym == nullptr) {
        if (void *handle = dlopen("libinbuiltmods.so", RTLD_LAZY | RTLD_NOLOAD)) {
            sym = dlsym(
                handle,
                "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
        }
    }

    if (sym != nullptr) {
        mInbuiltFpsFrameCount = decodeArm64InbuiltFpsFrameCount(sym);
        if (mInbuiltFpsFrameCount != nullptr) {
            mInbuiltCurrentFps = mInbuiltFpsFrameCount + 1;
        }
    }

    if (mInbuiltFpsFrameCount == nullptr) {
        // Retry in ~30 frames if libinbuiltmods.so has not been initialized yet
        mInbuiltFpsResolveCooldown = 30U;
    }
#else
    mInbuiltFpsResolveCooldown = 1000000U;
#endif
}

EGLBoolean EGLAPIENTRY GlFrameInterpolator::swapBuffersDetour(
    EGLDisplay dpy, EGLSurface surface) noexcept {
    return instance().onSwapBuffers(dpy, surface);
}

void GlFrameInterpolator::resetTemporalState() noexcept {
    mHasFrameHistory = false;
    mLastPresentedTimestampNs = 0;
    mLowFpsStreak = 0;
    mAdaptiveSuspendRemaining = 0;
}

bool GlFrameInterpolator::compileSynthesisProgram() noexcept {
    clearGlErrors();
    const GLuint vs =
        compileGlShader(GL_VERTEX_SHADER, kSynthesisVertexShaderSrc);
    if (vs == 0) {
        return false;
    }
    const GLuint fs =
        compileGlShader(GL_FRAGMENT_SHADER, kSynthesisFragmentShaderSrc);
    if (fs == 0) {
        glDeleteShader(vs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(prog);
        return false;
    }

    mSynthesisProgram = prog;
    mAttrPos = glGetAttribLocation(prog, "aPos");
    mAttrUV = glGetAttribLocation(prog, "aUV");
    mUniPrevTex = glGetUniformLocation(prog, "uPrevTex");
    mUniCurTex = glGetUniformLocation(prog, "uCurTex");
    mUniMotionVec = glGetUniformLocation(prog, "uMotionVec");
    mUniPrevMotionVec = glGetUniformLocation(prog, "uPrevMotionVec");
    mUniTexelSize = glGetUniformLocation(prog, "uTexelSize");
    mUniTanHalfFov = glGetUniformLocation(prog, "uTanHalfFov");
    mUniMotionConfidence = glGetUniformLocation(prog, "uMotionConfidence");
    mUniPhase = glGetUniformLocation(prog, "uPhase");
    mUniBlendWeight = glGetUniformLocation(prog, "uBlendWeight");
    mUniHudProtection = glGetUniformLocation(prog, "uHudProtection");
    mUniTrailFreeGuard = glGetUniformLocation(prog, "uTrailFreeGuard");
    mUniOpaque = glGetUniformLocation(prog, "uOpaque");
    return true;
}

bool GlFrameInterpolator::compileBlitProgram() noexcept {
    clearGlErrors();
    const GLuint vs = compileGlShader(GL_VERTEX_SHADER, kBlitVertexShaderSrc);
    if (vs == 0) return false;
    const GLuint fs = compileGlShader(GL_FRAGMENT_SHADER, kBlitFragmentShaderSrc);
    if (fs == 0) { glDeleteShader(vs); return false; }
    const GLuint prog = glCreateProgram();
    if (prog == 0) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) { glDeleteProgram(prog); return false; }
    mBlitProgram = prog;
    mBlitAttrPos = glGetAttribLocation(prog, "aPos");
    mBlitAttrUV = glGetAttribLocation(prog, "aUV");
    mBlitUniTex = glGetUniformLocation(prog, "uTex");
    return true;
}

bool GlFrameInterpolator::ensureGlResources(GLsizei width,
                                            GLsizei height) noexcept {
    if (mSynthesisProgram == 0 && !compileSynthesisProgram()) {
        return false;
    }
    if (mBlitProgram == 0 && !compileBlitProgram()) {
        // blit is optional, synthesis still works
    }

    if (mQuadVbo == 0) {
        glGenBuffers(1, &mQuadVbo);
        if (mQuadVbo != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuadVertices),
                         kFullscreenQuadVertices, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    if (mGlInitialized && mSurfaceWidth == width && mSurfaceHeight == height &&
        mTexPrev != 0 && mTexCur != 0) {
        return true;
    }

    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }

    mTexPrev = createTexture2D(width, height);
    mTexCur = createTexture2D(width, height);
    if (mTexPrev == 0 || mTexCur == 0) {
        if (mTexPrev != 0) {
            glDeleteTextures(1, &mTexPrev);
            mTexPrev = 0;
        }
        if (mTexCur != 0) {
            glDeleteTextures(1, &mTexCur);
            mTexCur = 0;
        }
        mGlInitialized = false;
        return false;
    }

    mSurfaceWidth = width;
    mSurfaceHeight = height;
    mGlInitialized = true;
    resetTemporalState();
    return true;
}

void GlFrameInterpolator::destroyGlResources() noexcept {
    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }
    if (mSynthesisProgram != 0) {
        glDeleteProgram(mSynthesisProgram);
        mSynthesisProgram = 0;
    }
    if (mBlitProgram != 0) {
        glDeleteProgram(mBlitProgram);
        mBlitProgram = 0;
    }
    if (mQuadVbo != 0) {
        glDeleteBuffers(1, &mQuadVbo);
        mQuadVbo = 0;
    }
    mGlInitialized = false;
    mSurfaceWidth = 0;
    mSurfaceHeight = 0;
    resetTemporalState();
}

void GlFrameInterpolator::captureBackbufferToTexture(GLuint targetTex) noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targetTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, mSurfaceWidth,
                        mSurfaceHeight);
}

void GlFrameInterpolator::drawSynthesizedQuad(
    const FrameMotionSample &motion, float phase, float blendWeight,
    float hudProtection, bool opaqueOutput) noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, mSurfaceWidth, mSurfaceHeight);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Two presentation modes:
    //  * Opaque (REAL -> GENERATED -> REAL): the pass writes every pixel of its
    //    own presented frame, so the back buffer is fully defined after the swap.
    //    Pixels the synthesis cannot trust keep the exact 1:1 real pixel.
    //  * Alpha-composited fallback into the live backbuffer: static HUD /
    //    crosshair / hotbar pixels (alpha = 0.0) stay 100% untouched and empty
    //    texture readbacks (0,0,0) can never dim the screen.
    if (!opaqueOutput && blendWeight > 0.001f) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ZERO, GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }

    glUseProgram(mSynthesisProgram);

    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(0);
    }

    glActiveTexture(GL_TEXTURE0);
    if (gBindSamplerFn != nullptr) {
        gBindSamplerFn(0, 0);
    }
    glBindTexture(GL_TEXTURE_2D, mTexPrev);
    if (mUniPrevTex >= 0) {
        glUniform1i(mUniPrevTex, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    if (gBindSamplerFn != nullptr) {
        gBindSamplerFn(1, 0);
    }
    glBindTexture(GL_TEXTURE_2D, mTexCur);
    if (mUniCurTex >= 0) {
        glUniform1i(mUniCurTex, 1);
    }

    if (mUniMotionVec >= 0) {
        glUniform2f(mUniMotionVec, motion.mvU, motion.mvV);
    }
    if (mUniPrevMotionVec >= 0) {
        glUniform2f(mUniPrevMotionVec, motion.prevMvU, motion.prevMvV);
    }
    if (mUniTexelSize >= 0) {
        const float invW = 1.0f / static_cast<float>(std::max(1, mSurfaceWidth));
        const float invH = 1.0f / static_cast<float>(std::max(1, mSurfaceHeight));
        glUniform2f(mUniTexelSize, invW, invH);
    }
    if (mUniTanHalfFov >= 0) {
        glUniform2f(mUniTanHalfFov, motion.tanHalfFovH, motion.tanHalfFovV);
    }
    if (mUniMotionConfidence >= 0) {
        glUniform1f(mUniMotionConfidence, motion.motionConfidence);
    }
    if (mUniPhase >= 0) {
        glUniform1f(mUniPhase, phase);
    }
    if (mUniBlendWeight >= 0) {
        glUniform1f(mUniBlendWeight, blendWeight);
    }
    if (mUniHudProtection >= 0) {
        glUniform1f(mUniHudProtection, hudProtection);
    }
    if (mUniTrailFreeGuard >= 0) {
        glUniform1f(mUniTrailFreeGuard,
                    mTrailFreeGuardEnabled.load(std::memory_order_relaxed) ? 1.0f
                                                                          : 0.0f);
    }
    if (mUniOpaque >= 0) {
        glUniform1f(mUniOpaque, opaqueOutput ? 1.0f : 0.0f);
    }

    const GLuint posLoc =
        (mAttrPos >= 0) ? static_cast<GLuint>(mAttrPos) : 0U;
    const GLuint uvLoc =
        (mAttrUV >= 0) ? static_cast<GLuint>(mAttrUV) : 1U;

    if (mQuadVbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices);
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices + 2);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
}

void GlFrameInterpolator::drawRealCopy() noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, mSurfaceWidth, mSurfaceHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    if (mBlitProgram == 0) {
        FrameMotionSample dummy{};
        drawSynthesizedQuad(dummy, 1.0f, 0.0f, 1.0f, true);
        return;
    }
    glUseProgram(mBlitProgram);
    if (gBindVertexArrayFn != nullptr) gBindVertexArrayFn(0);
    glUniform1f(13, 1.0f);
    glUniform1f(14, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    if (gBindSamplerFn != nullptr) gBindSamplerFn(0, 0);
    glBindTexture(GL_TEXTURE_2D, mTexCur);
    if (mBlitUniTex >= 0) glUniform1i(mBlitUniTex, 0);
    if (gBindSamplerFn != nullptr) gBindSamplerFn(1, 0);
    const GLuint posLoc = (mBlitAttrPos >= 0) ? static_cast<GLuint>(mBlitAttrPos) : 0U;
    const GLuint uvLoc = (mBlitAttrUV >= 0) ? static_cast<GLuint>(mBlitAttrUV) : 1U;
    if (mQuadVbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), reinterpret_cast<const void*>(0));
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), reinterpret_cast<const void*>(2*sizeof(float)));
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), kFullscreenQuadVertices);
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), kFullscreenQuadVertices+2);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
}

EGLBoolean GlFrameInterpolator::onSwapBuffers(EGLDisplay dpy,
                                              EGLSurface surface) noexcept {
    if (mOrigSwapBuffers == nullptr) {
        return EGL_FALSE;
    }

    // 1. Validate active context & window backbuffer surface (matching LeviLaunchroid FpsMod)
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint renderBuffer = 0;
    if (eglQuerySurface(dpy, surface, kEglRenderBufferAttr, &renderBuffer) ==
            EGL_TRUE &&
        renderBuffer != 0 && renderBuffer != kEglBackBufferValue) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width < 16 || height < 16) {
        return mOrigSwapBuffers(dpy, surface);
    }

    FrameGenConfig cfg;
    PostFrameCallback postCallback;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        cfg = mConfig;
        postCallback = mPostFrameCallback;
    }

    const bool modEnabled = mModuleEnabled.load(std::memory_order_relaxed);
    if (mResetRequested.exchange(false, std::memory_order_relaxed)) {
        resetTemporalState();
    }

    const bool fgRequested =
        modEnabled && (cfg.frameGenMode != FrameGenMode::Off) &&
        (cfg.blendStrength > 0.005f) && (mConsecutiveGlErrors < 6);
    const int requestedMultiplier =
        fgRequested ? std::clamp(cfg.frameGenMultiplier, 2, 4) : 1;
    const bool extraPresented =
        fgRequested && cfg.extraPresentedFrames &&
        !mExtraSwapUnsafe.load(std::memory_order_relaxed);

    // ---------------------------------------------------------------------------
    // Presentation cadence resolution.
    //
    // The configured FPS cap applies to the FINAL PRESENTED FPS (real + generated
    // frames) and is never bypassed by frame generation:
    //   FG OFF + cap 90                    -> <= 90 presented FPS (90 real)
    //   FG ON  + cap 90, multiplier 2      -> <= 90 presented FPS (45 real + 45 generated)
    //   FG ON  + cap 120, multiplier 2     -> <= 120 presented FPS (60 real + 60 generated)
    //
    // REAL frame slots are throttled by this pacer; generated frames then fill
    // the intervals between real frames (REAL -> GENERATED -> REAL -> GENERATED).
    // NOTE: this is the only place the module ever waits. Keyboard/mouse input and
    // the camera smoothing hooks never enter the pacer, so capping never adds
    // input latency: the wait happens on the render/present path only, after the
    // game has already rendered the real frame.
    // ---------------------------------------------------------------------------
    const FrameCadence cadence = FramePacer::resolveCadence(
        cfg.targetFps, cfg.frameGenOutputFps, fgRequested, requestedMultiplier,
        extraPresented);

    const FramePacingDecision realSlot = mFramePacer.paceSlot(
        cadence, PresentSlotKind::RealFrame, 0, steadyNowNs());
    std::int64_t nowNs = realSlot.wakeNs;

    // Presentation-time emitter: hands SurfaceFlinger the exact monotonic deadline
    // of each slot on the presentation grid so REAL and GENERATED frames stay
    // evenly spaced (EGL_ANDROID_presentation_time, when supported).
    const std::int64_t uncappedSlotIntervalNs = std::max<std::int64_t>(
        1'000'000LL,
        mFramePacer.metricsSnapshot().emaFrameIntervalNs /
            static_cast<std::int64_t>(std::max(1, cadence.effectiveMultiplier())));
    auto emitPresentationTime = [&](int slotIndexInCycle,
                                    std::int64_t pacedDeadlineNs) noexcept {
        if (mPresentationTimeFn == nullptr) {
            return;
        }
        std::int64_t targetNs = 0;
        if (cadence.capActive) {
            targetNs = pacedDeadlineNs;
        } else {
            const std::int64_t baseNs = std::max<std::int64_t>(
                steadyNowNs(), mLastPresentedTimestampNs + 1'000'000LL);
            targetNs = baseNs + uncappedSlotIntervalNs *
                                   static_cast<std::int64_t>(slotIndexInCycle);
        }
        if (targetNs <= mLastPresentedTimestampNs) {
            targetNs = mLastPresentedTimestampNs + 1'000'000LL;
        }
        if (mPresentationTimeFn(dpy, surface, targetNs) == EGL_TRUE) {
            mPresentationTimeSupported.store(true, std::memory_order_relaxed);
        }
        mLastPresentedTimestampNs = targetNs;
    };

    const float aspect =
        static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const FrameMotionSample motion =
        CameraSmoother::instance().consumeFrameMotion(aspect, nowNs);

    const bool cameraMoving = (std::abs(motion.deltaPitchDeg) > 1e-3f) ||
                              (std::abs(motion.deltaYawDeg) > 1e-3f);
    if (cameraMoving) {
        mStationaryFrameStreak = 0;
    } else {
        ++mStationaryFrameStreak;
    }

#if !defined(__ANDROID__)
    // Host-simulation path for GPU command queue flush
    if (modEnabled && cfg.hudOptimizerMode != HudOptimizerMode::Off &&
        cfg.hudLatencyFlush) {
        if (cfg.hudOptimizerMode == HudOptimizerMode::UltraLowLatency ||
            cameraMoving) {
            glFlush();
        }
    }
#endif

#if defined(__ANDROID__)
    // Frame generation owns the presentation clock: EGL VSync is released only
    // while the module is actually presenting real + generated frames, otherwise
    // Android's double-buffer VSync stalls would quantize our cadence to the
    // panel rate (e.g. a 45 real FPS target collapsing onto 30/60 FPS). The FPS
    // cap then applies to the whole pipeline. VSync is restored as soon as frame
    // generation is off so the renderer can never run uncapped by accident.
    if (mSwapIntervalFn != nullptr) {
        if (fgRequested && mLastUncappedSurface != surface) {
            mSwapIntervalFn(dpy, 0);
            mLastUncappedSurface = surface;
        } else if (!fgRequested && mLastUncappedSurface == surface) {
            mSwapIntervalFn(dpy, 1);
            mLastUncappedSurface = EGL_NO_SURFACE;
        }
    }
#endif

    int synthesizedThisSwap = 0;   // generated frames actually presented this cycle
    int activeMultiplier = 1;
    bool realFramePresented = false;
    EGLBoolean swapResult = EGL_TRUE;

    if (!fgRequested) {
        // ---------------------------------------------------------------------
        // Frame generation OFF: the cap still limits the presented FPS.
        // ---------------------------------------------------------------------
        resetTemporalState();
        emitPresentationTime(0, realSlot.targetPresentNs);
        swapResult = mOrigSwapBuffers(dpy, surface);
        realFramePresented = (swapResult == EGL_TRUE);
        if (realFramePresented) {
            mConsecutiveGlErrors = 0;
            mFramePacer.recordPresented(steadyNowNs(),
                                        PresentSlotKind::RealFrame);
        } else {
            ++mConsecutiveGlErrors;
        }
    } else {
        const FramePacingMetrics prevPacing = mFramePacer.metricsSnapshot();
        const bool isSceneCut =
            motion.sceneCut || prevPacing.discontinuityDetected;
        if (isSceneCut) {
            ++mSceneCutCount;
        }

        const int targetMult = cadence.effectiveMultiplier();
        const float strengthScale =
            (targetMult >= 4) ? 0.84f : ((targetMult == 3) ? 0.92f : 1.0f);

        if (!mHasFrameHistory || isSceneCut) {
            // Scene cut / first frame: re-seed ground truth, never interpolate
            // across a discontinuity (prevents any cross-cut ghosting).
            if (mConsecutiveGlErrors < 3) {
                GlStateGuard stateGuard;
                if (ensureGlResources(width, height)) {
                    captureBackbufferToTexture(mTexCur);
                    std::swap(mTexPrev, mTexCur);
                }
            }
            mHasFrameHistory = true;
            emitPresentationTime(0, realSlot.targetPresentNs);
            swapResult = mOrigSwapBuffers(dpy, surface);
            realFramePresented = (swapResult == EGL_TRUE);
            if (realFramePresented) {
                mFramePacer.recordPresented(steadyNowNs(),
                                            PresentSlotKind::RealFrame);
            }
        } else if (cadence.extraPresentedFrames && mConsecutiveGlErrors < 3) {
            // -----------------------------------------------------------------
            // GENERATED-before-REAL presentation pipeline (low-latency, chronologically
            // correct): A -> A.5 -> B -> B.5 -> C ...
            // After rendering REAL B, we have A(prev) and B(cur). The midpoint
            // A.5 lies temporally BEFORE B, so we present it first, then B.
            // Every presentation gets its own eglSwapBuffers + eglPresentationTime
            // deadline, so frames fill the cap grid evenly. Real is presented
            // via a blit (drawRealCopy) after the generated frames so the
            // original backbuffer isn't lost.
            // Fill-to-cap: real runs at its natural rate; cap only limits max.
            // When late (low FPS) we skip the wait and re-anchor, so 25 natural
            // -> 25+25=50 feels like 50 without throttle.
            // -----------------------------------------------------------------
            GlStateGuard stateGuard;
            if (ensureGlResources(width, height)) {
                // Capture ground truth (REAL B) BEFORE any swap.
                captureBackbufferToTexture(mTexCur);

                int extraAllowed = std::max(0, realSlot.generatedFramesAllowed);
                {
                    const auto snapGen = mFramePacer.metricsSnapshot();
                    if (cadence.capActive && snapGen.realFps > 1.0f &&
                        snapGen.realFps >= static_cast<float>(cadence.presentedTargetFps) * 0.82f) {
                        extraAllowed = 0;
                    }
                }
                if (mAdaptiveSuspendRemaining > 0) {
                    extraAllowed = 0;
                    --mAdaptiveSuspendRemaining;
                }

                // Low-FPS streak tracking for adaptive suspend: if we keep missing
                // deadlines at low FPS, briefly suspend generation to let real recover
                // (prevents queue buildup, not a throttle when below cap).
                {
                    const auto snap = mFramePacer.metricsSnapshot();
                    const bool lowFps = snap.realFps > 1.0f && snap.realFps < 38.0f;
                    if (lowFps && snap.pacingDegraded) {
                        ++mLowFpsStreak;
                    } else if (!snap.pacingDegraded) {
                        if (mLowFpsStreak > 0) --mLowFpsStreak;
                    }
                    if (mLowFpsStreak > 8) {
                        mAdaptiveSuspendRemaining = 3;
                        mLowFpsStreak = 0;
                    }
                }

                // Pre-compute deadlines for the whole cycle. realSlot is the earliest
                // slot; generated slots follow. For GEN-before-REAL we map:
                //   gen1 -> slot[0], gen2 -> slot[1], ..., real -> slot[extra]
                std::int64_t slotDeadlines[4]{};
                int slotCount = 1;
                slotDeadlines[0] = realSlot.targetPresentNs;
                FramePacingDecision genSlots[3]{};
                int genSlotCount = 0;
                for (int k = 1; k <= extraAllowed && genSlotCount < 3; ++k) {
                    const FramePacingDecision genSlot = mFramePacer.paceSlot(
                        cadence, PresentSlotKind::GeneratedFrame, k,
                        steadyNowNs());
                    if (genSlot.cycleDegraded && k > 1) {
                        break;
                    }
                    genSlots[genSlotCount++] = genSlot;
                    slotDeadlines[slotCount++] = genSlot.targetPresentNs;
                }
                extraAllowed = genSlotCount;

                if (extraAllowed == 0) {
                    // No generated frames this cycle: present REAL directly.
                    emitPresentationTime(0, slotDeadlines[0]);
                    swapResult = mOrigSwapBuffers(dpy, surface);
                    realFramePresented = (swapResult == EGL_TRUE);
                    if (realFramePresented) {
                        mFramePacer.recordPresented(steadyNowNs(),
                                                    PresentSlotKind::RealFrame);
                        std::swap(mTexPrev, mTexCur);
                        mConsecutiveGlErrors = 0;
                        activeMultiplier = 1;
                    } else {
                        ++mConsecutiveGlErrors;
                        resetTemporalState();
                    }
                } else {
                    bool genSwapFailed = false;
                    // 1) Present GENERATED frames first (chronological order)
                    for (int k = 1; k <= extraAllowed; ++k) {
                        const float phase =
                            static_cast<float>(k) / static_cast<float>(targetMult);
                        // Map k-th generated to slot[k-1] (earliest first)
                        emitPresentationTime(k - 1, slotDeadlines[k - 1]);
                        drawSynthesizedQuad(motion, phase,
                                            cfg.blendStrength * strengthScale,
                                            cfg.hudProtection,
                                            /*opaqueOutput=*/true);
                        if (mOrigSwapBuffers(dpy, surface) != EGL_TRUE) {
                            mExtraSwapUnsafe.store(true, std::memory_order_relaxed);
                            ++mConsecutiveGlErrors;
                            genSwapFailed = true;
                            break;
                        }
                        mFramePacer.recordPresented(
                            steadyNowNs(), PresentSlotKind::GeneratedFrame);
                        ++synthesizedThisSwap;
                    }
                    if (genSwapFailed) {
                        // Device refused an extra frame: we have already presented
                        // some gens; still need to present the REAL. Try to present
                        // it via blit as best effort, otherwise reset.
                        emitPresentationTime(extraAllowed, slotDeadlines[extraAllowed]);
                        drawRealCopy();
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        realFramePresented = (swapResult == EGL_TRUE);
                        if (realFramePresented) {
                            mFramePacer.recordPresented(steadyNowNs(),
                                                        PresentSlotKind::RealFrame);
                            std::swap(mTexPrev, mTexCur);
                            mConsecutiveGlErrors = 0;
                            activeMultiplier = synthesizedThisSwap + 1;
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                        }
                    } else {
                        // 2) Present REAL B via blit at the last slot deadline
                        emitPresentationTime(extraAllowed, slotDeadlines[extraAllowed]);
                        drawRealCopy();
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        realFramePresented = (swapResult == EGL_TRUE);
                        if (realFramePresented) {
                            mFramePacer.recordPresented(steadyNowNs(),
                                                        PresentSlotKind::RealFrame);
                            std::swap(mTexPrev, mTexCur);
                            mConsecutiveGlErrors = 0;
                            activeMultiplier = synthesizedThisSwap + 1;
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                        }
                    }
                }
            } else {
                ++mConsecutiveGlErrors;
                resetTemporalState();
                swapResult = mOrigSwapBuffers(dpy, surface);
                realFramePresented = (swapResult == EGL_TRUE);
            }
        } else {
            // -----------------------------------------------------------------
            // Fallback: alpha-composited synthesis into the single real swap
            // (devices that cannot take extra presented frames per game frame).
            // -----------------------------------------------------------------
            GlStateGuard stateGuard;
            if (!ensureGlResources(width, height)) {
                ++mConsecutiveGlErrors;
                resetTemporalState();
                swapResult = mOrigSwapBuffers(dpy, surface);
                realFramePresented = (swapResult == EGL_TRUE);
            } else {
                captureBackbufferToTexture(mTexCur);
                const float effectiveBlend =
                    prevPacing.autoTuneReduced
                        ? (cfg.blendStrength * 0.55f)
                        : (cfg.blendStrength * strengthScale);
                drawSynthesizedQuad(motion, 0.5f, effectiveBlend,
                                    cfg.hudProtection, /*opaqueOutput=*/false);
                emitPresentationTime(0, realSlot.targetPresentNs);
                swapResult = mOrigSwapBuffers(dpy, surface);
                if (swapResult == EGL_TRUE) {
                    realFramePresented = true;
                    std::swap(mTexPrev, mTexCur);
                    synthesizedThisSwap = targetMult - 1;
                    activeMultiplier = targetMult;
                    mConsecutiveGlErrors = 0;
                    mFramePacer.recordPresented(steadyNowNs(),
                                                PresentSlotKind::RealFrame);
                } else {
                    ++mConsecutiveGlErrors;
                    resetTemporalState();
                }
            }
        }
    }

    // Honest FPS: do NOT inflate the vanilla FpsMod counter. 300 now means
    // 300 real game frames, so it feels like 300. Generated frames are
    // counted separately in telemetry (Real / Generated / Presented) but
    // never added to sFrameCount, otherwise 45 real +45 gen =90 would show
    // as 90 and feel like 45, or 150+150=300 would feel like 150.
    (void)synthesizedThisSwap;

    const bool embeddedSynthesis =
        realFramePresented && cadence.frameGenEnabled &&
        !cadence.extraPresentedFrames;
    const FramePacingMetrics pacing = mFramePacer.endCycle(
        cadence, steadyNowNs(), realFramePresented ? 1 : 0, synthesizedThisSwap,
        cfg.autoTune, embeddedSynthesis);

    // Honest FPS: report REAL fps to FpsMod, not presented. The overlay now
    // shows what you feel. Generated/presented are still visible in the
    // LeviFrameGen HUD badge for debugging, but the vanilla counter is honest.
    if (realFramePresented && mInbuiltCurrentFps != nullptr &&
        pacing.realFps >= 1.0f) {
        const int honestFps = static_cast<int>(std::lround(pacing.realFps));
        if (honestFps > 0) {
            mInbuiltCurrentFps->store(honestFps, std::memory_order_relaxed);
        }
    }

    RuntimeTelemetrySnapshot snap{};
    snap.moduleEnabled = modEnabled;
    snap.eglHooked = mHookInstalled.load(std::memory_order_relaxed);
    snap.presentationTimeSupported =
        mPresentationTimeSupported.load(std::memory_order_relaxed);
    snap.turnDeltaHooked = CameraSmoother::instance().isTurnDeltaHooked();
    snap.fovHooked = CameraSmoother::instance().isFovHooked();
    snap.perspectiveHooked = CameraSmoother::instance().isPerspectiveHooked();

    snap.realFps = pacing.realFps;
    snap.generatedFps = pacing.generatedFps;
    snap.presentedFps = pacing.presentedFps;
    snap.effectiveFps = pacing.presentedFps;
    snap.onePercentLowFps = pacing.onePercentLowFps;
    snap.frameTimeMs = pacing.frameTimeMs;
    snap.jitterMs = pacing.jitterMs;
    snap.presentedIntervalMs = pacing.presentedIntervalMs;
    snap.deadlineErrorMs = pacing.deadlineErrorMs;

    snap.lastMotionU = motion.mvU;
    snap.lastMotionV = motion.mvV;
    snap.cameraPitchVelDeg = motion.deltaPitchDeg;
    snap.cameraYawVelDeg = motion.deltaYawDeg;
    snap.currentFovDeg = motion.currentFovDeg;
    snap.cameraPerspective = motion.cameraPerspective;

    snap.totalRealFrames = pacing.totalRealFrames;
    snap.totalSynthesizedFrames = pacing.totalSynthesizedFrames;
    snap.totalPresentedFrames = pacing.totalPresentedFrames;
    snap.sceneCutResets = mSceneCutCount;
    snap.activeMultiplier = activeMultiplier;
    snap.generatedPerCycle = pacing.generatedPerCycle;
    snap.presentedTargetFps = pacing.presentedTargetFps;
    snap.realTargetFps = pacing.realTargetFps;
    snap.frameCapActive = pacing.capActive;
    snap.extraPresentedFramesActive = pacing.extraPresentedFramesActive;
    snap.embeddedSynthesis = pacing.embeddedSynthesis;
    snap.capHoldPercent = pacing.capHoldPercent;
    snap.capExceedEvents = pacing.capExceedEvents;
    snap.pacingDegraded = pacing.pacingDegraded;
    snap.autoTuneReduced = pacing.autoTuneReduced;

    {
        std::lock_guard<std::mutex> lock(mTelemetryMutex);
        mTelemetry = snap;
    }

    if (postCallback) {
        postCallback(snap);
    }

    return swapResult;
}

SynthesizedPixelResult GlFrameInterpolator::evaluateSynthesizedPixelCpu(
    float u, float v,
    const FrameMotionSample &motion,
    float phase,
    float blendWeight,
    float hudProtection,
    float texelW, float texelH,
    const CpuTexSamplerFn &samplePrevA,
    const CpuTexSamplerFn &sampleCurB,
    bool opaqueOutput,
    float trailFreeGuard) noexcept {
    SynthesizedPixelResult out{};
    if (!samplePrevA || !sampleCurB) {
        return out;
    }

    auto luma3 = [](float r, float g, float b) -> float {
        return 0.299f * r + 0.587f * g + 0.114f * b;
    };
    auto len3 = [](float dr, float dg, float db) -> float {
        return std::sqrt(dr * dr + dg * dg + db * db);
    };
    auto smoothstepCpu = [](float edge0, float edge1, float x) -> float {
        const float t = std::clamp((x - edge0) / std::max(1e-6f, edge1 - edge0),
                                   0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    float curR = 0.0f, curG = 0.0f, curB = 0.0f;
    float prevR = 0.0f, prevG = 0.0f, prevB = 0.0f;
    sampleCurB(std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f),
               curR, curG, curB);
    samplePrevA(std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f),
                prevR, prevG, prevB);

    if (blendWeight <= 0.001f) {
        out.r = curR;
        out.g = curG;
        out.b = curB;
        out.alpha = 1.0f;
        return out;
    }

    const float p = std::clamp(std::abs(phase), 0.0f, 1.0f);
    const float conf = std::clamp(motion.motionConfidence, 0.15f, 1.0f);
    const float tsX = std::max(texelW, 0.0005f);
    const float tsY = std::max(texelH, 0.0005f);

    // 3D Gnomonic (Perspective-Tangent) Camera Flow
    auto perspFlow = [&](float baseU, float baseV, float &outU, float &outV) {
        const float ndcX = u * 2.0f - 1.0f;
        const float ndcY = v * 2.0f - 1.0f;
        const float tX = ndcX * motion.tanHalfFovH;
        const float tY = ndcY * motion.tanHalfFovV;
        const float radX = std::clamp(1.0f + tX * tX * 0.36f, 0.85f, 1.42f);
        const float radY = std::clamp(1.0f + tY * tY * 0.36f, 0.85f, 1.42f);
        const float cross = std::clamp(
            ndcX * ndcY * motion.tanHalfFovH * motion.tanHalfFovV * 0.14f,
            -0.12f, 0.12f);
        outU = baseU * radX + baseV * cross;
        outV = baseV * radY + baseU * cross;
    };

    float camU = 0.0f, camV = 0.0f;
    float prevCamU = 0.0f, prevCamV = 0.0f;
    perspFlow(motion.mvU, motion.mvV, camU, camV);
    perspFlow(motion.prevMvU, motion.prevMvV, prevCamU, prevCamV);

    // Multi-Tap Static HUD / Crosshair / Hotbar / Text Protection
    const float staticDiffCenter =
        std::abs(luma3(curR, curG, curB) - luma3(prevR, prevG, prevB)) +
        0.30f * len3(curR - prevR, curG - prevG, curB - prevB);
    float curER = 0.0f, curEG = 0.0f, curEB = 0.0f;
    float prevER = 0.0f, prevEG = 0.0f, prevEB = 0.0f;
    sampleCurB(std::clamp(u + tsX * 2.0f, 0.0f, 1.0f),
               std::clamp(v, 0.0f, 1.0f), curER, curEG, curEB);
    samplePrevA(std::clamp(u + tsX * 2.0f, 0.0f, 1.0f),
                std::clamp(v, 0.0f, 1.0f), prevER, prevEG, prevEB);
    const float staticDiffNeigh =
        std::abs(luma3(curER, curEG, curEB) - luma3(prevER, prevEG, prevEB));
    const float staticDiff = 0.65f * staticDiffCenter + 0.35f * staticDiffNeigh;

    float psR = 0.0f, psG = 0.0f, psB = 0.0f;
    float csR = 0.0f, csG = 0.0f, csB = 0.0f;
    samplePrevA(std::clamp(u - p * camU, 0.0f, 1.0f),
                std::clamp(v - p * camV, 0.0f, 1.0f), psR, psG, psB);
    sampleCurB(std::clamp(u + (1.0f - p) * camU, 0.0f, 1.0f),
               std::clamp(v + (1.0f - p) * camV, 0.0f, 1.0f), csR, csG, csB);
    const float motionDiff =
        std::abs(luma3(csR, csG, csB) - luma3(psR, psG, psB)) +
        0.25f * len3(csR - psR, csG - psG, csB - psB);

    const float cdX = (u - 0.5f) * 1.777f;
    const float cdY = (v - 0.5f);
    const float inCrosshairZone =
        1.0f - smoothstepCpu(0.028f, 0.048f, std::hypot(cdX, cdY));
    const float inHotbarZone =
        (v >= 0.86f && std::abs(u - 0.5f) <= 0.38f) ? 1.0f : 0.0f;
    const float inTopBarZone =
        (v < 0.065f && u < 0.35f) ? 1.0f : 0.0f;
    const float uiZoneBoost =
        std::max({inCrosshairZone, inHotbarZone, inTopBarZone});

    const float staticUiLock =
        (1.0f - smoothstepCpu(0.003f, 0.018f, staticDiff)) * uiZoneBoost;
    const float dynamicHudConf =
        std::clamp((motionDiff - staticDiff * 2.2f) * 6.0f, 0.0f, 1.0f);
    const float hudMask = std::clamp(
        std::max(dynamicHudConf, staticUiLock) * std::clamp(hudProtection, 0.0f, 1.0f),
        0.0f, 1.0f);
    out.hudMask = hudMask;

    // Static / sub-pixel motion passthrough: with essentially no camera motion
    // the real pixel already is the correct answer, so never resample it.
    const float mvPixels = std::hypot(motion.mvU / tsX, motion.mvV / tsY);
    const float staticGate = 1.0f - smoothstepCpu(0.20f, 0.70f, mvPixels);
    out.staticGate = staticGate;

    // 9-Candidate Local Motion & Depth-Parallax Refinement
    float bestU = camU;
    float bestV = camV;
    float bestCost = 1000.0f;

    auto evalCand = [&](float candU, float candV, float penalty) {
        const float uA = std::clamp(u - p * candU, 0.0f, 1.0f);
        const float vA = std::clamp(v - p * candV, 0.0f, 1.0f);
        const float uB = std::clamp(u + (1.0f - p) * candU, 0.0f, 1.0f);
        const float vB = std::clamp(v + (1.0f - p) * candV, 0.0f, 1.0f);
        float aR = 0.0f, aG = 0.0f, aB = 0.0f;
        float bR = 0.0f, bG = 0.0f, bB = 0.0f;
        samplePrevA(uA, vA, aR, aG, aB);
        sampleCurB(uB, vB, bR, bG, bB);
        const float lDiff = std::abs(luma3(aR, aG, aB) - luma3(bR, bG, bB));
        const float cDiff =
            (std::abs(aR - bR) + std::abs(aG - bG) + std::abs(aB - bB)) * 0.3333f;
        const float cost = lDiff * 0.65f + cDiff * 0.35f + penalty;
        if (cost < bestCost) {
            bestCost = cost;
            bestU = candU;
            bestV = candV;
        }
    };

    evalCand(camU, camV, 0.000f);
    evalCand(camU * 1.32f, camV * 1.32f, 0.005f);
    evalCand(camU * 0.58f, camV * 0.58f, 0.005f);
    evalCand(prevCamU, prevCamV, 0.007f);
    evalCand(0.0f, 0.0f, 0.012f);

    const float locRadU =
        std::clamp(std::abs(camU) * 0.35f + tsX * 3.0f, tsX * 2.0f, 0.035f);
    const float locRadV =
        std::clamp(std::abs(camV) * 0.35f + tsY * 3.0f, tsY * 2.0f, 0.035f);
    evalCand(camU + locRadU, camV, 0.009f);
    evalCand(camU - locRadU, camV, 0.009f);
    evalCand(camU, camV + locRadV, 0.009f);
    evalCand(camU, camV - locRadV, 0.009f);

    out.refinedMvU = bestU;
    out.refinedMvV = bestV;

    // Contrast-Adaptive Sub-Texel Sharpening (CAS)
    auto sampleSharp = [&](const CpuTexSamplerFn &sampler, float su, float sv,
                           float &sR, float &sG, float &sB) {
        const float cu = std::clamp(su, 0.0f, 1.0f);
        const float cv = std::clamp(sv, 0.0f, 1.0f);
        float cR = 0.0f, cG = 0.0f, cB = 0.0f;
        float nR = 0.0f, nG = 0.0f, nB = 0.0f;
        float soR = 0.0f, soG = 0.0f, soB = 0.0f;
        float eR = 0.0f, eG = 0.0f, eB = 0.0f;
        float wR = 0.0f, wG = 0.0f, wB = 0.0f;
        sampler(cu, cv, cR, cG, cB);
        sampler(cu, std::clamp(cv + tsY, 0.0f, 1.0f), nR, nG, nB);
        sampler(cu, std::clamp(cv - tsY, 0.0f, 1.0f), soR, soG, soB);
        sampler(std::clamp(cu + tsX, 0.0f, 1.0f), cv, eR, eG, eB);
        sampler(std::clamp(cu - tsX, 0.0f, 1.0f), cv, wR, wG, wB);

        const float minR = std::min({cR, nR, soR, eR, wR});
        const float minG = std::min({cG, nG, soG, eG, wG});
        const float minB = std::min({cB, nB, soB, eB, wB});
        const float maxR = std::max({cR, nR, soR, eR, wR});
        const float maxG = std::max({cG, nG, soG, eG, wG});
        const float maxB = std::max({cB, nB, soB, eB, wB});

        const float avgR = 0.25f * (nR + soR + eR + wR);
        const float avgG = 0.25f * (nG + soG + eG + wG);
        const float avgB = 0.25f * (nB + soB + eB + wB);

        const float fx = (cu / tsX) - std::floor(cu / tsX);
        const float fy = (cv / tsY) - std::floor(cv / tsY);
        const float subpixel =
            std::max(4.0f * fx * (1.0f - fx), 4.0f * fy * (1.0f - fy));
        const float localContrast =
            luma3(maxR - minR, maxG - minG, maxB - minB);
        const float casGain =
            (0.26f + 0.24f * subpixel) *
            (1.0f - smoothstepCpu(0.45f, 0.85f, localContrast));

        sR = std::clamp(cR + (cR - avgR) * casGain, minR, maxR);
        sG = std::clamp(cG + (cG - avgG) * casGain, minG, maxG);
        sB = std::clamp(cB + (cB - avgB) * casGain, minB, maxB);
    };

    const float rawUvAU = u - p * bestU;
    const float rawUvAV = v - p * bestV;
    const float rawUvBU = u + (1.0f - p) * bestU;
    const float rawUvBV = v + (1.0f - p) * bestV;

    const bool inBoundsA =
        (rawUvAU >= 0.0f && rawUvAU <= 1.0f && rawUvAV >= 0.0f && rawUvAV <= 1.0f);
    const bool inBoundsB =
        (rawUvBU >= 0.0f && rawUvBU <= 1.0f && rawUvBV >= 0.0f && rawUvBV <= 1.0f);

    float colAR = 0.0f, colAG = 0.0f, colAB = 0.0f;
    float colBR = 0.0f, colBG = 0.0f, colBB = 0.0f;
    sampleSharp(samplePrevA, rawUvAU, rawUvAV, colAR, colAG, colAB);
    sampleSharp(sampleCurB, rawUvBU, rawUvBV, colBR, colBG, colBB);

    const float diffAB =
        std::abs(luma3(colAR, colAG, colAB) - luma3(colBR, colBG, colBB)) +
        0.35f * len3(colAR - colBR, colAG - colBG, colAB - colBB);
    const float midRefR = prevR + (curR - prevR) * p;
    const float midRefG = prevG + (curG - prevG) * p;
    const float midRefB = prevB + (curB - prevB) * p;

    const float errA =
        std::abs(luma3(colAR, colAG, colAB) - luma3(midRefR, midRefG, midRefB)) +
        0.30f * len3(colAR - midRefR, colAG - midRefG, colAB - midRefB);
    const float errB =
        std::abs(luma3(colBR, colBG, colBB) - luma3(midRefR, midRefG, midRefB)) +
        0.30f * len3(colBR - midRefR, colBG - midRefG, colBB - midRefB);

    const float winnerB =
        smoothstepCpu(-0.035f, 0.035f, (errA - errB) + (p - 0.5f) * 0.02f);
    const float disocclusionGate = smoothstepCpu(0.06f, 0.18f, diffAB);
    float wB = p + (winnerB - p) * disocclusionGate;

    bool borderWarp = false;
    if (!inBoundsA && inBoundsB) {
        wB = 1.0f;
        borderWarp = true;
    } else if (!inBoundsB && inBoundsA) {
        wB = 0.0f;
        borderWarp = true;
    }
    out.occlusionWinnerB = wB;

    // Trail-Free Motion Guard: disagreement with no confident winner means the
    // local warp is unreliable -> keep the razor-sharp real pixel instead of a
    // smeared trail / ghosted double edge.
    float ambiguity =
        disocclusionGate * (1.0f - std::abs(winnerB - 0.5f) * 2.0f);
    if (borderWarp) {
        ambiguity = std::max(ambiguity, 0.55f);
    }
    if (!inBoundsA && !inBoundsB) {
        ambiguity = 1.0f;
    }
    const float trailSuppress =
        std::clamp(ambiguity * std::clamp(trailFreeGuard, 0.0f, 1.0f), 0.0f,
                   1.0f);
    out.trailGuard = trailSuppress;

    const float sliceConfidence =
        1.0f - std::clamp((bestCost - 0.02f) * 1.6f, 0.0f, 0.45f);

    float synthR = colAR + (colBR - colAR) * wB;
    float synthG = colAG + (colBG - colAG) * wB;
    float synthB = colAB + (colBB - colAB) * wB;

    // Zero-Dimming Photometric Guard
    const float curL = luma3(curR, curG, curB);
    const float prevL = luma3(prevR, prevG, prevB);
    const float minSceneL = std::min(curL, prevL);
    float synthL = luma3(synthR, synthG, synthB);
    if (synthL < minSceneL * 0.96f && synthL > 0.001f) {
        const float boost = std::clamp((minSceneL * 0.96f) / synthL, 1.0f, 1.35f);
        synthR = std::clamp(synthR * boost, 0.0f, 1.0f);
        synthG = std::clamp(synthG * boost, 0.0f, 1.0f);
        synthB = std::clamp(synthB * boost, 0.0f, 1.0f);
        synthL = luma3(synthR, synthG, synthB);
    }
    const float validCapture =
        smoothstepCpu(0.004f, 0.018f, minSceneL) *
        smoothstepCpu(0.004f, 0.018f, synthL);

    const float userStrength = std::clamp(blendWeight / 0.65f, 0.0f, 1.0f);
    const float selection = std::clamp(
        userStrength * conf * sliceConfidence * (1.0f - hudMask) * validCapture *
            (1.0f - staticGate) * (1.0f - trailSuppress),
        0.0f, 1.0f);
    out.selection = selection;

    if (opaqueOutput) {
        // Own presented frame: trustworthy warps contribute synthesized midpoint
        // pixels, everything else keeps the exact 1:1 real pixel (no blending).
        out.r = curR + (synthR - curR) * selection;
        out.g = curG + (synthG - curG) * selection;
        out.b = curB + (synthB - curB) * selection;
        out.alpha = 1.0f;
    } else {
        out.r = synthR;
        out.g = synthG;
        out.b = synthB;
        out.alpha = selection;
    }
    return out;
}

} // namespace framegen
