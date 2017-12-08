#include "AudioManager.h"
#include <sstream>
#include <algorithm>
#include <wx/wx.h>
#include <wx/string.h>
#include <wx/ffile.h>
#include <wx/log.h>
#include <math.h>
#include <stdlib.h>
#include "kiss_fft/tools/kiss_fftr.h"
#include <log4cpp/Category.hh>

using namespace Vamp;

// SDL Functions
SDL __sdl;
int __globalVolume = 100;
int AudioData::__nextId = 0;

#ifndef __WXOSX__
#define DEFAULT_NUM_SAMPLES 1024
#define DEFAULT_RATE 44100
#else
//OSX recommendation is to keep the number of samples buffered very small at 256
//Also recommend sampling to 48000 as that's what the drivers do internally anyway
#define DEFAULT_NUM_SAMPLES 256
#define RESAMPLE_RATE 48000
#define DEFAULT_RATE RESAMPLE_RATE
#endif

void fill_audio(void *udata, Uint8 *stream, int len)
{
    //SDL 2.0
    SDL_memset(stream, 0, len);
    
    std::mutex *audio_lock = (std::mutex*)udata;

    std::unique_lock<std::mutex> locker(*audio_lock);

    auto media = __sdl.GetAudio();

    for (auto it = media.begin(); it != media.end(); ++it)
    {
        if ((*it)->_audio_len == 0 || (*it)->_paused)		/*  Only  play  if  we  have  data  left and not paused */
        {
            // no data left
        }
        else
        {
            len = (len > (*it)->_audio_len ? (*it)->_audio_len : len);	/*  Mix  as  much  data  as  possible  */
            int volume = (*it)->_volume;
            if (__globalVolume != 100)
            {
                volume = (volume * __globalVolume) / 100;
            }
            SDL_MixAudio(stream, (*it)->_audio_pos, len, volume);
            (*it)->_audio_pos += len;
            (*it)->_audio_len -= len;
        }
    }
}

void SDL::SetGlobalVolume(int volume)
{
    __globalVolume = volume;
}

int SDL::GetGlobalVolume() const
{
    return __globalVolume;
}

SDL::SDL(const std::string& device)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    _state = SDLSTATE::SDLUNINITIALISED;
    _playbackrate = 1.0f;

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        logger_base.error("Could not initialize SDL");
        return;
    }

    _device = device;
    _state = SDLSTATE::SDLINITIALISED;
    _initialisedRate = DEFAULT_RATE;

    _wanted_spec.freq = _initialisedRate * _playbackrate;
    _wanted_spec.format = AUDIO_S16SYS;
    _wanted_spec.channels = 2;
    _wanted_spec.silence = 0;
    _wanted_spec.samples = DEFAULT_NUM_SAMPLES;
    _wanted_spec.callback = fill_audio;
    _wanted_spec.userdata = &_audio_Lock;

    if (!OpenAudioDevice(device))
    {
        logger_base.error("Could not open SDL audio");
        return;
    }

    _state = SDLSTATE::SDLOPENED;
    logger_base.debug("SDL initialised");
}

SDL::~SDL()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    if (_state != SDLSTATE::SDLOPENED && _state != SDLSTATE::SDLINITIALISED && _state != SDLSTATE::SDLUNINITIALISED)
    {
        Stop();
    }
    if (_state != SDLSTATE::SDLINITIALISED && _state != SDLSTATE::SDLUNINITIALISED)
    {
        SDL_CloseAudio();
    }
    if (_state != SDLSTATE::SDLUNINITIALISED)
    {
        SDL_Quit();
    }

    std::unique_lock<std::mutex> locker(_audio_Lock);

    while (_audioData.size() >0)
    {
        auto toremove = _audioData.front();
        _audioData.remove(toremove);
        delete toremove;
    }

    logger_base.debug("SDL uninitialised");
}

long SDL::Tell(int id)
{
    std::unique_lock<std::mutex> locker(_audio_Lock);

    auto d=  GetData(id);

    if (d == nullptr) return 0;

    return d->Tell(); // amount of track size played
}

void SDL::Seek(int id, long pos)
{
    std::unique_lock<std::mutex> locker(_audio_Lock);

    auto d = GetData(id);

    if (d == nullptr) return;

    d->Seek(pos);
}

bool SDL::HasAudio(int id)
{
    return GetData(id) != nullptr;
}

std::list<std::string> SDL::GetAudioDevices() const
{
    std::list<std::string> devices;

#ifndef __WXMSW__
    // Only windows supports multiple audio devices ... I think .. well at least I know Linux doesnt
    return devices;
#endif

    int count = SDL_GetNumAudioDevices(0);

    for (int i = 0; i < count; i++)
    {
        devices.push_back(SDL_GetAudioDeviceName(i, 0));
    }

    return devices;
}

bool SDL::OpenAudioDevice(const std::string device)
{
#ifndef __WXMSW__
    // Only windows supports multiple audio devices ... I think .. well at least I know Linux doesnt
    device = "";
#endif

    if (_state != SDLSTATE::SDLOPENED && _state != SDLSTATE::SDLINITIALISED && _state != SDLSTATE::SDLUNINITIALISED)
    {
        Stop();
    }

    if (_state != SDLSTATE::SDLINITIALISED && _state != SDLSTATE::SDLUNINITIALISED)
    {
        SDL_CloseAudio();
    }

    if (device == "")
    {
        if (SDL_OpenAudio(&_wanted_spec, nullptr) < 0)
        {
            return false;
        }
    }
    else
    {
        if (SDL_OpenAudioDevice(device.c_str(), 0, &_wanted_spec, nullptr, 0) <= 0)
        {
            return false;
        }
    }

    return true;
}

void SDL::SeekAndLimitPlayLength(int id, long pos, long len)
{
    std::unique_lock<std::mutex> locker(_audio_Lock);

    auto d = GetData(id);

    if (d == nullptr) return;
    
    d->SeekAndLimitPlayLength(pos, len);
}

void SDL::Reopen()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    SDLSTATE oldstate = _state;

    if (_state == SDLSTATE::SDLUNINITIALISED || _state == SDLSTATE::SDLINITIALISED) return;

    if (_state == SDLSTATE::SDLPLAYING) Stop();

    std::unique_lock<std::mutex> locker(_audio_Lock);

    for (auto it = _audioData.begin(); it != _audioData.end(); ++it)
    {
        (*it)->SavePos();
    }

    SDL_CloseAudio();

    _state = SDLSTATE::SDLINITIALISED;

    //SDL_AudioSpec
    _wanted_spec.freq = _initialisedRate * _playbackrate;
    _wanted_spec.format = AUDIO_S16SYS;
    _wanted_spec.channels = 2;
    _wanted_spec.silence = 0;
    _wanted_spec.samples = DEFAULT_NUM_SAMPLES;
    _wanted_spec.callback = fill_audio;
    _wanted_spec.userdata = &_audio_Lock;

    logger_base.info("Starting audio with frequency %d.", _wanted_spec.freq);

    if (!OpenAudioDevice(_device))
    {
        // a problem
    }
    else
    {
        _state = SDLSTATE::SDLINITIALISED;

        for (auto it = _audioData.begin(); it != _audioData.end(); ++it)
        {
            (*it)->RestorePos();
        }

        if (oldstate == SDLSTATE::SDLPLAYING)
        {
            Play();
        }
    }

    logger_base.info("SDL reinitialised.");
}

int SDL::GetVolume(int id)
{
    std::unique_lock<std::mutex> locker(_audio_Lock);

    auto d = GetData(id);

    if (d == nullptr) return 0;

    return (d->_volume * 100) / SDL_MIX_MAXVOLUME;
}

// volume is 0->100
void SDL::SetVolume(int id, int volume)
{
    std::unique_lock<std::mutex> locker(_audio_Lock);

    auto d = GetData(id);

    if (d == nullptr) return;

    if (volume > 100)
    {
        d->_volume = SDL_MIX_MAXVOLUME;
    }
    else if (volume < 0)
    {
        d->_volume = 0;
    }
    else
    {
        d->_volume = (volume * SDL_MIX_MAXVOLUME) / 100;
    }
}

AudioData* SDL::GetData(int id)
{
    for (auto it = _audioData.begin(); it != _audioData.end(); ++it)
    {
        if ((*it)->_id == id) return *it;
    }

    return nullptr;
}

AudioData::AudioData()
{
    _savedpos = 0;
    _id = -10;
    _audio_len = 0;
    _audio_pos = nullptr;
    _volume = SDL_MIX_MAXVOLUME;
    _rate = 14400;
    _original_len = 0;
    _original_pos = nullptr;
    _lengthMS = 0;
    _trackSize = 0;
    _paused = false;
}

long AudioData::Tell()
{
    long pos = (long)(((((Uint64)(_original_len - _audio_len) / 4) * _lengthMS)) / _trackSize);
    return pos;
}

void AudioData::Seek(long ms)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    if ((((Uint64)ms * _rate * 2 * 2) / 1000) > (Uint64)_original_len)
    {
        // I am not super sure about this
        logger_base.warn("ID %d Attempt to seek past the end of the loaded audio. Seeking to 0ms instead. Seek to %ldms. Length %ldms.", _id, ms, (long)(((Uint64)_original_len * 1000) / ((Uint64)_rate * 2 * 2)));
        ms = 0;
    }

    _audio_len = (long)((Uint64)_original_len - (((Uint64)ms * _rate * 2 * 2) / 1000));
    _audio_len -= _audio_len % 4;

    if (_audio_len > _original_len) _audio_len = _original_len;

    _audio_pos = _original_pos + (_original_len - _audio_len);

    logger_base.debug("ID %d Seeking to %ldMS ... calculated audio_len: %ld", _id, ms, _audio_len);
}

void AudioData::SeekAndLimitPlayLength(long ms, long len)
{
    _audio_len = (long)(((Uint64)len * _rate * 2 * 2) / 1000);
    _audio_len -= _audio_len % 4;
    _audio_pos = _original_pos + (((Uint64)ms * _rate * 2 * 2) / 1000);

    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("ID %d Seeking to %ldMS Length %ldMS ... calculated audio_len: %ld.", _id, ms, len, _audio_len);
}

void AudioData::SavePos()
{
    //static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    _savedpos = Tell();
    //logger_base.info("Saving position %ld 0x%lx as %d.", (long)_audio_len, (long)_audio_pos, _savedpos);
}

void AudioData::RestorePos()
{
    //static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    Seek(_savedpos);
    //logger_base.info("Restoring position %d as %ld 0x%ld.", _savedpos, (long)_audio_len, (long)_audio_pos);
}

int SDL::AddAudio(long len, Uint8* buffer, int volume, int rate, long tracksize, long lengthMS)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    int id = AudioData::__nextId++;

    AudioData* ad = new AudioData();
    ad->_id = id;
    ad->_audio_len = 0;
    ad->_audio_pos = buffer;
    ad->_rate = rate;
    ad->_original_len = len;
    ad->_original_pos = buffer;
    ad->_lengthMS = lengthMS;
    ad->_trackSize = tracksize;
    ad->_paused = false;

    {
        std::unique_lock<std::mutex> locker(_audio_Lock);
        _audioData.push_back(ad);
    }

    SetVolume(id, volume);

    if (rate != _initialisedRate)
    {
        if (_audioData.size() != 1)
        {
            logger_base.warn("Playing multiple audio files with different sample rates with play at least one of them at the wrong speed.");
        }

        _initialisedRate = rate;
        Reopen();
    }

    logger_base.debug("SDL Audio Added: id: %d, rate: %d, len: %ld, lengthMS: %ld, trackSize: %ld.", id, rate, len, lengthMS, tracksize);

    return id;
}

void SDL::RemoveAudio(int id)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::unique_lock<std::mutex> locker(_audio_Lock);
    auto toremove = GetData(id);
    if (toremove == nullptr) return;
    _audioData.remove(toremove);
    delete toremove;
    logger_base.debug("SDL Audio Removed: id: %d.", id);
}

void SDL::Pause(int id, bool pause)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("SDL Audio Pause: id: %d, pause %d.", id, pause);
    std::unique_lock<std::mutex> locker(_audio_Lock);
    auto topause = GetData(id);
    if (topause != nullptr)
        topause->Pause(pause);
}

void SDL::SetRate(float rate)
{
    if (_playbackrate != rate)
    {
        _playbackrate = rate;
        Reopen();
    }
}

void SDL::Play()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("SDL Audio Play.");
    SDL_PauseAudio(0);
    _state = SDLSTATE::SDLPLAYING;
}

void SDL::Pause()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("SDL Audio Pause.");
    SDL_PauseAudio(1);
    _state = SDLSTATE::SDLNOTPLAYING;
}

void SDL::Unpause()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("SDL Audio Unpause.");
    SDL_PauseAudio(0);
    _state = SDLSTATE::SDLPLAYING;
}

void SDL::TogglePause()
{
    if (_state == SDLSTATE::SDLPLAYING)
    {
        Pause();
    }
    else
    {
        Unpause();
    }
}

void SDL::Stop()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("SDL Audio Stop.");
    SDL_PauseAudio(1);
    _state = SDLSTATE::SDLNOTPLAYING;
}

// Audio Manager Functions

void AudioManager::SetVolume(int volume)
{
    __sdl.SetVolume(_sdlid, volume);
}

int AudioManager::GetVolume()
{
    return __sdl.GetVolume(_sdlid);
}

int AudioManager::GetGlobalVolume()
{
    return __sdl.GetGlobalVolume();
}

void AudioManager::SetGlobalVolume(int volume)
{
    __sdl.SetGlobalVolume(volume);
}

void AudioManager::Seek(long pos)
{
	if (pos < 0 || pos > _lengthMS)
	{
		return;
	}

    __sdl.Seek(_sdlid, pos);
}

void AudioManager::Pause()
{
    __sdl.Pause(_sdlid, true);
    //__sdl.Pause();
	_media_state = MEDIAPLAYINGSTATE::PAUSED;
}

void AudioManager::Play(long posms, long lenms)
{
    if (posms < 0 || posms > _lengthMS)
    {
        return;
    }

    if (!__sdl.HasAudio(_sdlid))
    {
        _sdlid = __sdl.AddAudio(_pcmdatasize, _pcmdata, 100, _rate, _trackSize, _lengthMS);
    }

    __sdl.SeekAndLimitPlayLength(_sdlid, posms, lenms);
    __sdl.Play();
    __sdl.Pause(_sdlid, false);
    _media_state = MEDIAPLAYINGSTATE::PLAYING;
}

void AudioManager::Play()
{
    if (!__sdl.HasAudio(_sdlid))
    {
        _sdlid = __sdl.AddAudio(_pcmdatasize, _pcmdata, 100, _rate, _trackSize, _lengthMS);
    }

    __sdl.Pause(_sdlid, false);
    __sdl.Play();
	_media_state = MEDIAPLAYINGSTATE::PLAYING;
}

void AudioManager::Stop()
{
    __sdl.Stop();
	_media_state = MEDIAPLAYINGSTATE::STOPPED;
}

void AudioManager::AbsoluteStop()
{
    __sdl.Stop();
    __sdl.RemoveAudio(_sdlid);
    _media_state = MEDIAPLAYINGSTATE::STOPPED;
}

void AudioManager::SetPlaybackRate(float rate)
{
    __sdl.SetRate(rate);
}

MEDIAPLAYINGSTATE AudioManager::GetPlayingState()
{
    return _media_state;
}

// return where in the file we are up to playing
long AudioManager::Tell()
{
    return __sdl.Tell(_sdlid);
}

size_t AudioManager::GetAudioFileLength(std::string filename)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    AVFormatContext* formatContext = nullptr;

    av_register_all();

    int res = avformat_open_input(&formatContext, filename.c_str(), nullptr, nullptr);
    if (res != 0)
    {
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
            formatContext = nullptr;
        }
        return 0;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0)
    {
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
            formatContext = nullptr;
        }
        return 0;
    }

    AVCodec* cdc;
    int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &cdc, 0);
    if (streamIndex < 0)
    {
        logger_base.error("AudioManager: Could not find any audio stream in " + filename);

        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
            formatContext = nullptr;
        }

        return 0;
    }

    AVStream* audioStream = formatContext->streams[streamIndex];

    if (audioStream != nullptr && audioStream->duration > 0 && audioStream->time_base.den > 0)
    {
        size_t duration = audioStream->duration * 1000 * audioStream->time_base.num / audioStream->time_base.den;

        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
            formatContext = nullptr;
        }

        return duration;
    }

    if (formatContext != nullptr)
    {
        avformat_close_input(&formatContext);
        formatContext = nullptr;
    }

    logger_base.error("AudioManager: Could not determine length of video " + filename);
    return 0;
}

long AudioManager::GetLoadedData()
{
    std::unique_lock<std::shared_timed_mutex> locker(_mutexAudioLoad);

    return _loadedData;
}

void AudioManager::SetLoadedData(long pos)
{
    std::unique_lock<std::shared_timed_mutex> locker(_mutexAudioLoad);

    _loadedData = pos;
}

bool AudioManager::IsDataLoaded(long pos)
{
    std::unique_lock<std::shared_timed_mutex> locker(_mutexAudioLoad);

    if (pos < 0)
    {
        return _loadedData == _trackSize;
    }
    else
    {
        return _loadedData >= std::min(pos, _trackSize);
    }
}

AudioManager::AudioManager(const std::string& audio_file, int step, int block)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	// save parameters and initialise defaults
    _ok = true;
	_job = nullptr;
    _jobAudioLoad = nullptr;
    _loadedData = 0;
	_audio_file = audio_file;
	_state = -1; // state uninitialised. 0 is error. 1 is loaded ok
	_resultMessage = "";
	_data[0] = nullptr; // Left channel data
	_data[1] = nullptr; // right channel data
	_intervalMS = -1; // no length
	_frameDataPrepared = false; // frame data is used by effects to react to the sone
	_media_state = MEDIAPLAYINGSTATE::STOPPED;
	_pcmdata = nullptr;
	_polyphonicTranscriptionDone = false;
    _sdlid = -1;

	// extra is the extra bytes added to the data we read. This allows analysis functions to exceed the file length without causing memory exceptions
	_extra = std::max(step, block) + 1;

	// Open the media file
	OpenMediaFile();

	// If we opened it successfully kick off the frame data extraction ... this will run on another thread
	if (_intervalMS > 0)
	{
		PrepareFrameData(true);
	}

	// if we got here without setting state to zero then all must be good so set state to 1 success
	if (_ok && _state == -1)
	{
		_state = 1;
        logger_base.info("Audio file loaded.");
        logger_base.info("    Filename: %s", (const char *)_audio_file.c_str());
        logger_base.info("    Title: %s", (const char *)_title.c_str());
        logger_base.info("    Album: %s", (const char *)_album.c_str());
        logger_base.info("    Artist: %s", (const char *)_artist.c_str());
        logger_base.info("    Length: %ldms", _lengthMS);
        logger_base.info("    Channels %d, Bits: %d, Rate %ld", _channels, _bits, _rate);
    }
    else
    {
        logger_base.error("Audio file not loaded: %s.", _resultMessage.c_str());
    }
}

std::list<float> AudioManager::CalculateSpectrumAnalysis(const float* in, int n, float& max, int id)
{
	std::list<float> res;
	int outcount = n / 2 + 1;
	kiss_fftr_cfg cfg;
	kiss_fft_cpx* out = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * (outcount));
	if (out != nullptr)
	{
		if ((cfg = kiss_fftr_alloc(n, 0/*is_inverse_fft*/, NULL, nullptr)) != nullptr)
		{
			kiss_fftr(cfg, in, out);
			free(cfg);
		}

		for (int j = 0; j < 127; j++)
		{
            // choose the right bucket for this MIDI note
            double freq = 440.0 * exp2f(((double)j - 69.0) / 12.0);
            int start = freq * (double)n / (double)_rate;
            double freqnext = 440.0 * exp2f(((double)j + 1.0 - 69.0) / 12.0);
            int end = freqnext * (double)n / (double)_rate;

            float val = 0.0;

            // got through all buckets up to the next note and take the maximums
            if (end < outcount-1)
            {
                for (int k = start; k <= end; k++)
                {
                    kiss_fft_cpx* cur = out + k;
                    val = std::max(val, sqrtf(cur->r * cur->r + cur->i * cur->i));
                    //float valscaled = valnew * scaling;
                }
            }

			float db = log10(val);
			if (db < 0.0)
			{
				db = 0.0;
			}

			res.push_back(db);
			if (db > max)
			{
				max = db;
			}
		}

		free(out);
	}

	return res;
}

void AudioManager::DoPolyphonicTranscription(wxProgressDialog* dlg, AudioManagerProgressCallback fn)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    // dont redo it
    if (_polyphonicTranscriptionDone)
    {
        return;
    }

    wxStopWatch sw;

    logger_base.info("DoPolyphonicTranscription: Polyphonic transcription started on file " + _audio_file);

    while (!IsDataLoaded())
    {
        logger_base.debug("DoPolyphonicTranscription: Waiting for audio data to load.");
        wxMilliSleep(100);
    }

    static log4cpp::Category &logger_pianodata = log4cpp::Category::getInstance(std::string("log_pianodata"));
    logger_pianodata.debug("Processing polyphonic transcription on file " + _audio_file);
    logger_pianodata.debug("Interval %d.", _intervalMS);
    logger_pianodata.debug("BitRate %d.", GetRate());

    // Initialise Polyphonic Transcription
    _vamp.GetAllAvailablePlugins(this); // this initialises Vamp
    Vamp::Plugin* pt = _vamp.GetPlugin("Polyphonic Transcription");
    size_t pref_step;

    if (pt == nullptr)
    {
        logger_base.warn("DoPolyphonicTranscription: Unable to load Polyphonic Transcription VAMP plugin.");
    }
    else
    {
        float *pdata[2];
        long frames = _lengthMS / _intervalMS;
        while (frames * _intervalMS < _lengthMS)
        {
            frames++;
        }

        pref_step = pt->getPreferredStepSize();
        size_t pref_block = pt->getPreferredBlockSize();

        int channels = GetChannels();
        if (channels > (int)pt->getMaxChannelCount()) {
            channels = 1;
        }

        logger_pianodata.debug("Channels %d.", GetChannels());
        logger_pianodata.debug("Step %d.", pref_step);
        logger_pianodata.debug("Block %d.", pref_block);
        pt->initialise(channels, pref_step, pref_block);

        bool first = true;
        int start = 0;
        long len = GetTrackSize();
        float totalLen = len;
        int lastProgress = 0;
        while (len)
        {
            int progress = (((float)(totalLen - len) * 25) / totalLen);
            if (lastProgress < progress)
            {
                fn(dlg, progress);
                lastProgress = progress;
            }
            pdata[0] = GetLeftDataPtr(start);
            //pdata[0] = GetRightDataPtr(start);
            pdata[1] = GetRightDataPtr(start);
            //pdata[1] = GetLeftDataPtr(start);

            Vamp::RealTime timestamp = Vamp::RealTime::frame2RealTime(start, GetRate());
            Vamp::Plugin::FeatureSet features = pt->process(pdata, timestamp);

            if (first && features.size() > 0)
            {
                logger_base.warn("DoPolyphonicTranscription: Polyphonic transcription data process oddly retrieved data.");
                first = false;
            }
            if (len > pref_step)
            {
                len -= pref_step;
            }
            else
            {
                len = 0;
            }
            start += pref_step;
        }

        // Process the Polyphonic Transcription
        try
        {
            unsigned int total = 0;
            logger_pianodata.debug("About to extract Polyphonic Transcription result.");
            Vamp::Plugin::FeatureSet features = pt->getRemainingFeatures();
            logger_pianodata.debug("Polyphonic Transcription result retrieved.");
            logger_pianodata.debug("Start,Duration,CalcStart,CalcEnd,midinote");
            for (size_t j = 0; j < features[0].size(); j++)
            {
                if (j % 10 == 0)
                {
                    fn(dlg, (int)(((float)j * 75.0) / (float)features[0].size()) + 25.0);
                }

                long currentstart = features[0][j].timestamp.sec * 1000 + features[0][j].timestamp.msec();
                long currentend = currentstart + features[0][j].duration.sec * 1000 + features[0][j].duration.msec();

                //printf("%f\t%f\t%f\n",(float)currentstart/1000.0, (float)currentend/1000.0, features[0][j].values[0]);
                if (logger_pianodata.isDebugEnabled())
                {
                    logger_pianodata.debug("%d.%03d,%d.%03d,%d,%d,%f", features[0][j].timestamp.sec, features[0][j].timestamp.msec(), features[0][j].duration.sec, features[0][j].duration.msec(), currentstart, currentend, features[0][j].values[0]);
                }
                total += features[0][j].values.size();

                int sframe = currentstart / _intervalMS;
                if (currentstart - sframe * _intervalMS > _intervalMS / 2) {
                    sframe++;
                }
                int eframe = currentend / _intervalMS;
                while (sframe <= eframe) {
                    _frameData[sframe][4].push_back(features[0][j].values[0]);
                    sframe++;
                }
            }

            fn(dlg, 100);

            if (logger_pianodata.isDebugEnabled())
            {
                logger_pianodata.debug("Piano data calculated:");
                logger_pianodata.debug("Time MS, Keys");
                for (size_t i = 0; i < _frameData.size(); i++)
                {
                    long ms = i * _intervalMS;
                    std::string keys = "";
                    for (auto it2 = _frameData[i][4].begin(); it2 != _frameData[i][4].end(); ++it2)
                    {
                        keys += " " + std::string(wxString::Format("%f", *it2).c_str());
                    }
                    logger_pianodata.debug("%ld,%s", ms, (const char *)keys.c_str());
                }
            }
            //printf("Total points: %u", total);
        }
        catch (...)
        {
            logger_base.warn("DoPolyphonicTranscription: Polyphonic Transcription threw an error getting the remaining features.");
        }

        //done with VAMP Polyphonic Transcriber
        delete pt;
    }
    _polyphonicTranscriptionDone = true;
    logger_base.info("DoPolyphonicTranscription: Polyphonic transcription completed in %ld.", sw.Time());
}

// Frame Data Extraction Functions
// process audio data and build data for each frame
void AudioManager::DoPrepareFrameData()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.info("DoPrepareFrameData: Start processing audio frame data.");

	// lock the mutex
    std::unique_lock<std::shared_timed_mutex> locker(_mutex);

    if (_data[0] == nullptr) return;

    wxStopWatch sw;

	// if we have already done it ... bail
	if (_frameDataPrepared)
	{
		logger_base.info("DoPrepareFrameData: Aborting processing audio frame data ... it has already been done.");
		return;
	}

    // wait for the data to load
    while (!IsDataLoaded())
    {
        logger_base.info("DoPrepareFrameData: waiting for audio data to load.");
        wxMilliSleep(1000);
    }

	// samples per frame
	int samplesperframe = _rate * _intervalMS / 1000;
	int frames = _lengthMS / _intervalMS;
	while (frames * _intervalMS < _lengthMS)
	{
		frames++;
	}
	int totalsamples = frames * samplesperframe;

	// these are used to normalise output
	_bigmax = -1;
	_bigspread = -1;
	_bigmin = 1;
	_bigspectogrammax = -1;

	size_t step = 2048;
	float *pdata[2];

	int pos = 0;
	std::list<float> spectrogram;

	// process each frome of the song
	for (int i = 0; i < frames; i++)
	{
		std::vector<std::list<float>> aFrameData;
		aFrameData.resize(5); // preallocate the spots we will need

		// accumulators
		float max = -100.0;
		float min = 100.0;
		float spread = -100;

		// clear the data if we are about to get new data ... dont clear it if we wont
		// this happens because the spectrogram function has a fixed window based on the parameters we set and it
		// does not match our time slices exactly so we have to select which one to use
		if (pos < i * samplesperframe + samplesperframe  && pos + step < totalsamples)
		{
			spectrogram.clear();
		}

		// only get the data if we are not ahead of the music
		while (pos < i * samplesperframe + samplesperframe && pos + step < totalsamples)
		{
			std::list<float> subspectrogram;
			pdata[0] = GetLeftDataPtr(pos);
			pdata[1] = GetRightDataPtr(pos);
			float max2 = 0;

			if (pdata[0] == nullptr)
			{
				subspectrogram.clear();
			}
			else
			{
				subspectrogram = CalculateSpectrumAnalysis(pdata[0], step, max2, i);
			}

			// and keep track of the larges value so we can normalise it
			if (max2 > _bigspectogrammax)
			{
				_bigspectogrammax = max2;
			}
			pos += step;

			// either take the newly calculated values or if we are merging two results take the maximum of each value
			if (spectrogram.size() == 0)
			{
				spectrogram = subspectrogram;
			}
			else
			{
				if (subspectrogram.size() > 0)
				{
					std::list<float>::iterator sub = subspectrogram.begin();
					for (std::list<float>::iterator fr = spectrogram.begin(); fr != spectrogram.end(); ++fr)
					{
						if (*sub > *fr)
						{
							*fr = *sub;
						}
						++sub;
					}
				}
			}
		}

		// now do the raw data analysis for the frame
		for (int j = 0; j < samplesperframe; j++)
		{
			float data = GetLeftData(i * samplesperframe + j);

			// Max data
			if (data > max)
			{
				max = data;
			}

			// Min data
			if (data < min)
			{
				min = data;
			}

			// Spread data
			if (max - min > spread)
			{
				spread = max - min;
			}
		}

		if (max > _bigmax)
		{
			_bigmax = max;
		}
		if (min < _bigmin)
		{
			_bigmin = min;
		}
		if (spread > _bigspread)
		{
			_bigspread = spread;
		}

		// Now save the results for the frame
		std::list<float> maxlist;
		maxlist.push_back(max);
		std::list<float> minlist;
		minlist.push_back(min);
		std::list<float> spreadlist;
		spreadlist.push_back(spread);
		aFrameData[0] = maxlist;
		aFrameData[1] = minlist;
		aFrameData[2] = spreadlist;
		aFrameData[3] = spectrogram;

		_frameData.push_back(aFrameData);
	}

	// normalise data ... basically scale the data so the highest value is the scale value.
	float scale = 1.0; // 0-1 ... where 0.x means that the max value displayed would be x0% of model size
	float bigmaxscale = 1 / (_bigmax * scale);
	float bigminscale = 1 / (_bigmin * scale);
	float bigspreadscale = 1 / (_bigspread * scale);
	float bigspectrogramscale = 1 / (_bigspectogrammax * scale);
	for (std::vector<std::vector<std::list<float>>>::iterator itframe = _frameData.begin(); itframe != _frameData.end(); ++itframe)
	{
		std::list<float>* fl = &(*itframe)[0];
		std::list<float>::iterator f = fl->begin();
		*f = (*f * bigmaxscale);

		fl = &(*itframe)[1];
		f = fl->begin();
		*f = (*f * bigminscale);

		fl = &(*itframe)[2];
		f = fl->begin();
		*f = (*f * bigspreadscale);

		fl = &(*itframe)[3];
		for (std::list<float>::iterator ff = fl->begin(); ff != fl->end(); ++ff)
		{
			*ff = *ff * bigspectrogramscale;
		}
	}

	// flag the fact that the data is all ready
	_frameDataPrepared = true;

	logger_base.info("DoPrepareFrameData: Audio frame data processing complete in %ld.", sw.Time());
}

// Called to trigger frame data creation
void AudioManager::PrepareFrameData(bool separateThread)
{
	if (separateThread)
	{
		// if we have not prepared the frame data and no job has been created
		if (!_frameDataPrepared && _job == nullptr)
		{
			_job = (Job*)new AudioScanJob(this);
			_jobPool.PushJob(_job);
		}
	}
	else
	{
		DoPrepareFrameData();
	}
}

void AudioManager::LoadAudioData(bool separateThread, AVFormatContext* formatContext, AVCodecContext* codecContext, AVStream* audioStream, AVFrame* frame)
{
    if (separateThread)
    {
        // if we have not prepared the frame data and no job has been created
        if (_jobAudioLoad == nullptr)
        {
            _jobAudioLoad = (Job*)new AudioLoadJob(this, formatContext, codecContext, audioStream, frame);
            _jobPool.PushJob(_jobAudioLoad);
        }
    }
    else
    {
        DoLoadAudioData(formatContext, codecContext, audioStream, frame);
    }
}

void ProgressFunction(wxProgressDialog* pd, int p)
{
    if (pd != nullptr)
    {
        pd->Update(p);
    }
}

// Get the pre-prepared data for this frame
std::list<float>* AudioManager::GetFrameData(int frame, FRAMEDATATYPE fdt, std::string timing)
{
    std::list<float>* rc = nullptr;

    // Grab the lock so we can safely access the frame data
    std::shared_lock<std::shared_timed_mutex> lock(_mutex);

    // make sure we have audio data
    if (_data[0] == nullptr) return rc;

    // if the frame data has not been prepared
    if (!_frameDataPrepared)
    {
        // prepare it
        lock.unlock();
        PrepareFrameData(false);

        lock.lock();
        // wait until the new thread grabs the lock
        while (!_frameDataPrepared)
        {
            lock.unlock();
            wxMilliSleep(5);
            lock.lock();
        }
    }
    if (fdt == FRAMEDATA_NOTES && !_polyphonicTranscriptionDone) {
        //need to do the polyphonic stuff
        wxProgressDialog dlg("Processing Audio", "");
        DoPolyphonicTranscription(&dlg, ProgressFunction);
    }

    // now we can grab the data we need
    try
    {
        if (frame < (int)_frameData.size())
        {
            std::vector<std::list<float>>* framedata = &_frameData[frame];

            if (framedata == nullptr)
            {
                log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
                logger_base.crit("AudioManager::GetFrameData framedata is nullptr ... this is going to crash.");
            }

            switch (fdt)
            {
            case FRAMEDATA_HIGH:
                rc = &framedata->at(0);
                break;
            case FRAMEDATA_LOW:
                rc = &framedata->at(1);
                break;
            case FRAMEDATA_SPREAD:
                rc = &framedata->at(2);
                break;
            case FRAMEDATA_VU:
                rc = &framedata->at(3);
                break;
            case FRAMEDATA_ISTIMINGMARK:
                // we dont need to do anything here
                break;
            case FRAMEDATA_NOTES:
                rc = &framedata->at(4);
                break;
            }
        }
    }
    catch (...)
    {
        rc = nullptr;
    }

    return rc;
}

// Constant Bitrate Detection Functions

// Decode bitrate
int AudioManager::decodebitrateindex(int bitrateindex, int version, int layertype)
{
	switch (version)
	{
	case 0: // v2.5
	case 2: // v2
		switch (layertype)
		{
		case 0:
        default:
			// invalid
			return 0;
		case 1: // L3
		case 2: // L2
			if (bitrateindex == 0 || bitrateindex == 0x0F)
			{
				return 0;
			}
			else if (bitrateindex < 8)
			{
				return 8 * bitrateindex;
			}
			else
			{
				return 64 + (bitrateindex - 8) * 16;
			}
		case 3: // L1
			if (bitrateindex == 0 || bitrateindex == 0x0F)
			{
				return 0;
			}
			else
			{
				return 16 + bitrateindex * 16;
			}
		}
	case 3: // v1
		switch (layertype)
		{
		case 0:
        default:
			// invalid
			return 0;
		case 1: // L3
			if (bitrateindex == 0 || bitrateindex == 0x0F)
			{
				return 0;
			}
			else if (bitrateindex < 6)
			{
				return 32 + (bitrateindex - 1) * 8;
			}
			else if (bitrateindex < 9)
			{
				return 64 + (bitrateindex - 6) * 16;
			}
			else if (bitrateindex < 14)
			{
				return 128 + (bitrateindex - 9) * 32;
			}
			else
			{
				return 320;
			}
		case 2: // L2
			if (bitrateindex == 0 || bitrateindex == 0x0F)
			{
				return 0;
			}
			else if (bitrateindex < 3)
			{
				return 32 + (bitrateindex - 1) * 16;
			}
			else if (bitrateindex < 5)
			{
				return 56 + (bitrateindex - 3) * 8;
			}
			else if (bitrateindex < 9)
			{
				return 80 + (bitrateindex - 5) * 16;
			}
			else if (bitrateindex < 13)
			{
				return 160 + (bitrateindex - 9) * 32;
			}
			else
			{
				return 320 + (bitrateindex - 13) * 64;
			}
		case 3: // L1
			if (bitrateindex == 0 || bitrateindex == 0x0F)
			{
				return 0;
			}
			else
			{
				return bitrateindex * 32;
			}
		}
	case 1:
    default:
        // invalid
		return 0;
	}
}

// Decode samplerate
int AudioManager::decodesamplerateindex(int samplerateindex, int version)
{
	switch (version)
	{
	case 0: // v2.5
		switch (samplerateindex)
		{
		case 0:
			return 11025;
		case 1:
			return 12000;
		case 2:
			return 8000;
		case 3:
        default:
			return 0;
		}
	case 2: // v2
		switch (samplerateindex)
		{
		case 0:
			return 22050;
		case 1:
			return 24000;
		case 2:
			return 16000;
		case 3:
        default:
			return 0;
		}
	case 3: // v1
		switch (samplerateindex)
		{
		case 0:
			return 44100;
		case 1:
			return 48000;
		case 2:
			return 32000;
		case 3:
        default:
			return 0;
		}
	case 1:
    default:
		// invalid
		return 0;
	}
}

// Decode side info
int AudioManager::decodesideinfosize(int version, int mono) const
{
	if (version == 3) // v1
	{
		if (mono == 3) // mono
		{
			return 17;
		}
		else
		{
			return 32;
		}
	}
	else
	{
		if (mono == 3) // mono
		{
			return 9;
		}
		else
		{
			return 17;
		}
	}
}

// Set the frame interval we will be using
void AudioManager::SetFrameInterval(int intervalMS)
{
	// If this is different from what it was previously
	if (_intervalMS != intervalMS)
	{
		// save it and regenerate the frame data for effects that rely upon it ... but do it on a background thread
		_intervalMS = intervalMS;
		PrepareFrameData(true);
	}
}

// Set the set and block that vamp analysis will be using
// This controls how much extra space at the end of the file we need so VAMP functions dont try to read past the end of the allocated memory
void AudioManager::SetStepBlock(int step, int block)
{
	int extra = std::max(step, block) + 1;

	// we only need to reopen if the extra bytes are greater
	if (extra > _extra)
	{
		_extra = extra;
		_state = -1;
		OpenMediaFile();
	}
}

// Clean up our data buffers
AudioManager::~AudioManager()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    while (!IsDataLoaded())
    {
        logger_base.debug("~AudioManager waiting for audio data to complete loading before destroying it.");
        wxMilliSleep(100);
    }

    if (_pcmdata != nullptr)
    {
        __sdl.Stop();
        __sdl.RemoveAudio(_sdlid);
        free(_pcmdata);
        _pcmdata = nullptr;
    }

    // wait for prepare frame data to finish ... if i delete the data before it is done we will crash
    // this is only tripped if we try to open a new song too soon after opening another one

    // Grab the lock so we know the background process isnt runnning
    std::shared_lock<std::shared_timed_mutex> lock(_mutex);

	if (_data[1] != _data[0] && _data[1] != nullptr)
	{
		free(_data[1]);
		_data[1] = nullptr;
	}
	if (_data[0] != nullptr)
	{
		free(_data[0]);
		_data[0] = nullptr;
	}

	// I am not deleting _job as I think JobPool takes care of this
}

// Split the MP# data into left and right and normalise the values
void AudioManager::SplitTrackDataAndNormalize(signed short* trackData, long trackSize, float* leftData, float* rightData) const
{
    for(size_t i=0; i<trackSize; i++)
    {
        float lSample = trackData[i*2];
        leftData[i] = lSample/32768.0f;
        float rSample = trackData[(i*2)+1];
        rightData[i] = rSample/32768.0f;
    }
}

// NOrmalise mono track data
void AudioManager::NormalizeMonoTrackData(signed short* trackData, long trackSize, float* leftData)
{
    signed short lSample;
    for(size_t i=0; i<trackSize; i++)
    {
        lSample = trackData[i];
        leftData[i] = (float)lSample/(float)32768;
    }
}

// Calculate the song lenth in MS
long AudioManager::CalcLengthMS() const
{
	float seconds = (float)_trackSize * (1.0f / (float)_rate);
	return (long)(seconds * 1000.0f);
}

// Open and read the media file into memory
int AudioManager::OpenMediaFile()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    int err = 0;

	if (_pcmdata != nullptr)
	{
		__sdl.Stop();
        __sdl.RemoveAudio(_sdlid);
        _sdlid = -1;
		free(_pcmdata);
		_pcmdata = nullptr;
	}

	// Initialize FFmpeg codecs
	av_register_all();

	AVFormatContext* formatContext = nullptr;
	int res = avformat_open_input(&formatContext, _audio_file.c_str(), nullptr, nullptr);
	if (res != 0)
	{
		logger_base.error("avformat_open_input Error opening the file %s => %d.", (const char *) _audio_file.c_str(), res);
        _ok = false;
		return 1;
	}

	if (avformat_find_stream_info(formatContext, nullptr) < 0)
	{
		avformat_close_input(&formatContext);
        logger_base.error("avformat_find_stream_info Error finding the stream info %s.", (const char *)_audio_file.c_str());
        _ok = false;
        return 1;
	}

	// Find the audio stream
	AVCodec* cdc = nullptr;
	int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &cdc, 0);
	if (streamIndex < 0)
	{
		avformat_close_input(&formatContext);
        logger_base.error("av_find_best_stream Could not find any audio stream in the file %s.", (const char *)_audio_file.c_str());
        _ok = false;
        return 1;
	}

	AVStream* audioStream = formatContext->streams[streamIndex];
	AVCodecContext* codecContext = audioStream->codec;
	codecContext->codec = cdc;

	if (avcodec_open2(codecContext, codecContext->codec, nullptr) != 0)
	{
		avformat_close_input(&formatContext);
        logger_base.error("avcodec_open2 Couldn't open the context with the decoder %s.", (const char *)_audio_file.c_str());
        _ok = false;
        return 1;
	}

	_channels = codecContext->channels;
#ifdef RESAMPLE_RATE
    _rate = RESAMPLE_RATE;
#else
    _rate = codecContext->sample_rate;
#endif
	_bits = av_get_bytes_per_sample(codecContext->sample_fmt);

	/* Get Track Size */
	GetTrackMetrics(formatContext, codecContext, audioStream);

	// Check if we have read this before ... if so dump the old data
	if (_data[1] != nullptr && _data[1] != _data[0])
	{
		free(_data[1]);
		_data[1] = nullptr;
	}
	if (_data[0] != nullptr)
	{
		free(_data[0]);
		_data[0] = nullptr;
	}
    _loadedData = 0;

    long size = sizeof(float)*(_trackSize + _extra);
	_data[0] = (float*)calloc(size, 1);

    if (_data[0] == nullptr)
    {
        wxASSERT(false);
        logger_base.error("Unable to allocate %ld memory to load audio file %s.", (long)size, (const char *)_audio_file.c_str());
        _ok = false;
        return 1;
    }

    memset(_data[0], 0x00, size);
	if (_channels == 2)
	{
		_data[1] = (float*)calloc(size, 1);
        if (_data[1] == nullptr)
        {
            wxASSERT(false);
            logger_base.error("Unable to allocate %ld memory to load audio file %s.", (long)size, (const char *)_audio_file.c_str());
            _ok = false;
            return 1;
        }
        memset(_data[1], 0x00, size);
	}
	else
	{
		_data[1] = _data[0];
	}

	LoadTrackData(formatContext, codecContext, audioStream);

    // only initialise if we successfully got data
    if (_pcmdata != nullptr)
    {
        //long total_len = (_lengthMS * _rate * 2 * 2) / 1000;
        //total_len -= total_len % 4;
        _sdlid = __sdl.AddAudio(_pcmdatasize, _pcmdata, 100, _rate, _trackSize, _lengthMS);
    }

	return err;
}

void AudioManager::LoadTrackData(AVFormatContext* formatContext, AVCodecContext* codecContext, AVStream* audioStream)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("Preparing to load song data.");

    // setup our conversion format ... we need to conver the input to a standard format before we can process anything
    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr)
    {
        logger_base.error("av_frame_alloc ... error allocating frame.");
        _resultMessage = "Error allocating the frame";
        _state = 0;
        _ok = false;
        return;
    }

    _pcmdatasize = _trackSize * out_channels * 2;
    _pcmdata = (Uint8*)malloc(_pcmdatasize + 16384); // 16384 is a fudge because some ogg files dont read consistently
    if (_pcmdata == nullptr)
    {
        logger_base.error("Error allocating memory for pcm data: %ld", (long)_pcmdatasize + 16384);
        _ok = false;
        return;
    }

    ExtractMP3Tags(formatContext);

    LoadAudioData(true, formatContext, codecContext, audioStream, frame);
}

void AudioManager::DoLoadAudioData(AVFormatContext* formatContext, AVCodecContext* codecContext, AVStream* audioStream, AVFrame* frame)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("DoLoadAudioData: Doing load of song data.");

    wxStopWatch sw;

    long read = 0;

    // setup our conversion format ... we need to conver the input to a standard format before we can process anything
    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = _rate;
    AVPacket readingPacket;
	av_init_packet(&readingPacket);

#define CONVERSION_BUFFER_SIZE 192000
    uint8_t* out_buffer = (uint8_t *)av_malloc(CONVERSION_BUFFER_SIZE * out_channels * 2); // 1 second of audio

	int64_t in_channel_layout = av_get_default_channel_layout(codecContext->channels);
	struct SwrContext *au_convert_ctx;
	au_convert_ctx = swr_alloc_set_opts(nullptr, out_channel_layout, out_sample_fmt, out_sample_rate,
		in_channel_layout, codecContext->sample_fmt, codecContext->sample_rate, 0, nullptr);
	swr_init(au_convert_ctx);

	// start at the beginning
	av_seek_frame(formatContext, 0, 0, AVSEEK_FLAG_ANY);

	// Read the packets in a loop
	while (av_read_frame(formatContext, &readingPacket) == 0)
	{
		if (readingPacket.stream_index == audioStream->index)
		{
			AVPacket decodingPacket = readingPacket;

			// Audio packets can have multiple audio frames in a single packet
			while (decodingPacket.size > 0)
			{
				// Try to decode the packet into a frame
				// Some frames rely on multiple packets, so we have to make sure the frame is finished before
				// we can use it
				int gotFrame = 0;
				int result = avcodec_decode_audio4(codecContext, frame, &gotFrame, &decodingPacket);

				if (result >= 0 && gotFrame)
				{
					decodingPacket.size -= result;
                    int outSamples;
					try
					{
						outSamples = swr_convert(au_convert_ctx, &out_buffer, CONVERSION_BUFFER_SIZE, (const uint8_t **)frame->data, frame->nb_samples);
					}
					catch (...)
					{
						swr_free(&au_convert_ctx);
						av_free(out_buffer);
						av_free(frame);
						return;
					}

					if (read + outSamples > _trackSize)
					{
						// I dont understand why this happens ... add logging when i can
                        // I have seen this happen with a wma file ... but i dont know why
						logger_base.warn("DoLoadAudioData: This shouldnt happen ... read ["+ wxString::Format("%i", (long)read) +"] + nb_samples ["+ wxString::Format("%i", outSamples) +"] > _tracksize ["+ wxString::Format("%i", (long)_trackSize) +"] .");

                        // override the track size
                        _trackSize = read + outSamples;
					}

					// copy the PCM data into the PCM buffer for playing
					memcpy(_pcmdata + (read * out_channels * 2), out_buffer, outSamples * out_channels * 2);

					for (int i = 0; i < outSamples; i++)
					{
						int16_t s;
						s = *(int16_t*)(out_buffer + i * sizeof(int16_t) * out_channels);
						_data[0][read + i] = ((float)s) / (float)0x8000;
						if (_channels > 1)
						{
							s = *(int16_t*)(out_buffer + i * sizeof(int16_t) * out_channels + sizeof(int16_t));
							_data[1][read + i] = ((float)s) / (float)0x8000;
						}
					}
					read += outSamples;
                    SetLoadedData(read);
				}
				else
				{
					decodingPacket.size = 0;
				}
			}
		}

		// You *must* call av_free_packet() after each call to av_read_frame() or else you'll leak memory
		av_packet_unref(&readingPacket);
	}

	// Some codecs will cause frames to be buffered up in the decoding process. If the CODEC_CAP_DELAY flag
	// is set, there can be buffered up frames that need to be flushed, so we'll do that
	if (codecContext->codec->capabilities & CODEC_CAP_DELAY)
	{
		av_init_packet(&readingPacket);
		// Decode all the remaining frames in the buffer, until the end is reached
		int gotFrame = 1;
		while (gotFrame)
		{
			int result = avcodec_decode_audio4(codecContext, frame, &gotFrame, &readingPacket);
			if (result >= 0 && gotFrame)
			{
                int outSamples;
				try
				{
					outSamples = swr_convert(au_convert_ctx, &out_buffer, CONVERSION_BUFFER_SIZE, (const uint8_t **)frame->data, frame->nb_samples);
				}
				catch (...)
				{
					swr_free(&au_convert_ctx);
					av_free(out_buffer);
					av_free(frame);
					return;
				}

				// copy the PCM data into the PCM buffer for playing
				memcpy(_pcmdata + (read * out_channels * 2), out_buffer, outSamples * out_channels * 2);

				for (int i = 0; i < outSamples; i++)
				{
					int16_t s;
					s = *(int16_t*)(out_buffer + i * sizeof(int16_t) * out_channels);
					_data[0][read + i] = ((float)s) / (float)0x8000;
					if (_channels > 1)
					{
						s = *(int16_t*)(out_buffer + i * sizeof(int16_t) * out_channels + sizeof(int16_t));
						_data[1][read + i] = ((float)s) / (float)0x8000;
					}
				}
				read += outSamples;
                SetLoadedData(read);
            }
		}
	}

#ifdef RESAMPLE_RATE
    _trackSize = _loadedData;
#endif
    wxASSERT(_trackSize == _loadedData);
    
	// Clean up!
	swr_free(&au_convert_ctx);
	av_free(out_buffer);
	av_free(frame);

    avcodec_close(codecContext);
    avformat_close_input(&formatContext);

    logger_base.debug("DoLoadAudioData: Song data loaded in %ld.", sw.Time());
}

void AudioManager::GetTrackMetrics(AVFormatContext* formatContext, AVCodecContext* codecContext, AVStream* audioStream)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    _trackSize = 0;

	AVFrame* frame = av_frame_alloc();
	if (!frame)
	{
        logger_base.error("av_frame_alloc ... error allocating frame.");
        _resultMessage = "Error allocating the frame";
		_state = 0;
        _ok = false;
        return;
	}

	AVPacket readingPacket;
	av_init_packet(&readingPacket);

	// start at the beginning
	av_seek_frame(formatContext, 0, 0, AVSEEK_FLAG_ANY);

	// Read the packets in a loop
	while (av_read_frame(formatContext, &readingPacket) == 0)
	{
		if (readingPacket.stream_index == audioStream->index)
		{
			AVPacket decodingPacket = readingPacket;

			// Audio packets can have multiple audio frames in a single packet
			while (decodingPacket.size > 0)
			{
				// Try to decode the packet into a frame
				// Some frames rely on multiple packets, so we have to make sure the frame is finished before
				// we can use it
				int gotFrame = 0;
				int result = avcodec_decode_audio4(codecContext, frame, &gotFrame, &decodingPacket);

				if (result >= 0 && gotFrame)
				{
					decodingPacket.size -= result;
					_trackSize += frame->nb_samples;
				}
				else
				{
					decodingPacket.size = 0;
				}

			}
		}

		// You *must* call av_free_packet() after each call to av_read_frame() or else you'll leak memory
		av_packet_unref(&readingPacket);
	}

	// Some codecs will cause frames to be buffered up in the decoding process. If the CODEC_CAP_DELAY flag
	// is set, there can be buffered up frames that need to be flushed, so we'll do that
	if (codecContext->codec->capabilities & CODEC_CAP_DELAY)
	{
		av_init_packet(&readingPacket);
		// Decode all the remaining frames in the buffer, until the end is reached
		int gotFrame = 1;
		while (gotFrame)
		{
			int result = avcodec_decode_audio4(codecContext, frame, &gotFrame, &readingPacket);
			if (result >= 0 && gotFrame)
			{
				_trackSize += frame->nb_samples;
			}
		}
	}

	// Clean up!
	av_free(frame);

	_lengthMS = (long)(((Uint64)_trackSize * 1000) / ((codecContext->time_base.den)));
#ifdef RESAMPLE_RATE
    //if we resample, we need to estimate the new size
    float f = _trackSize;
    f *= RESAMPLE_RATE;
    f /= codecContext->sample_rate;
    _trackSize = f;
    _extra += 2048;  //add some extra space just in case the estimate is not accurate
#endif

    logger_base.info("    Track Size: %ld, Time Base Den: %d => Length %ldms", _trackSize, codecContext->time_base.den, _lengthMS);
}

void AudioManager::ExtractMP3Tags(AVFormatContext* formatContext)
{
	AVDictionaryEntry* tag = av_dict_get(formatContext->metadata, "title", nullptr, 0);
	if (tag != nullptr)
	{
		_title = tag->value;
	}
	tag = av_dict_get(formatContext->metadata, "album", nullptr, 0);
	if (tag != nullptr)
	{
		_album = tag->value;
	}
	tag = av_dict_get(formatContext->metadata, "artist", nullptr, 0);
	if (tag != nullptr)
	{
		_artist = tag->value;
	}
}

// Access a single piece of track data
float AudioManager::GetLeftData(long offset)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    while (!IsDataLoaded(offset))
    {
        logger_base.debug("GetLeftData waiting for data to be loaded.");
        wxMilliSleep(100);
    }

	if (_data[0] == nullptr || offset > _trackSize)
	{
		return 0;
	}
	return _data[0][offset];
}

// Access a single piece of track data
float AudioManager::GetRightData(long offset)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    while (!IsDataLoaded(offset))
    {
        logger_base.debug("GetRightData waiting for data to be loaded.");
        wxMilliSleep(100);
    }

    if (_data[1] == nullptr || offset > _trackSize)
	{
		return 0;
	}
	return _data[1][offset];
}

// Access track data but get a pointer so you can then read a block directly
float* AudioManager::GetLeftDataPtr(long offset)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    while (!IsDataLoaded(offset))
    {
        logger_base.debug("GetLeftDataPtr waiting for data to be loaded.");
        wxMilliSleep(100);
    }

    wxASSERT(_data[0] != nullptr);
	if (offset > _trackSize)
	{
		return 0;
	}
	return &_data[0][offset];
}

// Access track data but get a pointer so you can then read a block directly
float* AudioManager::GetRightDataPtr(long offset)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    while (!IsDataLoaded(offset))
    {
        logger_base.debug("GetRightDataPtr waiting for data to be loaded.");
        wxMilliSleep(100);
    }

    wxASSERT(_data[1] != nullptr);
	if (offset > _trackSize)
	{
		return 0;
	}
	return &_data[1][offset];
}

// AudioScanJob Functions
// This job runs the frame data extraction on a background thread
AudioScanJob::AudioScanJob(AudioManager* audio)
{
	_audio = audio;
	_status = "Idle.";
}

// Run the job
void AudioScanJob::Process()
{
	_status = "Processing.";
	_audio->DoPrepareFrameData();
	_status = "Done.";
}

// AudioLoadJob Functions
// This job runs the frame data extraction on a background thread
AudioLoadJob::AudioLoadJob(AudioManager* audio, AVFormatContext* formatContext, AVCodecContext* codecContext, AVStream* audioStream, AVFrame* frame)
{
    _formatContext = formatContext;
    _codecContext = codecContext;
    _audioStream = audioStream;
    _frame = frame;
    _audio = audio;
    _status = "Idle.";
}

// Run the job
void AudioLoadJob::Process()
{
    _status = "Processing.";
    _audio->DoLoadAudioData(_formatContext, _codecContext, _audioStream, _frame);
    _status = "Done.";
}

// xLightsVamp Functions
xLightsVamp::xLightsVamp()
{
	_loader = Vamp::HostExt::PluginLoader::getInstance();
}

xLightsVamp::~xLightsVamp() {}

// extract the features data from a Vamp plugins output
void xLightsVamp::ProcessFeatures(Vamp::Plugin::FeatureList &feature, std::vector<int> &starts, std::vector<int> &ends, std::vector<std::string> &labels)
{
	bool hadDuration = true;

	for (size_t x = 0; x < feature.size(); x++)
	{
		int start = feature[x].timestamp.msec() + feature[x].timestamp.sec * 1000;
		starts.push_back(start);

		if (!hadDuration)
		{
			ends.push_back(start);
		}
		hadDuration = feature[x].hasDuration;

		if (hadDuration)
		{
			int end = start + feature[x].duration.msec() + feature[x].duration.sec * 1000;
			ends.push_back(end);
		}
		labels.push_back(feature[x].label);
	}

	if (!hadDuration)
	{
		ends.push_back(starts[starts.size() - 1]);
	}
}

// Load plugins
void xLightsVamp::LoadPlugins(AudioManager* paudio)
{
    // dont need to load it twice
	if (_loadedPlugins.size() > 0)
	{
		return;
	}

    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("Loading plugins.");

    Vamp::HostExt::PluginLoader::PluginKeyList pluginList = _loader->listPlugins();
    
    logger_base.debug("Plugins found %d.", pluginList.size());

	for (size_t x = 0; x < pluginList.size(); x++)
	{
		Vamp::Plugin *p = _loader->loadPlugin(pluginList[x], paudio->GetRate());
		if (p == nullptr)
		{
			// skip any that dont load
			continue;
		}
		_loadedPlugins.push_back(p);
	}
}

// Get & load plugins that return timing marks
std::list<std::string> xLightsVamp::GetAvailablePlugins(AudioManager* paudio)
{
	std::list<std::string> ret;

	// Load the plugins in case they have not already been loaded
	LoadPlugins(paudio);

	for (std::vector<Vamp::Plugin *>::iterator it = _loadedPlugins.begin(); it != _loadedPlugins.end(); ++it)
	{
		Plugin::OutputList outputs = (*it)->getOutputDescriptors();

		for (Plugin::OutputList::iterator j = outputs.begin(); j != outputs.end(); ++j)
		{
			if (j->sampleType == Plugin::OutputDescriptor::FixedSampleRate ||
				j->sampleType == Plugin::OutputDescriptor::OneSamplePerStep ||
				!j->hasFixedBinCount ||
				(j->hasFixedBinCount && j->binCount > 1))
			{
				// We are filering out this from our return array
				continue;
			}

			std::string name = std::string(wxString::FromUTF8((*it)->getName().c_str()).c_str());

			if (outputs.size() > 1)
			{
				// This is not the plugin's only output.
				// Use "plugin name: output name" as the effect name,
				// unless the output name is the same as the plugin name
				std::string outputName = std::string(wxString::FromUTF8(j->name.c_str()).c_str());
				if (outputName != name)
				{
					std::ostringstream stringStream;
					stringStream << name << ": " << outputName.c_str();
					name = stringStream.str();
				}
			}

			_plugins[name] = (*it);
		}
	}

	for (std::map<std::string, Vamp::Plugin *>::iterator it = _plugins.begin(); it != _plugins.end(); ++it)
	{
		ret.push_back(it->first);
	}

	return ret;
}

// Get a list of all plugins
std::list<std::string> xLightsVamp::GetAllAvailablePlugins(AudioManager* paudio)
{
    std::list<std::string> ret;

	// load the plugins if they have not already been loaded
	LoadPlugins(paudio);

	for (std::vector<Vamp::Plugin *>::iterator it = _loadedPlugins.begin(); it != _loadedPlugins.end(); ++it)
	{
		Plugin::OutputList outputs = (*it)->getOutputDescriptors();

		for (Plugin::OutputList::iterator j = outputs.begin(); j != outputs.end(); ++j)
		{
			std::string name = std::string(wxString::FromUTF8((*it)->getName().c_str()).c_str());

			if (outputs.size() > 1)
			{
				// This is not the plugin's only output.
				// Use "plugin name: output name" as the effect name,
				// unless the output name is the same as the plugin name
				std::string outputName = std::string(wxString::FromUTF8(j->name.c_str()).c_str());
				if (outputName != name)
				{
					std::ostringstream stringStream;
					stringStream << name << ": " << outputName.c_str();
					name = stringStream.str();
				}
			}

			_allplugins[name] = *it;
		}
	}

	for (std::map<std::string, Vamp::Plugin *>::iterator it = _allplugins.begin(); it != _allplugins.end(); ++it)
	{
		ret.push_back(it->first);
	}

	return ret;
}

// Get a plugin
Vamp::Plugin* xLightsVamp::GetPlugin(std::string name)
{
    Plugin* p = _plugins[name];

	if (p == nullptr)
	{
		p = _allplugins[name];
	}

	return p;
}

void SDL::SetAudioDevice(const std::string device)
{
    _device = device;
}

void AudioManager::SetAudioDevice(const std::string device)
{
    __sdl.SetAudioDevice(device);
}

std::list<std::string> AudioManager::GetAudioDevices()
{
    return __sdl.GetAudioDevices();
}
