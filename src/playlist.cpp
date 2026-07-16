/*
 * playlist.cpp — see playlist.hpp.
 */
#include "playlist.hpp"

#include <algorithm>
#include <filesystem>
#include <cctype>

namespace fs = std::filesystem;

static std::string to_lower(std::string s)
{
    for(char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool is_midi(const fs::path &p)
{
    std::string ext = to_lower(p.extension().string());
    return ext == ".mid" || ext == ".midi";
}

static std::string stem_of(const std::string &path)
{
    return fs::path(path).stem().string();
}

// splitmix64 — deterministic, no global RNG, seedable per instance.
uint64_t Playlist::next_rand()
{
    uint64_t z = (m_rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Playlist::clear()
{
    m_storage.clear();
    m_tracks.clear();
    m_cursor = 0;
}

void Playlist::add_file(const std::string &path)
{
    std::string keep = current() ? current()->path : std::string();
    m_storage.push_back(Track{path, stem_of(path)});
    std::sort(m_storage.begin(), m_storage.end(),
              [](const Track &a, const Track &b){ return a.path < b.path; });
    rebuild_view(keep);
}

int Playlist::add_folder(const std::string &folder, bool recurse)
{
    std::string keep = current() ? current()->path : std::string();
    int added = 0;
    std::error_code ec;

    auto consider = [&](const fs::path &p){
        if(is_midi(p)){
            m_storage.push_back(Track{p.string(), p.stem().string()});
            ++added;
        }
    };

    if(recurse){
        for(fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
            it != end; it.increment(ec)){
            if(ec) break;
            if(it->is_regular_file(ec)) consider(it->path());
        }
    } else {
        for(fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
            it != end; it.increment(ec)){
            if(ec) break;
            if(it->is_regular_file(ec)) consider(it->path());
        }
    }

    std::sort(m_storage.begin(), m_storage.end(),
              [](const Track &a, const Track &b){ return a.path < b.path; });
    rebuild_view(keep);
    return added;
}

void Playlist::rebuild_view(const std::string &keep_current_path)
{
    m_tracks = m_storage;   // natural (sorted) order

    if(m_shuffle){
        // Fisher-Yates over the natural order.
        for(size_t i = m_tracks.size(); i > 1; --i){
            size_t j = (size_t)(next_rand() % i);
            std::swap(m_tracks[i - 1], m_tracks[j]);
        }
    }

    // Keep the previously-current track current across the reorder.
    m_cursor = 0;
    if(!keep_current_path.empty()){
        for(size_t i = 0; i < m_tracks.size(); ++i){
            if(m_tracks[i].path == keep_current_path){ m_cursor = (int)i; break; }
        }
    }
}

const Track *Playlist::current() const
{
    if(m_tracks.empty()) return nullptr;
    int c = m_cursor;
    if(c < 0 || c >= (int)m_tracks.size()) return nullptr;
    return &m_tracks[c];
}

int Playlist::current_display_index() const
{
    return m_tracks.empty() ? -1 : m_cursor;
}

bool Playlist::next(bool loop)
{
    if(m_tracks.empty()) return false;
    if(m_cursor + 1 < (int)m_tracks.size()){ ++m_cursor; return true; }
    if(loop){ m_cursor = 0; return true; }
    return false;   // past the end, not looping
}

bool Playlist::prev(bool loop)
{
    if(m_tracks.empty()) return false;
    if(m_cursor > 0){ --m_cursor; return true; }
    if(loop){ m_cursor = (int)m_tracks.size() - 1; return true; }
    return false;
}

void Playlist::select_display_index(int display_idx)
{
    if(display_idx < 0 || display_idx >= (int)m_tracks.size()) return;
    m_cursor = display_idx;
}

void Playlist::set_shuffle(bool on)
{
    if(on == m_shuffle) return;
    std::string keep = current() ? current()->path : std::string();
    m_shuffle = on;
    rebuild_view(keep);
}

void Playlist::reshuffle()
{
    if(!m_shuffle) return;
    std::string keep = current() ? current()->path : std::string();
    rebuild_view(keep);
}
