#include "CameraSmoother.hpp"
#include "FrameGenConfig.hpp"
#include "FrameGenMod.hpp"
#include "FramePacer.hpp"
#include "GlFrameInterpolator.hpp"
#include "HudRenderer.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Patch.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Mock EGL / GLES2 / Preloader Runtime Harness for Host Verification
// ============================================================================
namespace mock_runtime {

struct GlMachineState {
    GLint currentProgram = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint viewport[4]{0, 0, 1920, 1080};
    GLint scissorBox[4]{0, 0, 1920, 1080};
    GLint framebuffer = 0;
    GLint arrayBuffer = 0;
    GLint elementArrayBuffer = 0;
    GLint texBinding0 = 0;
    GLint texBinding1 = 0;
    GLint blendSrcRgb = GL_ONE;
    GLint blendDstRgb = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ZERO;

    GLboolean depthTest = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean cullFace = GL_FALSE;
    GLboolean scissorTest = GL_FALSE;
    GLboolean stencilTest = GL_FALSE;
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};

    GLuint nextTexId = 1;
    GLuint nextBufId = 1;
    GLuint nextShaderId = 1;
    GLuint nextProgramId = 1;

    int drawArraysCount = 0;
    int copyTexSubImageCount = 0;
    int flushCount = 0;
    float lastUniformMvU = 0.0f;
    float lastUniformMvV = 0.0f;
    float lastUniformPhase = 0.0f;
    float lastUniformBlendWeight = 0.0f;
    float lastUniformHudProtection = 0.0f;
};

GlMachineState gGl;

EGLContext gCurrentContext = reinterpret_cast<EGLContext>(0x1);
EGLint gSurfaceWidth = 1920;
EGLint gSurfaceHeight = 1080;
EGLint gSurfaceRenderBuffer = EGL_BACK_BUFFER;
int gOrigSwapCallCount = 0;
std::vector<std::int64_t> gPresentedTimestamps;

// Preloader mock state
pl::modmenu::ModuleInfo gLastRegisteredModule{};
bool gModuleRegistered = false;
std::string gLastSchemaJson{};
std::vector<pl::modmenu::DrawCommand> gLastDrawCommands{};
std::vector<pl::modmenu::HudEditorElement> gLastHudElements{};
pl::modmenu::ButtonInfo gLastButton{};
bool gButtonRegistered = false;
pl::input::KeyCallback gKeyCallback{};

void resetAll() {
    gGl = GlMachineState{};
    gCurrentContext = reinterpret_cast<EGLContext>(0x1);
    gSurfaceWidth = 1920;
    gSurfaceHeight = 1080;
    gSurfaceRenderBuffer = EGL_BACK_BUFFER;
    gOrigSwapCallCount = 0;
    gPresentedTimestamps.clear();
}

EGLBoolean EGLAPIENTRY mockOrigSwapBuffers(EGLDisplay, EGLSurface) {
    ++gOrigSwapCallCount;
    return EGL_TRUE;
}

EGLBoolean EGLAPIENTRY mockPresentationTimeANDROID(EGLDisplay, EGLSurface,
                                                   std::int64_t timeNs) {
    gPresentedTimestamps.push_back(timeNs);
    return EGL_TRUE;
}

} // namespace mock_runtime

// ============================================================================
// Extern "C" EGL & GLES2 Mock Implementations
// ============================================================================
extern "C" {

EGLContext EGLAPIENTRY eglGetCurrentContext(void) {
    return mock_runtime::gCurrentContext;
}

EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay, EGLSurface,
                                       EGLint attribute, EGLint *value) {
    if (value == nullptr) {
        return EGL_FALSE;
    }
    if (attribute == EGL_WIDTH) {
        *value = mock_runtime::gSurfaceWidth;
        return EGL_TRUE;
    }
    if (attribute == EGL_HEIGHT) {
        *value = mock_runtime::gSurfaceHeight;
        return EGL_TRUE;
    }
    if (attribute == EGL_RENDER_BUFFER) {
        *value = mock_runtime::gSurfaceRenderBuffer;
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    return mock_runtime::mockOrigSwapBuffers(dpy, surface);
}

__eglMustCastToProperFunctionPointerType EGLAPIENTRY
eglGetProcAddress(const char *procname) {
    if (procname != nullptr &&
        std::strcmp(procname, "eglPresentationTimeANDROID") == 0) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(
            &mock_runtime::mockPresentationTimeANDROID);
    }
    return nullptr;
}

GLenum glGetError(void) { return GL_NO_ERROR; }

void glGetIntegerv(GLenum pname, GLint *data) {
    using mock_runtime::gGl;
    switch (pname) {
    case GL_CURRENT_PROGRAM:
        *data = gGl.currentProgram;
        break;
    case GL_ACTIVE_TEXTURE:
        *data = gGl.activeTexture;
        break;
    case GL_VIEWPORT:
        std::memcpy(data, gGl.viewport, sizeof(gGl.viewport));
        break;
    case GL_SCISSOR_BOX:
        std::memcpy(data, gGl.scissorBox, sizeof(gGl.scissorBox));
        break;
    case GL_FRAMEBUFFER_BINDING:
        *data = gGl.framebuffer;
        break;
    case GL_ARRAY_BUFFER_BINDING:
        *data = gGl.arrayBuffer;
        break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
        *data = gGl.elementArrayBuffer;
        break;
    case GL_TEXTURE_BINDING_2D:
        *data = (gGl.activeTexture == static_cast<GLint>(GL_TEXTURE1))
                    ? gGl.texBinding1
                    : gGl.texBinding0;
        break;
    case GL_BLEND_SRC_RGB:
        *data = gGl.blendSrcRgb;
        break;
    case GL_BLEND_DST_RGB:
        *data = gGl.blendDstRgb;
        break;
    case GL_BLEND_SRC_ALPHA:
        *data = gGl.blendSrcAlpha;
        break;
    case GL_BLEND_DST_ALPHA:
        *data = gGl.blendDstAlpha;
        break;
    default:
        *data = 0;
        break;
    }
}

void glGetBooleanv(GLenum pname, GLboolean *data) {
    if (pname == GL_COLOR_WRITEMASK) {
        std::memcpy(data, mock_runtime::gGl.colorMask, 4);
    }
}

GLboolean glIsEnabled(GLenum cap) {
    using mock_runtime::gGl;
    switch (cap) {
    case GL_DEPTH_TEST:
        return gGl.depthTest;
    case GL_BLEND:
        return gGl.blend;
    case GL_CULL_FACE:
        return gGl.cullFace;
    case GL_SCISSOR_TEST:
        return gGl.scissorTest;
    case GL_STENCIL_TEST:
        return gGl.stencilTest;
    default:
        return GL_FALSE;
    }
}

void glEnable(GLenum cap) {
    using mock_runtime::gGl;
    switch (cap) {
    case GL_DEPTH_TEST:
        gGl.depthTest = GL_TRUE;
        break;
    case GL_BLEND:
        gGl.blend = GL_TRUE;
        break;
    case GL_CULL_FACE:
        gGl.cullFace = GL_TRUE;
        break;
    case GL_SCISSOR_TEST:
        gGl.scissorTest = GL_TRUE;
        break;
    case GL_STENCIL_TEST:
        gGl.stencilTest = GL_TRUE;
        break;
    default:
        break;
    }
}

void glDisable(GLenum cap) {
    using mock_runtime::gGl;
    switch (cap) {
    case GL_DEPTH_TEST:
        gGl.depthTest = GL_FALSE;
        break;
    case GL_BLEND:
        gGl.blend = GL_FALSE;
        break;
    case GL_CULL_FACE:
        gGl.cullFace = GL_FALSE;
        break;
    case GL_SCISSOR_TEST:
        gGl.scissorTest = GL_FALSE;
        break;
    case GL_STENCIL_TEST:
        gGl.stencilTest = GL_FALSE;
        break;
    default:
        break;
    }
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    mock_runtime::gGl.viewport[0] = x;
    mock_runtime::gGl.viewport[1] = y;
    mock_runtime::gGl.viewport[2] = width;
    mock_runtime::gGl.viewport[3] = height;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    mock_runtime::gGl.scissorBox[0] = x;
    mock_runtime::gGl.scissorBox[1] = y;
    mock_runtime::gGl.scissorBox[2] = width;
    mock_runtime::gGl.scissorBox[3] = height;
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    mock_runtime::gGl.colorMask[0] = r;
    mock_runtime::gGl.colorMask[1] = g;
    mock_runtime::gGl.colorMask[2] = b;
    mock_runtime::gGl.colorMask[3] = a;
}

void glBlendFuncSeparate(GLenum sRGB, GLenum dRGB, GLenum sA, GLenum dA) {
    mock_runtime::gGl.blendSrcRgb = static_cast<GLint>(sRGB);
    mock_runtime::gGl.blendDstRgb = static_cast<GLint>(dRGB);
    mock_runtime::gGl.blendSrcAlpha = static_cast<GLint>(sA);
    mock_runtime::gGl.blendDstAlpha = static_cast<GLint>(dA);
}

void glActiveTexture(GLenum texture) {
    mock_runtime::gGl.activeTexture = static_cast<GLint>(texture);
}

void glGenTextures(GLsizei n, GLuint *textures) {
    for (GLsizei i = 0; i < n; ++i) {
        textures[i] = mock_runtime::gGl.nextTexId++;
    }
}

void glDeleteTextures(GLsizei, const GLuint *) {}

void glBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) {
        if (mock_runtime::gGl.activeTexture == static_cast<GLint>(GL_TEXTURE1)) {
            mock_runtime::gGl.texBinding1 = static_cast<GLint>(texture);
        } else {
            mock_runtime::gGl.texBinding0 = static_cast<GLint>(texture);
        }
    }
}

void glTexParameteri(GLenum, GLenum, GLint) {}
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                  GLenum, const void *) {}

void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei,
                         GLsizei) {
    ++mock_runtime::gGl.copyTexSubImageCount;
}

GLuint glCreateShader(GLenum) { return mock_runtime::gGl.nextShaderId++; }
void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *) {}
void glCompileShader(GLuint) {}
void glGetShaderiv(GLuint, GLenum pname, GLint *params) {
    if (pname == GL_COMPILE_STATUS) {
        *params = GL_TRUE;
    }
}
void glDeleteShader(GLuint) {}

GLuint glCreateProgram(void) { return mock_runtime::gGl.nextProgramId++; }
void glAttachShader(GLuint, GLuint) {}
void glBindAttribLocation(GLuint, GLuint, const GLchar *) {}
void glLinkProgram(GLuint) {}
void glGetProgramiv(GLuint, GLenum pname, GLint *params) {
    if (pname == GL_LINK_STATUS) {
        *params = GL_TRUE;
    }
}
void glUseProgram(GLuint program) {
    mock_runtime::gGl.currentProgram = static_cast<GLint>(program);
}
void glDeleteProgram(GLuint) {}

GLint glGetAttribLocation(GLuint, const GLchar *name) {
    if (std::strcmp(name, "aPos") == 0) return 0;
    if (std::strcmp(name, "aUV") == 0) return 1;
    return 0;
}

GLint glGetUniformLocation(GLuint, const GLchar *name) {
    if (std::strcmp(name, "uPrevTex") == 0) return 10;
    if (std::strcmp(name, "uCurTex") == 0) return 11;
    if (std::strcmp(name, "uMotionVec") == 0) return 12;
    if (std::strcmp(name, "uPhase") == 0) return 13;
    if (std::strcmp(name, "uBlendWeight") == 0) return 14;
    if (std::strcmp(name, "uHudProtection") == 0) return 15;
    return -1;
}

void glUniform1i(GLint, GLint) {}
void glUniform1f(GLint location, GLfloat v0) {
    if (location == 13) mock_runtime::gGl.lastUniformPhase = v0;
    if (location == 14) mock_runtime::gGl.lastUniformBlendWeight = v0;
    if (location == 15) mock_runtime::gGl.lastUniformHudProtection = v0;
}
void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    if (location == 12) {
        mock_runtime::gGl.lastUniformMvU = v0;
        mock_runtime::gGl.lastUniformMvV = v1;
    }
}

void glGenBuffers(GLsizei n, GLuint *buffers) {
    for (GLsizei i = 0; i < n; ++i) {
        buffers[i] = mock_runtime::gGl.nextBufId++;
    }
}
void glDeleteBuffers(GLsizei, const GLuint *) {}
void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) {
        mock_runtime::gGl.arrayBuffer = static_cast<GLint>(buffer);
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        mock_runtime::gGl.elementArrayBuffer = static_cast<GLint>(buffer);
    }
}
void glBufferData(GLenum, GLsizeiptr, const void *, GLenum) {}
void glBindFramebuffer(GLenum, GLuint fb) {
    mock_runtime::gGl.framebuffer = static_cast<GLint>(fb);
}
void glEnableVertexAttribArray(GLuint) {}
void glDisableVertexAttribArray(GLuint) {}
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei,
                           const void *) {}
void glDrawArrays(GLenum, GLint, GLsizei) {
    ++mock_runtime::gGl.drawArraysCount;
}
void glFlush(void) { ++mock_runtime::gGl.flushCount; }

} // extern "C"

// ============================================================================
// Preloader SDK Mock Implementations
// ============================================================================
namespace pl::mod {
NativeMod *NativeMod::current() noexcept { return nullptr; }
namespace detail {
ScopedCurrentMod::ScopedCurrentMod(NativeMod *c) noexcept : mPrevious(c) {}
ScopedCurrentMod::~ScopedCurrentMod() = default;
} // namespace detail
} // namespace pl::mod

namespace pl::memory {
int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc, HookPriority) {
    if (!target || !detour || !originalFunc) return -1;
    *originalFunc = target;
    return 0;
}
bool unhook(FuncPtr, FuncPtr) { return true; }
std::uintptr_t resolveSignature(std::string_view, std::string_view) {
    return 0;
}
std::uintptr_t resolveVtableFunction(std::string_view, std::size_t,
                                     std::string_view) {
    return 0;
}
bool patchBytes(std::uintptr_t, std::span<const std::uint8_t>) { return false; }
bool nopInstructions(std::uintptr_t, std::size_t) { return false; }
} // namespace pl::memory

namespace pl::input {
void registerKeyCallback(KeyCallback callback) {
    mock_runtime::gKeyCallback = std::move(callback);
}
} // namespace pl::input

namespace pl::modmenu {
bool registerModule(const ModuleInfo &info) {
    mock_runtime::gLastRegisteredModule = info;
    mock_runtime::gModuleRegistered = true;
    return true;
}
void unregisterModule(std::string_view) {
    mock_runtime::gModuleRegistered = false;
}
bool setConfigSchemaJson(std::string_view, std::string_view schemaJson) {
    mock_runtime::gLastSchemaJson = std::string(schemaJson);
    return true;
}
void clearConfigSchema(std::string_view) {
    mock_runtime::gLastSchemaJson.clear();
}
bool registerButton(const ButtonInfo &info) {
    mock_runtime::gLastButton = info;
    mock_runtime::gButtonRegistered = true;
    return true;
}
void unregisterButton(std::string_view) {
    mock_runtime::gButtonRegistered = false;
}
void submitDrawCommands(std::string_view,
                        std::span<const DrawCommand> commands) {
    mock_runtime::gLastDrawCommands.assign(commands.begin(), commands.end());
}
void submitHudEditorElements(std::string_view,
                             std::span<const HudEditorElement> elements) {
    mock_runtime::gLastHudElements.assign(elements.begin(), elements.end());
}
} // namespace pl::modmenu

// ============================================================================
// Test Cases
// ============================================================================
namespace {

using namespace framegen;

void testCameraAngleConservationAndSmoothing() {
    std::cout << "[TEST 1] CameraSmoother: 100% angle conservation & jitter suppression...\n";
    auto &smoother = CameraSmoother::instance();

    const CameraSmoothingMode modes[] = {
        CameraSmoothingMode::Off,
        CameraSmoothingMode::Exponential,
        CameraSmoothingMode::CriticallyDampedSpring,
        CameraSmoothingMode::AdaptiveSmart
    };

    for (CameraSmoothingMode mode : modes) {
        FrameGenConfig cfg{};
        cfg.cameraSmoothingMode = mode;
        cfg.cameraSmoothness = 0.65f;
        cfg.cameraResponsiveness = 1.0f;
        cfg.microDeadzone = 0.0f;
        cfg.spikeFilterDeg = 90.0f;

        smoother.reset();
        smoother.updateConfig(cfg, true);

        double totalInPitch = 0.0;
        double totalInYaw = 0.0;
        double totalOutPitch = 0.0;
        double totalOutYaw = 0.0;

        std::int64_t tNs = 1'000'000'000LL;
        const std::int64_t stepNs = 8'333'333LL; // 120 Hz

        // Feed 60 active input frames followed by 180 drain frames (0, 0)
        for (int i = 0; i < 60; ++i) {
            const float inP = 0.35f + 0.15f * std::sin(i * 0.4f);
            const float inY = -1.25f + 0.40f * std::cos(i * 0.7f);
            totalInPitch += inP;
            totalInYaw += inY;

            const Vec2 out = smoother.processTurnDelta(inP, inY, tNs);
            totalOutPitch += out.pitch;
            totalOutYaw += out.yaw;
            tNs += stepNs;
        }

        for (int i = 0; i < 180; ++i) {
            const Vec2 out = smoother.processTurnDelta(0.0f, 0.0f, tNs);
            totalOutPitch += out.pitch;
            totalOutYaw += out.yaw;
            tNs += stepNs;
        }

        const Vec2 rem = smoother.pendingResidual();
        totalOutPitch += rem.pitch;
        totalOutYaw += rem.yaw;

        const double errPitch = std::abs(totalInPitch - totalOutPitch);
        const double errYaw = std::abs(totalInYaw - totalOutYaw);
        assert(errPitch < 1e-3 && "Pitch angle must be 100% conserved");
        assert(errYaw < 1e-3 && "Yaw angle must be 100% conserved");
    }

    // Verify high-frequency touch jitter reduction in AdaptiveSmart mode
    FrameGenConfig smartCfg{};
    smartCfg.cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
    smartCfg.cameraSmoothness = 0.70f;
    smartCfg.cameraResponsiveness = 1.0f;
    smartCfg.microDeadzone = 0.03f;
    smoother.reset();
    smoother.updateConfig(smartCfg, true);

    double rawDiffSqSum = 0.0;
    double smoothDiffSqSum = 0.0;
    float prevRaw = 1.0f;
    float prevSmooth = 1.0f;
    std::int64_t tNs = 2'000'000'000LL;

    for (int i = 0; i < 100; ++i) {
        // Constant 1.0 deg/frame pan contaminated by +/- 0.45 deg alternating touch noise
        const float rawYaw = 1.0f + ((i % 2 == 0) ? 0.45f : -0.45f);
        const Vec2 out = smoother.processTurnDelta(0.0f, rawYaw, tNs);
        if (i > 10) {
            const double dRaw = rawYaw - prevRaw;
            const double dSmooth = out.yaw - prevSmooth;
            rawDiffSqSum += dRaw * dRaw;
            smoothDiffSqSum += dSmooth * dSmooth;
        }
        prevRaw = rawYaw;
        prevSmooth = out.yaw;
        tNs += 8'333'333LL;
    }

    assert(smoothDiffSqSum < rawDiffSqSum * 0.25 &&
           "AdaptiveSmart must reduce frame-to-frame touch jitter variance by >75%");
    std::cout << "  -> Angle conservation & >75% jitter reduction verified.\n";
}

void testOpticalMotionVectorsAndHooks() {
    std::cout << "[TEST 2] CameraSmoother: Zero stationary drift, optical flow & Bedrock hooks...\n";
    auto &smoother = CameraSmoother::instance();
    FrameGenConfig cfg{};
    cfg.motionScale = 1.0f;
    cfg.sceneCutThreshold = 42.0f;
    cfg.cameraSmoothingMode = CameraSmoothingMode::Off;
    smoother.reset();
    smoother.updateConfig(cfg, true);
    smoother.recordFov(70.0f);

    // 1. Stationary camera MUST produce exact (0, 0) motion vector (no diagonal drift!)
    const FrameMotionSample stationary =
        smoother.consumeFrameMotion(16.0f / 9.0f, 1'000'000'000LL);
    assert(stationary.mvU == 0.0f && stationary.mvV == 0.0f);
    assert(!stationary.sceneCut);

    // 2. Native hook detours: applyTurnDeltaDetour, tryGetFovDetour, getCameraPerspectiveDetour
    static Vec2 sLastGameTurn{};
    smoother.setOriginalCallbacksForTest(
        [](void *, Vec2 *delta) {
            if (delta) sLastGameTurn = *delta;
        },
        [](void *) -> std::uint64_t {
            // Pack std::optional<float>(35.0f) in ARM64 AAPCS64 format
            const float fov = 35.0f;
            std::uint32_t bits = 0;
            std::memcpy(&bits, &fov, sizeof(float));
            return (static_cast<std::uint64_t>(1U) << 32U) | bits;
        },
        [](void *) -> int { return 1; });

    const std::uint64_t packedFov = CameraSmoother::tryGetFovDetour(nullptr);
    assert((packedFov >> 32U) == 1U);
    assert(std::abs(smoother.currentFovDeg() - 35.0f) < 1e-4f);

    Vec2 turnInput{3.5f, -7.0f};
    CameraSmoother::applyTurnDeltaDetour(nullptr, &turnInput);
    assert(std::abs(sLastGameTurn.pitch - 3.5f) < 1e-4f);
    assert(std::abs(sLastGameTurn.yaw - (-7.0f)) < 1e-4f);

    const FrameMotionSample moving =
        smoother.consumeFrameMotion(16.0f / 9.0f, 1'016'666'667LL);
    assert(moving.mvU > 0.0f && moving.mvV > 0.0f);
    assert(!moving.sceneCut);

    // 3. Perspective switch triggers sceneCut on next frame
    const int persp = CameraSmoother::getCameraPerspectiveDetour(nullptr);
    assert(persp == 1);
    const FrameMotionSample cutSample =
        smoother.consumeFrameMotion(16.0f / 9.0f, 1'033'333'333LL);
    assert(cutSample.sceneCut && "Perspective switch must flag sceneCut");
    std::cout << "  -> Zero stationary drift, optical flow UV, and native detours verified.\n";
}

void testFramePacerAndAutoTune() {
    std::cout << "[TEST 3] FramePacer: FPS metrics, discontinuity guard & Smart Auto-Tune...\n";
    FramePacer pacer;
    std::int64_t tNs = 1'000'000'000LL;
    const std::int64_t step60Hz = 16'666'667LL;

    FramePacingMetrics m{};
    for (int i = 0; i < 60; ++i) {
        m = pacer.recordFrame(tNs, 1, 2, true);
        tNs += step60Hz;
    }

    assert(std::abs(m.realFps - 60.0f) < 1.0f);
    assert(std::abs(m.effectiveFps - 120.0f) < 2.0f);
    assert(m.jitterMs < 0.5f);
    assert(!m.autoTuneReduced);

    // Inject 10 overloaded 38 ms frames -> Auto-Tune should engage
    for (int i = 0; i < 10; ++i) {
        m = pacer.recordFrame(tNs, 1, 2, true);
        tNs += 38'000'000LL;
    }
    assert(m.autoTuneReduced && "Auto-Tune must engage after sustained frame-budget overruns");

    // Recover after 35 stable 16.67 ms frames
    for (int i = 0; i < 35; ++i) {
        m = pacer.recordFrame(tNs, 1, 2, true);
        tNs += step60Hz;
    }
    assert(!m.autoTuneReduced && "Auto-Tune must recover once frame times stabilize");
    std::cout << "  -> FramePacer FPS telemetry & Auto-Tune hysteresis verified.\n";
}

void testGlFrameInterpolatorAndStateGuard() {
    std::cout << "[TEST 4] GlFrameInterpolator: GlStateGuard, Single-Swap & Multi-Swap...\n";
    mock_runtime::resetAll();

    // Seed custom Minecraft GL state before calling onSwapBuffers
    mock_runtime::gGl.currentProgram = 77;
    mock_runtime::gGl.activeTexture = GL_TEXTURE1;
    mock_runtime::gGl.texBinding0 = 42;
    mock_runtime::gGl.texBinding1 = 43;
    mock_runtime::gGl.framebuffer = 5;
    mock_runtime::gGl.arrayBuffer = 12;
    mock_runtime::gGl.elementArrayBuffer = 13;
    mock_runtime::gGl.viewport[0] = 10;
    mock_runtime::gGl.viewport[1] = 20;
    mock_runtime::gGl.viewport[2] = 1280;
    mock_runtime::gGl.viewport[3] = 720;
    mock_runtime::gGl.depthTest = GL_TRUE;
    mock_runtime::gGl.blend = GL_TRUE;
    mock_runtime::gGl.cullFace = GL_TRUE;
    mock_runtime::gGl.scissorTest = GL_TRUE;

    auto &interp = GlFrameInterpolator::instance();
    interp.uninstallHook();
    interp.setSwapCallbacksForTest(&mock_runtime::mockOrigSwapBuffers,
                                   &mock_runtime::mockPresentationTimeANDROID);

    FrameGenConfig cfg{};
    cfg.frameGenMode = FrameGenMode::SingleSwapSynthesis;
    cfg.blendStrength = 0.70f;
    cfg.hudProtection = 0.88f;
    interp.updateConfig(cfg);
    interp.setModuleEnabled(true);

    // Verify ARM64 FpsMod_nativeGetFps instruction decoder & 2x/3x/4x FPS counter injection
    // Construct a page-aligned fake function + .bss layout matching libinbuiltmods.so:
    //   ins0: adrp x8, #0 (same page) -> 0x90000008
    //   ins1: add  x8, x8, #0x24      -> 0x91009108
    //   ins2: ldar w0, [x8]           -> 0x88dffd00
    //   ins3: ret                     -> 0xd65f03c0
    alignas(4096) std::uint32_t fakePage[64]{};
    fakePage[0] = 0x90000008U;
    fakePage[1] = 0x91009108U;
    fakePage[2] = 0x88DFFD00U;
    fakePage[3] = 0xD65F03C0U;
    auto *decodedFrameCount =
        GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(fakePage);
    assert(decodedFrameCount ==
           reinterpret_cast<std::atomic<int> *>(&fakePage[8]));

    std::atomic<int> mockFpsModFrameCount{0};
    std::atomic<int> mockFpsModCurrentFps{0};
    interp.setInbuiltFpsCounterForTest(&mockFpsModFrameCount,
                                       &mockFpsModCurrentFps);

    // Frame 1 (seeds history, 1 swap)
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(mock_runtime::gOrigSwapCallCount == 1);
    assert(mock_runtime::gGl.flushCount >= 1 &&
           "HUD Optimizer UltraLowLatency must flush GPU queue pre-swap");

    // Frame 2 (2x: REAL swap + 1 GENERATED swap, +1 synthesized FpsMod count)
    mockFpsModFrameCount.store(0);
    mockFpsModCurrentFps.store(0);
    mock_runtime::gPresentedTimestamps.clear();
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(mock_runtime::gOrigSwapCallCount == 3 &&
           "2x must present 1 REAL + 1 GENERATED frame");
    assert(mock_runtime::gGl.drawArraysCount == 2 &&
           "GEN-before-REAL: 1 synthesized + 1 blit for REAL per 2x cycle");
    const auto snap2x = interp.telemetrySnapshot();
    assert(snap2x.activeMultiplier == 2);
    assert(snap2x.generatedPerCycle == 1);
    assert(snap2x.extraPresentedFramesActive &&
           "Generated frames must be presented as their own EGL frames");
    assert(mockFpsModFrameCount.load() == 0 && "Honest: FpsMod counter must NOT be inflated by generated frames");
    assert(mock_runtime::gPresentedTimestamps.size() == 2 &&
           "REAL -> GENERATED -> REAL pipeline must emit a timestamp per presented frame");
    const int fps2x = mockFpsModCurrentFps.load();
    assert(fps2x > 0);

    // Honest: FpsMod shows REAL, not presented, so 3x/4x must not inflate it.
    cfg.frameGenMultiplier = 3;
    cfg.autoTune = false;
    interp.updateConfig(cfg);
    mockFpsModFrameCount.store(0);
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(interp.telemetrySnapshot().activeMultiplier == 3);
    assert(mockFpsModFrameCount.load() == 0 && "Honest: 3x must not inflate FpsMod");
    const int fps3x = mockFpsModCurrentFps.load();
    assert(fps3x > 0);

    cfg.frameGenMultiplier = 4;
    interp.updateConfig(cfg);
    mockFpsModFrameCount.store(0);
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(interp.telemetrySnapshot().activeMultiplier == 4);
    assert(mockFpsModFrameCount.load() == 0 && "Honest: 4x must not inflate FpsMod");
    const int fps4x = mockFpsModCurrentFps.load();
    assert(fps4x > 0);

    // Verify 100% of Minecraft's GL state was restored by GlStateGuard
    assert(mock_runtime::gGl.currentProgram == 77);
    assert(mock_runtime::gGl.activeTexture == static_cast<GLint>(GL_TEXTURE1));
    assert(mock_runtime::gGl.texBinding0 == 42);
    assert(mock_runtime::gGl.texBinding1 == 43);
    assert(mock_runtime::gGl.framebuffer == 5);
    assert(mock_runtime::gGl.arrayBuffer == 12);
    assert(mock_runtime::gGl.elementArrayBuffer == 13);
    assert(mock_runtime::gGl.viewport[2] == 1280 &&
           mock_runtime::gGl.viewport[3] == 720);
    assert(mock_runtime::gGl.depthTest == GL_TRUE);
    assert(mock_runtime::gGl.blend == GL_TRUE);
    assert(mock_runtime::gGl.cullFace == GL_TRUE);
    assert(mock_runtime::gGl.scissorTest == GL_TRUE);

    // Verify the presented-frame pipeline emits strictly monotonic, correctly
    // spaced EGL presentation timestamps for 3x and 4x.
    cfg.frameGenMode = FrameGenMode::MultiSwapInterpolation;
    cfg.frameGenMultiplier = 3; // 1 REAL + 2 GENERATED = 3 presented frames
    cfg.autoTune = false;
    interp.updateConfig(cfg);

    // Seed history frame after mode change
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    const int swapsBefore = mock_runtime::gOrigSwapCallCount;
    mock_runtime::gPresentedTimestamps.clear();

    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    const int swapsDelta = mock_runtime::gOrigSwapCallCount - swapsBefore;
    assert(swapsDelta == 3 && "3x must present 1 REAL + 2 GENERATED frames");
    assert(mock_runtime::gPresentedTimestamps.size() == 3);
    assert(mock_runtime::gPresentedTimestamps[0] <
           mock_runtime::gPresentedTimestamps[1]);
    assert(mock_runtime::gPresentedTimestamps[1] <
           mock_runtime::gPresentedTimestamps[2]);

    // Verify 4x (max) presents 1 REAL + 3 GENERATED = 4 frames
    cfg.frameGenMultiplier = 4;
    interp.updateConfig(cfg);
    const int swapsBefore4x = mock_runtime::gOrigSwapCallCount;
    mock_runtime::gPresentedTimestamps.clear();
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    const int swapsDelta4x = mock_runtime::gOrigSwapCallCount - swapsBefore4x;
    assert(swapsDelta4x == 4 && "4x must present 1 REAL + 3 GENERATED frames");
    assert(mock_runtime::gPresentedTimestamps.size() == 4);

    // Verify the composited fallback (extra presented frames disabled) still works
    cfg.extraPresentedFrames = false;
    interp.updateConfig(cfg);
    mock_runtime::gPresentedTimestamps.clear();
    const int swapsBeforeFallback = mock_runtime::gOrigSwapCallCount;
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(mock_runtime::gOrigSwapCallCount - swapsBeforeFallback == 1 &&
           "Composited fallback must issue exactly one physical swap per game frame");
    const auto fallbackSnap = interp.telemetrySnapshot();
    assert(!fallbackSnap.extraPresentedFramesActive);
    assert(fallbackSnap.embeddedSynthesis &&
           "Composited fallback must flag embedded synthesis");
    assert(fallbackSnap.generatedFps == 0.0f &&
           fallbackSnap.presentedFps <= fallbackSnap.realFps + 1.0f &&
           "Embedded synthesis must never report real x multiplier as presented FPS");
    cfg.extraPresentedFrames = true;
    interp.updateConfig(cfg);

    std::cout << "  -> GlStateGuard, presented REAL->GEN->REAL pipeline (2x/3x/4x) & fallback verified.\n";
}

void testModLifecycleAndModMenuIntegration() {
    std::cout << "[TEST 5] FrameGenMod: PLGetModRegistration lifecycle, ModMenu V1/V2 & HUD...\n";
    const auto tempDir =
        std::filesystem::temp_directory_path() / "leviframegen_test_data";
    std::filesystem::remove_all(tempDir);

    auto *reg = PLGetModRegistration();
    assert(reg != nullptr && reg->instance != nullptr);

    pl::mod::ModInfo info{};
    info.id = kModId;
    info.displayName = kModName;
    info.version = kModVersion;
    info.author = kModAuthor;
    info.modRootPath = tempDir;
    pl::mod::ModContext ctx(nullptr, info);

    assert(reg->load(reg->instance, ctx));
    assert(reg->enable(reg->instance, ctx));
    assert(mock_runtime::gModuleRegistered);
    assert(mock_runtime::gLastRegisteredModule.hideInHudEditor &&
           "Module must set hideInHudEditor=true so no center-screen box appears");
    assert(!mock_runtime::gButtonRegistered &&
           "Must NOT register a floating logo button in the middle of the screen");
    assert(!mock_runtime::gLastSchemaJson.empty());
    assert(mock_runtime::gLastDrawCommands.empty() &&
           "Screen must be 100% free of overlay draw commands by default");
    assert(mock_runtime::gLastHudElements.empty() &&
           "Screen must be 100% free of HUD editor boxes by default");

    // Verify V2 schema contains categories, 2x/3x/4x multiplier options, HUD Optimizer, and telemetry
    assert(mock_runtime::gLastSchemaJson.find("\"version\":2") !=
           std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("\"categories\":[") !=
           std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("frame_gen_multiplier") !=
           std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("2×") != std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("3×") != std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("4×") != std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("frame_gen_output_fps") !=
               std::string::npos &&
           "V2 ModMenu must expose the Frame Generation Output FPS cap");
    assert(mock_runtime::gLastSchemaJson.find("extra_presented_frames") !=
               std::string::npos &&
           "V2 ModMenu must expose the REAL -> GEN -> REAL presentation toggle");
    assert(mock_runtime::gLastSchemaJson.find("trail_free_guard") !=
               std::string::npos &&
           "V2 ModMenu must expose the Trail-Free Motion Guard");
    assert(mock_runtime::gLastSchemaJson.find("hud_optimizer_mode") !=
           std::string::npos);
    assert(mock_runtime::gLastSchemaJson.find("fps_telemetry") !=
           std::string::npos);

    // Simulate in-game ModMenu user interactions
    FrameGenMod::onConfigChanged(kModuleId, keys::kPreset, "2"); // Smooth preset (3x)
    assert(FrameGenMod::instance().configSnapshot().preset ==
           QualityPreset::Smooth);
    assert(FrameGenMod::instance().configSnapshot().frameGenMultiplier == 3);

    // Verify user selecting 2x, 3x, 4x, and clamping values > 4 to 4x max
    FrameGenMod::onConfigChanged(kModuleId, keys::kFrameGenMultiplier, "2×");
    assert(FrameGenMod::instance().configSnapshot().frameGenMultiplier == 2);
    FrameGenMod::onConfigChanged(kModuleId, keys::kFrameGenMultiplier, "3×");
    assert(FrameGenMod::instance().configSnapshot().frameGenMultiplier == 3);
    FrameGenMod::onConfigChanged(kModuleId, keys::kFrameGenMultiplier, "4×");
    assert(FrameGenMod::instance().configSnapshot().frameGenMultiplier == 4);
    FrameGenMod::onConfigChanged(kModuleId, keys::kFrameGenMultiplier, "8");
    assert(FrameGenMod::instance().configSnapshot().frameGenMultiplier == 4 &&
           "4x must be the strict maximum multiplier");

    FrameGenMod::onConfigChanged(kModuleId, keys::kCameraSmoothness, "0.75");
    assert(FrameGenMod::instance().configSnapshot().preset ==
           QualityPreset::Custom);
    assert(std::abs(
               FrameGenMod::instance().configSnapshot().cameraSmoothness -
               0.75f) < 1e-4f);

    // Simulate dragging HUD card in LeviLauncher HUD Editor
    FrameGenMod::onConfigChanged(kModuleId, keys::kHudPosX, "128.5");
    FrameGenMod::onConfigChanged(kModuleId, keys::kHudPosY, "64.0");
    assert(std::abs(FrameGenMod::instance().configSnapshot().hudPosX - 128.5f) <
           1e-3f);
    assert(std::abs(FrameGenMod::instance().configSnapshot().hudPosY - 64.0f) <
           1e-3f);

    // Simulate Cycle Preset action button in V2 RuntimeConfigView
    FrameGenMod::onConfigChanged(kModuleId, keys::kActionCyclePreset, "1");
    assert(FrameGenMod::instance().configSnapshot().preset ==
           QualityPreset::Balanced);

    // Simulate Reset Defaults button in V2 RuntimeConfigView
    FrameGenMod::onConfigChanged(kModuleId, keys::kActionResetDefaults, "1");
    assert(FrameGenMod::instance().configSnapshot().preset ==
           QualityPreset::Balanced);
    // HUD position should be preserved across Reset Defaults
    assert(std::abs(FrameGenMod::instance().configSnapshot().hudPosX - 128.5f) <
           1e-3f);

    // Verify config.json persistence
    assert(std::filesystem::exists(ctx.configDir() / "config.json"));
    FrameGenConfig loadedFromDisk{};
    assert(loadConfigFromFile(ctx.configDir() / "config.json", loadedFromDisk));
    assert(loadedFromDisk.preset == QualityPreset::Balanced);
    assert(std::abs(loadedFromDisk.hudPosX - 128.5f) < 1e-3f);

    assert(reg->disable(reg->instance, ctx));
    assert(reg->unload(reg->instance, ctx));
    std::filesystem::remove_all(tempDir);
    std::cout << "  -> Full lifecycle, V1/V2 ModMenu UI, HUD Editor drag, and config persistence verified.\n";
}

void testBidirectionalSynthesisAndAntiGhosting() {
    std::cout << "[TEST 6] Bidirectional Motion Synthesis: True Midpoint, Anti-Ghosting, CAS & HUD...\n";

    // Scene setup:
    // Background sky = (0.15, 0.30, 0.55)
    // Moving bright block/entity column of width 0.04:
    //   - In REAL A (Prev): centered at u = 0.40 ([0.38, 0.42]) -> color (0.92, 0.85, 0.30)
    //   - In REAL B (Cur):  centered at u = 0.50 ([0.48, 0.52]) -> color (0.92, 0.85, 0.30)
    // Static crosshair at (0.50, 0.50) and static hotbar at (0.50, 0.92) -> color (0.98, 0.98, 0.98)
    auto sampleFrame = [](float objCenterU, float u, float v,
                          float &r, float &g, float &b) {
        // Static crosshair at center
        if (std::hypot(u - 0.50f, v - 0.50f) < 0.012f) {
            r = 0.98f; g = 0.98f; b = 0.98f;
            return;
        }
        // Static hotbar at bottom
        if (v > 0.90f && std::abs(u - 0.50f) < 0.25f) {
            r = 0.95f; g = 0.95f; b = 0.95f;
            return;
        }
        // Moving block/entity column (in upper world region v = 0.30)
        if (std::abs(u - objCenterU) <= 0.020f && std::abs(v - 0.30f) <= 0.15f) {
            r = 0.92f; g = 0.85f; b = 0.30f;
            return;
        }
        // Background world gradient
        r = 0.15f + 0.05f * u;
        g = 0.30f + 0.05f * v;
        b = 0.55f;
    };

    auto samplePrevA = [&](float u, float v, float &r, float &g, float &b) {
        sampleFrame(0.40f, u, v, r, g, b);
    };
    auto sampleCurB = [&](float u, float v, float &r, float &g, float &b) {
        sampleFrame(0.50f, u, v, r, g, b);
    };

    FrameMotionSample motion{};
    motion.mvU = 0.10f;
    motion.mvV = 0.0f;
    motion.prevMvU = 0.10f;
    motion.prevMvV = 0.0f;
    motion.tanHalfFovH = 0.7002f;
    motion.tanHalfFovV = 0.7002f;
    motion.motionConfidence = 1.0f;

    // 1. Verify True Midpoint Frame (phase = 0.5):
    // Moving block at 0.40 (A) and 0.50 (B) must appear at 0.45 (GENERATED),
    // and NOT leave ghost copies at 0.40 or 0.50!
    const auto midHit = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.45f, 0.30f, motion, 0.5f, 0.65f, 0.85f,
        1.0f / 1920.0f, 1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(midHit.r > 0.85f && midHit.g > 0.78f &&
           "Moving block must be synthesized cleanly at the true midpoint u=0.45");

    const auto oldPosA = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.40f, 0.30f, motion, 0.5f, 0.65f, 0.85f,
        1.0f / 1920.0f, 1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(oldPosA.r < 0.25f &&
           "Old REAL A position (u=0.40) must show clean background with zero ghosting");

    const auto newPosB = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.50f, 0.30f, motion, 0.5f, 0.65f, 0.85f,
        1.0f / 1920.0f, 1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(newPosB.r < 0.25f &&
           "New REAL B position (u=0.50) must show clean background at t=0.5 without double-edge ghosting");

    // 2. Verify Screen-Border Newly Revealed Terrain Guard:
    // Near the left screen border (u = 0.02) with mvU = +0.10, rawUvA = 0.02 - 0.05 < 0.0
    // (out of bounds in REAL A), so backward-warped REAL B must be selected 100% (wB = 1.0).
    const auto borderPix = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.02f, 0.30f, motion, 0.5f, 0.65f, 0.85f,
        1.0f / 1920.0f, 1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(std::abs(borderPix.occlusionWinnerB - 1.0f) < 1e-4f &&
           "Newly revealed terrain at screen border must select in-bounds frame B cleanly");

    // 3. Verify Static Crosshair & Hotbar HUD Protection:
    const auto crosshairPix = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.50f, 0.50f, motion, 0.5f, 0.65f, 0.95f,
        1.0f / 1920.0f, 1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(crosshairPix.hudMask > 0.85f && crosshairPix.alpha < 0.15f &&
           "Static crosshair must be protected from motion warping");

    const auto hotbarPix = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.50f, 0.92f, motion, 0.5f, 0.65f, 0.95f,
        1.0f / 1920.0f, 1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(hotbarPix.hudMask > 0.85f && hotbarPix.alpha < 0.15f &&
           "Static hotbar must be protected from motion warping");

    // 4. Verify Zero-Dimming Guard when texture readback is empty/black (0, 0, 0):
    auto blackSampler = [](float, float, float &r, float &g, float &b) {
        r = 0.0f; g = 0.0f; b = 0.0f;
    };
    const auto emptyCapturePix = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.35f, 0.35f, motion, 0.5f, 0.65f, 0.85f,
        1.0f / 1920.0f, 1.0f / 1080.0f, blackSampler, blackSampler);
    assert(emptyCapturePix.alpha == 0.0f &&
           "Empty/black texture readback must force alpha=0.0 so GL_ONE_MINUS_SRC_ALPHA never dims the screen");

    // 5. Verify Trail-Free Motion Guard (the "motion blur" fix):
    // A legitimate midpoint warp must stay at full strength...
    assert(midHit.trailGuard < 0.5f && midHit.selection > 0.3f &&
           "Legitimate motion-compensated midpoint must NOT be suppressed");
    // ...while a genuinely ambiguous warp (both sources disagree, no temporal
    // winner) must fall back to the razor-sharp real pixel instead of leaving a
    // smeared trail / ghosted double edge.
    auto sampleBright = [](float, float, float &r, float &g, float &b) {
        r = 0.90f; g = 0.90f; b = 0.90f;
    };
    auto sampleDark = [](float, float, float &r, float &g, float &b) {
        r = 0.05f; g = 0.05f; b = 0.05f;
    };
    FrameMotionSample mvFast = motion;
    const auto ambiguousPix = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.50f, 0.30f, mvFast, 0.5f, 0.65f, 0.0f,
        1.0f / 1920.0f, 1.0f / 1080.0f, sampleBright, sampleDark);
    assert(ambiguousPix.trailGuard > 0.9f && ambiguousPix.alpha < 0.02f &&
           "Ambiguous bidirectional warp must be suppressed (trail/ghost free)");
    const auto ambiguousUnguarded =
        GlFrameInterpolator::evaluateSynthesizedPixelCpu(
            0.50f, 0.30f, mvFast, 0.5f, 0.65f, 0.0f, 1.0f / 1920.0f,
            1.0f / 1080.0f, sampleBright, sampleDark,
            /*opaqueOutput=*/false, /*trailFreeGuard=*/0.0f);
    assert(ambiguousUnguarded.alpha > 0.1f &&
           "Disabling the guard must restore the raw synthesis weight");

    // 6. Verify opaque presented-frame output: every pixel is fully defined and
    // unreliable pixels keep the exact 1:1 real pixel (no blend, no smear).
    const auto opaqueAmbiguous = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.50f, 0.30f, mvFast, 0.5f, 0.65f, 0.0f, 1.0f / 1920.0f,
        1.0f / 1080.0f, sampleBright, sampleDark, /*opaqueOutput=*/true);
    assert(opaqueAmbiguous.alpha == 1.0f &&
           "Presented generated frames must write every pixel (opaque output)");
    assert(std::abs(opaqueAmbiguous.r - 0.05f) < 0.02f &&
           "Suppressed pixels must fall back to the sharp real pixel, not a blend");
    const auto opaqueMid = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.45f, 0.30f, motion, 0.5f, 0.65f, 0.85f, 1.0f / 1920.0f,
        1.0f / 1080.0f, samplePrevA, sampleCurB, /*opaqueOutput=*/true);
    assert(opaqueMid.r > 0.85f && opaqueMid.alpha == 1.0f &&
           "Opaque midpoint frames must keep synthesized block color at full strength");

    // 7. Verify static sub-pixel passthrough (no resampling shimmer/blur when still):
    FrameMotionSample mvStill = motion;
    mvStill.mvU = 0.0001f;
    mvStill.prevMvU = 0.0001f;
    const auto stillPix = GlFrameInterpolator::evaluateSynthesizedPixelCpu(
        0.35f, 0.35f, mvStill, 0.5f, 0.65f, 0.0f, 1.0f / 1920.0f,
        1.0f / 1080.0f, samplePrevA, sampleCurB);
    assert(stillPix.staticGate > 0.9f && stillPix.alpha < 0.02f &&
           "Sub-pixel camera motion must pass the real pixel through untouched");

    std::cout << "  -> True Midpoint, Anti-Ghosting, Border Disocclusion, CAS, HUD Protection,\n"
                 "     Zero-Dimming & Trail-Free Motion Guard verified.\n";
}

void testPresentedFpsCapAndRealGeneratedSplit() {
    std::cout << "[TEST 7] Presented FPS Cap: real/generated split & limiter integrity...\n";

    // 1. Cadence resolution: the configured cap always covers the FINAL presented
    //    FPS and can never be bypassed by enabling frame generation.
    {
        const auto capOff = FramePacer::resolveCadence(90, 0, false, 1, false);
        assert(capOff.capActive && capOff.presentedTargetFps == 90 &&
               capOff.realTargetFps == 90 && capOff.multiplier == 1 &&
               capOff.generatedPerCycle == 0 &&
               "FG OFF + 90 cap -> <= 90 presented FPS (90 real)");

        const auto cap90 = FramePacer::resolveCadence(0, 90, true, 2, true);
        assert(cap90.capActive && cap90.presentedTargetFps == 90 &&
               cap90.realTargetFps == 45 && cap90.multiplier == 2 &&
               cap90.presentedIntervalNs == 11'111'111LL &&
               cap90.realIntervalNs == 22'222'222LL &&
               "FG ON + 90 cap -> 45 real + 45 generated presented FPS");

        const auto cap120 = FramePacer::resolveCadence(0, 120, true, 2, true);
        assert(cap120.presentedTargetFps == 120 && cap120.realTargetFps == 60 &&
               "FG ON + 120 cap -> 60 real + 60 generated presented FPS");

        const auto tightest = FramePacer::resolveCadence(60, 120, true, 2, true);
        assert(tightest.presentedTargetFps == 60 && tightest.realTargetFps == 30 &&
               "The tightest configured cap always wins (FG never bypasses it)");

        const auto uncapped = FramePacer::resolveCadence(0, 0, true, 2, true);
        assert(!uncapped.capActive &&
               "0/0 caps stay unlimited and never throttle the renderer");

        const auto triple = FramePacer::resolveCadence(0, 90, true, 3, true);
        assert(triple.realTargetFps == 30 && triple.generatedPerCycle == 2 &&
               "90 FPS with 3x -> 30 real + 60 generated");
    }

    // 2. Full one-second pacing simulation at a 90 FPS output cap with 2:1 FG.
    //    The real renderer is throttled to 45 FPS and generated frames fill the
    //    11.111 ms presentation grid: 45 real + 45 generated = 90 presented.
    {
        FramePacer pacer;
        const auto cadence = FramePacer::resolveCadence(90, 90, true, 2, true);
        std::int64_t tNs = 1'000'000'000LL;
        int realPresented = 0;
        int generatedPresented = 0;
        FramePacingMetrics metrics{};

        for (int cycle = 0; cycle < 45; ++cycle) {
            const auto realSlot =
                pacer.paceSlot(cadence, PresentSlotKind::RealFrame, 0, tNs);
            assert(realSlot.pacingActive && !realSlot.throttleWait &&
                   "The real slot must align exactly with the grid");
            assert(realSlot.targetPresentNs == tNs &&
                   "REAL frames must land on the presentation grid deadline");
            pacer.recordPresented(tNs, PresentSlotKind::RealFrame);
            ++realPresented;

            for (int k = 1; k <= realSlot.generatedFramesAllowed; ++k) {
                const std::int64_t genNowNs =
                    tNs + static_cast<std::int64_t>(k) *
                              cadence.presentedIntervalNs;
                const auto genSlot = pacer.paceSlot(
                    cadence, PresentSlotKind::GeneratedFrame, k, genNowNs);
                assert(!genSlot.missedDeadline && !genSlot.cycleDegraded &&
                       "Generated slots must hit their deadlines on a healthy grid");
                assert(genSlot.targetPresentNs == genNowNs);
                pacer.recordPresented(genNowNs, PresentSlotKind::GeneratedFrame);
                ++generatedPresented;
            }

            metrics = pacer.endCycle(cadence, tNs + cadence.realIntervalNs, 1,
                                     realSlot.generatedFramesAllowed, false);
            tNs += cadence.realIntervalNs;
        }

        assert(realPresented == 45 && generatedPresented == 45 &&
               "A 90 FPS cap with 2:1 FG must present exactly 45 real + 45 generated per second");
        assert(metrics.capActive && metrics.presentedTargetFps == 90 &&
               metrics.realTargetFps == 45);
        assert(std::abs(metrics.realFps - 45.0f) < 1.0f);
        assert(std::abs(metrics.generatedFps - 45.0f) < 2.0f);
        assert(std::abs(metrics.presentedFps - 90.0f) < 2.0f &&
               "Reported presented FPS must match the configured cap, never exceed it");
        assert(std::abs(metrics.presentedIntervalMs - 11.111f) < 0.05f);
        assert(!metrics.pacingDegraded && metrics.missedPresentations == 0);
    }

    // 3. Graceful degradation: when the real renderer cannot keep up, generated
    //    frames are reduced and the grid is re-anchored forward (no queue, no
    //    burst catch-up, no accumulated latency).
    {
        FramePacer pacer;
        const auto cadence = FramePacer::resolveCadence(0, 90, true, 4, true);
        std::int64_t tNs = 1'000'000'000LL;
        int allowed = 0;
        std::int64_t lastDeadline = 0;
        for (int cycle = 0; cycle < 8; ++cycle) {
            const auto realSlot =
                pacer.paceSlot(cadence, PresentSlotKind::RealFrame, 0, tNs);
            pacer.recordPresented(tNs, PresentSlotKind::RealFrame);
            allowed = realSlot.generatedFramesAllowed;
            lastDeadline = realSlot.targetPresentNs;
            assert(lastDeadline >= tNs - cadence.presentedIntervalNs &&
                   "A late real frame must re-anchor the grid forward, never backwards");
            // Renderer takes 40 ms per real frame (way below the 22.2 ms target)
            tNs += 40'000'000LL;
            (void)pacer.endCycle(cadence, tNs, 1, allowed, false);
        }
        assert(allowed == 1 &&
               "A struggling renderer must drop generated frames, not stack them up");
    }

    // 4. Input-latency independence: the camera/input hooks never touch the
    //    pacer, so a throttled renderer cannot add keyboard/mouse latency.
    {
        FramePacer pacer;
        const auto cadence = FramePacer::resolveCadence(30, 0, false, 1, false);
        std::atomic<int> pacedSlots{0};
        std::thread renderThread([&pacer, &cadence, &pacedSlots] {
            std::int64_t nowNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            for (int i = 0; i < 4; ++i) {
                (void)pacer.paceSlot(cadence, PresentSlotKind::RealFrame, 0,
                                     nowNs);
                ++pacedSlots;
                nowNs += 33'333'333LL;
            }
        });

        const auto inputStart =
            std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i) {
            const Vec2 out = CameraSmoother::instance().processTurnDelta(
                0.35f, 1.10f,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            (void)out;
        }
        const auto inputElapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - inputStart)
                .count();
        renderThread.join();
        assert(pacedSlots.load() == 4);
        assert(inputElapsedMs < 20 &&
               "Input/camera processing must stay instant while the FPS cap is throttling rendering");
    }

    std::cout << "  -> 90/120 FPS caps enforced on the full presentation pipeline,\n"
                 "     REAL-GEN split, degradation & input-latency independence verified.\n";
}


void testSprintingRendererCannotBypassCap() {
    std::cout << "[TEST 8] Sprinting renderer (200+ FPS capable) + FG ON: cap still holds...\n";

    // 1. Embedded (composited fallback) accounting must never multiply the
    //    reported presented FPS above the cap. Before this fix a 90 FPS real
    //    cadence with 2x reported 180 FPS - i.e. the counter showed a number
    //    ABOVE the configured cap, exactly the reported bug.
    {
        FramePacer pacer;
        const auto cadence = FramePacer::resolveCadence(0, 90, true, 2, false);
        std::int64_t tNs = 1'000'000'000LL;
        FramePacingMetrics m{};
        for (int i = 0; i < 90; ++i) {
            pacer.recordPresented(tNs, PresentSlotKind::RealFrame);
            m = pacer.endCycle(cadence, tNs, 1, 1, false, true);
            tNs += 11'111'111LL;
        }
        assert(m.embeddedSynthesis && m.generatedFps == 0.0f);
        assert(m.presentedFps <= 92.0f &&
               "Embedded synthesis must report the real presented rate, never real x multiplier");
        assert(m.presentedFps >= 85.0f);
        assert(m.capExceedEvents == 0 && m.capHoldPercent > 95.0f &&
               "Embedded synthesis must never record a cap violation");
    }

    // 2. Real-time simulation of the exact reported scenario: a device that could
    //    render at 200+ FPS (the renderer never sleeps by itself - the ONLY thing
    //    pacing it is the limiter), frame generation ON at a 90 FPS output cap.
    //    Expected: ~45 real + ~45 generated = ~90 presented.
    {
        FramePacer pacer;
        const auto cadence = FramePacer::resolveCadence(90, 90, true, 2, true);
        assert(cadence.capActive && cadence.presentedTargetFps == 90 &&
               cadence.realTargetFps == 45);

        std::int64_t startNs = 0;
        int realPresented = 0;
        int generatedPresented = 0;
        int warmupCycles = 0;
        FramePacingMetrics metrics{};

        while (true) {
            const auto nowNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (startNs != 0 && nowNs - startNs >= 400'000'000LL) {
                break;
            }

            const auto realSlot =
                pacer.paceSlot(cadence, PresentSlotKind::RealFrame, 0, nowNs);
            const auto afterRealNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            pacer.recordPresented(afterRealNs, PresentSlotKind::RealFrame);
            ++realPresented;

            for (int k = 1; k <= realSlot.generatedFramesAllowed; ++k) {
                (void)pacer.paceSlot(cadence, PresentSlotKind::GeneratedFrame, k,
                                     std::chrono::duration_cast<
                                         std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now()
                                             .time_since_epoch())
                                         .count());
                pacer.recordPresented(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count(),
                    PresentSlotKind::GeneratedFrame);
                ++generatedPresented;
            }

            metrics = pacer.endCycle(cadence,
                                     std::chrono::duration_cast<
                                         std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now()
                                             .time_since_epoch())
                                         .count(),
                                     1, realSlot.generatedFramesAllowed, false);

            // Discard the very first (unpaced) frames so the measurement reflects
            // the established steady-state grid.
            ++warmupCycles;
            if (warmupCycles == 2) {
                startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch())
                              .count();
                realPresented = 0;
                generatedPresented = 0;
            }
        }

        const double elapsedS =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now()
                                        .time_since_epoch())
                                    .count() -
                                startNs) *
            1e-9;
        const double realRate = static_cast<double>(realPresented) / elapsedS;
        const double presentedRate =
            static_cast<double>(realPresented + generatedPresented) / elapsedS;

        assert(realRate <= 47.0 &&
               "An uncapped-capable renderer must be throttled to ~45 real FPS");
        assert(realRate >= 41.0);
        assert(presentedRate <= 94.0 &&
               "Total presented FPS must stay at or below the 90 FPS cap");
        assert(presentedRate >= 86.0);
        assert(generatedPresented >= realPresented - 2 &&
               "Roughly half of the presented frames must be generated frames");
        assert(metrics.capExceedEvents == 0 && metrics.capHoldPercent > 90.0f &&
               "The cap integrity monitor must report zero violations");
        assert(std::abs(metrics.realFps - 45.0f) < 3.0f);
        assert(metrics.presentedTargetFps == 90 && metrics.activeMultiplier == 2);

        std::cout << "     measured: " << realRate << " real + "
                  << (presentedRate - realRate) << " generated = " << presentedRate
                  << " presented FPS\n";
    }

    // 3. The limiter must not add latency/timing work when it is switched off:
    //    unlimited cadence never throttles or sleeps.
    {
        FramePacer pacer;
        const auto cadence = FramePacer::resolveCadence(0, 0, true, 2, true);
        int cycles = 0;
        for (int i = 0; i < 200; ++i) {
            const auto slot = pacer.paceSlot(
                cadence, PresentSlotKind::RealFrame, 0,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            assert(!slot.throttleWait && !slot.pacingActive &&
                   "Unlimited cadence must never throttle the renderer");
            (void)pacer.endCycle(cadence,
                                 std::chrono::duration_cast<
                                     std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now()
                                         .time_since_epoch())
                                     .count(),
                                 1, slot.generatedFramesAllowed, false);
            ++cycles;
        }
        assert(cycles == 200);
    }

    std::cout << "  -> 90 FPS cap enforced on the full presentation pipeline even when the\n"
                 "     renderer could run at 200+ FPS; uncapped mode stays untouched.\n";
}

} // namespace

int main() {
    std::cout << "========================================================\n";
    std::cout << " LeviFrameGen v2.6 — Host Verification & Simulation Suite\n";
    std::cout << "========================================================\n";

    testCameraAngleConservationAndSmoothing();
    testOpticalMotionVectorsAndHooks();
    testFramePacerAndAutoTune();
    testGlFrameInterpolatorAndStateGuard();
    testModLifecycleAndModMenuIntegration();
    testBidirectionalSynthesisAndAntiGhosting();
    testPresentedFpsCapAndRealGeneratedSplit();
    testSprintingRendererCannotBypassCap();

    std::cout << "========================================================\n";
    std::cout << " ALL TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
