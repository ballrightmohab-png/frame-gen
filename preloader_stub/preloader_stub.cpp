// Build-time stub of libpreloader.so for linking LeviLaunchroid native mods.
//
// At runtime, LeviLaunchroid injects the real libpreloader.so with RTLD_GLOBAL
// before loading mod shared libraries. Linking against this stub at build time
// produces the required DT_NEEDED entry ("libpreloader.so") and exact NDK/libc++
// mangled undefined symbol references without bundling libpreloader.so in the
// .levipack archive.

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Patch.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pl::mod {

NativeMod *NativeMod::current() noexcept {
    return nullptr;
}

namespace detail {

ScopedCurrentMod::ScopedCurrentMod(NativeMod *current) noexcept
    : mPrevious(current) {}

ScopedCurrentMod::~ScopedCurrentMod() = default;

} // namespace detail

} // namespace pl::mod

namespace pl::memory {

int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
         HookPriority priority) {
    (void)target;
    (void)detour;
    (void)originalFunc;
    (void)priority;
    return -1;
}

bool unhook(FuncPtr target, FuncPtr detour) {
    (void)target;
    (void)detour;
    return false;
}

std::uintptr_t resolveSignature(std::string_view signature,
                                std::string_view moduleName) {
    (void)signature;
    (void)moduleName;
    return 0;
}

std::uintptr_t resolveVtableFunction(std::string_view typeInfoName,
                                     std::size_t slot,
                                     std::string_view moduleName) {
    (void)typeInfoName;
    (void)slot;
    (void)moduleName;
    return 0;
}

bool patchBytes(std::uintptr_t address, std::span<const std::uint8_t> bytes) {
    (void)address;
    (void)bytes;
    return false;
}

bool nopInstructions(std::uintptr_t address, std::size_t instructionCount) {
    (void)address;
    (void)instructionCount;
    return false;
}

} // namespace pl::memory

namespace pl::input {

void registerKeyCallback(KeyCallback callback) {
    (void)callback;
}

} // namespace pl::input

namespace pl::modmenu {

bool registerModule(const ModuleInfo &info) {
    (void)info;
    return false;
}

void unregisterModule(std::string_view moduleId) {
    (void)moduleId;
}

bool setConfigSchemaJson(std::string_view moduleId,
                         std::string_view schemaJson) {
    (void)moduleId;
    (void)schemaJson;
    return false;
}

void clearConfigSchema(std::string_view moduleId) {
    (void)moduleId;
}

bool registerButton(const ButtonInfo &info) {
    (void)info;
    return false;
}

void unregisterButton(std::string_view buttonId) {
    (void)buttonId;
}

void submitDrawCommands(std::string_view moduleId,
                        std::span<const DrawCommand> commands) {
    (void)moduleId;
    (void)commands;
}

void submitHudEditorElements(std::string_view moduleId,
                             std::span<const HudEditorElement> elements) {
    (void)moduleId;
    (void)elements;
}

} // namespace pl::modmenu
