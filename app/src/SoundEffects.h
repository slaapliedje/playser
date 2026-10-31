// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QTimer>
#include <cstdint>

struct SDL_AudioStream;

// Plays short PCM clips (a disc menu's click sounds) through SDL's audio,
// beside mpv's own output. The output is opened for a sound and closed once
// it has played, so nothing holds the audio device between sounds (mpv's
// passthrough needs it to itself).
class SoundEffects : public QObject
{
    Q_OBJECT

public:
    explicit SoundEffects(QObject *parent = nullptr);
    ~SoundEffects() override;

    // 48 kHz 16-bit PCM, interleaved if stereo, at `gain` (1: as recorded).
    // Cuts off a sound still playing.
    void play(const int16_t *pcm, int frames, int channels, float gain);

private:
    void closeOutput();

    bool m_audio = false; // SDL's audio subsystem is up
    SDL_AudioStream *m_stream = nullptr;
    int m_channels = 0;
    QTimer m_closeTimer;
};
