/*
 * playlist.cpp — see playlist.hpp.
 */
#include "playlist.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cctype>
#include <random>

namespace fs = std::filesystem;

static std::string to_lower(std::string s)
{
    for(char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool is_midi(const fs::path &p)
{
    std::string ext = to_lower(p.extension().string());
    return ext == ".mid" || ext == ".midi" || ext == ".kar" || ext == ".rmi";
}

// Absolute, normalised form, so the same file added twice (via "./x.mid" and
// "/home/.../x.mid", or the same folder re-opened) is recognised as one track.
static std::string norm_path(const fs::path &p)
{
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return (ec ? p : a).lexically_normal().string();
}

// Natural, case-insensitive order: digit runs compare as numbers, letters
// ignore case; exact bytes only break ties (keeping it a strict ordering).
static bool natural_less(const std::string &a, const std::string &b)
{
    auto dig = [](char c){ return std::isdigit((unsigned char)c) != 0; };
    size_t i = 0, j = 0;
    while(i < a.size() && j < b.size()){
        if(dig(a[i]) && dig(b[j])){
            size_t i0 = i, j0 = j;
            while(i0 < a.size() && a[i0] == '0') ++i0;   // leading zeros don't count
            while(j0 < b.size() && b[j0] == '0') ++j0;
            size_t i1 = i0, j1 = j0;
            while(i1 < a.size() && dig(a[i1])) ++i1;
            while(j1 < b.size() && dig(b[j1])) ++j1;
            if(i1 - i0 != j1 - j0) return i1 - i0 < j1 - j0;   // more digits = bigger
            int c = a.compare(i0, i1 - i0, b, j0, j1 - j0);
            if(c) return c < 0;
            i = i1; j = j1;
            continue;
        }
        int x = std::tolower((unsigned char)a[i]), y = std::tolower((unsigned char)b[j]);
        if(x != y) return x < y;
        ++i; ++j;
    }
    bool a_done = i >= a.size(), b_done = j >= b.size();
    if(a_done != b_done) return a_done;
    return a < b;
}

Playlist::Playlist()
{
    std::random_device rd;
    m_rng_state ^= ((uint64_t)rd() << 32) ^ rd()
                 ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
}

void Playlist::sort_storage()
{
    std::sort(m_storage.begin(), m_storage.end(),
              [](const Track &a, const Track &b){ return natural_less(a.path, b.path); });
}

// splitmix64 — no global RNG, one state per instance.
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
    m_paths.clear();
    m_cursor = 0;
}

int Playlist::add_files(const std::vector<std::string> &paths)
{
    std::string keep = current() ? current()->path : std::string();
    int added = 0;
    for(const std::string &path : paths){
        std::string n = norm_path(path);
        if(!m_paths.insert(n).second) continue;   // already listed
        m_storage.push_back(Track{n, fs::path(n).stem().string()});
        ++added;
    }
    if(added){
        sort_storage();
        rebuild_view(keep);
    }
    return added;
}

int Playlist::add_folder(const std::string &folder, bool recurse)
{
    std::string keep = current() ? current()->path : std::string();
    int added = 0;
    std::error_code ec;

    auto consider = [&](const fs::path &p){
        if(!is_midi(p)) return;
        std::string n = norm_path(p);
        if(!m_paths.insert(n).second) return;   // already listed
        m_storage.push_back(Track{n, p.stem().string()});
        ++added;
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

    if(added){
        sort_storage();
        rebuild_view(keep);
    }
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

    // Keep the previously-current track current across the reorder. In
    // shuffle it goes to the front, so the whole rest of the list follows it
    // (left mid-list, everything shuffled ahead of it would never play).
    m_cursor = 0;
    if(!keep_current_path.empty()){
        for(size_t i = 0; i < m_tracks.size(); ++i){
            if(m_tracks[i].path != keep_current_path) continue;
            if(m_shuffle) std::swap(m_tracks[0], m_tracks[i]);
            else          m_cursor = (int)i;
            break;
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
    if(loop){
        if(m_shuffle && m_tracks.size() > 1){
            // New lap, new order — but don't open it with the track that
            // just finished.
            std::string last = m_tracks.back().path;
            rebuild_view(std::string());
            if(m_tracks[0].path == last) std::swap(m_tracks[0], m_tracks.back());
        }
        m_cursor = 0;
        return true;
    }
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

bool Playlist::remove_display_index(int display_idx)
{
    if(display_idx < 0 || display_idx >= (int)m_tracks.size()) return false;
    const std::string path = m_tracks[display_idx].path;
    m_tracks.erase(m_tracks.begin() + display_idx);
    m_paths.erase(path);
    for(size_t i = 0; i < m_storage.size(); ++i)
        if(m_storage[i].path == path){ m_storage.erase(m_storage.begin() + (long)i); break; }

    bool was_current = (display_idx == m_cursor);
    if(display_idx < m_cursor) --m_cursor;
    if(m_cursor >= (int)m_tracks.size()) m_cursor = m_tracks.empty() ? 0 : (int)m_tracks.size() - 1;
    return was_current;
}

void Playlist::set_shuffle(bool on)
{
    if(on == m_shuffle) return;
    std::string keep = current() ? current()->path : std::string();
    m_shuffle = on;
    rebuild_view(keep);
}

void Playlist::reshuffle(bool keep_current)
{
    if(!m_shuffle) return;
    std::string keep = (keep_current && current()) ? current()->path : std::string();
    rebuild_view(keep);
}
