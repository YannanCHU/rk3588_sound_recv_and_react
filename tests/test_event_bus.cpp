// EventBus 单元测试: 订阅/发布路由、多订阅者、丢弃策略、关停语义。
#include "test_framework.hpp"

#include "event_bus.hpp"

#include <atomic>
#include <thread>

using namespace sound_recv;

// 1. 订阅的类型能收到, 未订阅的类型收不到
static void test_basic_routing()
{
    EventBus bus;
    auto* asr_q = bus.subscribe(EventType::AudioChunk, 8);
    bus.publish(make_event(EventType::SpeechStart, SpeechStartData{1.0}));
    bus.publish(make_event(EventType::AudioChunk, AudioChunkData{{1.0f, 2.0f}}));
    Event e;
    CHECK(asr_q->pop_for(e, std::chrono::milliseconds(100)));
    CHECK(e.type == EventType::AudioChunk);
    CHECK_EQ(event_data<AudioChunkData>(e)->samples.size(), 2u);
    // SpeechStart 未被订阅 → 队列里只有 1 条
    CHECK(!asr_q->pop_for(e, std::chrono::milliseconds(20)));
}

// 2. 同一类型两个订阅者都收到(拷贝语义)
static void test_two_subscribers()
{
    EventBus bus;
    auto* qa = bus.subscribe(EventType::LlmToken, 4);
    auto* qb = bus.subscribe(EventType::LlmToken, 4);
    bus.publish(make_event(EventType::LlmToken, LlmTokenData{u8"你", 0}));
    Event ea, eb;
    CHECK(qa->pop_for(ea, std::chrono::milliseconds(100)));
    CHECK(qb->pop_for(eb, std::chrono::milliseconds(100)));
    CHECK(event_data<LlmTokenData>(ea)->token == event_data<LlmTokenData>(eb)->token);
}

// 3. 经总线的丢弃策略: 容量 2 的订阅队列, 发布 3 条 → 最旧被丢
static void test_drop_policy_via_bus()
{
    EventBus bus;
    auto* q = bus.subscribe(EventType::TextRecognized, 2);
    for (int i = 0; i < 3; ++i) {
        bus.publish(make_event(EventType::TextRecognized, TextRecognizedData{std::to_string(i), 0.0}));
    }
    CHECK_EQ(q->dropped(), 1u);
    Event e;
    CHECK(q->pop(e));
    CHECK(event_data<TextRecognizedData>(e)->text == "1");
    CHECK(q->pop(e));
    CHECK(event_data<TextRecognizedData>(e)->text == "2");
    CHECK(!q->pop_for(e, std::chrono::milliseconds(20)));
}

// 4. close_all 解除所有订阅者阻塞
static void test_close_all()
{
    EventBus bus;
    auto* q1 = bus.subscribe(EventType::AudioChunk, 4);
    auto* q2 = bus.subscribe(EventType::TextRecognized, 4);
    std::atomic<int> woken{0};
    std::thread c1([&] { Event e; if (!q1->pop(e)) woken++; });
    std::thread c2([&] { Event e; if (!q2->pop(e)) woken++; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bus.close_all();
    c1.join();
    c2.join();
    CHECK_EQ(woken.load(), 2);
    // close_all 后 publish 静默丢弃
    bus.publish(make_event(EventType::AudioChunk, AudioChunkData{{0.1f}}));
    CHECK(q1->closed());
}

// 5. 关闭后的订阅队列 pop 立即返回 false(空)
static void test_pop_after_close()
{
    EventBus bus;
    auto* q = bus.subscribe(EventType::Error, 2);
    bus.publish(make_event(EventType::Error, ErrorData{u8"x"}));
    bus.close_all();
    Event e;
    CHECK(q->pop(e)); // close 前残留数据仍可取
    CHECK(!q->pop(e));
}

void run_event_bus_tests()
{
    test_basic_routing();
    test_two_subscribers();
    test_drop_policy_via_bus();
    test_close_all();
    test_pop_after_close();
}
