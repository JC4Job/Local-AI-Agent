#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

/*
 * Hormones - the chemical messengers of the agent's body (bio/EndocrineSystem.hpp).
 *
 * Each hormone is a level from 0 (depleted) to 100 (saturated) that drifts back toward its baseline over time,
 * as the glands and the liver bring a real one back. What moves the levels away from it, the agent's tasks and
 * the hormones acting on each other, is the EndocrineSystem's business: a Hormone only knows its level and how
 * fast it settles.
 *
 * Time is in simulated hours; see EndocrineSystem::kSimHoursPerSecond for how they map to the wall clock.
 */
enum class HormoneType {
    Cortisol,      // stress, alertness, catabolism
    Adrenaline,    // acute fight-or-flight, immediate energy
    Melatonin,     // sleep pressure, rest
    Serotonin,     // mood stability, satiety, calm
    Dopamine,      // motivation, reward, drive
    Insulin,       // storing what is taken in: here, what the searches bring back
    Testosterone,  // vitality, recovery, drive
    Count
};

constexpr size_t kHormoneCount = static_cast<size_t>(HormoneType::Count);

// The position of an enum value (HormoneType, LifeState, Feeling) in the tables indexed by it.
template <class Enum>
constexpr size_t EnumIndex(Enum value) {
    return static_cast<size_t>(value);
}

struct HormoneInfo {
    const char* name;
    const char* key;          // its key in the life file; see EndocrineSystem::ToText
    double      initial;      // the level of a newborn agent
    double      baseline;     // the resting level it settles back to
    double      settle_rate;  // per simulated hour: how fast it settles (1 - e^-rate of the gap closes in an hour)
};

/*
 * In HormoneType order.
 *
 * WARNING: the keys are the life file's: renaming one loses that hormone's level in every saved life.
 */
constexpr HormoneInfo kHormoneInfo[kHormoneCount] = {
    // name           key             initial baseline settle
    {"Cortisol",     "cortisol",     30.0,   25.0,    0.08},
    {"Adrenaline",   "adrenaline",   15.0,   10.0,    0.35},  // spikes and clears fast
    {"Melatonin",    "melatonin",    20.0,   15.0,    0.12},
    {"Serotonin",    "serotonin",    60.0,   60.0,    0.05},
    {"Dopamine",     "dopamine",     55.0,   50.0,    0.15},
    {"Insulin",      "insulin",      25.0,   20.0,    0.20},
    {"Testosterone", "testosterone", 60.0,   60.0,    0.03},  // changes slowly
};

inline const HormoneInfo& Info(HormoneType type) {
    return kHormoneInfo[EnumIndex(type)];
}

class Hormone {
public:
    Hormone() = default;
    explicit Hormone(HormoneType type) : type_(type), level_(Info(type).initial) {}

    HormoneType Type()     const { return type_; }
    const char* Name()     const { return Info(type_).name; }
    double      Level()    const { return level_; }
    double      Baseline() const { return Info(type_).baseline; }

    // Clamped between 0 (depleted) and 100 (saturated).
    void Adjust(double delta) { level_ = std::clamp(level_ + delta, 0.0, 100.0); }
    void Set(double level)    { level_ = std::clamp(level, 0.0, 100.0); }

    /*
     * The drift back toward the baseline over `hours`.
     *
     * NOTE: exponential, where the prototype stepped level += gap * rate * hours. That overshoots the baseline
     * once rate * hours passes 1 and diverges past 2 (adrenaline after 6 hours), which a long gap, such as the
     * time the program was closed, reaches. This one is exact for any `hours`.
     */
    void Settle(double hours) {
        level_ += (Baseline() - level_) * (1.0 - std::exp(-Info(type_).settle_rate * hours));
    }

private:
    HormoneType type_  = HormoneType::Cortisol;
    double      level_ = 50.0;
};
