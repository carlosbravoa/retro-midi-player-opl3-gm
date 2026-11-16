/*
 * durations.hpp — track lengths for the playlist, worked out in the background.
 *
 * Knowing how long a MIDI file lasts means parsing all of it, so a worker
 * thread does that with its own libADLMIDI player (never the one that is
 * playing) and caches the answers. The GUI just asks for the rows it is
 * showing; unknown ones are queued, most recently asked first.
 */
#ifndef DURATIONS_HPP
#define DURATIONS_HPP

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class DurationScanner {
public:
    ~DurationScanner() { stop(); }
    void stop();

    // Length of `path` in seconds; 0 if the file can't be read; -1 if not
    // known yet (it is then queued, and a later call will have it).
    double get(const std::string &path);

private:
    void run();

    std::thread             m_thread;
    std::mutex              m_mx;
    std::condition_variable m_cv;
    std::unordered_map<std::string, double> m_known;   // -1 = queued
    std::vector<std::string> m_queue;                  // back = next
    bool                    m_quit = false;
};

#endif /* DURATIONS_HPP */
