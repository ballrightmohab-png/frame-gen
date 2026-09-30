#pragma once

#include <atomic>
#include <cstdint>

namespace framegen {

class FrameRenderer {
public:
  FrameRenderer();
  ~FrameRenderer();

  void initialize();
  void render(float deltaTime);
  void shutdown();

  void setEnabled(bool enabled) { mEnabled = enabled; }
  void setTargetFPS(int fps) { mTargetFPS = fps; }
  void setOpacity(int opacity) { mOpacity = opacity; }
  void setScale(double scale) { mScale = scale; }
  void setOverlayColor(uint32_t color) { mOverlayColor = color; }

  bool isEnabled() const { return mEnabled; }
  int getTargetFPS() const { return mTargetFPS; }
  int getOpacity() const { return mOpacity; }
  double getScale() const { return mScale; }
  uint32_t getOverlayColor() const { return mOverlayColor; }
  uint32_t getFrameCount() const { return mFrameCount; }
  float getLastDeltaTime() const { return mLastDeltaTime; }

private:
  std::atomic_bool mEnabled{true};
  std::atomic_int mTargetFPS{60};
  std::atomic_int mOpacity{80};
  std::atomic<double> mScale{1.0};
  std::atomic_uint32_t mOverlayColor{0xFF00FF00}; // ARGB: Green
  std::atomic_uint32_t mFrameCount{0};
  std::atomic<float> mLastDeltaTime{0.0f};

  bool mInitialized{false};
};

} // namespace framegen
