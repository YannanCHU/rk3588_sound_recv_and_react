// blocking_queue.hpp — 有界阻塞队列(mutex + 条件变量), 全模块唯一跨线程通道。
//
// 语义(与 design.md 3.3 数据流解耦原则对应):
//   push:   永不阻塞。队列满时丢弃最旧元素(dropped++), 返回 false。
//           —— 保护实时采集线程; 语音场景"新数据比旧数据有价值"。
//   pop:    阻塞直到有数据或队列被关闭; 空且关闭 → false。
//   pop_for: 带超时版本, 供工作线程周期性检查停止标志。
//   close:  幂等。唤醒全部等待者, 此后 push 静默丢弃。
//
// 内存: 元素以 move 进出, 无拷贝。AudioChunk(std::vector<float>) 进出即指针转移。
#ifndef SOUND_RECV_BLOCKING_QUEUE_HPP
#define SOUND_RECV_BLOCKING_QUEUE_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace sound_recv {

template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity)
        : capacity_(capacity)
    {
    }

    /// 推入元素。满则丢最旧; 返回 false 表示本次发生了丢弃; 队列已关闭则静默丢弃。
    bool push(T item)
    {
        bool dropped_now = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_) {
                return false;
            }
            if (items_.size() >= capacity_) {
                items_.pop_front();
                ++dropped_;
                dropped_now = true;
            }
            items_.push_back(std::move(item));
        }
        cv_.notify_one();
        return !dropped_now;
    }

    /// 阻塞弹出。队列空且已关闭 → false。
    bool pop(T& out)
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) {
            return false; // closed_ 且空
        }
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    /// 带超时弹出。超时或(空且关闭) → false。
    bool pop_for(T& out, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout, [this] { return !items_.empty() || closed_; })) {
            return false; // 超时
        }
        if (items_.empty()) {
            return false; // closed_ 且空
        }
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    /// 关闭队列(幂等), 唤醒全部等待者。
    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return items_.size();
    }

    /// 累计丢弃数(性能监控)。
    uint64_t dropped() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return dropped_;
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return closed_;
    }

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<T> items_;
    const size_t capacity_;
    uint64_t dropped_ = 0;
    bool closed_ = false;
};

} // namespace sound_recv

#endif // SOUND_RECV_BLOCKING_QUEUE_HPP
