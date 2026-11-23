#include <iostream>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <iomanip>

enum class HormoneType {
    Cortisol,      // Stress, alertness, catabolism
    Adrenaline,    // Acute fight-or-flight, immediate energy
    Melatonin,     // Sleep regulation, circadian rhythm
    Serotonin,     // Mood stability, satiety, calmness
    Dopamine,      // Motivation, reward, drive
    Insulin,       // Nutrient storage, blood sugar regulation
    Testosterone   // Vitality, recovery, dominance/drive
};

enum class LifeState {
    Homeostasis,       // Balanced and healthy
    FightOrFlight,     // High adrenaline/cortisol (acute stress)
    ChronicBurnout,    // High cortisol, depleted dopamine/serotonin/testosterone
    DeepRest,          // High melatonin/serotonin, low cortisol/adrenaline
    HighMotivation,    // High dopamine/testosterone, low cortisol
    Lethargic          // Low dopamine, low adrenaline, high insulin/melatonin
};

class Hormone {
public:
    Hormone() = default;
    Hormone(std::string name, double initial, double baseline, double decayRate)
        : name_(std::move(name)), 
          level_(initial), 
          baseline_(baseline), 
          decayRate_(decayRate) {}

    // Getters
    const std::string& getName() const { return name_; }
    double getLevel() const { return level_; }
    double getBaseline() const { return baseline_; }

    // Clamps hormone levels between 0.0 (depleted) and 100.0 (max saturation)
    void adjust(double delta) {
        level_ = std::clamp(level_ + delta, 0.0, 100.0);
    }

    // Natural homeostatic pull back toward baseline over time
    void applyHomeostasis(double dt) {
        double diff = baseline_ - level_;
        level_ += diff * decayRate_ * dt;
    }

private:
    std::string name_;
    double level_ = 50.0;      // Normalized scale: 0.0 to 100.0
    double baseline_ = 50.0;   // Resting target level
    double decayRate_ = 0.1;   // How quickly it returns to baseline
};

class EndocrineSystem {
public:
    EndocrineSystem() {
        // Initialize hormones: {Name, Initial Level, Baseline, Homeostatic Decay Rate}
        hormones_[HormoneType::Cortisol]     = Hormone("Cortisol",     30.0, 25.0, 0.08);
        hormones_[HormoneType::Adrenaline]   = Hormone("Adrenaline",   15.0, 10.0, 0.35); // Spikes & clears fast
        hormones_[HormoneType::Melatonin]    = Hormone("Melatonin",    20.0, 15.0, 0.12);
        hormones_[HormoneType::Serotonin]    = Hormone("Serotonin",    60.0, 60.0, 0.05);
        hormones_[HormoneType::Dopamine]     = Hormone("Dopamine",     55.0, 50.0, 0.15);
        hormones_[HormoneType::Insulin]      = Hormone("Insulin",      25.0, 20.0, 0.20);
        hormones_[HormoneType::Testosterone] = Hormone("Testosterone", 60.0, 60.0, 0.03); // Changes slowly
    }

    // External stimulus (e.g., eating, threat, sleeping, exercising)
    void stimulate(HormoneType type, double amount) {
        if (hormones_.count(type)) {
            hormones_[type].adjust(amount);
        }
    }

    // Advances simulation by dt (e.g., 1.0 = 1 hour)
    void update(double dt) {
        // 1. Snapshot current values so evaluation order doesn't skew interactions
        std::unordered_map<HormoneType, double> current;
        for (const auto& [type, h] : hormones_) {
            current[type] = h.getLevel();
        }

        // 2. Calculate interaction deltas based on biological feedback loops
        std::unordered_map<HormoneType, double> deltas;

        // Cortisol & Adrenaline: Stress suppresses sleep, mood, and anabolic hormones
        if (current[HormoneType::Cortisol] > 60.0) {
            double excessStress = (current[HormoneType::Cortisol] - 60.0) * 0.15;
            deltas[HormoneType::Melatonin]    -= excessStress * 1.5; // Stress blocks sleep
            deltas[HormoneType::Serotonin]    -= excessStress * 0.8; // Stress lowers mood
            deltas[HormoneType::Testosterone] -= excessStress * 0.6; // Catabolic suppression
            deltas[HormoneType::Insulin]      += excessStress * 0.5; // Cortisol raises blood sugar
        }

        // Adrenaline triggers secondary Cortisol release
        if (current[HormoneType::Adrenaline] > 50.0) {
            deltas[HormoneType::Cortisol] += (current[HormoneType::Adrenaline] - 50.0) * 0.25;
        }

        // Melatonin & Serotonin: Serotonin is a precursor to Melatonin; Melatonin suppresses Dopamine
        if (current[HormoneType::Serotonin] > 65.0) {
            deltas[HormoneType::Melatonin] += (current[HormoneType::Serotonin] - 65.0) * 0.10;
        }
        if (current[HormoneType::Melatonin] > 55.0) {
            deltas[HormoneType::Dopamine] -= (current[HormoneType::Melatonin] - 55.0) * 0.20;
            deltas[HormoneType::Cortisol] -= (current[HormoneType::Melatonin] - 55.0) * 0.15;
        }

        // Dopamine & Testosterone: Positive reinforcement loop for drive/energy
        if (current[HormoneType::Dopamine] > 70.0 && current[HormoneType::Cortisol] < 40.0) {
            deltas[HormoneType::Testosterone] += 0.5;
        }

        // Insulin spike triggers Serotonin uptake, but crashes Dopamine if excessive
        if (current[HormoneType::Insulin] > 65.0) {
            deltas[HormoneType::Serotonin] += 1.5;
            deltas[HormoneType::Dopamine]  -= 1.0; // Post-meal sluggishness
        }

        // 3. Apply interaction deltas and natural homeostatic decay
        for (auto& [type, hormone] : hormones_) {
            hormone.adjust(deltas[type] * dt);
            hormone.applyHomeostasis(dt);
        }
    }

    // Evaluates overall LifeState from current hormone balances
    LifeState evaluateLifeState() const {
        double cort = hormones_.at(HormoneType::Cortisol).getLevel();
        double adre = hormones_.at(HormoneType::Adrenaline).getLevel();
        double mela = hormones_.at(HormoneType::Melatonin).getLevel();
        double sero = hormones_.at(HormoneType::Serotonin).getLevel();
        double dopa = hormones_.at(HormoneType::Dopamine).getLevel();
        double test = hormones_.at(HormoneType::Testosterone).getLevel();

        if (adre > 65.0 && cort > 55.0) {
            return LifeState::FightOrFlight;
        }
        if (cort > 70.0 && dopa < 35.0 && test < 35.0) {
            return LifeState::ChronicBurnout;
        }
        if (mela > 65.0 && cort < 30.0 && adre < 20.0) {
            return LifeState::DeepRest;
        }
        if (dopa > 70.0 && test > 60.0 && cort < 45.0) {
            return LifeState::HighMotivation;
        }
        if (dopa < 30.0 && sero < 40.0 && adre < 25.0) {
            return LifeState::Lethargic;
        }
        return LifeState::Homeostasis;
    }

    std::string getLifeStateString() const {
        switch (evaluateLifeState()) {
            case LifeState::Homeostasis:    return "Homeostasis (Balanced)";
            case LifeState::FightOrFlight:  return "Fight or Flight (Acute Stress)";
            case LifeState::ChronicBurnout: return "Chronic Burnout (Depleted)";
            case LifeState::DeepRest:       return "Deep Rest / Recovery";
            case LifeState::HighMotivation: return "High Motivation & Drive";
            case LifeState::Lethargic:      return "Lethargic / Low Energy";
        }
        return "Unknown";
    }

    void printStatus(const std::string& eventLabel) const {
        std::cout << "=== " << eventLabel << " ===\n";
        std::cout << "Life State: [" << getLifeStateString() << "]\n";
        for (const auto& [type, h] : hormones_) {
            std::cout << "  " << std::left << std::setw(14) << h.getName() 
                      << ": " << std::fixed << std::setprecision(1) << h.getLevel() << "\n";
        }
        std::cout << "\n";
    }

private:
    std::unordered_map<HormoneType, Hormone> hormones_;
};

int main() {
    EndocrineSystem body;
    body.printStatus("Initial Morning State");

    // Scenario 1: Acute stressor (e.g., sudden danger or high-pressure deadline)
    body.stimulate(HormoneType::Adrenaline, 65.0);
    body.stimulate(HormoneType::Cortisol, 35.0);
    body.update(1.0);
    body.printStatus("After Sudden Threat / High Stress");

    // Scenario 2: Time passes, stress lingers without recovery
    for (int i = 0; i < 4; ++i) {
        body.stimulate(HormoneType::Cortisol, 15.0);
        body.update(1.0);
    }
    body.printStatus("After Prolonged Stress");

    // Scenario 3: Nighttime winding down (darkness + meal + relaxation)
    body.stimulate(HormoneType::Melatonin, 60.0);
    body.stimulate(HormoneType::Serotonin, 25.0);
    body.update(2.0);
    body.printStatus("Nighttime Recovery");

    return 0;
}