#pragma once

// Decodes replacement images on worker threads so the render thread never waits for a PNG. The renderer submits a job
// when it first wants an image, keeps drawing the original picture, and takes finished images a few at a time at
// frame boundaries, which bounds the upload work any one frame does.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dw3/texrep/pack.h"

namespace dw3::texrep {

class ImageLoader {
public:
    // Fills the image; returns false and describes the problem in `error` when it cannot. Runs on a worker thread.
    using Job = std::function<bool(Image& out, std::string& error)>;

    struct Result {
        uint64_t ticket = 0;
        bool ok = false;
        Image image;
        std::string error;
    };

    explicit ImageLoader(unsigned worker_threads);
    ~ImageLoader();
    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // Queues a job and returns its ticket. Jobs start in submission order.
    uint64_t submit(Job job);

    // Moves finished results out, oldest finish first, until `byte_budget` bytes of RGBA pixels are taken. Always
    // takes at least one when any is ready, so an image larger than the budget still gets through.
    std::vector<Result> take(std::size_t byte_budget);

    // Jobs submitted whose result has not been taken yet.
    std::size_t outstanding() const;

    // Blocks until every submitted job has finished (tests and shutdown).
    void wait_idle();

    // Worker count for this machine: a quarter of the cores, between one and three, so emulation keeps its own core.
    static unsigned default_worker_count();

private:
    struct Pending {
        uint64_t ticket;
        Job job;
    };
    void work();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Pending> queue_;
    std::deque<Result> done_;
    uint64_t next_ticket_ = 1;
    std::size_t running_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace dw3::texrep
