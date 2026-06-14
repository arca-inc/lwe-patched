#include <SDL.h>
#include <cstdlib>
#include "CSound.h"

#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Render::Objects;

CSound::CSound (Wallpapers::CScene& scene, const Sound& sound) : CObject (scene, sound), m_sound (sound) {
    if (this->getContext ().getApp ().getContext ().settings.audio.enabled) {
	this->load ();
    }
}

CSound::~CSound () {
    // free all the sound buffers and streams
    for (const auto& stream : this->m_audioStreams) {
	this->getScene ().getAudioContext ().removeStream (stream.first);
	delete stream.second;
    }

    this->m_audioStreams.clear ();
}

void CSound::load () {
    if (this->m_sound.sounds.empty()) return;

    // Pick a random sound instead of playing all of them simultaneously
    int randomIndex = std::rand() % this->m_sound.sounds.size();
    const auto& cur = this->m_sound.sounds[randomIndex];

    auto stream
        = new Audio::AudioStream (this->getScene ().getAudioContext (), this->getAssetLocator ().read (cur));

    stream->setRepeat (this->m_sound.playbackmode.has_value () && this->m_sound.playbackmode == "loop");

    // add the stream to the context so it can be played
    this->m_audioStreams.insert_or_assign (this->getScene ().getAudioContext ().addStream (stream), stream);
}

void CSound::render () { }
