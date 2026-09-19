// 基础设施单元测试: BlockingQueue 语义 + PCM 转换 + Event 类型契约。
#include "test_framework.hpp"

#include "blocking_queue.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <thread>

using namespace sound_recv;

// ---------- BlockingQueue ----------

// 1. 基本 push/pop 与 FIFO 顺序
static void test_queue_fifo()
{
    BlockingQueue<int> q(4);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    int v = 0;
    CHECK(q.pop(v) && v == 1);
    CHECK(q.pop(v) && v == 2);
    CHECK(q.pop(v) && v == 3);
    CHECK(q.size() == 0);
}

// 2. 有界丢弃: 容量 2, 推 3 个 → 最旧被丢, push 返回 false, dropped()==1
static void test_queue_drop_oldest()
{
    BlockingQueue<int> q(2);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3) == false); // 发生丢弃
    CHECK_EQ(q.dropped(), 1u);
    int v = 0;
    CHECK(q.pop(v) && v == 2);
    CHECK(q.pop(v) && v == 3);
}

// 3. 阻塞 pop 被生产者唤醒
static void test_queue_blocking_wakeup()
{
    BlockingQueue<int> q(4);
    std::atomic<bool> popped{false};
    std::thread consumer([&] {
        int v = 0;
        if (q.pop(v)) {
            popped = true;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.push(42); // 必须唤醒阻塞中的消费者
    consumer.join();
    CHECK(popped.load());
}

// 4. pop_for 超时返回 false
static void test_queue_pop_timeout()
{
    BlockingQueue<int> q(4);
    int v = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = q.pop_for(v, std::chrono::milliseconds(80));
    auto dt = std::chrono::steady_clock::now() - t0;
    CHECK(!ok);
    CHECK(dt >= std::chrono::milliseconds(70));
}

// 5. close() 唤醒所有阻塞消费者, 空队列 pop 返回 false
static void test_queue_close_wakes()
{
    BlockingQueue<int> q(4);
    std::atomic<int> results{0};
    auto consumer = [&] {
        int v = 0;
        if (!q.pop(v)) {
            results++;
        }
    };
    std::thread c1(consumer);
    std::thread c2(consumer);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    c1.join();
    c2.join();
    CHECK_EQ(results.load(), 2);
    CHECK(q.closed());
    // 关闭后的 push 静默丢弃, 不崩溃
    CHECK(q.push(9) == false);
}

// 6. move-only 类型可通过队列(move 语义, 无拷贝)
static void test_queue_move_only()
{
    BlockingQueue<std::unique_ptr<int>> q(2);
    q.push(std::make_unique<int>(7));
    std::unique_ptr<int> out;
    CHECK(q.pop(out));
    CHECK(out != nullptr && *out == 7);
}

// 7. 多生产者不丢数据(丢弃计数为 0 的场景)
static void test_queue_multi_producer()
{
    BlockingQueue<int> q(1024);
    constexpr int kPerProducer = 500;
    auto producer = [&](int base) {
        for (int i = 0; i < kPerProducer; ++i) {
            q.push(base + i);
        }
    };
    std::thread p1(producer, 0);
    std::thread p2(producer, 100000);
    std::set<int> seen;
    std::thread consumer([&] {
        int v = 0;
        while (seen.size() < 2u * kPerProducer) {
            if (q.pop_for(v, std::chrono::milliseconds(200))) {
                seen.insert(v);
            }
        }
    });
    p1.join();
    p2.join();
    consumer.join();
    CHECK_EQ(seen.size(), 2u * kPerProducer);
    CHECK_EQ(q.dropped(), 0u);
}

// ---------- PCM 转换 ----------

static void test_pcm_conversion()
{
    // 边界值: -32768 → -1.0, 0 → 0.0, 32767 → ~0.99997
    const int16_t in[3] = {-32768, 0, 32767};
    auto out = pcm_s16_to_float(in, 3);
    CHECK_EQ(out.size(), 3u);
    CHECK(out[0] == -1.0f);
    CHECK(out[1] == 0.0f);
    CHECK(out[2] > 0.9999f && out[2] <= 1.0f);

    // 空输入
    auto empty = pcm_s16_to_float(nullptr, 0);
    CHECK(empty.empty());
}

// ---------- Event 类型契约 ----------

static void test_event_contract()
{
    // AudioChunk: move 构造后源数据为空(确认指针转移而非拷贝)
    std::vector<float> big(1024, 0.5f);
    float* raw = big.data();
    Event e = make_event(EventType::AudioChunk, AudioChunkData{std::move(big)});
    CHECK(big.empty()); // move 生效
    auto* d = event_data<AudioChunkData>(e);
    CHECK(d != nullptr);
    CHECK_EQ(d->samples.size(), 1024u);
    CHECK(d->samples.data() == raw); // 同一块内存

    // TextRecognized
    Event t = make_event(EventType::TextRecognized, TextRecognizedData{u8"你好世界", 12.5});
    auto* td = event_data<TextRecognizedData>(t);
    CHECK(td != nullptr && td->text == u8"你好世界" && td->latency_ms == 12.5);

    // 类型不匹配 get_if → nullptr
    CHECK(event_data<LlmTokenData>(t) == nullptr);

    // LlmTokenDoneData 字段
    Event done = make_event(EventType::LlmDone, LlmTokenDoneData{u8"全文", 88});
    auto* dd = event_data<LlmTokenDoneData>(done);
    CHECK(dd != nullptr && dd->total_tokens == 88);

    // 时间戳为正
    CHECK(e.ts > 0.0);
}

void run_infra_tests()
{
    test_queue_fifo();
    test_queue_drop_oldest();
    test_queue_blocking_wakeup();
    test_queue_pop_timeout();
    test_queue_close_wakes();
    test_queue_move_only();
    test_queue_multi_producer();
    test_pcm_conversion();
    test_event_contract();
}
