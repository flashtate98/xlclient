#include "Engine/Source/Model2/M2AnimatePool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace M2AnimatePool {
    namespace {
        // On the heap and never freed: the threads are detached and still waiting when the process's statics
        // are destroyed at exit (a condition variable destroyed under a waiter is undefined).
        struct State {
            std::vector<std::thread> threads;
            std::mutex mutex;                // the round's values, and the counts below
            std::condition_variable wake, done;
            std::mutex callers;              // one ForEach at a time; a second caller runs its own inline
        };
        State& S() {
            static State* s = new State();
            return *s;
        }

        std::once_flag s_started;
        void (*s_fn)(void*, uint32_t) = nullptr;
        void* s_param = nullptr;
        HPROPCONTEXT s_context = nullptr;
        uint32_t s_count = 0;
        std::atomic<uint32_t> s_next{ 0 };
        uint32_t s_left = 0;                 // items not finished
        int s_inside = 0;                    // threads taking items now
        unsigned s_round = 0;

        // Items until none are left; how many this call finished.
        uint32_t Take() {
            uint32_t done = 0;

            for (;;) {
                const uint32_t i = s_next.fetch_add(1);

                if (i >= s_count) {
                    return done;
                }

                s_fn(s_param, i);
                done++;
            }
        }

        void Main() {
            unsigned seen = 0;
            HPROPCONTEXT selected = nullptr;

            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(S().mutex);
                    S().wake.wait(lock, [&] { return s_round != seen; });
                    seen = s_round;
                    s_inside++;
                }

                // The client's per-thread state (Prop.h), as CM2Cache::ThreadProc selects it.
                if (s_context != selected) {
                    PropSelectContext(s_context);
                    selected = s_context;
                }

                const uint32_t done = Take();
                {
                    std::lock_guard<std::mutex> lock(S().mutex);
                    s_left -= done;
                    s_inside--;
                }
                S().done.notify_all();
            }
        }

        void Start() {
            // Physical cores, about: the main thread, the stock "Model2" thread, the GPU driver's and the mods'
            // workers keep theirs; at most 6 here.
            const unsigned cores = std::thread::hardware_concurrency();
            const int count = std::clamp(static_cast<int>(cores) / 2 - 2, 0, 6);

            try {
                for (int i = 0; i < count; i++) {
                    S().threads.emplace_back(Main);
                    S().threads.back().detach();   // they live as long as the process (the client never joins its own either)
                }
            } catch (...) {
                // Fewer (or none): the rest runs on the caller.
            }
        }
    }

    void ForEach(uint32_t count, void (*fn)(void* param, uint32_t i), void* param, HPROPCONTEXT context) {
        std::call_once(s_started, Start);
        std::unique_lock<std::mutex> caller(S().callers, std::try_to_lock);

        if (!caller.owns_lock() || S().threads.empty() || count < 2) {
            for (uint32_t i = 0; i < count; i++) {
                fn(param, i);
            }

            return;
        }

        {
            // Threads late from the last round leave first: then each reads this round's values under the lock.
            std::unique_lock<std::mutex> lock(S().mutex);
            S().done.wait(lock, [] { return s_inside == 0; });
            s_fn = fn;
            s_param = param;
            s_context = context;
            s_count = count;
            s_next.store(0);
            s_left = count;
            s_round++;
        }

        S().wake.notify_all();
        const uint32_t done = Take();
        std::unique_lock<std::mutex> lock(S().mutex);
        s_left -= done;
        S().done.wait(lock, [] { return s_left == 0 && s_inside == 0; });
    }

    int Threads() {
        return static_cast<int>(S().threads.size());
    }
}
