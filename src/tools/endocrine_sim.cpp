/*
 * Console simulator of the agent's body (bio/EndocrineSystem.hpp), to tune the hormones without running a model:
 * no Ollama, no window, a few milliseconds.
 *
 * It plays the first prototype's scenarios (hormones moved by hand, hours at a time), then a working session
 * paced as the window lives it, in seconds of the wall clock: good chats, tasks that finish, the model not
 * replying, tasks that fail, a long run of failures, a long idle. After each part it prints the hormones, the
 * life state and its mood; at the end, the share of the session spent in each state, and a check that the life
 * reads back from its file format unchanged.
 *
 * Usage: bin\endocrine_sim.exe
 */

#include <cstdio>
#include <string>

#include "bio/EndocrineSystem.hpp"

namespace {

void PrintLevels(const EndocrineSystem& body) {
    for (size_t i = 0; i < kHormoneCount; ++i) {
        std::printf(" %s %5.1f", kHormoneInfo[i].key, body.Level(static_cast<HormoneType>(i)));
    }
    std::printf("\n");
}

void PrintStatus(const EndocrineSystem& body, const char* label) {
    const Mood mood = body.CurrentMood();
    std::printf("=== %s ===\n", label);
    std::printf("Life state: %s", Info(mood.state).name);
    if (mood.temperature > 0.0) {
        std::printf("  (temperature %.1f)", mood.temperature);
    }
    std::printf("\n ");
    PrintLevels(body);
    std::printf("\n");
}

// The life state last printed by Wait, so a change a feeling made between two waits is printed too.
LifeState g_shown = LifeState::Homeostasis;

/*
 * `seconds` of the wall clock, a second at a time as the window's frames would (each second is counted in the
 * state it starts in). Each change of life state is printed with the minute of the session it was seen at.
 */
void Wait(EndocrineSystem& body, int seconds, bool busy) {
    for (int i = 0; i < seconds; ++i) {
        const LifeState now = body.State();
        if (now != g_shown) {
            std::printf("    min %5.1f: %s -> %s\n", body.AliveSeconds() / 60.0, Info(g_shown).name, Info(now).name);
            g_shown = now;
        }
        body.Live(1.0, busy);
    }
}

// Failures 15 s apart, from a newborn, until the levels read fight or flight: how many it takes.
int FailuresToFightOrFlight() {
    EndocrineSystem body;
    for (int n = 1; n <= 50; ++n) {
        body.Feel(Feeling::Failed);
        if (body.Evaluate() == LifeState::FightOrFlight) {
            return n;
        }
        for (int i = 0; i < 15; ++i) {
            body.Live(1.0, true);
        }
    }
    return -1;
}

// One chat message: the user asks, the agent searches, then answers, then the user reads for a minute.
void GoodChat(EndocrineSystem& body) {
    body.Feel(Feeling::Asked);
    Wait(body, 20, true);
    body.Feel(Feeling::Found);
    Wait(body, 20, true);
    body.Feel(Feeling::Answered);
    Wait(body, 60, false);
}

/*
 * A Run task of `steps` steps of a minute each, with `did` tools that work and `failed` that fail per step, a
 * repeated call and a step with nothing new every other step when it fails. It ends with finish when `done`.
 */
void Task(EndocrineSystem& body, int steps, int did, int failed, bool done) {
    body.Feel(Feeling::GoalGiven);
    for (int step = 1; step <= steps; ++step) {
        for (int i = 0; i < did; ++i) {
            Wait(body, 15, true);
            body.Feel(Feeling::Did);
        }
        for (int i = 0; i < failed; ++i) {
            Wait(body, 15, true);
            body.Feel(Feeling::Failed);
        }
        if (failed > 0 && step % 2 == 0) {
            body.Feel(Feeling::Repeated);
            body.Feel(Feeling::Stuck);
        }
        Wait(body, 60 - 15 * (did + failed), true);
    }
    body.Feel(done ? Feeling::GoalDone : Feeling::GoalMissed);
    Wait(body, 30, false);
}

// The first prototype's scenarios, unchanged: hormones moved by hand, time in hours.
void PrototypeScenarios() {
    EndocrineSystem body;
    PrintStatus(body, "Prototype: initial morning state");

    // An acute stressor, such as sudden danger or a high-pressure deadline.
    body.Stimulate(HormoneType::Adrenaline, 65.0);
    body.Stimulate(HormoneType::Cortisol, 35.0);
    body.Update(1.0);
    PrintStatus(body, "Prototype: after a sudden threat (1 h)");

    // Time passes and the stress lingers, with no recovery.
    for (int i = 0; i < 4; ++i) {
        body.Stimulate(HormoneType::Cortisol, 15.0);
        body.Update(1.0);
    }
    PrintStatus(body, "Prototype: after prolonged stress (4 h)");

    // Winding down at night: darkness, a meal, relaxation.
    body.Stimulate(HormoneType::Melatonin, 60.0);
    body.Stimulate(HormoneType::Serotonin, 25.0);
    body.Update(2.0);
    PrintStatus(body, "Prototype: nighttime recovery (2 h)");
}

void PrintLife(const EndocrineSystem& body) {
    std::printf("=== The session: %.0f minutes lived ===\n", body.AliveSeconds() / 60.0);
    for (size_t i = 0; i < kLifeStateCount; ++i) {
        const LifeState state = static_cast<LifeState>(i);
        std::printf("  %-16s %5.1f%%  %5.1f min\n", kLifeStateInfo[i].name, body.ShareOf(state) * 100.0,
                    body.SecondsIn(state) / 60.0);
    }
    std::printf("  Feelings:");
    for (size_t i = 0; i < kFeelingCount; ++i) {
        std::printf(" %s %lld", kFeelingInfo[i].key, body.Count(static_cast<Feeling>(i)));
    }
    std::printf("\n\n");
}

// The life written as its file holds it and read back must write the same, but for the time of the save.
bool ReadsBack(const EndocrineSystem& body) {
    const auto without_saved = [](std::string text) {
        const size_t at = text.find("\nsaved = ");
        return text.erase(at, text.find('\n', at + 1) - at);
    };
    EndocrineSystem copy;
    copy.FromText(body.ToText("sim"));
    return without_saved(copy.ToText("sim")) == without_saved(body.ToText("sim"));
}

}  // namespace

int main() {
    PrototypeScenarios();
    std::printf("Failures 15 s apart that bring fight or flight to a newborn: %d\n\n", FailuresToFightOrFlight());

    EndocrineSystem body;
    PrintStatus(body, "Session: newborn");

    for (int i = 0; i < 3; ++i) {
        GoodChat(body);
    }
    PrintStatus(body, "Session: three good chat messages (5 min)");

    Task(body, 3, 2, 0, true);
    PrintStatus(body, "Session: a task finished in 3 steps, 2 tools working per step");

    body.Feel(Feeling::Asked);
    Wait(body, 5, true);
    body.Feel(Feeling::NoReply);
    Wait(body, 10, false);
    body.Feel(Feeling::Asked);
    Wait(body, 5, true);
    body.Feel(Feeling::NoReply);
    Wait(body, 10, false);
    body.Feel(Feeling::Asked);
    Wait(body, 5, true);
    body.Feel(Feeling::NoReply);
    PrintStatus(body, "Session: the model did not reply, three times in 45 s");

    Wait(body, 5 * 60, false);
    PrintStatus(body, "Session: 5 min later");

    Task(body, 6, 0, 2, false);
    PrintStatus(body, "Session: a task that failed for 6 steps, 2 failures per step");

    for (int i = 0; i < 3; ++i) {
        Task(body, 8, 0, 2, false);
    }
    PrintStatus(body, "Session: three more failed tasks in a row (25 min)");

    Wait(body, 10 * 60, false);
    PrintStatus(body, "Session: idle for 10 min");

    Wait(body, 30 * 60, false);
    PrintStatus(body, "Session: idle for 40 min");

    PrintLife(body);
    const bool same = ReadsBack(body);
    std::printf("Life file: %s\n", same ? "reads back unchanged" : "DOES NOT read back the same");
    return same ? 0 : 1;
}
