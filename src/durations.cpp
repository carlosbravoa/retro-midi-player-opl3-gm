/*
 * durations.cpp — see durations.hpp.
 */
#include "durations.hpp"

#include <adlmidi.h>

double DurationScanner::get(const std::string &path)
{
    std::lock_guard<std::mutex> lk(m_mx);
    auto it = m_known.find(path);
    if (it != m_known.end()) return it->second;
    m_known[path] = -1.0;
    m_queue.push_back(path);
    if (!m_thread.joinable()) m_thread = std::thread(&DurationScanner::run, this);
    m_cv.notify_one();
    return -1.0;
}

void DurationScanner::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_quit = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void DurationScanner::run()
{
    ADL_MIDIPlayer *adl = adl_init(44100);
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this]{ return m_quit || !m_queue.empty(); });
            if (m_quit) break;
            path = std::move(m_queue.back());   // newest request: what's on screen now
            m_queue.pop_back();
        }
        double len = 0.0;
        if (adl && adl_openFile(adl, path.c_str()) == 0) {
            len = adl_totalTimeLength(adl);
            if (len < 0.0) len = 0.0;
        }
        std::lock_guard<std::mutex> lk(m_mx);
        m_known[path] = len;
    }
    if (adl) adl_close(adl);
}
