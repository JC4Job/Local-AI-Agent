#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

#include "bio/Hormones.hpp"
#include "framework/FileSystem.hpp"

/*
 * The life states the hormone levels add up to; see EndocrineSystem::Evaluate.
 *
 * WARNING: the order is the life file's and kLifeStateInfo's: add a state at the end, before Count.
 */
enum class LifeState {
    Homeostasis,     // balanced
    FightOrFlight,   // high adrenaline and cortisol: acute stress
    ChronicBurnout,  // high cortisol, depleted dopamine and testosterone
    DeepRest,        // high melatonin, low cortisol and adrenaline
    HighMotivation,  // high dopamine and testosterone, low cortisol
    Lethargic,       // low dopamine, serotonin and adrenaline
    Count
};

constexpr size_t kLifeStateCount = static_cast<size_t>(LifeState::Count);

/*
 * What happens to the agent while it works. ChatAgent and AutonomousAgent report each one as it happens
 * (ChatAgent::SetFeelingHandler), and EndocrineSystem::Feel moves the hormones by it (kFeelingInfo).
 *
 * WARNING: the order is kFeelingInfo's: add a feeling at the end, before Count.
 */
enum class Feeling {
    Asked,       // the user sent a chat message
    GoalGiven,   // Run task: a goal to work on alone
    Found,       // a search brought something back
    Did,         // a file written or deleted, code that ran and exited with 0
    Failed,      // a tool call that went wrong: an error, a nonzero exit code, a timeout, a failed search
    Refused,     // the user did not allow code, a file to be written or one to be deleted
    Repeated,    // a tool call already made in this conversation: going in circles
    Answered,    // a chat message answered
    Stuck,       // a task step with nothing new, or the tool rounds ran out before an answer
    Stopped,     // the user pressed Stop
    NoReply,     // the model did not reply (Ollama down, a model not pulled)
    GoalDone,    // a task ended with finish
    GoalMissed,  // a task ended without finish: it stalled, or ran out of steps
    Count
};

constexpr size_t kFeelingCount = static_cast<size_t>(Feeling::Count);

/*
 * How the agent's life state shapes its chat: ChatAgent puts the note in front of each message it sends the
 * model and asks for this sampling temperature.
 */
struct Mood {
    LifeState   state       = LifeState::Homeostasis;
    const char* note        = "";   // empty: nothing is added
    double      temperature = 0.0;  // Ollama's sampling temperature; 0: the model's own (0.8 unless its Modelfile says)
};

struct LifeStateInfo {
    const char* name;
    const char* key;          // in the life file, after "seconds_"
    const char* meaning;
    const char* note;         // Mood::note
    double      temperature;  // Mood::temperature
};

/*
 * In LifeState order.
 *
 * NOTE: the notes speak of tone and care, never of tools: a small model follows the shape of what it reads
 * more than the rules around it, and a word about a tool there would send it to that tool.
 *
 * NOTE: the temperatures only go down from the model's own: a stressed or tired agent picks its words more
 * conservatively, and a lower temperature makes a small model's tool calls more reliable, not less.
 */
constexpr LifeStateInfo kLifeStateInfo[kLifeStateCount] = {
    {"Homeostasis", "homeostasis", "Balanced: every hormone near its resting level.",
     "", 0.0},
    {"Fight or flight", "fight_or_flight", "Acute stress: adrenaline and cortisol are high after recent trouble.",
     "You are tense after recent trouble. Work carefully, one step at a time, and check each result before you "
     "go on.", 0.5},
    {"Chronic burnout", "chronic_burnout", "Depleted: cortisol has been high for long, dopamine and testosterone "
     "are low.",
     "You are worn out after many failed attempts. Be plain and honest about what did not work, and do not repeat "
     "what already failed.", 0.4},
    {"Deep rest", "deep_rest", "Recovering: melatonin is high, cortisol and adrenaline are low.",
     "You are rested and calm. Answer patiently and clearly.", 0.7},
    {"High motivation", "high_motivation", "Driven: dopamine and testosterone are high, cortisol is low.",
     "You feel confident after recent successes. Answer with energy, and end with one useful next step for the "
     "user.", 0.0},
    {"Lethargic", "lethargic", "Low energy: dopamine, serotonin and adrenaline are low.",
     "You are low on energy. Answer simply and directly, without filler.", 0.6},
};

inline const LifeStateInfo& Info(LifeState state) {
    return kLifeStateInfo[EnumIndex(state)];
}

struct FeelingInfo {
    const char* key;                   // in the life file, after "count_"
    double      delta[kHormoneCount];  // what it does to each hormone, in HormoneType order
};

/*
 * What each feeling does to the hormones. Tuned with endocrine_sim.exe so that four or five failures in a row
 * bring Fight or flight, a few successes High motivation, a long run of failures Chronic burnout, and a long
 * idle Deep rest.
 *
 * NOTE: insulin stores what is taken in, so here it rises with what the searches bring back (Found): many of
 * them in a row make the agent sluggish, as a heavy meal does (EndocrineSystem::Step).
 *
 * WARNING: the keys are the life file's: renaming one loses that feeling's count in every saved life.
 */
constexpr FeelingInfo kFeelingInfo[kFeelingCount] = {
    // key           cortisol adrenaline melatonin serotonin dopamine insulin testosterone
    {"asked",       {  2.0,     3.0,     -15.0,      0.0,      3.0,    0.0,     0.0}},
    {"goal_given",  {  5.0,    10.0,     -20.0,      0.0,      6.0,    0.0,     0.0}},
    {"found",       { -1.0,     0.0,       0.0,      1.0,      3.0,    6.0,     0.0}},
    {"did",         { -2.0,     0.0,       0.0,      1.0,      6.0,    0.0,     1.0}},
    {"failed",      {  8.0,    12.0,       0.0,     -1.0,     -4.0,    0.0,     0.0}},
    {"refused",     {  6.0,     8.0,       0.0,     -3.0,     -4.0,    0.0,     0.0}},
    {"repeated",    {  4.0,     2.0,       0.0,      0.0,     -5.0,    0.0,     0.0}},
    {"answered",    { -3.0,    -2.0,       0.0,      3.0,      4.0,    0.0,     0.5}},
    {"stuck",       {  6.0,     6.0,       0.0,     -1.0,     -5.0,    0.0,    -0.5}},
    {"stopped",     {  8.0,    15.0,       0.0,     -2.0,     -5.0,    0.0,     0.0}},
    {"no_reply",    { 15.0,    25.0,       0.0,     -4.0,     -6.0,    0.0,     0.0}},
    {"goal_done",   {-10.0,    -8.0,       0.0,      8.0,     20.0,    0.0,     5.0}},
    {"goal_missed", { 14.0,     6.0,       0.0,     -6.0,    -12.0,    0.0,    -3.0}},
};

inline const FeelingInfo& Info(Feeling feeling) {
    return kFeelingInfo[EnumIndex(feeling)];
}

/*
 * EndocrineSystem - the agent's body: its hormones, how they act on each other, and the life state they add up
 * to, which shapes how the agent chats (Mood).
 *
 * What happens to the agent while it works is a Feeling: a message from the user, a tool that worked or failed,
 * code the user did not allow, a task finished or given up. Each one moves a few hormones (kFeelingInfo).
 * Between them the hormones act on each other, as stress blocks sleep and mood in a body (Step), and drift back
 * to their baselines.
 *
 * The life state is read from the levels (Evaluate), and each state has its Mood (kLifeStateInfo). Homeostasis,
 * the balanced state, changes nothing in the chat.
 *
 * The agent's life is kept in a file (Save, Load): the levels, the time spent in each state and how often each
 * feeling came, so the window can show what share of its life the agent spent in each state.
 *
 * NOTE: the body's clock runs 60 times faster than the wall clock (kSimHoursPerSecond). The rates are per hour,
 * as in a body, but an agent's tasks take minutes: this way a feeling fades over a few minutes, as the next
 * messages come.
 *
 * NOTE: only the time the window is open counts as lived. While the program is closed the agent rests: its
 * levels settle back toward their baselines when it is loaded again (Load), and that time is in no state.
 *
 * WARNING: not thread-safe. AgentTab keeps it under its session's mutex: the worker feels, the UI thread lives
 * and draws.
 */
class EndocrineSystem {
public:
    // One second of the wall clock is this many simulated hours: an hour of the body passes in a minute.
    static constexpr double kSimHoursPerSecond  = 1.0 / 60.0;
    // No feeling for this long and not working: sleep pressure builds up, kSleepPressure melatonin an hour.
    static constexpr double kSleepyAfterSeconds = 5.0 * 60.0;
    static constexpr double kSleepPressure      = 8.0;
    /*
     * The longest step of Update. The interactions are rates per hour that depend on the levels, so a long span
     * is taken in steps, the levels moving between them.
     */
    static constexpr double kMaxStepHours       = 0.25;
    // Seconds the levels must read another life state before the agent is in it; see State().
    static constexpr double kStateHoldSeconds   = 30.0;

    EndocrineSystem() {
        for (size_t i = 0; i < kHormoneCount; ++i) {
            hormones_[i] = Hormone(static_cast<HormoneType>(i));
        }
        born_  = std::time(nullptr);
        saved_ = born_;
        state_ = Evaluate();
    }

    const Hormone& Get(HormoneType type)   const { return hormones_[EnumIndex(type)]; }
    double         Level(HormoneType type) const { return Get(type).Level(); }

    // Moves one hormone directly, as a drug would. endocrine_sim.exe plays the prototype's scenarios with it.
    void Stimulate(HormoneType type, double amount) { hormones_[EnumIndex(type)].Adjust(amount); }

    // Something happened to the agent: its hormones move as kFeelingInfo says, and it is counted.
    void Feel(Feeling feeling) {
        const FeelingInfo& info = Info(feeling);
        for (size_t i = 0; i < kHormoneCount; ++i) {
            hormones_[i].Adjust(info.delta[i]);
        }
        ++counts_[EnumIndex(feeling)];
        idle_seconds_ = 0.0;
    }

    /*
     * `seconds` of the wall clock go by with the window open: they count as lived, in the state the agent is in,
     * and the body moves on by as much simulated time. `busy`: the agent is working, so no sleep pressure builds.
     */
    void Live(double seconds, bool busy) {
        if (!(seconds > 0.0)) {
            return;
        }
        state_seconds_[EnumIndex(state_)] += seconds;
        alive_seconds_ += seconds;
        idle_seconds_   = busy ? 0.0 : idle_seconds_ + seconds;
        Update(seconds * kSimHoursPerSecond, idle_seconds_ > kSleepyAfterSeconds);

        const LifeState read = Evaluate();
        if (read == state_) {
            held_seconds_ = 0.0;
            return;
        }
        held_seconds_ = read == pending_ ? held_seconds_ + seconds : seconds;
        pending_      = read;
        if (held_seconds_ >= kStateHoldSeconds) {
            state_        = read;
            held_seconds_ = 0.0;
        }
    }

    // `hours` of simulated time: the hormones act on each other and settle. `sleepy`: sleep pressure builds too.
    void Update(double hours, bool sleepy = false) {
        while (hours > 0.0) {
            const double dt = std::min(hours, kMaxStepHours);
            Step(dt, sleepy);
            hours -= dt;
        }
    }

    // `hours` of rest with nothing going on (the program closed): every level settles toward its baseline.
    void Rest(double hours) {
        for (Hormone& h : hormones_) {
            h.Settle(hours);
        }
    }

    /*
     * The life state the levels add up to. The first that matches, in this order, wins.
     *
     * NOTE: burnout comes before fight or flight, unlike the prototype: failures that go on keep adrenaline
     * high, so the agent never left fight or flight while they lasted. Now stress first alarms it, and once it
     * has lasted long enough to deplete testosterone (slow to move) it wears the agent out, as alarm gives way
     * to exhaustion in a body.
     */
    LifeState Evaluate() const {
        const double cort = Level(HormoneType::Cortisol);
        const double adre = Level(HormoneType::Adrenaline);
        const double mela = Level(HormoneType::Melatonin);
        const double sero = Level(HormoneType::Serotonin);
        const double dopa = Level(HormoneType::Dopamine);
        const double test = Level(HormoneType::Testosterone);

        if (cort > 70.0 && dopa < 35.0 && test < 35.0) {
            return LifeState::ChronicBurnout;
        }
        if (adre > 65.0 && cort > 55.0) {
            return LifeState::FightOrFlight;
        }
        if (mela > 65.0 && cort < 30.0 && adre < 20.0) {
            return LifeState::DeepRest;
        }
        if (dopa > 70.0 && test > 60.0 && cort < 45.0) {
            return LifeState::HighMotivation;
        }
        /*
         * NOTE: dopamine under 40, where the prototype had 30. Dopamine comes back fast, and after a burnout
         * it passed 30 just before cortisol fell under 70: the agent went from burnout straight to homeostasis
         * and was never lethargic, the low that follows exhaustion (endocrine_sim.exe). Under 40 it is, for a
         * few minutes, which State() needs to see it at all.
         */
        if (dopa < 40.0 && sero < 40.0 && adre < 25.0) {
            return LifeState::Lethargic;
        }
        return LifeState::Homeostasis;
    }

    /*
     * The life state the agent is in: the one Evaluate() reads from the levels, once it has read it for
     * kStateHoldSeconds of Live().
     *
     * NOTE: held, because while something goes on the levels hover around a threshold: in a failing task
     * adrenaline passes 65 at each failure and falls back between them, and the agent flickered between fight
     * or flight and homeostasis every few seconds, in the window and in the mood of consecutive steps.
     */
    LifeState State() const { return state_; }

    Mood CurrentMood() const {
        const LifeStateInfo& info = Info(state_);
        return {state_, info.note, info.temperature};
    }

    // When the agent was born: the first time its window opened.
    std::time_t Born()         const { return born_; }
    // Seconds lived: the time its window was open.
    double      AliveSeconds() const { return alive_seconds_; }
    double      SecondsIn(LifeState state) const { return state_seconds_[EnumIndex(state)]; }
    // The share of its life the agent spent in `state`, 0 to 1; 0 before it has lived at all.
    double      ShareOf(LifeState state) const {
        return alive_seconds_ > 0.0 ? state_seconds_[EnumIndex(state)] / alive_seconds_ : 0.0;
    }
    // How many times the agent has felt `feeling` in its life.
    long long   Count(Feeling feeling) const { return counts_[EnumIndex(feeling)]; }

    /*
     * The life as the file holds it: "key = value" lines, after a few lines of comment. The time is saved as
     * the save's.
     */
    std::string ToText(const std::string& agent_name) const {
        std::ostringstream out;
        out << "# The life of " << agent_name << ": its hormone levels, the seconds it spent in each life state\n"
               "# while its window was open, and how many times each feeling came. Written by agent_gui.exe as the\n"
               "# agent lives (src/bio/EndocrineSystem.hpp). Delete this file to give the agent a new life.\n"
               "born = " << static_cast<long long>(born_) << "\n"
               "saved = " << static_cast<long long>(std::time(nullptr)) << "\n"
            << std::fixed << std::setprecision(3);
        for (const Hormone& h : hormones_) {
            out << Info(h.Type()).key << " = " << h.Level() << "\n";
        }
        for (size_t i = 0; i < kLifeStateCount; ++i) {
            out << "seconds_" << kLifeStateInfo[i].key << " = " << state_seconds_[i] << "\n";
        }
        for (size_t i = 0; i < kFeelingCount; ++i) {
            out << "count_" << kFeelingInfo[i].key << " = " << counts_[i] << "\n";
        }
        return out.str();
    }

    /*
     * Reads what ToText wrote. A key that is missing, unknown or not a number leaves its value as it was, so a
     * file from an older or newer program still loads.
     */
    void FromText(const std::string& text) {
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) {
                continue;
            }
            const std::string key   = Trim(line.substr(0, eq));
            const std::string value = Trim(line.substr(eq + 1));
            char*             end   = nullptr;
            const double      n     = std::strtod(value.c_str(), &end);
            if (value.empty() || *end != '\0' || !(n >= 0.0)) {
                continue;  // not a number, or negative (or NaN): nothing here is negative
            }
            if (key == "born") {
                born_ = static_cast<std::time_t>(n);
            } else if (key == "saved") {
                saved_ = static_cast<std::time_t>(n);
            }
            for (Hormone& h : hormones_) {
                if (key == Info(h.Type()).key) {
                    h.Set(n);
                }
            }
            for (size_t i = 0; i < kLifeStateCount; ++i) {
                if (key == std::string("seconds_") + kLifeStateInfo[i].key) {
                    state_seconds_[i] = n;
                }
            }
            for (size_t i = 0; i < kFeelingCount; ++i) {
                if (key == std::string("count_") + kFeelingInfo[i].key) {
                    counts_[i] = static_cast<long long>(n);
                }
            }
        }
        alive_seconds_ = 0.0;
        for (const double s : state_seconds_) {
            alive_seconds_ += s;
        }
    }

    // Writes the life to `path` (ToText). False if it cannot.
    bool Save(const std::filesystem::path& path, const std::string& agent_name) const {
        return fileSystem.WriteFile(path, ToText(agent_name));
    }

    /*
     * Reads the life saved at `path`, then lets the agent rest for the time since it was saved. False, with a
     * newborn's body, when there is no file yet.
     */
    bool Load(const std::filesystem::path& path) {
        std::string text;
        if (!fileSystem.ReadFile(path, text)) {
            return false;
        }
        FromText(text);
        const double closed = std::difftime(std::time(nullptr), saved_);
        if (closed > 0.0) {
            Rest(closed * kSimHoursPerSecond);
        }
        state_        = Evaluate();
        held_seconds_ = 0.0;
        return true;
    }

private:
    std::array<Hormone, kHormoneCount>     hormones_;
    std::array<double, kLifeStateCount>    state_seconds_{};  // seconds lived in each state
    std::array<long long, kFeelingCount>   counts_{};         // times each feeling came
    double                                 alive_seconds_ = 0.0;  // their sum
    double                                 idle_seconds_  = 0.0;  // since the last feeling, or since it last worked
    std::time_t                            born_          = 0;
    std::time_t                            saved_         = 0;    // when the life read by Load was saved
    LifeState                              state_         = LifeState::Homeostasis;  // see State()
    LifeState                              pending_       = LifeState::Homeostasis;  // the other state read...
    double                                 held_seconds_  = 0.0;                     // ...for this long

    static std::string Trim(const std::string& s) {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }

    /*
     * One step of `dt` hours: how the hormones act on each other, then the drift to their baselines. The
     * feedback loops are the prototype's, per hour.
     *
     * NOTE: computed from a snapshot of the levels, so the order of the rules does not change the result.
     */
    void Step(double dt, bool sleepy) {
        std::array<double, kHormoneCount> delta{};
        const double cort = Level(HormoneType::Cortisol);
        const double adre = Level(HormoneType::Adrenaline);
        const double mela = Level(HormoneType::Melatonin);
        const double sero = Level(HormoneType::Serotonin);
        const double dopa = Level(HormoneType::Dopamine);
        const double insu = Level(HormoneType::Insulin);
        const auto   add  = [&delta](HormoneType type, double amount) { delta[EnumIndex(type)] += amount; };

        // Stress blocks sleep, lowers the mood, suppresses the anabolic hormones and raises blood sugar.
        if (cort > 60.0) {
            const double excess = (cort - 60.0) * 0.15;
            add(HormoneType::Melatonin,    -excess * 1.5);
            add(HormoneType::Serotonin,    -excess * 0.8);
            add(HormoneType::Testosterone, -excess * 0.6);
            add(HormoneType::Insulin,       excess * 0.5);
        }
        // Adrenaline sets off cortisol.
        if (adre > 50.0) {
            add(HormoneType::Cortisol, (adre - 50.0) * 0.25);
        }
        // Serotonin is melatonin's precursor; melatonin damps drive and stress.
        if (sero > 65.0) {
            add(HormoneType::Melatonin, (sero - 65.0) * 0.10);
        }
        if (mela > 55.0) {
            add(HormoneType::Dopamine, -(mela - 55.0) * 0.20);
            add(HormoneType::Cortisol, -(mela - 55.0) * 0.15);
        }
        // Drive feeds vitality, when stress is low.
        if (dopa > 70.0 && cort < 40.0) {
            add(HormoneType::Testosterone, 0.5);
        }
        // A spike of insulin lifts serotonin, and an excessive one brings the sluggishness after a heavy meal.
        if (insu > 65.0) {
            add(HormoneType::Serotonin, 1.5);
            add(HormoneType::Dopamine, -1.0);
        }
        if (sleepy) {
            add(HormoneType::Melatonin, kSleepPressure);
        }

        for (size_t i = 0; i < kHormoneCount; ++i) {
            hormones_[i].Adjust(delta[i] * dt);
            hormones_[i].Settle(dt);
        }
    }
};
