/*
* XBMC Media Center
* Copyright (c) 2002 d7o3g4q and RUNTiME
* Portions Copyright (c) by the authors of ffmpeg and xvid
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#endif

#include "OMXAudio.h"
#include "utils/log.h"

#define CLASSNAME "COMXAudio"

#include "linux/XMemUtils.h"

#ifndef VOLUME_MINIMUM
#define VOLUME_MINIMUM 0
#endif

#include <algorithm>

using namespace std;

#define OMX_MAX_CHANNELS 10

static enum PCMChannels OMXChannelMap[OMX_MAX_CHANNELS] =
{
  PCM_FRONT_LEFT  , PCM_FRONT_RIGHT,
  PCM_FRONT_CENTER, PCM_LOW_FREQUENCY,
  PCM_BACK_LEFT   , PCM_BACK_RIGHT,
  PCM_SIDE_LEFT   , PCM_SIDE_RIGHT,
  PCM_BACK_CENTER
};

static enum OMX_AUDIO_CHANNELTYPE OMXChannels[OMX_MAX_CHANNELS] =
{
  OMX_AUDIO_ChannelLF, OMX_AUDIO_ChannelRF,
  OMX_AUDIO_ChannelCF, OMX_AUDIO_ChannelLFE,
  OMX_AUDIO_ChannelLR, OMX_AUDIO_ChannelRR,
  OMX_AUDIO_ChannelLS, OMX_AUDIO_ChannelRS,
  OMX_AUDIO_ChannelCS, OMX_AUDIO_ChannelNone
};

static unsigned int WAVEChannels[OMX_MAX_CHANNELS] =
{
  SPEAKER_FRONT_LEFT,       SPEAKER_FRONT_RIGHT,
  SPEAKER_TOP_FRONT_CENTER, SPEAKER_LOW_FREQUENCY,
  SPEAKER_BACK_LEFT,        SPEAKER_BACK_RIGHT,
  SPEAKER_SIDE_LEFT,        SPEAKER_SIDE_RIGHT,
  SPEAKER_BACK_CENTER,      0
};

static const uint16_t AC3Bitrates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
static const uint16_t AC3FSCod   [] = {48000, 44100, 32000, 0};

static const uint16_t DTSFSCod   [] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0};

// 7.1 downmixing coefficients
const float downmixing_coefficients_8[OMX_AUDIO_MAXCHANNELS] = {
  //        L       R
  /* L */   1,      0,
  /* R */   0,      1,
  /* C */   0.7071, 0.7071,
  /* LFE */ 0.7071, 0.7071,
  /* Ls */  0.7071, 0,
  /* Rs */  0,      0.7071,
  /* Lr */  0.7071, 0,
  /* Rr */  0,      0.7071
};

// 7.1 downmixing coefficients with boosted centre channel
const float downmixing_coefficients_8_boostcentre[OMX_AUDIO_MAXCHANNELS] = {
  //        L       R
  /* L */   0.7071, 0,
  /* R */   0,      0.7071,
  /* C */   1,      1,
  /* LFE */ 0.7071, 0.7071,
  /* Ls */  0.7071, 0,
  /* Rs */  0,      0.7071,
  /* Lr */  0.7071, 0,
  /* Rr */  0,      0.7071
};

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
//***********************************************************************************************
COMXAudio::COMXAudio() :
  m_Initialized     (false  ),
  m_CurrentVolume   (0      ),
  m_Mute            (false  ),
  m_drc             (0      ),
  m_Passthrough     (false  ),
  m_HWDecode        (false  ),
  m_normalize_downmix(true   ),
  m_BytesPerSec     (0      ),
  m_BufferLen       (0      ),
  m_ChunkLen        (0      ),
  m_InputChannels   (0      ),
  m_downmix_channels(0      ),
  m_BitsPerSample   (0      ),
  m_maxLevel        (0.0f   ),
  m_avgLevel        (0.0f   ),
  m_amplification   (1.0f   ),
  m_attenuation     (1.0f   ),
  m_desired_attenuation(1.0f),
  m_omx_clock       (NULL   ),
  m_av_clock        (NULL   ),
  m_settings_changed(false  ),
  m_setStartTime    (false  ),
  m_LostSync        (true   ),
  m_SampleRate      (0      ),
  m_eEncoding       (OMX_AUDIO_CodingPCM),
  m_extradata       (NULL   ),
  m_extrasize       (0      ),
  m_last_pts        (DVD_NOPTS_VALUE),
  m_submitted_eos   (false  ),
  m_failed_eos      (false  ),
  m_fifo_size       (0.0    )
{
}

COMXAudio::~COMXAudio()
{
  Deinitialize();
}


bool COMXAudio::PortSettingsChanged()
{
  CSingleLock lock (m_critSection);
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  if (m_settings_changed)
  {
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    return true;
  }

  if(!m_Passthrough)
  {
    if(!m_omx_mixer.Initialize("OMX.broadcom.audio_mixer", OMX_IndexParamAudioInit))
      return false;
  }

  if(!m_omx_render.Initialize("OMX.broadcom.audio_render", OMX_IndexParamAudioInit))
    return false;

  ApplyVolume();

  if(!m_Passthrough)
  {
    /* setup mixer output */
    OMX_INIT_STRUCTURE(m_pcm_output);
    m_pcm_output.nPortIndex      = m_omx_decoder.GetOutputPort();
    omx_err = m_omx_decoder.GetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
    if(omx_err != OMX_ErrorNone)
    {
       CLog::Log(LOGERROR, "%s::%s - error SetParameter 1 output omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    }
    m_pcm_output.nPortIndex      = m_omx_mixer.GetOutputPort();

    /* mixer output is always stereo */
    m_pcm_output.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
    m_pcm_output.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
    m_pcm_output.nChannels = 2;

    omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - error SetParameter 1 output omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    }
    CLog::Log(LOGDEBUG, "%s::%s - Output bps %d samplerate %d channels %d buffer size %d bytes per second %d",
        CLASSNAME, __func__, (int)m_pcm_output.nBitPerSample, (int)m_pcm_output.nSamplingRate, (int)m_pcm_output.nChannels, m_BufferLen, m_BytesPerSec);
    PrintPCM(&m_pcm_output, std::string("output"));
  }

  m_omx_tunnel_clock.Initialize(m_omx_clock, m_omx_clock->GetInputPort(), &m_omx_render, m_omx_render.GetInputPort()+1);

  omx_err = m_omx_tunnel_clock.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_clock.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_omx_render.ResetEos();

  // By default audio_render is the clock master, and if output samples don't fit the timestamps, it will speed up/slow down the clock.
  // This tends to be better for maintaining audio sync and avoiding audio glitches, but can affect video/display sync
  if(1)
  {
    OMX_CONFIG_BOOLEANTYPE configBool;
    OMX_INIT_STRUCTURE(configBool);
    configBool.bEnabled = OMX_FALSE;

    omx_err = m_omx_render.SetConfig(OMX_IndexConfigBrcmClockReferenceSource, &configBool);
    if (omx_err != OMX_ErrorNone)
       return false;
  }

  OMX_CONFIG_BRCMAUDIODESTINATIONTYPE audioDest;
  OMX_INIT_STRUCTURE(audioDest);
  strncpy((char *)audioDest.sName, m_deviceuse.c_str(), strlen(m_deviceuse.c_str()));

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigBrcmAudioDestination, &audioDest);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_render.SetConfig omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if(!m_Passthrough)
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_mixer, m_omx_mixer.GetInputPort());
    m_omx_tunnel_mixer.Initialize(&m_omx_mixer, m_omx_mixer.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
    printf("A:PortSettingsChanged: bits:%d mode:%d channels:%d srate:%d nopassthrough\n",
            (int)m_pcm_input.nBitPerSample, m_pcm_input.ePCMMode, (int)m_pcm_input.nChannels, (int)m_pcm_input.nSamplingRate);
  }
  else
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
    printf("A:PortSettingsChanged: bits:%d mode:%d channels:%d srate:%d passthrough\n",
            0, 0, 0, 0);
  }

  omx_err = m_omx_tunnel_decoder.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_decoder.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }
  if(!m_Passthrough)
  {
    omx_err = m_omx_mixer.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone) {
      CLog::Log(LOGERROR, "%s::%s - m_omx_mixer OMX_StateExecuting omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
    omx_err = m_omx_tunnel_mixer.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_decoder.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  omx_err = m_omx_render.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_render OMX_StateExecuting omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_settings_changed = true;
  return true;
}

bool COMXAudio::Initialize(const CStdString& device, int iChannels, enum PCMChannels *channelMap,
                           COMXStreamInfo &hints, unsigned int downmixChannels, unsigned int uiSampleRate, unsigned int uiBitsPerSample, bool boostOnDownmix,
                           OMXClock *clock, bool bUsePassthrough, bool bUseHWDecode, float fifo_size)
{
  CSingleLock lock (m_critSection);
  OMX_ERRORTYPE omx_err;

  Deinitialize();

  if(!m_dllAvUtil.Load())
    return false;

  if(device == "hdmi") {
    m_deviceuse = "hdmi";
  } else {
    m_deviceuse = "local";
  }

  m_HWDecode    = bUseHWDecode;
  m_Passthrough = bUsePassthrough;
  m_InputChannels = iChannels;
  m_fifo_size = fifo_size;
  m_downmix_channels = downmixChannels;
  m_normalize_downmix = !boostOnDownmix;

  if(m_InputChannels == 0)
    return false;

  if(hints.samplerate == 0)
    return false;

  m_av_clock = clock;

  if(!m_av_clock)
    return false;

  /* passthrough overwrites hw decode */
  if(m_Passthrough)
  {
    m_HWDecode = false;
  }
  else if(m_HWDecode)
  {
    /* check again if we are capable to hw decode the format */
    m_HWDecode = CanHWDecode(hints.codec);
  }

  if(m_Passthrough || m_HWDecode)
    SetCodingType(hints.codec);
  else
    SetCodingType(CODEC_ID_PCM_S16LE);

  if(hints.extrasize > 0 && hints.extradata != NULL)
  {
    m_extrasize = hints.extrasize;
    m_extradata = (uint8_t *)malloc(m_extrasize);
    memcpy(m_extradata, hints.extradata, hints.extrasize);
  }

  m_omx_clock = m_av_clock->GetOMXClock();

  m_drc         = 0;

  memset(m_input_channels, 0x0, sizeof(m_input_channels));

  memset(&m_wave_header, 0x0, sizeof(m_wave_header));

  for(int i = 0; i < OMX_AUDIO_MAXCHANNELS; i++)
  {
    m_pcm_input.eChannelMapping[i] = OMX_AUDIO_ChannelNone;
    m_input_channels[i] = OMX_AUDIO_ChannelMax;
  }

  m_input_channels[0] = OMX_AUDIO_ChannelLF;
  m_input_channels[1] = OMX_AUDIO_ChannelRF;
  m_input_channels[2] = OMX_AUDIO_ChannelMax;

  m_wave_header.Format.nChannels  = 2;
  m_wave_header.dwChannelMask     = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  // set the input format, and get the channel layout so we know what we need to open
  enum PCMChannels *outLayout = NULL;

  m_remap.Reset();
  if (!m_Passthrough && channelMap)
    outLayout = m_remap.SetInputFormat (iChannels, channelMap, uiBitsPerSample / 8, uiSampleRate);;

  if (!m_Passthrough && channelMap && outLayout)
  {
    /* setup input channel map */
    int map = 0;
    int chan = 0;

    for (unsigned int ch = 0; ch < m_InputChannels; ++ch)
    {
      for(map = 0; map < OMX_MAX_CHANNELS; ++map)
      {
        if (channelMap[ch] == OMXChannelMap[map])
        {
          m_input_channels[chan] = OMXChannels[map];
          m_wave_header.dwChannelMask |= WAVEChannels[map];
          chan++;
          break;
        }
      }
    }
  }

  OMX_INIT_STRUCTURE(m_pcm_input);

  memcpy(m_pcm_input.eChannelMapping, m_input_channels, sizeof(m_input_channels));

  m_SampleRate = uiSampleRate;
  m_BitsPerSample = uiBitsPerSample;
  m_BufferLen     = m_BytesPerSec = m_SampleRate * (16 >> 3) * m_InputChannels;
  m_BufferLen     *= m_fifo_size;

  // should be big enough that common formats (e.g. 6 channel DTS) fit in a single packet.
  // we don't mind less common formats being split (e.g. ape/wma output large frames)
  // the audio_decode output buffer size is 32K, and typically we convert from
  // 6 channel 32bpp float to 8 channel 16bpp in, so a full 48K input buffer will fit the outbut buffer
  m_ChunkLen      = 48*1024;

  m_wave_header.Samples.wSamplesPerBlock    = 0;
  m_wave_header.Format.nChannels            = m_InputChannels;
  m_wave_header.Format.nBlockAlign          = m_InputChannels *
    (m_BitsPerSample >> 3);
  // 0x8000 is custom format interpreted by GPU as WAVE_FORMAT_IEEE_FLOAT_PLANAR
  m_wave_header.Format.wFormatTag           = m_BitsPerSample == 32 ? 0x8000 : WAVE_FORMAT_PCM;
  m_wave_header.Format.nSamplesPerSec       = m_SampleRate;
  m_wave_header.Format.nAvgBytesPerSec      = m_BytesPerSec;
  m_wave_header.Format.wBitsPerSample       = m_BitsPerSample;
  m_wave_header.Samples.wValidBitsPerSample = m_BitsPerSample;
  m_wave_header.Format.cbSize               = 0;
  m_wave_header.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;

  m_pcm_input.eNumData              = OMX_NumericalDataSigned;
  m_pcm_input.eEndian               = OMX_EndianLittle;
  m_pcm_input.bInterleaved          = OMX_TRUE;
  m_pcm_input.nBitPerSample         = m_BitsPerSample;
  m_pcm_input.ePCMMode              = OMX_AUDIO_PCMModeLinear;
  m_pcm_input.nChannels             = m_InputChannels;
  m_pcm_input.nSamplingRate         = m_SampleRate;

  if(!m_omx_decoder.Initialize("OMX.broadcom.audio_decode", OMX_IndexParamAudioInit))
    return false;

  OMX_CONFIG_BOOLEANTYPE boolType;
  OMX_INIT_STRUCTURE(boolType);
  if(m_Passthrough)
    boolType.bEnabled = OMX_TRUE;
  else
    boolType.bEnabled = OMX_FALSE;
  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamBrcmDecoderPassThrough, &boolType);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error OMX_IndexParamBrcmDecoderPassThrough 0x%08x", omx_err);
    printf("OMX_IndexParamBrcmDecoderPassThrough omx_err(0x%08x)\n", omx_err);
    return false;
  }

  // set up the number/size of buffers for decoder input
  OMX_PARAM_PORTDEFINITIONTYPE port_param;
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error get OMX_IndexParamPortDefinition (input) omx_err(0x%08x)\n", omx_err);
    return false;
  }

  port_param.format.audio.eEncoding = m_eEncoding;
  port_param.nBufferSize = m_ChunkLen;
  port_param.nBufferCountActual = std::max(port_param.nBufferCountMin, 16U);

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error set OMX_IndexParamPortDefinition (intput) omx_err(0x%08x)\n", omx_err);
    return false;
  }

  // set up the number/size of buffers for decoder output
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_decoder.GetOutputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error get OMX_IndexParamPortDefinition (output) omx_err(0x%08x)\n", omx_err);
    return false;
  }

  port_param.nBufferCountActual = std::max((unsigned int)port_param.nBufferCountMin, m_BufferLen / port_param.nBufferSize);

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error set OMX_IndexParamPortDefinition (output) omx_err(0x%08x)\n", omx_err);
    return false;
  }

  {
    OMX_AUDIO_PARAM_PORTFORMATTYPE formatType;
    OMX_INIT_STRUCTURE(formatType);
    formatType.nPortIndex = m_omx_decoder.GetInputPort();

    formatType.eEncoding = m_eEncoding;

    omx_err = m_omx_decoder.SetParameter(OMX_IndexParamAudioPortFormat, &formatType);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
      return false;
    }
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone) 
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error alloc buffers 0x%08x", omx_err);
    return false;
  }

    omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone) {
      CLog::Log(LOGERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
      return false;
    }


  if(m_eEncoding == OMX_AUDIO_CodingPCM)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
    if(omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize - buffer error 0x%08x", omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = sizeof(m_wave_header);
    if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize - omx_buffer->nFilledLen > omx_buffer->nAllocLen");
      return false;
    }
    memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
    memcpy((unsigned char *)omx_buffer->pBuffer, &m_wave_header, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  } 
  else if(m_HWDecode)
  {
    // send decoder config
    if(m_extrasize > 0 && m_extradata != NULL)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
  
      if(omx_buffer == NULL)
      {
        CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
        return false;
      }
  
      omx_buffer->nOffset = 0;
      omx_buffer->nFilledLen = m_extrasize;
      if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
      {
        CLog::Log(LOGERROR, "%s::%s - omx_buffer->nFilledLen > omx_buffer->nAllocLen", CLASSNAME, __func__);
        return false;
      }

      memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
      memcpy((unsigned char *)omx_buffer->pBuffer, m_extradata, omx_buffer->nFilledLen);
      omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
  
      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        return false;
      }
    }
  }

  /* return on decoder error so m_Initialized stays false */
  if(m_omx_decoder.BadState())
    return false;

  m_Initialized   = true;
  m_settings_changed = false;
  m_setStartTime  = true;
  m_submitted_eos = false;
  m_failed_eos = false;
  m_last_pts      = DVD_NOPTS_VALUE;

  CLog::Log(LOGDEBUG, "COMXAudio::Initialize Input bps %d samplerate %d channels %d buffer size %d bytes per second %d",
      (int)m_pcm_input.nBitPerSample, (int)m_pcm_input.nSamplingRate, (int)m_pcm_input.nChannels, m_BufferLen, m_BytesPerSec);
  PrintPCM(&m_pcm_input, std::string("input"));
  CLog::Log(LOGDEBUG, "COMXAudio::Initialize device %s passthrough %d hwdecode %d",
      device.c_str(), m_Passthrough, m_HWDecode);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Deinitialize()
{
  CSingleLock lock (m_critSection);

  m_omx_tunnel_decoder.Flush();
  if(!m_Passthrough)
    m_omx_tunnel_mixer.Flush();
  m_omx_tunnel_clock.Flush();

  m_omx_tunnel_clock.Deestablish();
  if(!m_Passthrough)
    m_omx_tunnel_mixer.Deestablish();
  m_omx_tunnel_decoder.Deestablish();

  m_omx_decoder.FlushInput();

  m_omx_render.Deinitialize(true);
  if(!m_Passthrough)
    m_omx_mixer.Deinitialize(true);
  m_omx_decoder.Deinitialize(true);

  m_BytesPerSec = 0;
  m_BufferLen   = 0;

  m_omx_clock = NULL;
  m_av_clock  = NULL;

  m_Initialized = false;
  m_LostSync    = true;
  m_HWDecode    = false;

  if(m_extradata)
    free(m_extradata);
  m_extradata = NULL;
  m_extrasize = 0;

  m_dllAvUtil.Unload();

  m_last_pts      = DVD_NOPTS_VALUE;

  return true;
}

void COMXAudio::Flush()
{
  CSingleLock lock (m_critSection);
  if(!m_Initialized)
    return;

  m_omx_decoder.FlushInput();
  m_omx_tunnel_decoder.Flush();
  if(!m_Passthrough)
    m_omx_tunnel_mixer.Flush();
  
  m_last_pts      = DVD_NOPTS_VALUE;
  m_LostSync      = true;
  m_setStartTime  = true;
}

//***********************************************************************************************
void COMXAudio::SetDynamicRangeCompression(long drc)
{
  CSingleLock lock (m_critSection);
  m_amplification = powf(10.0f, (float)drc / 2000.0f);
  if (m_settings_changed)
    ApplyVolume();
}

//***********************************************************************************************
void COMXAudio::SetMute(bool bMute)
{
  CSingleLock lock (m_critSection);
  m_Mute = bMute;
  if (m_settings_changed)
    ApplyVolume();
}

//***********************************************************************************************
void COMXAudio::SetVolume(float fVolume)
{
  CSingleLock lock (m_critSection);
  m_CurrentVolume = fVolume;
  if (m_settings_changed)
    ApplyVolume();
}

float COMXAudio::GetVolume() 
{
  return m_Mute ? VOLUME_MINIMUM : m_CurrentVolume;
}

//***********************************************************************************************
bool COMXAudio::ApplyVolume(void)
{
  float m_ac3Gain = 12.0f;
  bool m_boostcentre = false;
  CSingleLock lock (m_critSection);

  if(!m_Initialized || m_Passthrough)
    return false;

  float fVolume = m_Mute ? VOLUME_MINIMUM : m_CurrentVolume;

  // the analogue volume is too quiet for some. Allow use of an advancedsetting to boost this (at risk of distortion) (deprecated)
  double gain = pow(10, (m_ac3Gain - 12.0f) / 20.0);

  double r = 1.0;
  const float* coeff = downmixing_coefficients_8;

  // alternate coffeciciants that boost centre channel more
  if(m_boostcentre)
    coeff = downmixing_coefficients_8_boostcentre;

  // normally we normalise the levels, can be skipped (boosted) at risk of distortion
  if(!m_normalize_downmix)
  {
    double sum_L = 0;
    double sum_R = 0;

    for(size_t i = 0; i < OMX_AUDIO_MAXCHANNELS; ++i)
    {
      if (m_input_channels[i] == OMX_AUDIO_ChannelMax)
        break;
      if(i & 1)
        sum_R += coeff[i];
      else
        sum_L += coeff[i];
    }

    r /= max(sum_L, sum_R);
  }

  r *= gain;

  OMX_CONFIG_BRCMAUDIODOWNMIXCOEFFICIENTS mix;
  OMX_INIT_STRUCTURE(mix);
  OMX_ERRORTYPE omx_err;

  assert(sizeof(mix.coeff)/sizeof(mix.coeff[0]) == 16);

  // reduce scaling so overflow can be seen
  for(size_t i = 0; i < 16; ++i)
    mix.coeff[i] = static_cast<unsigned int>(0x10000 * (coeff[i] * r * 0.01f));

  mix.nPortIndex = m_omx_decoder.GetInputPort();
  omx_err = m_omx_decoder.SetConfig(OMX_IndexConfigBrcmAudioDownmixCoefficients, &mix);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error setting decoder OMX_IndexConfigBrcmAudioDownmixCoefficients, error 0x%08x\n",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  if (m_amplification != 1.0)
  {
    for(size_t i = 0; i < 16; ++i)
      mix.coeff[i] = static_cast<unsigned int>(0x10000 * (coeff[i] * r * fVolume * m_amplification * m_attenuation));

    mix.nPortIndex = m_omx_mixer.GetInputPort();
    omx_err = m_omx_mixer.SetConfig(OMX_IndexConfigBrcmAudioDownmixCoefficients, &mix);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - error setting mixer OMX_IndexConfigBrcmAudioDownmixCoefficients, error 0x%08x\n",
                CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  CLog::Log(LOGINFO, "%s::%s - Volume=%.2f (* %.2f * %.2f)\n", CLASSNAME, __func__, fVolume, m_amplification, m_attenuation);
  return true;
}


//***********************************************************************************************
unsigned int COMXAudio::AddPackets(const void* data, unsigned int len)
{
  return AddPackets(data, len, 0, 0);
}

//***********************************************************************************************
unsigned int COMXAudio::AddPackets(const void* data, unsigned int len, double dts, double pts)
{
  CSingleLock lock (m_critSection);

  if(!m_Initialized)
  {
    CLog::Log(LOGERROR,"COMXAudio::AddPackets - sanity failed. no valid play handle!");
    return len;
  }

  if(m_eEncoding == OMX_AUDIO_CodingDTS && m_LostSync && (m_Passthrough || m_HWDecode))
  {
    int skip = SyncDTS((uint8_t *)data, len);
    if(skip > 0)
      return len;
  }

  if(m_eEncoding == OMX_AUDIO_CodingDDP && m_LostSync && (m_Passthrough || m_HWDecode))
  {
    int skip = SyncAC3((uint8_t *)data, len);
    if(skip > 0)
      return len;
  }

  unsigned pitch = (m_Passthrough || m_HWDecode) ? 1:(m_BitsPerSample >> 3) * m_InputChannels;
  unsigned int demuxer_samples = len / pitch;
  unsigned int demuxer_samples_sent = 0;
  uint8_t *demuxer_content = (uint8_t *)data;

  OMX_ERRORTYPE omx_err;

  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

  while(demuxer_samples_sent < demuxer_samples)
  {
    // 200ms timeout
    omx_buffer = m_omx_decoder.GetInputBuffer(200);

    if(omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "COMXAudio::Decode timeout\n");
      printf("COMXAudio::Decode timeout\n");
      return len;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFlags  = 0;

    unsigned int remaining = demuxer_samples-demuxer_samples_sent;
    unsigned int samples_space = omx_buffer->nAllocLen/pitch;
    unsigned int samples = std::min(remaining, samples_space);

    omx_buffer->nFilledLen = samples * pitch;

    if (samples < demuxer_samples && m_BitsPerSample==32 && !(m_Passthrough || m_HWDecode))
    {
       uint8_t *dst = omx_buffer->pBuffer;
       uint8_t *src = demuxer_content + demuxer_samples_sent * (m_BitsPerSample >> 3);
       // we need to extract samples from planar audio, so the copying needs to be done per plane
       for (int i=0; i<(int)m_InputChannels; i++)
       {
         memcpy(dst, src, omx_buffer->nFilledLen / m_InputChannels);
         dst += omx_buffer->nFilledLen / m_InputChannels;
         src += demuxer_samples * (m_BitsPerSample >> 3);
       }
       assert(dst <= omx_buffer->pBuffer + m_ChunkLen);
    }
    else
    {
       uint8_t *dst = omx_buffer->pBuffer;
       uint8_t *src = demuxer_content + demuxer_samples_sent * pitch;
       memcpy(dst, src, omx_buffer->nFilledLen);
    }

    uint64_t val  = (uint64_t)(pts == DVD_NOPTS_VALUE) ? 0 : pts;

    if(m_setStartTime)
    {
      omx_buffer->nFlags = OMX_BUFFERFLAG_STARTTIME;
      if(pts == DVD_NOPTS_VALUE)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;

      m_last_pts = pts;

      CLog::Log(LOGDEBUG, "COMXAudio::Decode ADec : setStartTime %f\n", (float)val / DVD_TIME_BASE);
      m_setStartTime = false;
    }
    else
    {
      if(pts == DVD_NOPTS_VALUE)
      {
        omx_buffer->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
        m_last_pts = pts;
      }
      else if (m_last_pts != pts)
      {
        if(pts > m_last_pts)
          m_last_pts = pts;
        else
          omx_buffer->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;;
      }
      else if (m_last_pts == pts)
      {
        omx_buffer->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
      }
    }

    omx_buffer->nTimeStamp = ToOMXTime(val);

    demuxer_samples_sent += samples;

    if(demuxer_samples_sent == demuxer_samples)
      omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

    int nRetry = 0;
    while(true)
    {
      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err == OMX_ErrorNone)
      {
        //CLog::Log(LOGINFO, "AudiD: dts:%.0f pts:%.0f size:%d\n", dts, pts, len);
        break;
      }
      else
      {
        CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        nRetry++;
      }
      if(nRetry == 5)
      {
        CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() finaly failed\n", CLASSNAME, __func__);
        printf("%s::%s - OMX_EmptyThisBuffer() finaly failed\n", CLASSNAME, __func__);
        return 0;
      }
    }

    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
    if (omx_err == OMX_ErrorNone)
    {
      if(!PortSettingsChanged())
      {
        CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
      }
    }
  }
  if (m_amplification != 1.0)
  {
    double level_pts = 0.0;
    float level = GetMaxLevel(level_pts);
    if (level_pts != 0.0)
    {
      float m_limiterHold = 0.025f;
      float m_limiterRelease = 0.100f;
      float alpha_h = -1.0f/(0.025f*log10f(0.999f));
      float alpha_r = -1.0f/(0.100f*log10f(0.900f));
      float hold    = powf(10.0f, -1.0f / (alpha_h * m_limiterHold));
      float release = powf(10.0f, -1.0f / (alpha_r * m_limiterRelease));
      m_maxLevel = level > m_maxLevel ? level : hold * m_maxLevel + (1.0f-hold) * level;
      m_avgLevel = 0.9f * m_avgLevel + 0.1f * level;

      float amp = m_amplification * m_desired_attenuation;

      // want m_maxLevel * amp -> 1.0
      m_desired_attenuation = std::min(1.0f, std::max(m_desired_attenuation / (amp * m_maxLevel), 1.0f/m_amplification));
      m_attenuation = release * m_attenuation + (1.0f-release) * m_desired_attenuation;

      ApplyVolume();
    }
  }
  return len;
}

//***********************************************************************************************
unsigned int COMXAudio::GetSpace()
{
  int free = m_omx_decoder.GetInputBufferSpace();
  return free;
}

float COMXAudio::GetDelay()
{
  unsigned int free = m_omx_decoder.GetInputBufferSize() - m_omx_decoder.GetInputBufferSpace();
  return m_BytesPerSec ? (float)free / (float)m_BytesPerSec : 0.0f;
}

float COMXAudio::GetCacheTime()
{
  float fBufferLenFull = (float)m_BufferLen - (float)GetSpace();
  if(fBufferLenFull < 0)
    fBufferLenFull = 0;
  float ret = m_BytesPerSec ? fBufferLenFull / (float)m_BytesPerSec : 0.0f;
  return ret;
}

float COMXAudio::GetCacheTotal()
{
  return m_BytesPerSec ? (float)m_BufferLen / (float)m_BytesPerSec : 0.0f;
}

//***********************************************************************************************
unsigned int COMXAudio::GetChunkLen()
{
  return m_ChunkLen;
}

unsigned int COMXAudio::GetAudioRenderingLatency()
{
  CSingleLock lock (m_critSection);

  if(!m_Initialized)
    return 0;

  OMX_PARAM_U32TYPE param;
  OMX_INIT_STRUCTURE(param);
  param.nPortIndex = m_omx_render.GetInputPort();

  OMX_ERRORTYPE omx_err =
    m_omx_render.GetConfig(OMX_IndexConfigAudioRenderingLatency, &param);

  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error getting OMX_IndexConfigAudioRenderingLatency error 0x%08x\n",
      CLASSNAME, __func__, omx_err);
    return 0;
  }

  return param.nU32;
}

float COMXAudio::GetMaxLevel(double &pts)
{
  CSingleLock lock (m_critSection);

  if(!m_Initialized)
    return 0;

  OMX_CONFIG_BRCMAUDIOMAXSAMPLE param;
  OMX_INIT_STRUCTURE(param);

  if(m_omx_decoder.IsInitialized())
  {
    param.nPortIndex = m_omx_decoder.GetInputPort();

    OMX_ERRORTYPE omx_err = m_omx_decoder.GetConfig(OMX_IndexConfigBrcmAudioMaxSample, &param);

    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - error getting OMX_IndexConfigBrcmAudioMaxSample error 0x%08x\n",
        CLASSNAME, __func__, omx_err);
      return 0;
    }
  }
  pts = FromOMXTime(param.nTimeStamp);
  return (float)param.nMaxSample * (100.0f / (1<<15));
}

void COMXAudio::SubmitEOS()
{
  CSingleLock lock (m_critSection);

  if(!m_Initialized)
    return;

  m_submitted_eos = true;
  m_failed_eos = false;

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(1000);

  if(omx_buffer == NULL)
  {
    CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
    m_failed_eos = true;
    return;
  }

  omx_buffer->nOffset     = 0;
  omx_buffer->nFilledLen  = 0;
  omx_buffer->nTimeStamp  = ToOMXTime(0LL);

  omx_buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_TIME_UNKNOWN;

  omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return;
  }
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
}

bool COMXAudio::IsEOS()
{
  if(!m_Initialized)
    return true;
  unsigned int latency = GetAudioRenderingLatency();
  CSingleLock lock (m_critSection);

  if (!m_failed_eos && !(m_omx_decoder.IsEOS() && latency == 0))
    return false;

  if (m_submitted_eos)
  {
    CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
    m_submitted_eos = false;
  }
  return true;
}

void COMXAudio::SetCodingType(AVCodecID codec)
{
  switch(codec)
  { 
    case CODEC_ID_DTS:
      CLog::Log(LOGDEBUG, "COMXAudio::SetCodingType OMX_AUDIO_CodingDTS\n");
      m_eEncoding = OMX_AUDIO_CodingDTS;
      break;
    case CODEC_ID_AC3:
    case CODEC_ID_EAC3:
      CLog::Log(LOGDEBUG, "COMXAudio::SetCodingType OMX_AUDIO_CodingDDP\n");
      m_eEncoding = OMX_AUDIO_CodingDDP;
      break;
    default:
      CLog::Log(LOGDEBUG, "COMXAudio::SetCodingType OMX_AUDIO_CodingPCM\n");
      m_eEncoding = OMX_AUDIO_CodingPCM;
      break;
  } 
}

bool COMXAudio::CanHWDecode(AVCodecID codec)
{
  switch(codec)
  { 
    /*
    case CODEC_ID_VORBIS:
      CLog::Log(LOGDEBUG, "COMXAudio::CanHWDecode OMX_AUDIO_CodingVORBIS\n");
      m_eEncoding = OMX_AUDIO_CodingVORBIS;
      m_HWDecode = true;
      break;
    case CODEC_ID_AAC:
      CLog::Log(LOGDEBUG, "COMXAudio::CanHWDecode OMX_AUDIO_CodingAAC\n");
      m_eEncoding = OMX_AUDIO_CodingAAC;
      m_HWDecode = true;
      break;
    */
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
      CLog::Log(LOGDEBUG, "COMXAudio::CanHWDecode OMX_AUDIO_CodingMP3\n");
      m_eEncoding = OMX_AUDIO_CodingMP3;
      m_HWDecode = true;
      break;
    case CODEC_ID_DTS:
      CLog::Log(LOGDEBUG, "COMXAudio::CanHWDecode OMX_AUDIO_CodingDTS\n");
      m_eEncoding = OMX_AUDIO_CodingDTS;
      m_HWDecode = true;
      break;
    case CODEC_ID_AC3:
    case CODEC_ID_EAC3:
      CLog::Log(LOGDEBUG, "COMXAudio::CanHWDecode OMX_AUDIO_CodingDDP\n");
      m_eEncoding = OMX_AUDIO_CodingDDP;
      m_HWDecode = true;
      break;
    default:
      CLog::Log(LOGDEBUG, "COMXAudio::CanHWDecode OMX_AUDIO_CodingPCM\n");
      m_eEncoding = OMX_AUDIO_CodingPCM;
      m_HWDecode = false;
      break;
  } 

  return m_HWDecode;
}

bool COMXAudio::HWDecode(AVCodecID codec)
{
  bool ret = false;

  switch(codec)
  { 
    /*
    case CODEC_ID_VORBIS:
      CLog::Log(LOGDEBUG, "COMXAudio::HWDecode CODEC_ID_VORBIS\n");
      ret = true;
      break;
    case CODEC_ID_AAC:
      CLog::Log(LOGDEBUG, "COMXAudio::HWDecode CODEC_ID_AAC\n");
      ret = true;
      break;
    */
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
      CLog::Log(LOGDEBUG, "COMXAudio::HWDecode CODEC_ID_MP2 / CODEC_ID_MP3\n");
      ret = true;
      break;
    case CODEC_ID_DTS:
      CLog::Log(LOGDEBUG, "COMXAudio::HWDecode CODEC_ID_DTS\n");
      ret = true;
      break;
    case CODEC_ID_AC3:
    case CODEC_ID_EAC3:
      CLog::Log(LOGDEBUG, "COMXAudio::HWDecode CODEC_ID_AC3 / CODEC_ID_EAC3\n");
      ret = true;
      break;
    default:
      ret = false;
      break;
  } 

  return ret;
}

void COMXAudio::PrintChannels(OMX_AUDIO_CHANNELTYPE eChannelMapping[])
{
  for(int i = 0; i < OMX_AUDIO_MAXCHANNELS; i++)
  {
    switch(eChannelMapping[i])
    {
      case OMX_AUDIO_ChannelLF:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLF\n");
        break;
      case OMX_AUDIO_ChannelRF:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelRF\n");
        break;
      case OMX_AUDIO_ChannelCF:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelCF\n");
        break;
      case OMX_AUDIO_ChannelLS:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLS\n");
        break;
      case OMX_AUDIO_ChannelRS:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelRS\n");
        break;
      case OMX_AUDIO_ChannelLFE:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLFE\n");
        break;
      case OMX_AUDIO_ChannelCS:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelCS\n");
        break;
      case OMX_AUDIO_ChannelLR:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLR\n");
        break;
      case OMX_AUDIO_ChannelRR:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelRR\n");
        break;
      case OMX_AUDIO_ChannelNone:
      case OMX_AUDIO_ChannelKhronosExtensions:
      case OMX_AUDIO_ChannelVendorStartUnused:
      case OMX_AUDIO_ChannelMax:
      default:
        break;
    }
  }
}

void COMXAudio::PrintPCM(OMX_AUDIO_PARAM_PCMMODETYPE *pcm, std::string direction)
{
  CLog::Log(LOGDEBUG, "pcm->direction      : %s\n", direction.c_str());
  CLog::Log(LOGDEBUG, "pcm->nPortIndex     : %d\n", (int)pcm->nPortIndex);
  CLog::Log(LOGDEBUG, "pcm->eNumData       : %d\n", pcm->eNumData);
  CLog::Log(LOGDEBUG, "pcm->eEndian        : %d\n", pcm->eEndian);
  CLog::Log(LOGDEBUG, "pcm->bInterleaved   : %d\n", (int)pcm->bInterleaved);
  CLog::Log(LOGDEBUG, "pcm->nBitPerSample  : %d\n", (int)pcm->nBitPerSample);
  CLog::Log(LOGDEBUG, "pcm->ePCMMode       : %d\n", pcm->ePCMMode);
  CLog::Log(LOGDEBUG, "pcm->nChannels      : %d\n", (int)pcm->nChannels);
  CLog::Log(LOGDEBUG, "pcm->nSamplingRate  : %d\n", (int)pcm->nSamplingRate);

  PrintChannels(pcm->eChannelMapping);
}

void COMXAudio::PrintDDP(OMX_AUDIO_PARAM_DDPTYPE *ddparm)
{
  CLog::Log(LOGDEBUG, "ddparm->nPortIndex         : %d\n", (int)ddparm->nPortIndex);
  CLog::Log(LOGDEBUG, "ddparm->nChannels          : %d\n", (int)ddparm->nChannels);
  CLog::Log(LOGDEBUG, "ddparm->nBitRate           : %d\n", (int)ddparm->nBitRate);
  CLog::Log(LOGDEBUG, "ddparm->nSampleRate        : %d\n", (int)ddparm->nSampleRate);
  CLog::Log(LOGDEBUG, "ddparm->eBitStreamId       : %d\n", (int)ddparm->eBitStreamId);
  CLog::Log(LOGDEBUG, "ddparm->eBitStreamMode     : %d\n", (int)ddparm->eBitStreamMode);
  CLog::Log(LOGDEBUG, "ddparm->eDolbySurroundMode : %d\n", (int)ddparm->eDolbySurroundMode);

  PrintChannels(ddparm->eChannelMapping);
}

void COMXAudio::PrintDTS(OMX_AUDIO_PARAM_DTSTYPE *dtsparam)
{
  CLog::Log(LOGDEBUG, "dtsparam->nPortIndex         : %d\n", (int)dtsparam->nPortIndex);
  CLog::Log(LOGDEBUG, "dtsparam->nChannels          : %d\n", (int)dtsparam->nChannels);
  CLog::Log(LOGDEBUG, "dtsparam->nBitRate           : %d\n", (int)dtsparam->nBitRate);
  CLog::Log(LOGDEBUG, "dtsparam->nSampleRate        : %d\n", (int)dtsparam->nSampleRate);
  CLog::Log(LOGDEBUG, "dtsparam->nFormat            : 0x%08x\n", (int)dtsparam->nFormat);
  CLog::Log(LOGDEBUG, "dtsparam->nDtsType           : %d\n", (int)dtsparam->nDtsType);
  CLog::Log(LOGDEBUG, "dtsparam->nDtsFrameSizeBytes : %d\n", (int)dtsparam->nDtsFrameSizeBytes);

  PrintChannels(dtsparam->eChannelMapping);
}

/* ========================== SYNC FUNCTIONS ========================== */
unsigned int COMXAudio::SyncDTS(BYTE* pData, unsigned int iSize)
{
  OMX_INIT_STRUCTURE(m_dtsParam);

  unsigned int skip;
  unsigned int srCode;
  unsigned int dtsBlocks;
  bool littleEndian;

  for(skip = 0; iSize - skip > 8; ++skip, ++pData)
  {
    if (pData[0] == 0x7F && pData[1] == 0xFE && pData[2] == 0x80 && pData[3] == 0x01) 
    {
      /* 16bit le */
      littleEndian = true; 
      dtsBlocks    = ((pData[4] >> 2) & 0x7f) + 1;
      m_dtsParam.nFormat = 0x1 | 0x2;
    }
    else if (pData[0] == 0x1F && pData[1] == 0xFF && pData[2] == 0xE8 && pData[3] == 0x00 && pData[4] == 0x07 && (pData[5] & 0xF0) == 0xF0) 
    {
      /* 14bit le */
      littleEndian = true;
      dtsBlocks    = (((pData[4] & 0x7) << 4) | (pData[7] & 0x3C) >> 2) + 1;
      m_dtsParam.nFormat = 0x1 | 0x0;
    }
    else if (pData[1] == 0x7F && pData[0] == 0xFE && pData[3] == 0x80 && pData[2] == 0x01) 
    {
      /* 16bit be */ 
      littleEndian = false;
      dtsBlocks    = ((pData[5] >> 2) & 0x7f) + 1;
      m_dtsParam.nFormat = 0x0 | 0x2;
    }
    else if (pData[1] == 0x1F && pData[0] == 0xFF && pData[3] == 0xE8 && pData[2] == 0x00 && pData[5] == 0x07 && (pData[4] & 0xF0) == 0xF0) 
    {
      /* 14bit be */
      littleEndian = false; 
      dtsBlocks    = (((pData[5] & 0x7) << 4) | (pData[6] & 0x3C) >> 2) + 1;
      m_dtsParam.nFormat = 0x0 | 0x0;
    }
    else
    {
      continue;
    }

    if (littleEndian)
    {
      /* if it is not a termination frame, check the next 6 bits are set */
      if ((pData[4] & 0x80) == 0x80 && (pData[4] & 0x7C) != 0x7C)
        continue;

      /* get the frame size */
      m_dtsParam.nDtsFrameSizeBytes = ((((pData[5] & 0x3) << 8 | pData[6]) << 4) | ((pData[7] & 0xF0) >> 4)) + 1;
      srCode = (pData[8] & 0x3C) >> 2;
   }
   else
   {
      /* if it is not a termination frame, check the next 6 bits are set */
      if ((pData[5] & 0x80) == 0x80 && (pData[5] & 0x7C) != 0x7C)
        continue;

      /* get the frame size */
      m_dtsParam.nDtsFrameSizeBytes = ((((pData[4] & 0x3) << 8 | pData[7]) << 4) | ((pData[6] & 0xF0) >> 4)) + 1;
      srCode = (pData[9] & 0x3C) >> 2;
   }

    /* make sure the framesize is sane */
    if (m_dtsParam.nDtsFrameSizeBytes < 96 || m_dtsParam.nDtsFrameSizeBytes > 16384)
      continue;

    m_dtsParam.nSampleRate = DTSFSCod[srCode];

    switch(dtsBlocks << 5)
    {
      case 512 : 
        m_dtsParam.nDtsType = 1;
        break;
      case 1024: 
        m_dtsParam.nDtsType = 2;
        break;
      case 2048: 
        m_dtsParam.nDtsType = 3;
        break;
      default:
        m_dtsParam.nDtsType = 0;
        break;
    }

    //m_dtsParam.nFormat = 1;
    m_dtsParam.nDtsType = 1;

    m_LostSync = false;

    return skip;
  }

  m_LostSync = true;
  return iSize;
}

unsigned int COMXAudio::SyncAC3(BYTE* pData, unsigned int iSize)
{
  unsigned int skip = 0;

  for(skip = 0; iSize - skip > 6; ++skip, ++pData)
  {
    /* search for an ac3 sync word */
    if(pData[0] != 0x0b || pData[1] != 0x77)
      continue;

    uint8_t fscod      = pData[4] >> 6;
    uint8_t frmsizecod = pData[4] & 0x3F;
    uint8_t bsid       = pData[5] >> 3;

    /* sanity checks on the header */
    if (
        fscod      ==   3 ||
        frmsizecod >   37 ||
        bsid       > 0x11
    ) continue;

    /* get the details we need to check crc1 and framesize */
    uint16_t     bitrate   = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch(fscod)
    {
      case 0: framesize = bitrate * 2; break;
      case 1: framesize = (320 * bitrate / 147 + (frmsizecod & 1 ? 1 : 0)); break;
      case 2: framesize = bitrate * 4; break;
    }

    m_SampleRate = AC3FSCod[fscod];

    /* dont do extensive testing if we have not lost sync */
    if (!m_LostSync && skip == 0)
      return 0;

    unsigned int crc_size;
    /* if we have enough data, validate the entire packet, else try to validate crc2 (5/8 of the packet) */
    if (framesize <= iSize - skip)
         crc_size = framesize - 1;
    else crc_size = (framesize >> 1) + (framesize >> 3) - 1;

    if (crc_size <= iSize - skip)
      if(m_dllAvUtil.av_crc(m_dllAvUtil.av_crc_get_table(AV_CRC_16_ANSI), 0, &pData[2], crc_size * 2))
        continue;

    /* if we get here, we can sync */
    m_LostSync = false;
    return skip;
  }

  /* if we get here, the entire packet is invalid and we have lost sync */
  m_LostSync = true;
  return iSize;
}

