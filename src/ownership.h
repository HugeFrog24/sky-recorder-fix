#pragma once
#include <cstdint>

enum class Phase : uint8_t { Free, Queued, Pending, Frozen, Failed, Delivering };
enum class Delivery : uint8_t { None, Original, Frozen };

struct Ownership {
    Phase phase=Phase::Free;
    bool reserve() {
        if (phase!=Phase::Free) return false;
        phase=Phase::Queued;
        return true;
    }
    bool submitted() {
        if (phase!=Phase::Queued) return false;
        phase=Phase::Pending;
        return true;
    }
    template<class Save> bool protect(Save save) {
        if (phase!=Phase::Pending) return false;
        phase=save()?Phase::Frozen:Phase::Failed;
        return phase==Phase::Frozen;
    }
    Delivery claim() {
        if (phase==Phase::Free || phase==Phase::Delivering) return Delivery::None;
        Delivery result=phase==Phase::Frozen?Delivery::Frozen:Delivery::Original;
        phase=Phase::Delivering;
        return result;
    }
    bool retire() {
        if (phase!=Phase::Delivering) return false;
        phase=Phase::Free;
        return true;
    }
};
