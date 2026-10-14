#include "dw3/texrep/image_loader.h"

#include <algorithm>

namespace dw3::texrep {

ImageLoader::ImageLoader(unsigned worker_threads) {
    for (unsigned i = 0; i < std::max(1u, worker_threads); ++i) workers_.emplace_back([this] { work(); });
}

ImageLoader::~ImageLoader() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

unsigned ImageLoader::default_worker_count() {
    return std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 3u);
}

uint64_t ImageLoader::submit(Job job) {
    uint64_t ticket;
    {
        std::lock_guard lock(mutex_);
        ticket = next_ticket_++;
        queue_.push_back({ticket, std::move(job)});
    }
    wake_.notify_one();
    return ticket;
}

std::vector<ImageLoader::Result> ImageLoader::take(std::size_t byte_budget) {
    std::vector<Result> out;
    std::lock_guard lock(mutex_);
    std::size_t used = 0;
    while (!done_.empty()) {
        const std::size_t bytes = done_.front().image.rgba.size() * sizeof(uint32_t);
        if (!out.empty() && used + bytes > byte_budget) break;
        used += bytes;
        out.push_back(std::move(done_.front()));
        done_.pop_front();
    }
    return out;
}

std::size_t ImageLoader::outstanding() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + running_ + done_.size();
}

void ImageLoader::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
}

void ImageLoader::work() {
    for (;;) {
        Pending item;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        Result result;
        result.ticket = item.ticket;
        result.ok = item.job(result.image, result.error);
        {
            std::lock_guard lock(mutex_);
            done_.push_back(std::move(result));
            --running_;
            if (queue_.empty() && running_ == 0) idle_.notify_all();
        }
    }
}

}  // namespace dw3::texrep
