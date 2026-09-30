#pragma once

/**
 * @file Logger.hpp
 * @brief Build-time shim of the preloader's logging API.
 *
 * The real `pl::log::Logger` is part of libpreloader.so and formats messages
 * with fmt. Mods that only need logcat output can rely on this header-only
 * shim, which keeps the same public surface used by `pl/Mod.hpp` but writes
 * plain text lines to logcat directly — no fmt dependency.
 */

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pl::log {

/**
 * @brief Named Android logger (no fmt; logs raw text to logcat).
 */
class Logger {
public:
  explicit Logger(std::string name) : mLoggerName(std::move(name)) {}

  static Logger &getOrCreate(std::string name) {
    std::lock_guard<std::mutex> lock(sLoggerMutex);

    auto it = sLoggers.find(name);
    if (it != sLoggers.end()) {
      return *it->second;
    }

    auto logger = std::make_unique<Logger>(std::move(name));
    auto &ref = *logger;
    sLoggers.emplace(ref.mLoggerName, std::move(logger));
    return ref;
  }

  template <typename... Args>
  void info(std::string_view msg, Args &&...) const {
    write(ANDROID_LOG_INFO, msg);
  }

  template <typename... Args>
  void debug(std::string_view msg, Args &&...) const {
    write(ANDROID_LOG_DEBUG, msg);
  }

  template <typename... Args>
  void warn(std::string_view msg, Args &&...) const {
    write(ANDROID_LOG_WARN, msg);
  }

  template <typename... Args>
  void error(std::string_view msg, Args &&...) const {
    write(ANDROID_LOG_ERROR, msg);
  }

private:
  std::string mLoggerName;

  inline static std::unordered_map<std::string, std::unique_ptr<Logger>>
      sLoggers{};
  inline static std::mutex sLoggerMutex{};

  void write(int androidLevel, std::string_view msg) const {
    __android_log_print(androidLevel, mLoggerName.c_str(), "%.*s",
                        static_cast<int>(msg.size()),
                        msg.data() ? msg.data() : "");
  }
};

} // namespace pl::log

/**
 * @brief Shared logger used by the preloader runtime itself.
 */
inline auto &preloaderLogger = pl::log::Logger::getOrCreate("Preloader");
