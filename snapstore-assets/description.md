**Summary:** MIDI player for PC speakers, a RetroWave OPL3 board or a MIDI synth

---

A MIDI file player with three ways to hear your music. Pick one with the OUT button and switch at any time, even mid-song:

- **PC speakers**: authentic Yamaha OPL3 FM sound, the Sound Blaster-era chip of 90s PC games, synthesized with libADLMIDI and an accurate OPL3 emulator. No extra hardware needed.
- **RetroWave OPL3 Express board**: the same FM synthesis played on a real YMF262 chip over USB, register for register, for genuine hardware sound.
- **External MIDI synth**: the original MIDI (including SysEx) sent through ALSA to a General MIDI module or synth (Roland SC-55, Yamaha MU, etc.), which plays it with its own voices.

**Features**

- Dozens of built-in OPL3 instrument banks from classic games and sound drivers (AIL, HMI, DMX, Apogee, Windows 9x and more), switchable while a song plays
- Live 18-channel OPL3 visualizer in three styles (LED, neon, spectrum). Click a channel to mute it, right-click to solo it
- Winamp-style playlist: load whole folders (optionally with subfolders), shuffle, repeat one track or the whole list, click the progress bar to seek
- Tempo control, keyboard shortcuts, and settings that are remembered between runs

PC audio, display and home-folder access work automatically. For the board or an external synth, connect the matching interface once:

```
snap connect retro-midi-player-opl3-gm:serial-port   # RetroWave OPL3 board
snap connect retro-midi-player-opl3-gm:alsa          # external MIDI device
```
