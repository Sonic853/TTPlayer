#include "ttplayer/playlist/playback_order.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace ttplayer::playlist {
namespace {
using Round = std::shared_ptr<const std::vector<size_t>>;
using Rounds = std::array<Round, 3>;
struct Cancelled {};

// No Track, path, decoder or window objects ever enter the worker.
Round BuildRound(size_t count, std::mt19937& generator,
                 const std::atomic<std::uint64_t>& generation, std::uint64_t ticket,
                 Round left = {}, Round right = {}, Round other = {},
                 std::optional<size_t> anchor = {}) {
    auto result = std::make_shared<std::vector<size_t>>();
    result->reserve(count);
    const auto check = [&] {
        if (generation.load(std::memory_order_relaxed) != ticket) throw Cancelled{};
    };
    for (size_t row = 0; row < count; ++row) {
        if ((row & 1023) == 0) check();
        result->push_back(row);
    }
    const auto acceptable = [&] {
        if (left && result->front() == left->back()) return false;
        if (right && result->back() == right->front()) return false;
        // For two tracks, avoiding a repeated boundary requires AB|AB.
        return count < 3 || ((!left || *result != *left) &&
            (!right || *result != *right) && (!other || *result != *other));
    };
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        for (size_t row = 1; row < count; ++row) {
            if ((row & 1023) == 0) check();
            std::uniform_int_distribution<size_t> pick(0, row);
            std::swap((*result)[row], (*result)[pick(generator)]);
        }
        if (anchor) std::iter_swap(result->begin(),
            std::find(result->begin(), result->end(), *anchor));
        if (left && result->front() == left->back())
            std::swap((*result)[0], (*result)[1]);
        if (right && result->back() == right->front())
            std::swap((*result)[count - 1], (*result)[count - 2]);
        if (acceptable()) return result;
    }
    // Six permutations suffice to avoid two retained orders. For N >= 4
    // leave the constrained endpoint fixed; for N == 3 check all six.
    const size_t first = count >= 4 && left ? 1 : 0;
    std::sort(result->begin() + first, result->begin() + first + 3);
    do {
        if (acceptable()) return result;
    } while (std::next_permutation(result->begin() + first,
                                  result->begin() + first + 3));
    throw Cancelled{}; // N < 3 always succeeds above.
}
}

struct RandomPlaybackOrder::State {
    struct Job {
        std::uint64_t generation{};
        size_t count{};
        int slot{-1}; // -1 initializes; 0/2 refill the missing neighboring round.
        std::optional<size_t> anchor;
        Rounds retained;
    };
    struct Result {
        Rounds rounds;
        bool failed{};
    };
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping{};
    std::atomic<std::uint64_t> generation{};
    std::optional<Job> job;
    std::optional<Result> result;
    // UI-owned fields. The worker only sees Job snapshots.
    Rounds rounds;
    size_t count{};
    std::optional<size_t> expected;
    std::int64_t position{-1};
    bool pending{};
    bool failed{};
    std::thread worker;

    State() : worker([this] { Run(); }) {}
    ~State() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            ++generation;
            job.reset();
        }
        condition.notify_all();
        worker.join();
    }
    void Run() noexcept {
        std::mt19937 generator;
        try { generator.seed(std::random_device{}()); } catch (...) {}
        for (;;) {
            Job work;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || job.has_value(); });
                if (stopping) return;
                work = std::move(*job);
                job.reset();
            }
            Result completed;
            try {
                auto& r = completed.rounds;
                r = std::move(work.retained);
                if (work.slot < 0) {
                    r[1] = BuildRound(work.count, generator, generation,
                                     work.generation, {}, {}, {}, work.anchor);
                    if (work.count <= kThreeRoundLimit) {
                        r[0] = BuildRound(work.count, generator, generation,
                                         work.generation, {}, r[1]);
                        r[2] = BuildRound(work.count, generator, generation,
                                         work.generation, r[1], {}, r[0]);
                    }
                } else if (work.slot == 0) {
                    r[0] = BuildRound(work.count, generator, generation,
                                     work.generation, {}, r[1], r[2]);
                } else {
                    r[2] = BuildRound(work.count, generator, generation,
                                     work.generation, r[1], {}, r[0]);
                }
            } catch (...) {
                completed.rounds = {};
                completed.failed = true;
            }
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                if (work.generation == generation.load(std::memory_order_relaxed))
                    result = std::move(completed);
            }
            condition.notify_all();
        }
    }
    void Clear() noexcept {
        std::lock_guard lock(mutex);
        ++generation;
        job.reset();
        result.reset();
        rounds = {};
        count = 0;
        expected.reset();
        position = -1;
        pending = failed = false;
    }
    void Start(int slot) {
        std::lock_guard lock(mutex);
        pending = true;
        job = Job{++generation, count, slot, expected, rounds};
        result.reset();
        condition.notify_one();
    }
    void Collect() {
        std::lock_guard lock(mutex);
        if (!result) return;
        rounds = std::move(result->rounds);
        failed = result->failed;
        pending = false;
        result.reset();
    }
};

RandomPlaybackOrder::RandomPlaybackOrder() = default;
RandomPlaybackOrder::~RandomPlaybackOrder() = default;

void RandomPlaybackOrder::Reset() noexcept {
    if (state_) state_->Clear();
}

void RandomPlaybackOrder::SetContext(std::uint64_t source, std::uint64_t revision) {
    if (source_ == source && revision_ == revision) return;
    source_ = source;
    revision_ = revision;
    Reset();
}

void RandomPlaybackOrder::Prepare(size_t count, std::optional<size_t> playing) {
    if (count < 2) {
        if (state_ && state_->count) Reset();
        return;
    }
    if (playing && *playing >= count) playing.reset();
    if (!state_) state_ = std::make_unique<State>();
    auto& s = *state_;
    if (s.count == count && s.expected == playing) return;
    s.Clear();
    s.count = count;
    s.expected = playing;
    s.position = playing ? 0 : -1;
    s.Start(-1);
}

bool RandomPlaybackOrder::AtEnd(size_t count) const noexcept {
    return count && state_ && state_->count == count &&
        state_->position == static_cast<std::int64_t>(count - 1);
}

RandomPlaybackOrder::Selection RandomPlaybackOrder::TrySelect(
    size_t count, std::optional<size_t> playing, Request request) {
    Prepare(count, playing);
    if (count < 2) return {count ? std::optional<size_t>{0} : std::nullopt};
    auto& s = *state_;
    s.Collect();
    if (s.failed) return {}; // Never silently substitute ordered playback.
    if (!s.rounds[1]) return {{}, true};
    const bool previous = request == Request::previous;
    const bool preview = request == Request::initialize_preview || request == Request::preview_next;
    auto position = s.position + (previous ? -1 : 1);
    if (position < 0 || position >= static_cast<std::int64_t>(count)) {
        if (count > kThreeRoundLimit) {
            position = previous ? static_cast<std::int64_t>(count - 1) : 0;
        } else {
            const size_t slot = previous ? 0 : 2;
            if (!s.rounds[slot]) return {{}, true};
            const auto row = previous ? s.rounds[0]->back() : s.rounds[2]->front();
            if (preview) return {row};
            // Refill results contain both retained rounds. Collect before any
            // further rotation so an older orientation cannot overwrite this one.
            if (s.pending) return {{}, true};
            if (previous) {
                s.rounds[2] = std::move(s.rounds[1]);
                s.rounds[1] = std::move(s.rounds[0]);
                s.position = static_cast<std::int64_t>(count - 1);
                s.expected = row;
                s.Start(0);
            } else {
                s.rounds[0] = std::move(s.rounds[1]);
                s.rounds[1] = std::move(s.rounds[2]);
                s.position = 0;
                s.expected = row;
                s.Start(2);
            }
            return {row};
        }
    }
    const auto row = (*s.rounds[1])[static_cast<size_t>(position)];
    if (!preview) { s.position = position; s.expected = row; }
    return {row};
}

std::optional<size_t> RandomPlaybackOrder::Select(
    size_t count, std::optional<size_t> playing, Request request) {
    for (;;) {
        const auto selection = TrySelect(count, playing, request);
        if (!selection.pending) return selection.row;
        std::unique_lock lock(state_->mutex);
        state_->condition.wait(lock, [&] { return state_->result.has_value(); });
    }
}
} // namespace ttplayer::playlist
