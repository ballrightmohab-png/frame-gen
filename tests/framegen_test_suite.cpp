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
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
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
    interp.setInbuiltFpsCounterForTest(&mockFpsModFrameCount);

    // Frame 1 (seeds history, 1 swap)
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(mock_runtime::gOrigSwapCallCount == 1);
    assert(mock_runtime::gGl.flushCount >= 1 &&
           "HUD Optimizer UltraLowLatency must flush GPU queue pre-swap");

    // Frame 2 (2x mode: adds +1 synthesized frame to FpsMod counter -> 1 real + 1 synth = 2x FPS)
    mockFpsModFrameCount.store(0);
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(mock_runtime::gOrigSwapCallCount == 2);
    assert(mock_runtime::gGl.drawArraysCount == 1);
    assert(interp.telemetrySnapshot().activeMultiplier == 2);
    assert(mockFpsModFrameCount.load() == 1);

    // Verify 3x and 4x Single-Swap multipliers and FpsMod counter injection (+2 and +3)
    cfg.frameGenMultiplier = 3;
    cfg.autoTune = false;
    interp.updateConfig(cfg);
    mockFpsModFrameCount.store(0);
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(interp.telemetrySnapshot().activeMultiplier == 3);
    assert(mockFpsModFrameCount.load() == 2);

    cfg.frameGenMultiplier = 4;
    interp.updateConfig(cfg);
    mockFpsModFrameCount.store(0);
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    assert(interp.telemetrySnapshot().activeMultiplier == 4);
    assert(mockFpsModFrameCount.load() == 3);

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

    // Verify Multi-Swap mode generates strictly monotonic future timestamps for 2x, 3x, 4x
    cfg.frameGenMode = FrameGenMode::MultiSwapInterpolation;
    cfg.frameGenMultiplier = 3; // 2 intermediate + 1 final = 3 swaps
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
    assert(swapsDelta == 3 && "3x Multi-Swap must issue 2 synthetic + 1 final swap");
    assert(mock_runtime::gPresentedTimestamps.size() == 3);
    assert(mock_runtime::gPresentedTimestamps[0] <
           mock_runtime::gPresentedTimestamps[1]);
    assert(mock_runtime::gPresentedTimestamps[1] <
           mock_runtime::gPresentedTimestamps[2]);

    // Verify 4x (max) Multi-Swap issues 3 synthetic + 1 final = 4 swaps
    cfg.frameGenMultiplier = 4;
    interp.updateConfig(cfg);
    const int swapsBefore4x = mock_runtime::gOrigSwapCallCount;
    mock_runtime::gPresentedTimestamps.clear();
    interp.onSwapBuffers(reinterpret_cast<EGLDisplay>(1),
                         reinterpret_cast<EGLSurface>(1));
    const int swapsDelta4x = mock_runtime::gOrigSwapCallCount - swapsBefore4x;
    assert(swapsDelta4x == 4 && "4x Multi-Swap must issue 3 synthetic + 1 final swap");
    assert(mock_runtime::gPresentedTimestamps.size() == 4);

    std::cout << "  -> GlStateGuard, Single-Swap (2x/3x/4x), and Multi-Swap (2x/3x/4x) verified.\n";
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

} // namespace

int main() {
    std::cout << "========================================================\n";
    std::cout << " LeviFrameGen v2.0 — Host Verification & Simulation Suite\n";
    std::cout << "========================================================\n";

    testCameraAngleConservationAndSmoothing();
    testOpticalMotionVectorsAndHooks();
    testFramePacerAndAutoTune();
    testGlFrameInterpolatorAndStateGuard();
    testModLifecycleAndModMenuIntegration();

    std::cout << "========================================================\n";
    std::cout << " ALL TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
