#include "PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Logging/Log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <glm/common.hpp>

float movetowards (float current, float target, float maxDelta) {
    if (abs (target - current) <= maxDelta) {
	return target;
    }

    return current + glm::sign (target - current) * maxDelta;
}

namespace WallpaperEngine::Audio::Drivers::Recorders {
void pa_stream_notify_cb (pa_stream* stream, void* /*userdata*/) {
    switch (pa_stream_get_state (stream)) {
	case PA_STREAM_FAILED:
	    sLog.error ("Cannot open stream for capture. Audio processing is disabled");
	    break;
	case PA_STREAM_READY:
	    sLog.debug ("Capture stream ready");
	    break;
	default:
	    break;
    }
}

void pa_stream_read_cb (pa_stream* stream, const size_t /*nbytes*/, void* userdata) {
    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    // Careful when to pa_stream_peek() and pa_stream_drop()!
    // c.f. https://www.freedesktop.org/software/pulseaudio/doxygen/stream_8h.html#ac2838c449cde56e169224d7fe3d00824
    const uint8_t* data = nullptr;
    size_t currentSize;
    if (pa_stream_peek (stream, reinterpret_cast<const void**> (&data), &currentSize) != 0) {
	sLog.error ("Failed to peek at stream data...");
	return;
    }

    if (data == nullptr && currentSize == 0) {
	// No data in the buffer, ignore.
	return;
    }

    if (data == nullptr && currentSize > 0) {
	// Hole in the buffer. We must drop it.
	if (pa_stream_drop (stream) != 0) {
	    sLog.error ("Failed to drop a hole while capturing!");
	    return;
	}
    } else if (currentSize > 0 && data) {
	const size_t dataToCopy = std::min (currentSize, WAVE_BUFFER_SIZE - recorder->currentWritePointer);

	// depending on the amount of data available, we might want to read one or multiple frames
	const size_t end = recorder->currentWritePointer + dataToCopy;

	// this packet will fill the buffer, perform some extra checks for extra full buffers and get the latest one
	if (end == WAVE_BUFFER_SIZE) {
	    if (const size_t numberOfFullBuffers = (currentSize - dataToCopy) / WAVE_BUFFER_SIZE;
		numberOfFullBuffers > 0) {
		// calculate the start of the last block (we need the end of the previous block, hence the - 1)
		const size_t startOfLastBuffer = std::max (
		    dataToCopy + (numberOfFullBuffers - 1) * WAVE_BUFFER_SIZE, currentSize - WAVE_BUFFER_SIZE
		);
		// copy directly into the final buffer
		memcpy (recorder->audioBuffer, &data[startOfLastBuffer], WAVE_BUFFER_SIZE * sizeof (uint8_t));
		// copy whatever is left to the read/write buffer
		recorder->currentWritePointer = currentSize - startOfLastBuffer - WAVE_BUFFER_SIZE;
		memcpy (
		    recorder->audioBufferTmp, &data[startOfLastBuffer + WAVE_BUFFER_SIZE],
		    recorder->currentWritePointer * sizeof (uint8_t)
		);
	    } else {
		// okay, no full extra packets available, copy the rest of the data and flip the buffers
		memcpy (&recorder->audioBufferTmp[recorder->currentWritePointer], data, dataToCopy * sizeof (uint8_t));
		uint8_t* tmp = recorder->audioBuffer;
		recorder->audioBuffer = recorder->audioBufferTmp;
		recorder->audioBufferTmp = tmp;
		// reset write pointer
		recorder->currentWritePointer = 0;
	    }

	    // signal a new frame is ready
	    recorder->fullFrameReady = true;
	} else {
	    // copy over available data to the tmp buffer and everything should be set
	    memcpy (&recorder->audioBufferTmp[recorder->currentWritePointer], data, dataToCopy * sizeof (uint8_t));
	    recorder->currentWritePointer += dataToCopy;
	}
    }

    if (pa_stream_drop (stream) != 0) {
	sLog.error ("Failed to drop data after peeking");
    }
}

// (Re)bind the capture stream to monitor source `monitor`, optionally filtered to a
// single application via its sink-input index (sinkInputIdx >= 0). Idempotent: a no-op
// when already bound to the same target, so it is safe to call from frequent PulseAudio
// subscribe events without churning the stream. An empty `monitor` goes silent.
void applyCapture (
    pa_context* ctx, PulseAudioPlaybackRecorder::PulseAudioData* rec, const std::string& monitor, int sinkInputIdx
) {
    if (monitor == rec->currentMonitor && sinkInputIdx == rec->currentSinkInput) {
	return;
    }

    if (rec->captureStream) {
	pa_stream_set_read_callback (rec->captureStream, nullptr, nullptr);
	pa_stream_disconnect (rec->captureStream);
	pa_stream_unref (rec->captureStream);
	rec->captureStream = nullptr;
    }
    rec->currentMonitor = monitor;
    rec->currentSinkInput = sinkInputIdx;

    if (monitor.empty ()) {
	sLog.out ("Audio capture: no matching source yet (silent until it appears)");
	return;
    }

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_U8;
    spec.rate = 44100;
    spec.channels = 1;

    rec->captureStream = pa_stream_new (ctx, "output monitor", &spec, nullptr);
    pa_stream_set_state_callback (rec->captureStream, &pa_stream_notify_cb, rec);
    pa_stream_set_read_callback (rec->captureStream, &pa_stream_read_cb, rec);
    // Filter the monitor to a single application's playback stream. Must be set before
    // connecting. The monitor must be the monitor of the sink that sink-input plays on.
    if (sinkInputIdx >= 0) {
	pa_stream_set_monitor_stream (rec->captureStream, static_cast<uint32_t> (sinkInputIdx));
    }

    pa_buffer_attr attr {};
    size_t bytesPerSec = pa_bytes_per_second (&spec);
    attr.fragsize = bytesPerSec * 10 / 100;             // ~10 ms latency
    attr.maxlength = attr.fragsize + bytesPerSec * 750 / 100; // ~750 ms max buffered

    if (pa_stream_connect_record (rec->captureStream, monitor.c_str (), &attr, PA_STREAM_ADJUST_LATENCY) != 0) {
	sLog.error ("Failed to connect audio capture to ", monitor);
    } else {
	sLog.out ("Audio capture source: ", monitor, sinkInputIdx >= 0 ? " (application stream)" : "");
    }
}

// Default-output mode: capture the monitor of the current default sink.
void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata) {
    if (info == nullptr) {
	return;
    }
    auto* rec = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);
    if (info->default_sink_name != nullptr && info->default_sink_name[0] != '\0') {
	applyCapture (ctx, rec, std::string (info->default_sink_name) + ".monitor", -1);
    } else {
	sLog.error ("PulseAudio reported no default sink; cannot pick an audio capture source");
    }
}

// Transient state for the two-step app resolution (sink-input list -> its sink's monitor).
struct AppCapture {
    PulseAudioPlaybackRecorder::PulseAudioData* rec;
    std::string app;
    bool found = false;
    uint32_t sinkInputIdx = 0;
    uint32_t sinkIdx = 0;
};

bool icontains (const std::string& haystack, const std::string& needle) {
    if (needle.empty ()) {
	return false;
    }
    const auto it = std::search (
	haystack.begin (), haystack.end (), needle.begin (), needle.end (),
	[] (char a, char b) { return std::tolower ((unsigned char) a) == std::tolower ((unsigned char) b); }
    );
    return it != haystack.end ();
}

// Step 2: we have the app's sink-input + its sink; bind to that sink's monitor, filtered.
void app_sink_info_cb (pa_context* ctx, const pa_sink_info* i, int eol, void* userdata) {
    auto* ac = static_cast<AppCapture*> (userdata);
    if (eol > 0) {
	delete ac;
	return;
    }
    if (i == nullptr) {
	return;
    }
    const std::string monitor
	= (i->monitor_source_name != nullptr) ? i->monitor_source_name : std::string (i->name) + ".monitor";
    applyCapture (ctx, ac->rec, monitor, static_cast<int> (ac->sinkInputIdx));
}

// Step 1: scan playback streams for one whose app name/binary matches the target.
void app_sink_input_cb (pa_context* ctx, const pa_sink_input_info* i, int eol, void* userdata) {
    auto* ac = static_cast<AppCapture*> (userdata);
    if (eol > 0) {
	if (ac->found) {
	    pa_operation* o = pa_context_get_sink_info_by_index (ctx, ac->sinkIdx, app_sink_info_cb, ac);
	    if (o != nullptr) {
		pa_operation_unref (o);
		return; // ac ownership passed to app_sink_info_cb
	    }
	}
	// Not playing right now (or the query failed): go silent and wait for it to appear.
	applyCapture (ctx, ac->rec, "", -1);
	delete ac;
	return;
    }
    if (i == nullptr || ac->found) {
	return;
    }
    const char* appName = pa_proplist_gets (i->proplist, PA_PROP_APPLICATION_NAME);
    const char* binary = pa_proplist_gets (i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    const std::string haystack = std::string (appName != nullptr ? appName : "") + "\n"
	+ std::string (binary != nullptr ? binary : "") + "\n" + std::string (i->name != nullptr ? i->name : "");
    if (icontains (haystack, ac->app)) {
	ac->found = true;
	ac->sinkInputIdx = i->index;
	ac->sinkIdx = i->sink;
    }
}

// Resolve the capture target and (re)bind. Dispatches on rec->target:
//   ""            -> default output sink monitor (follows default changes)
//   "app:<name>"  -> only that application's stream (follows it starting/stopping)
//   <source name> -> that monitor source verbatim
void reconnectCapture (pa_context* ctx, PulseAudioPlaybackRecorder::PulseAudioData* rec) {
    const std::string& t = rec->target;
    if (t.rfind ("app:", 0) == 0) {
	auto* ac = new AppCapture { .rec = rec, .app = t.substr (4) };
	pa_operation* o = pa_context_get_sink_input_info_list (ctx, app_sink_input_cb, ac);
	if (o != nullptr) {
	    pa_operation_unref (o);
	} else {
	    delete ac;
	}
    } else if (!t.empty ()) {
	applyCapture (ctx, rec, t, -1);
    } else {
	pa_operation* o = pa_context_get_server_info (ctx, pa_server_info_cb, rec);
	if (o != nullptr) {
	    pa_operation_unref (o);
	}
    }
}

void pa_context_subscribe_cb (pa_context* ctx, pa_subscription_event_type_t t, uint32_t /*idx*/, void* userdata) {
    auto* rec = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);
    const unsigned facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    const unsigned type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    const bool appMode = rec->target.rfind ("app:", 0) == 0;

    // Only re-resolve on events that can change our chosen source. applyCapture is
    // idempotent, so a default/sink event that resolves to the same monitor is a no-op.
    bool relevant = facility == PA_SUBSCRIPTION_EVENT_SERVER || facility == PA_SUBSCRIPTION_EVENT_SINK;
    if (appMode && facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT
	&& (type == PA_SUBSCRIPTION_EVENT_NEW || type == PA_SUBSCRIPTION_EVENT_REMOVE)) {
	relevant = true; // the target app started or stopped playing
    }
    if (relevant) {
	reconnectCapture (ctx, rec);
    }
}

void pa_context_notify_cb (pa_context* ctx, void* userdata) {
    switch (pa_context_get_state (ctx)) {
	case PA_CONTEXT_READY:
	    {
		auto* rec = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);
		pa_context_set_subscribe_callback (ctx, pa_context_subscribe_cb, userdata);
		// SERVER: default-sink changes (a server event, not a sink event). SINK_INPUT:
		// the target app starting/stopping when capturing a single application.
		pa_operation* o = pa_context_subscribe (
		    ctx,
		    static_cast<pa_subscription_mask_t> (
			PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER
			| PA_SUBSCRIPTION_MASK_SINK_INPUT
		    ),
		    nullptr, nullptr
		);
		if (o != nullptr) {
		    pa_operation_unref (o);
		}

		reconnectCapture (ctx, rec);
		break;
	    }
	case PA_CONTEXT_FAILED:
	    sLog.error ("PulseAudio context initialization failed. Audio processing is disabled");
	    break;
	default:
	    break;
    }
}

PulseAudioPlaybackRecorder::PulseAudioPlaybackRecorder () :
    m_captureData (
	{ .kisscfg = kiss_fftr_alloc (WAVE_BUFFER_SIZE, 0, nullptr, nullptr),
	  .audioBuffer = new uint8_t[WAVE_BUFFER_SIZE],
	  .audioBufferTmp = new uint8_t[WAVE_BUFFER_SIZE] }
    ) {
    // Capture target, resolved when the context becomes ready (see reconnectCapture).
    if (const char* dev = std::getenv ("LWE_AUDIO_DEVICE"); dev != nullptr) {
	this->m_captureData.target = dev;
    }

    this->m_mainloop = pa_mainloop_new ();
    this->m_mainloopApi = pa_mainloop_get_api (this->m_mainloop);
    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine-audioprocessing");

    pa_context_set_state_callback (this->m_context, &pa_context_notify_cb, &this->m_captureData);

    if (pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
	sLog.error ("PulseAudio connection failed! Audio processing is disabled");
	return;
    }

    // wait until the context is ready
    while (pa_context_get_state (this->m_context) != PA_CONTEXT_READY) {
	pa_mainloop_iterate (this->m_mainloop, 1, nullptr);
    }
}

PulseAudioPlaybackRecorder::~PulseAudioPlaybackRecorder () {
    if (m_captureData.captureStream) {
	pa_stream_unref (m_captureData.captureStream);
    }

    delete[] this->m_captureData.audioBufferTmp;
    delete[] this->m_captureData.audioBuffer;
    free (this->m_captureData.kisscfg);

    pa_context_disconnect (this->m_context);
    pa_context_unref (this->m_context);
    pa_mainloop_free (this->m_mainloop);
}

void PulseAudioPlaybackRecorder::update () {
    pa_mainloop_iterate (this->m_mainloop, 0, nullptr);

    // interpolate current values to the destination
    for (int i = 0; i < 64; i++) {
	this->audio64[i] = movetowards (this->audio64[i], this->m_FFTdestination64[i], 0.3f);
	if (i >= 32) {
	    continue;
	}
	this->audio32[i] = movetowards (this->audio32[i], this->m_FFTdestination32[i], 0.3f);
	if (i >= 16) {
	    continue;
	}
	this->audio16[i] = movetowards (this->audio16[i], this->m_FFTdestination16[i], 0.3f);
    }

    if (!this->m_captureData.fullFrameReady) {
	return;
    }

    this->m_captureData.fullFrameReady = false;

    // convert audio data to deltas so the fft library can properly handle it
    for (int i = 0; i < WAVE_BUFFER_SIZE; i++) {
	this->m_audioFFTbuffer[i] = (this->m_captureData.audioBuffer[i] - 128) / 128.0f;
    }

    // perform full fft pass
    kiss_fftr (this->m_captureData.kisscfg, this->m_audioFFTbuffer, this->m_FFTinfo);

    // now reduce to the different bands
    // use just one for loop to produce all 3
    for (int band = 0; band < 64; band++) {
	int index = band * 2;
	float f1 = this->m_FFTinfo[index].r;
	float f2 = this->m_FFTinfo[index].i;
	f2 = f1 * f1 + f2 * f2; // magnitude
	f1 = 0.0f;

	if (f2 > 0.0f) {
	    f1 = 0.35f * log10 (f2);
	}

	this->m_FFTdestination64[band]
	    = fmin (1.0f, f1 * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 63.0f) * 1.0f - 0.5f)));
	this->m_FFTdestination32[band >> 1]
	    = fmin (1.0f, f1 * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 31.0f) * 1.0f - 0.5f)));
	this->m_FFTdestination16[band >> 2]
	    = fmin (1.0f, f1 * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 15.0f) * 1.0f - 0.5f)));
    }
}

} // namespace WallpaperEngine::Audio::Drivers::Recorders