/*
 * See the README file for copyright information and how to reach the author.
 *
 * $Id$
 */

#include "omxdevice.h"
#include "omx.h"
#include "audio.h"
#include "display.h"
#include "setup.h"

#include <vdr/thread.h>
#include <vdr/remux.h>
#include <vdr/tools.h>
#include <vdr/skins.h>

#include <string.h>

#define S(x) ((int)(floor(x * pow(2, 16))))

// trick speeds as defined in vdr/dvbplayer.c
const int cOmxDevice::s_playbackSpeeds[eNumDirections][eNumPlaybackSpeeds] = {
	{ S(0.0f), S( 0.125f), S( 0.25f), S( 0.5f), S( 1.0f), S( 2.0f), S( 4.0f), S( 12.0f) },
	{ S(0.0f), S(-0.125f), S(-0.25f), S(-0.5f), S(-1.0f), S(-2.0f), S(-4.0f), S(-12.0f) }
};

// speed correction factors for live mode
// HDMI specification allows a tolerance of 1000ppm, however on the Raspberry Pi
// it's limited to 175ppm to avoid audio drops one some A/V receivers
const int cOmxDevice::s_liveSpeeds[eNumLiveSpeeds] = {
	S(0.999f), S(0.99985f), S(1.000f), S(1.00015), S(1.001)
};

const uchar cOmxDevice::PesVideoHeader[14] = {
	0x00, 0x00, 0x01, 0xe0, 0x00, 0x00, 0x80, 0x80, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00
};

cOmxDevice::cOmxDevice(void (*onPrimaryDevice)(void)) :
	cDevice(),
	m_onPrimaryDevice(onPrimaryDevice),
	m_omx(new cOmx()),
	m_audio(new cRpiAudioDecoder(m_omx)),
	m_mutex(new cMutex()),
	m_videoCodec(cVideoCodec::eInvalid),
	m_liveSpeed(eNoCorrection),
	m_playbackSpeed(eNormal),
	m_direction(eForward),
	m_hasVideo(false),
	m_hasAudio(false),
	m_skipAudio(false),
	m_playDirection(0),
	m_trickRequest(0),
	m_audioPts(0),
	m_videoPts(0),
	m_audioId(0),
	m_latencySamples(0),
	m_latencyTarget(0),
	m_posMaxCorrections(0),
	m_negMaxCorrections(0)
{
}

cOmxDevice::~cOmxDevice()
{
	DeInit();

	delete m_omx;
	delete m_audio;
	delete m_mutex;
}

int cOmxDevice::Init(void)
{
	if (m_omx->Init() < 0)
	{
		ELOG("failed to initialize OMX!");
		return -1;
	}
	if (m_audio->Init() < 0)
	{
		ELOG("failed to initialize audio!");
		return -1;
	}
	m_omx->SetBufferStallCallback(&OnBufferStall, this);
	m_omx->SetEndOfStreamCallback(&OnEndOfStream, this);
	m_omx->SetStreamStartCallback(&OnStreamStart, this);

	cRpiSetup::SetVideoSetupChangedCallback(&OnVideoSetupChanged, this);

	return 0;
}

int cOmxDevice::DeInit(void)
{
	cRpiSetup::SetVideoSetupChangedCallback(0);
	if (m_audio->DeInit() < 0)
	{
		ELOG("failed to deinitialize audio!");
		return -1;
	}
	if (m_omx->DeInit() < 0)
	{
		ELOG("failed to deinitialize OMX!");
		return -1;
	}
	return 0;
}

bool cOmxDevice::Start(void)
{
	HandleVideoSetupChanged();
	return true;
}

void cOmxDevice::GetOsdSize(int &Width, int &Height, double &PixelAspect)
{
	cRpiDisplay::GetSize(Width, Height, PixelAspect);
}

void cOmxDevice::GetVideoSize(int &Width, int &Height, double &VideoAspect)
{
	bool interlaced;
	int frameRate;

	m_omx->GetVideoFormat(Width, Height, frameRate, interlaced);

	if (Height)
		VideoAspect = (double)Width / Height;
	else
		VideoAspect = 1.0;
}

void cOmxDevice::ScaleVideo(const cRect &Rect)
{
	DBG("ScaleVideo(%d, %d, %d, %d)",
		Rect.X(), Rect.Y(), Rect.Width(), Rect.Height());

	m_omx->SetDisplayRegion(Rect.X(), Rect.Y(), Rect.Width(), Rect.Height());
}

bool cOmxDevice::SetPlayMode(ePlayMode PlayMode)
{
	m_mutex->Lock();
	DBG("SetPlayMode(%s)",
		PlayMode == pmNone			 ? "none" 			   :
		PlayMode == pmAudioVideo	 ? "Audio/Video" 	   :
		PlayMode == pmAudioOnly		 ? "Audio only" 	   :
		PlayMode == pmAudioOnlyBlack ? "Audio only, black" :
		PlayMode == pmVideoOnly		 ? "Video only" 	   : 
									   "unsupported");

	// Stop audio / video if play mode is set to pmNone. Start
	// is triggered once a packet is going to be played, since
	// we don't know what kind of stream we'll get (audio-only,
	// video-only or both) after SetPlayMode() - VDR will always
	// pass pmAudioVideo as argument.

	switch (PlayMode)
	{
	case pmNone:
		FlushStreams(true);
		if (m_hasVideo)
			m_omx->StopVideo();

		m_hasAudio = false;
		m_hasVideo = false;
		m_videoCodec = cVideoCodec::eInvalid;
		break;

	case pmAudioVideo:
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	case pmVideoOnly:
		m_playbackSpeed = eNormal;
		m_direction = eForward;
		break;

	default:
		break;
	}

	m_mutex->Unlock();
	return true;
}

void cOmxDevice::StillPicture(const uchar *Data, int Length)
{
	if (Data[0] == 0x47)
		cDevice::StillPicture(Data, Length);
	else
	{
		DBG("StillPicture()");
		int pesLength = 0;
		uchar *pesPacket = 0;

		cVideoCodec::eCodec codec = ParseVideoCodec(Data, Length);
		if (codec != cVideoCodec::eInvalid)
		{
			// some plugins deliver raw MPEG data, but PlayVideo() needs a
			// complete PES packet with valid header
			pesLength = Length + sizeof(PesVideoHeader);
			pesPacket = MALLOC(uchar, pesLength);
			if (!pesPacket)
				return;

			memcpy(pesPacket, PesVideoHeader, sizeof(PesVideoHeader));
			memcpy(pesPacket + sizeof(PesVideoHeader), Data, Length);
		}
		else
			codec = ParseVideoCodec(Data + PesPayloadOffset(Data),
					Length - PesPayloadOffset(Data));

		if (codec == cVideoCodec::eInvalid)
			return;

		m_mutex->Lock();
		m_playbackSpeed = eNormal;
		m_direction = eForward;
		m_omx->StopClock();

		// to get a picture displayed, PlayVideo() needs to be called
		// 4x for MPEG2 and 10x for H264... ?
		int repeat = codec == cVideoCodec::eMPEG2 ? 4 : 10;
		while (repeat--)
		{
			int length = pesPacket ? pesLength : Length;
			const uchar *data = pesPacket ? pesPacket : Data;

			// play every single PES packet, rise ENDOFFRAME flag on last
			while (PesLongEnough(length))
			{
				int pktLen = PesHasLength(data) ? PesLength(data) : length;

				// skip non-video packets as they may occur in PES recordings
				if ((data[3] & 0xf0) == 0xe0)
					PlayVideo(data, pktLen, pktLen == length);

				data += pktLen;
				length -= pktLen;
			}
		}
		SubmitEOS();
		m_mutex->Unlock();

		if (pesPacket)
			free(pesPacket);
	}
}

int cOmxDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
	m_mutex->Lock();

	if (!m_hasAudio)
	{
		m_hasAudio = true;
		m_audioId = Id;
		m_omx->SetClockReference(cOmx::eClockRefAudio);

		if (!m_hasVideo)
		{
			DBG("audio first");
			m_omx->SetClockScale(s_playbackSpeeds[m_direction][m_playbackSpeed]);
			m_omx->StartClock(m_hasVideo, m_hasAudio);
		}

		if (Transferring())
			ResetLatency();
	}

	int64_t pts = PesHasPts(Data) ? PesGetPts(Data) : 0;

	// keep track of direction in case of trick speed
	if (m_trickRequest && pts)
	{
		if (m_audioPts)
			PtsTracker(PtsDiff(m_audioPts, pts));

		m_audioPts = pts;
	}

	if (Transferring() && pts)
	{
		if (m_audioId != Id)
		{
			ResetLatency();
			m_audioId = Id;
		}
		UpdateLatency(pts);
	}

	int ret = Length;
	int length = Length - PesPayloadOffset(Data);

	// ignore packets with invalid payload offset
	if (length > 0)
	{
		const uchar *data = Data + PesPayloadOffset(Data);

		// remove audio substream header as seen in PES recordings with AC3
		// audio track (0x80: AC3, 0x88: DTS, 0xA0: LPCM)
		if ((data[0] == 0x80 || data[0] == 0x88 || data[0] == 0xa0)
				&& data[0] == Id)
		{
			data += 4;
			length -= 4;
		}
		if (!m_audio->WriteData(data, length, pts))
			ret = 0;
	}
	m_mutex->Unlock();
	return ret;
}

int cOmxDevice::PlayVideo(const uchar *Data, int Length, bool EndOfFrame)
{
	m_mutex->Lock();
	int ret = Length;

	cVideoCodec::eCodec codec = PesHasPts(Data) ? ParseVideoCodec(
			Data + PesPayloadOffset(Data), Length - PesPayloadOffset(Data)) :
			cVideoCodec::eInvalid;

	// video restart after Clear() with same codec
	bool videoRestart = (!m_hasVideo && codec == m_videoCodec &&
			cRpiSetup::IsVideoCodecSupported(codec));

	// video restart after SetPlayMode() or codec changed
	if (codec != cVideoCodec::eInvalid && codec != m_videoCodec)
	{
		m_videoCodec = codec;

		if (m_hasVideo)
		{
			m_omx->StopVideo();
			m_hasVideo = false;
		}

		if (cRpiSetup::IsVideoCodecSupported(codec))
		{
			videoRestart = true;
			m_omx->SetVideoCodec(codec);
			DLOG("set video codec to %s", cVideoCodec::Str(codec));
		}
		else
			Skins.QueueMessage(mtError, tr("video format not supported!"));
	}

	if (videoRestart)
	{
		m_hasVideo = true;

		if (!m_hasAudio)
		{
			DBG("video first");
			m_omx->SetClockReference(cOmx::eClockRefVideo);
			m_omx->SetClockScale(s_playbackSpeeds[m_direction][m_playbackSpeed]);
			m_omx->StartClock(m_hasVideo, m_hasAudio);
		}

		if (Transferring())
			ResetLatency();
	}

	if (m_hasVideo)
	{
		int64_t pts = PesHasPts(Data) ? PesGetPts(Data) : 0;

		// keep track of direction in case of trick speed
		if (m_trickRequest && pts && m_videoPts)
			PtsTracker(PtsDiff(m_videoPts, pts));

		if (!m_hasAudio && Transferring() && pts)
			UpdateLatency(pts);

		// skip PES header, proceed with payload towards OMX
		Length -= PesPayloadOffset(Data);
		Data += PesPayloadOffset(Data);

		while (Length > 0)
		{
			OMX_BUFFERHEADERTYPE *buf = m_omx->GetVideoBuffer(pts);
			if (buf)
			{
				buf->nFilledLen = buf->nAllocLen < (unsigned)Length ?
						buf->nAllocLen : Length;

				memcpy(buf->pBuffer, Data, buf->nFilledLen);
				Length -= buf->nFilledLen;
				Data += buf->nFilledLen;

				if (EndOfFrame && !Length)
					buf->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

				if (!m_omx->EmptyVideoBuffer(buf))
				{
					ret = 0;
					ELOG("failed to pass buffer to video decoder!");
					break;
				}
			}
			else
			{
				ret = 0;
				break;
			}
			pts = 0;
		}
	}
	m_mutex->Unlock();
	return ret;
}

bool cOmxDevice::SubmitEOS(void)
{
	DBG("SubmitEOS()");
	OMX_BUFFERHEADERTYPE *buf = m_omx->GetVideoBuffer(0);
	if (buf)
		buf->nFlags = /*OMX_BUFFERFLAG_ENDOFFRAME | */ OMX_BUFFERFLAG_EOS;

	return m_omx->EmptyVideoBuffer(buf);
}

int64_t cOmxDevice::GetSTC(void)
{
	return m_omx->GetSTC();
}

uchar *cOmxDevice::GrabImage(int &Size, bool Jpeg, int Quality,
		int SizeX, int SizeY)
{
	DBG("GrabImage(%s, %dx%d)", Jpeg ? "JPEG" : "PNM", SizeX, SizeY);

	uint8_t* ret = NULL;
	int width, height;
	cRpiDisplay::GetSize(width, height);

	SizeX = (SizeX > 0) ? SizeX : width;
	SizeY = (SizeY > 0) ? SizeY : height;
	Quality = (Quality >= 0) ? Quality : 100;

	// bigger than needed, but uint32_t ensures proper alignment
	uint8_t* frame = (uint8_t*)(MALLOC(uint32_t, SizeX * SizeY));

	if (!frame)
	{
		ELOG("failed to allocate image buffer!");
		return ret;
	}

	if (cRpiDisplay::Snapshot(frame, SizeX, SizeY))
	{
		ELOG("failed to grab image!");
		free(frame);
		return ret;
	}

	if (Jpeg)
		ret = RgbToJpeg(frame, SizeX, SizeY, Size, Quality);
	else
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", SizeX, SizeY);
		int l = strlen(buf);
		Size = l + SizeX * SizeY * 3;
		ret = MALLOC(uint8_t, Size);
		if (ret)
		{
			memcpy(ret, buf, l);
			memcpy(ret + l, frame, SizeX * SizeY * 3);
		}
	}
	free(frame);
	return ret;
}

void cOmxDevice::Clear(void)
{
	DBG("Clear()");
	m_mutex->Lock();

	FlushStreams();
	m_hasAudio = false;
	m_hasVideo = false;

	m_mutex->Unlock();
	cDevice::Clear();
}

void cOmxDevice::Play(void)
{
	DBG("Play()");
	m_mutex->Lock();

	m_playbackSpeed = eNormal;
	m_direction = eForward;
	m_omx->SetClockScale(s_playbackSpeeds[m_direction][m_playbackSpeed]);

	m_mutex->Unlock();
	cDevice::Play();
}

void cOmxDevice::Freeze(void)
{
	DBG("Freeze()");
	m_mutex->Lock();

	m_omx->SetClockScale(s_playbackSpeeds[eForward][ePause]);

	m_mutex->Unlock();
	cDevice::Freeze();
}

#if APIVERSNUM >= 20103
void cOmxDevice::TrickSpeed(int Speed, bool Forward)
{
	m_mutex->Lock();
	ApplyTrickSpeed(Speed, Forward);
	m_mutex->Unlock();
}
#else
void cOmxDevice::TrickSpeed(int Speed)
{
	m_mutex->Lock();
	m_audioPts = 0;
	m_videoPts = 0;
	m_playDirection = 0;

	// play direction is ambiguous for fast modes, start PTS tracking
	if (Speed == 1 || Speed == 3 || Speed == 6)
		m_trickRequest = Speed;
	else
		ApplyTrickSpeed(Speed, (Speed == 8 || Speed == 4 || Speed == 2));

	m_mutex->Unlock();
}
#endif

void cOmxDevice::ApplyTrickSpeed(int trickSpeed, bool forward)
{
	m_direction = forward ? eForward : eBackward;
	m_playbackSpeed =

		// slow forward
		trickSpeed ==  8 ? eSlowest :
		trickSpeed ==  4 ? eSlower  :
		trickSpeed ==  2 ? eSlow    :

		// fast for-/backward
		trickSpeed ==  6 ? eFast    :
		trickSpeed ==  3 ? eFaster  :
		trickSpeed ==  1 ? eFastest :

		// slow backward
		trickSpeed == 63 ? eSlowest :
		trickSpeed == 48 ? eSlower  :
		trickSpeed == 24 ? eSlow    : eNormal;

	m_omx->SetClockScale(s_playbackSpeeds[m_direction][m_playbackSpeed]);

	DBG("ApplyTrickSpeed(%s, %s)",
			PlaybackSpeedStr(m_playbackSpeed), DirectionStr(m_direction));
	return;
}

void cOmxDevice::PtsTracker(int64_t ptsDiff)
{
	DBG("PtsTracker(%lld)", ptsDiff);

	if (ptsDiff < 0)
		--m_playDirection;
	else if (ptsDiff > 0)
		m_playDirection += 2;

	if (m_playDirection < -2 || m_playDirection > 3)
	{
		ApplyTrickSpeed(m_trickRequest, m_playDirection > 0);
		m_trickRequest = 0;
	}
}

bool cOmxDevice::HasIBPTrickSpeed(void)
{
	return !m_hasVideo;
}

void cOmxDevice::UpdateLatency(int64_t pts)
{
	if (!pts || !m_omx->IsClockRunning())
		return;

	int64_t stc = m_omx->GetSTC();
	if (!stc || pts <= stc)
		return;

	for (int i = LATENCY_FILTER_SIZE - 1; i > 0; i--)
		m_latency[i] = m_latency[i - 1];
	m_latency[0] = (pts - stc) / 90;

	if (m_latencySamples < LATENCY_FILTER_SIZE - 1)
	{
		m_latencySamples++;
		return;
	}

#ifdef DEBUG_LATENCY
	eLiveSpeed oldSpeed = m_liveSpeed;
#endif
	int average = 0;

	for (int i = 0; i < LATENCY_FILTER_SIZE; i++)
		average += m_latency[i];
	average = average / LATENCY_FILTER_SIZE;

	if (!m_latencyTarget)
		m_latencyTarget = 1.4f * average;

	if (average > 2.0f * m_latencyTarget)
	{
		if (m_liveSpeed < ePosMaxCorrection)
		{
			m_liveSpeed = ePosMaxCorrection;
			m_posMaxCorrections++;
			DBG("latency too big, speeding up...");
		}
	}
	else if (average < 0.5f * m_latencyTarget)
	{
		if (m_liveSpeed > eNegMaxCorrection)
		{
			m_liveSpeed = eNegMaxCorrection;
			m_negMaxCorrections++;
			DBG("latency too small, slowing down...");
		}
	}
	else if (average > 1.1f * m_latencyTarget)
	{
		if (m_liveSpeed < ePosMaxCorrection)
			m_liveSpeed = ePosCorrection;
	}
	else if (average < 0.9f * m_latencyTarget)
	{
		if (m_liveSpeed > eNegMaxCorrection)
			m_liveSpeed = eNegCorrection;
	}
	else if (average > m_latencyTarget)
	{
		if (m_liveSpeed < eNoCorrection)
			m_liveSpeed = eNoCorrection;
	}
	else if (average < m_latencyTarget)
	{
		if (m_liveSpeed > eNoCorrection)
			m_liveSpeed = eNoCorrection;
	} else
		m_liveSpeed = eNoCorrection;

	m_omx->SetClockScale(s_liveSpeeds[m_liveSpeed]);

#ifdef DEBUG_LATENCY
	if (oldSpeed != m_liveSpeed)
	{
		DLOG("%s%s latency = %4dms, target = %4dms, corr = %s, "
				"max neg/pos corr = %d/%d",
				m_hasAudio ? "A" : "-",  m_hasVideo ? "V" : "-",
				average, m_latencyTarget,
				m_liveSpeed == eNegMaxCorrection ? "--|  " :
				m_liveSpeed == eNegCorrection    ? " -|  " :
				m_liveSpeed == eNoCorrection     ? "  |  " :
				m_liveSpeed == ePosCorrection    ? "  |+ " :
				m_liveSpeed == ePosMaxCorrection ? "  |++" : "  ?  ",
				m_negMaxCorrections, m_posMaxCorrections);
	}
#endif
}

void cOmxDevice::ResetLatency(void)
{
	m_latencySamples = - LATENCY_FILTER_PREROLL;
	m_latencyTarget = 0;
	m_liveSpeed = eNoCorrection;
	m_posMaxCorrections = 0;
	m_negMaxCorrections = 0;
}

void cOmxDevice::HandleBufferStall()
{
	ELOG("buffer stall!");
	m_mutex->Lock();

	FlushStreams();
	m_omx->SetClockScale(s_playbackSpeeds[m_direction][m_playbackSpeed]);
	m_omx->StartClock(m_hasVideo, m_hasAudio);

	m_mutex->Unlock();
}

void cOmxDevice::HandleEndOfStream()
{
	DBG("HandleEndOfStream()");
	m_mutex->Lock();

	// flush pipes and restart clock after still image
	FlushStreams();
	m_omx->SetClockScale(s_playbackSpeeds[m_direction][m_playbackSpeed]);
	m_omx->StartClock(m_hasVideo, m_hasAudio);

	m_mutex->Unlock();
}

void cOmxDevice::HandleStreamStart()
{
	DBG("HandleStreamStart()");

	int width, height, frameRate;
	bool interlaced;

	m_omx->GetVideoFormat(width, height, frameRate, interlaced);

	DLOG("video stream started %dx%d@%d%s",
			width, height, frameRate, interlaced ? "i" : "p");

	cRpiDisplay::SetVideoFormat(width, height, frameRate, interlaced);
}

void cOmxDevice::HandleVideoSetupChanged()
{
	DBG("HandleVideoSettingsChanged()");

	switch (cRpiSetup::GetVideoFraming())
	{
	default:
	case cVideoFraming::eFrame:
		m_omx->SetDisplayMode(false, false);
		break;

	case cVideoFraming::eCut:
		m_omx->SetDisplayMode(true, false);
		break;

	case cVideoFraming::eStretch:
		m_omx->SetDisplayMode(true, true);
		break;
	}

	int width, height, frameRate;
	bool interlaced;

	m_omx->GetVideoFormat(width, height, frameRate, interlaced);
	cRpiDisplay::SetVideoFormat(width, height, frameRate, interlaced);
}

void cOmxDevice::FlushStreams(bool flushVideoRender)
{
	DBG("FlushStreams(%s)", flushVideoRender ? "flushVideoRender" : "");

	m_omx->StopClock();
	m_omx->SetClockScale(0.0f);

	if (m_hasVideo)
		m_omx->FlushVideo(flushVideoRender);

	if (m_hasAudio)
		m_audio->Reset();

	m_omx->SetCurrentReferenceTime(0);
}

void cOmxDevice::SetVolumeDevice(int Volume)
{
	DBG("SetVolume(%d)", Volume);
	if (Volume)
	{
		m_omx->SetVolume(Volume);
		m_omx->SetMute(false);
	}
	else
		m_omx->SetMute(true);
}

bool cOmxDevice::Poll(cPoller &Poller, int TimeoutMs)
{
	cTimeMs time;
	time.Set();
	while (!m_omx->PollVideoBuffers() || !m_audio->Poll())
	{
		if (time.Elapsed() >= (unsigned)TimeoutMs)
			return false;
		cCondWait::SleepMs(5);
	}
	return true;
}

void cOmxDevice::MakePrimaryDevice(bool On)
{
	if (On && m_onPrimaryDevice)
		m_onPrimaryDevice();
	cDevice::MakePrimaryDevice(On);
}

cVideoCodec::eCodec cOmxDevice::ParseVideoCodec(const uchar *data, int length)
{
	const uchar *p = data;

	for (int i = 0; (i < 5) && (i + 4 < length); i++)
	{
		// find start code prefix - should be right at the beginning of payload
		if ((!p[i] && !p[i + 1] && p[i + 2] == 0x01))
		{
			if (p[i + 3] == 0xb3)		// sequence header
				return cVideoCodec::eMPEG2;

			//p[i + 3] = 0xf0
			else if (p[i + 3] == 0x09)	// slice
			{
				// quick hack for converted mkvs
				if (p[i + 4] == 0xf0)
					return cVideoCodec::eH264;

				switch (p[i + 4] >> 5)
				{
				case 0: case 3: case 5: // I frame
					return cVideoCodec::eH264;

				case 2: case 7:			// B frame
				case 1: case 4: case 6:	// P frame
				default:
//					return cVideoCodec::eInvalid;
					return cVideoCodec::eH264;
				}
			}
			return cVideoCodec::eInvalid;
		}
	}
	return cVideoCodec::eInvalid;
}
