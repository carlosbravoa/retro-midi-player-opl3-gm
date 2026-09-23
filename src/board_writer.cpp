/*
 * board_writer.cpp — see board_writer.hpp.
 */
#include "board_writer.hpp"

extern "C" {
#include "retrowave_serial.h"
}

void BoardWriter::start()
{
    if (m_thread.joinable()) return;
    m_quit = false;
    m_thread = std::thread(&BoardWriter::run, this);
}

void BoardWriter::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_quit = true;
        m_q.clear();
    }
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void BoardWriter::submit(std::vector<uint8_t> &&bytes, Clock::time_point when)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_q.push_back(Chunk{ std::move(bytes), when, false });
    }
    m_cv.notify_one();
}

void BoardWriter::reset()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_q.clear();
        m_q.push_back(Chunk{ {}, Clock::now(), true });
    }
    m_cv.notify_one();
}

void BoardWriter::run()
{
    for (;;) {
        Chunk c;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this]{ return m_quit || !m_q.empty(); });
            if (m_quit) return;
            c = std::move(m_q.front());
            m_q.pop_front();
        }
        if (c.reset) { retrowave_reset(); continue; }
        // A chunk popped just before a reset() may still be sent; the queued
        // reset follows it, so the board still ends up silent.
        std::this_thread::sleep_until(c.when);
        if (!c.bytes.empty()) retrowave_send(c.bytes.data(), c.bytes.size());
    }
}
