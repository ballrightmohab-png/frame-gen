#include <pl/Mod.hpp>

class LeviFrameGen {
public:
    static LeviFrameGen& instance();

    LeviFrameGen()
        : mSelf(*ll::mod::NativeMod::current()) {}

    ll::mod::NativeMod& getSelf() const {
        return mSelf;
    }

    bool load() {
        getSelf().getLogger().info("Levi Frame Generator loaded!");
        return true;
    }

    bool enable() {
        getSelf().getLogger().info("Levi Frame Generator enabled!");
        return true;
    }

    bool disable() {
        getSelf().getLogger().info("Levi Frame Generator disabled!");
        return true;
    }

    bool unload() {
        getSelf().getLogger().info("Levi Frame Generator unloaded!");
        return true;
    }

private:
    ll::mod::NativeMod& mSelf;
};

LeviFrameGen& LeviFrameGen::instance() {
    static LeviFrameGen instance;
    return instance;
}

PL_REGISTER_MOD(LeviFrameGen, LeviFrameGen::instance())
