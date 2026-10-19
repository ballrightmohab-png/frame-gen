#include "ConfigManager.hpp"
#include "FrameGenConfig.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace framegen::config {

ConfigManager::~ConfigManager() {
    flush();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::string ConfigManager::getConfigPath() const {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_configPath.empty()) return m_configPath;
    }
    // Use JNGS path primary, BedrockTools path as fallback for legacy
    return "/sdcard/games/JNGSFrameGen/config.json";
}

void ConfigManager::setConfigPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configPath = path;
}

nlohmann::json ConfigManager::loadConfig() {
    std::string path = getConfigPath();
    // Try primary, then legacy BedrockTools path, then legacy flat path
    std::string legacyPath = "/sdcard/games/BedrockTools/config.json";
    std::string flatPath = "/sdcard/games/jngsframegen.json";
    
    nlohmann::json result;
    // Try primary path
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream inFile(path);
            nlohmann::json j;
            inFile >> j;
            if (j.contains("Modules") && j["Modules"].contains("JNGS Frame Gen")) {
                return j["Modules"]["JNGS Frame Gen"];
            } else if (j.contains("Modules") && j["Modules"].contains("jngsframegen")) {
                return j["Modules"]["jngsframegen"];
            } else {
                // Flat JSON legacy at same path
                return j;
            }
        } catch (...) {}
    }
    // Try BedrockTools path
    if (std::filesystem::exists(legacyPath)) {
        try {
            std::ifstream inFile(legacyPath);
            nlohmann::json j;
            inFile >> j;
            if (j.contains("Modules") && j["Modules"].contains("JNGS Frame Gen")) {
                return j["Modules"]["JNGS Frame Gen"];
            }
        } catch (...) {}
    }
    // Try flat legacy
    if (std::filesystem::exists(flatPath)) {
        try {
            std::ifstream inFile(flatPath);
            nlohmann::json j;
            inFile >> j;
            return j;
        } catch (...) {}
    }
    return result;
}

void ConfigManager::saveConfig(const nlohmann::json& frameGenModuleJson) {
    nlohmann::json j;
    // Original LeviFrame Gen toggles ONLY - saved via BedrockTools WAY: Modules["JNGS Frame Gen"] + flat keys stacked for compat
    j["Modules"]["JNGS Frame Gen"] = frameGenModuleJson;
    // Also keep flat keys for backwards compat (original LeviFrame Gen loader)
    for (auto& [k, v] : frameGenModuleJson.items()) {
        j[k] = v;
    }

    std::string path = getConfigPath();
    std::string payload = j.dump(2);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        ensureWorkerLocked();
        m_pendingPath = std::move(path);
        m_pendingPayload = std::move(payload);
        m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        ++m_queuedGeneration;
        m_hasPending = true;
    }
    m_condition.notify_all();
}

void ConfigManager::load() {
    // This is called on startup - load into global config via FrameGenConfig helpers
    // Actual load is done via loadConfig() -> FrameGenConfig::fromJson
    // Here we just ensure file exists, if not create it
    std::string path = getConfigPath();
    if (!std::filesystem::exists(path)) {
        // Trigger a save with defaults to create file
        nlohmann::json defaults;
        // Will be filled by FrameGenMod on first save
        saveConfig(defaults);
        flush();
        return;
    }
}

void ConfigManager::save() {
    // This version is called when ModuleRegistry wants to save all modules
    // For FrameGen we have only one module - the caller should use saveConfig(json)
    // Keep for API compat - if called without args, just flush
    flush();
}

void ConfigManager::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_worker.joinable() || !m_hasPending) return;
    const std::uint64_t targetGeneration = m_queuedGeneration;
    m_deadline = std::chrono::steady_clock::now();
    m_condition.notify_all();
    m_condition.wait(lock, [&] {
        return m_completedGeneration >= targetGeneration || m_stopping;
    });
}

void ConfigManager::ensureWorkerLocked() {
    if (!m_worker.joinable()) m_worker = std::thread(&ConfigManager::workerLoop, this);
}

void ConfigManager::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_condition.wait(lock, [&] { return m_stopping || m_hasPending; });
        if (m_stopping && !m_hasPending) break;

        const std::uint64_t generation = m_queuedGeneration;
        const auto deadline = m_deadline;
        if (!m_stopping && m_condition.wait_until(lock, deadline, [&] {
            return m_stopping || m_queuedGeneration != generation || m_deadline != deadline;
        })) {
            continue;
        }

        std::string path = std::move(m_pendingPath);
        std::string payload = std::move(m_pendingPayload);
        m_hasPending = false;
        lock.unlock();
        writePayload(path, payload);
        lock.lock();
        if (m_completedGeneration < generation) m_completedGeneration = generation;
        m_condition.notify_all();
    }
}

void ConfigManager::writePayload(const std::string& pathValue, const std::string& payload) {
    std::filesystem::path path(pathValue);
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    try {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
            outFile.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            outFile.flush();
            if (!outFile) return;
        }
        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tempPath, path, ec);
            if (ec) std::filesystem::remove(tempPath, ec);
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }
}

}
