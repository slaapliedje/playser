#include "SoundEffects.h"

#include <SDL3/SDL.h>

namespace {
constexpr int kRate = 48000;
constexpr int kCloseAfterMs = 300; // past the sound's end, for the device's buffer
}

SoundEffects::SoundEffects(QObject *parent) : QObject(parent)
{
    m_closeTimer.setSingleShot(true);
    connect(&m_closeTimer, &QTimer::timeout, this, &SoundEffects::closeOutput);
}

SoundEffects::~SoundEffects()
{
    closeOutput();
    if (m_audio)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SoundEffects::play(const int16_t *pcm, int frames, int channels, float gain)
{
    if (frames <= 0 || channels < 1 || channels > 2)
        return;
    if (!m_audio) {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Disc menu sounds");
        m_audio = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!m_audio) {
            qWarning("sound effects: no audio: %s", SDL_GetError());
            return;
        }
    }
    if (m_stream && channels != m_channels)
        closeOutput();
    if (!m_stream) {
        const SDL_AudioSpec spec = { SDL_AUDIO_S16, channels, kRate };
        m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!m_stream) {
            qWarning("sound effects: can't open the output: %s", SDL_GetError());
            return;
        }
        m_channels = channels;
    }
    SDL_ClearAudioStream(m_stream);
    SDL_SetAudioStreamGain(m_stream, gain);
    SDL_PutAudioStreamData(m_stream, pcm, frames * channels * int(sizeof(int16_t)));
    SDL_ResumeAudioStreamDevice(m_stream);
    m_closeTimer.start(int(qint64(frames) * 1000 / kRate) + kCloseAfterMs);
}

void SoundEffects::closeOutput()
{
    m_closeTimer.stop();
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
}
