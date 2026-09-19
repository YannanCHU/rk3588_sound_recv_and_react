// event_bus.hpp — 进程内发布/订阅事件总线(无调度线程版)。
//
// 与 design.md 3.3 原四线程 EventBus 的差异(会话修订, 详见 docs/plans/):
//   publish() 在生产者线程内同步执行 fan-out, 把事件 move 进各匹配订阅者的
//   有界队列; 不存在专职调度线程。跨线程唤醒次数与点对点队列相同, 延迟更低。
//
// 线程安全: subscribe 与 publish 可并发(publish 持注册表锁, 无竞争时纳秒级)。
// 约定: subscribe 应集中在初始化阶段(main 拉起线程之前)完成, 运行期不再变更。
//
// 内存注意: 同一事件类型若有 N 个订阅者, 前 N-1 份为拷贝、最后一份为 move。
// AudioChunk 类大载荷事件在当前阶段仅有一个消费者(ASR), 无拷贝开销;
// 未来多消费者(如 TTS)接入时再评估载荷共享方案。
#ifndef SOUND_RECV_EVENT_BUS_HPP
#define SOUND_RECV_EVENT_BUS_HPP

#include "blocking_queue.hpp"
#include "types.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sound_recv {

class EventBus {
public:
    using Queue = BlockingQueue<Event>;

    EventBus() = default;
    ~EventBus() = default;

    /// 订阅一组事件类型, 返回该订阅者的专属队列指针(归 bus 所有, 生命周期同 bus)。
    /// capacity: 该订阅者队列容量(满则丢最旧)。音频类事件建议 ≥ 消费者一秒消费块数。
    Queue* subscribe(EventType type, size_t capacity = 16)
    {
        std::lock_guard<std::mutex> lk(registry_mu_);
        auto q = std::make_unique<Queue>(capacity);
        Queue* raw = q.get();
        owned_.push_back(std::move(q));
        subscribers_[static_cast<size_t>(type)].push_back(raw);
        return raw;
    }

    /// 发布事件: 同步 move 进该类型全部订阅者队列(每个队列独立执行丢最旧策略)。
    void publish(Event e)
    {
        std::lock_guard<std::mutex> lk(registry_mu_);
        auto& subs = subscribers_[static_cast<size_t>(e.type)];
        for (size_t i = 0; i < subs.size(); ++i) {
            if (i + 1 == subs.size()) {
                subs[i]->push(std::move(e)); // 最后一份 move
            } else {
                subs[i]->push(e); // 其余拷贝(当前阶段各类型均为单订阅者, 不触发)
            }
        }
    }

    /// 关停: 关闭全部订阅队列, 解除所有阻塞消费者(进程退出路径)。
    void close_all()
    {
        std::lock_guard<std::mutex> lk(registry_mu_);
        for (auto& q : owned_) {
            q->close();
        }
    }

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

private:
    static constexpr size_t kTypeCount = static_cast<size_t>(EventType::Error) + 1;

    std::mutex registry_mu_;
    std::vector<std::unique_ptr<Queue>> owned_;
    std::array<std::vector<Queue*>, kTypeCount> subscribers_;
};

} // namespace sound_recv

#endif // SOUND_RECV_EVENT_BUS_HPP
