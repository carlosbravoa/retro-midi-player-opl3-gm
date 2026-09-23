/*
 * board_writer.hpp — paced delivery of OPL3 register writes to the RetroWave
 * board.
 *
 * adl_play() renders a whole audio block (~23 ms) in microseconds, so sending
 * its register writes straight to the serial port would deliver them in one
 * burst per block: up to a block of timing jitter on the board. Instead the
 * engine renders the block in short slices, collects each slice's framed bytes
 * (retrowave_take) and submits them here with a deadline. This thread owns the
 * serial port: it sleeps until each deadline and writes the whole slice with
 * one syscall, so the board hears the notes evenly spaced, ~1.5 ms apart.
 *
 * Resets are queued too, so a reset can never interleave with a frame
 * mid-write, and the 20 ms reset sleep runs here, not under the audio lock.
 */
#ifndef BOARD_WRITER_HPP
#define BOARD_WRITER_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class BoardWriter {
public:
    using Clock = std::chrono::steady_clock;

    void start();
    void stop();     // joins the thread; anything still queued is dropped

    // Queue framed bytes to be written at (or just after) `when`.
    void submit(std::vector<uint8_t> &&bytes, Clock::time_point when);

    // Drop everything queued and reset the chip (asynchronously, in order).
    void reset();

private:
    struct Chunk {
        std::vector<uint8_t> bytes;
        Clock::time_point    when;
        bool                 reset = false;
    };
    void run();

    std::thread             m_thread;
    std::mutex              m_mx;
    std::condition_variable m_cv;
    std::deque<Chunk>       m_q;
    bool                    m_quit = false;
};

#endif /* BOARD_WRITER_HPP */
