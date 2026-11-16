/*
 * playlist.hpp — the set of MIDI files to play and the order to play them in.
 *
 * A Playlist is a flat list of tracks discovered by scanning one or more
 * folders (optionally recursing into subfolders) for Standard MIDI Files
 * (.mid / .midi / .kar / .rmi, case-insensitive). It owns two orderings: the natural
 * (sorted) order and a shuffled order. Toggling shuffle never loses your place
 * — the currently playing track stays current, the rest are reordered around
 * it. Navigation (next / prev) walks whichever order is active.
 *
 * The model holds no audio state; the engine asks it "what plays now / next".
 */
#ifndef PLAYLIST_HPP
#define PLAYLIST_HPP

#include <string>
#include <unordered_set>
#include <vector>
#include <cstdint>

struct Track {
    std::string path;      // absolute path to the .mid/.midi file
    std::string name;      // basename without extension, for display
};

class Playlist {
public:
    Playlist();   // seeds the shuffle RNG, so every run shuffles differently

    // Scan `folder` for MIDI files. If recurse, descend into subfolders.
    // Appends to the current list (call clear() first to replace), skipping
    // files already listed. Returns the number of tracks added. Tracks are
    // kept in natural, case-insensitive path order ("Track 2" before
    // "Track 10").
    int add_folder(const std::string &folder, bool recurse);

    // Add files (any extension — the caller vouches they are MIDIs), skipping
    // any already listed. Returns the number added. One sort for the batch.
    int  add_files(const std::vector<std::string> &paths);
    void add_file(const std::string &path) { add_files({ path }); }

    void clear();

    // Drop one track by display position. Returns true if it was the current
    // track (the cursor then rests on the one that followed it, if any).
    bool remove_display_index(int display_idx);

    bool   empty() const { return m_tracks.empty(); }
    size_t size()  const { return m_tracks.size(); }

    // Tracks in *display* order (natural when unshuffled, shuffled otherwise).
    // Index into this with display positions used by the GUI list.
    const std::vector<Track> &tracks() const { return m_tracks; }

    // Current track (in underlying storage). Returns nullptr if empty.
    const Track *current() const;
    int  current_display_index() const;   // position in tracks() view, or -1

    // Move the cursor. Wraps only when `loop` is true; otherwise next() past
    // the end returns false (playlist finished) and leaves the cursor put.
    bool next(bool loop);
    bool prev(bool loop);

    // Jump straight to a display-order position (e.g. user clicked a row).
    void select_display_index(int display_idx);

    // Shuffle control. Toggling on builds a random order with the current
    // track moved to the front, so every other track is still to come;
    // toggling off restores natural order with the same track still current.
    // In shuffle, next() wrapping around (loop) re-rolls the order.
    void set_shuffle(bool on);
    bool shuffle() const { return m_shuffle; }

    // Re-roll the shuffle order. keep_current pins the current track to the
    // front; pass false before playback starts so the first track is random
    // too. No-op if the list isn't shuffled.
    void reshuffle(bool keep_current = true);

private:
    // Storage is stable; m_order maps display position -> storage index.
    std::vector<Track>    m_storage;   // natural (sorted) order
    std::vector<Track>    m_tracks;    // display order (view; rebuilt on toggle)
    std::unordered_set<std::string> m_paths;   // what m_storage holds (dedupe)
    bool                  m_shuffle = false;
    int                   m_cursor  = 0;   // index into m_tracks (display order)
    uint64_t              m_rng_state = 0x9E3779B97F4A7C15ull;

    uint64_t next_rand();
    void sort_storage();
    void rebuild_view(const std::string &keep_current_path);
};

#endif /* PLAYLIST_HPP */
