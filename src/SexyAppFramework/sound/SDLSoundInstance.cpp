/*
 * Portions of this file are based on the PopCap Games Framework
 * Copyright (C) 2005-2009 PopCap Games, Inc.
 *
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later AND LicenseRef-PopCap
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

#include "SDLSoundInstance.h"
#include "SDLSoundManager.h"
#include "SexyAppBase.h"

using namespace Sexy;

SDLSoundInstance::SDLSoundInstance(SDLSoundManager* theSoundManager, Mix_Chunk* theSourceSound, int theReservedChannel)
{
	mSoundManagerP = theSoundManager;
	mMixChunk = theSourceSound;
	mReleased = false;
	mAutoRelease = false;
	mHasPlayed = false;
	mReservedChannel = theReservedChannel;
	mChannel = -1;

	mBaseVolume = 1.0;
	mBasePan = 0;

	mVolume = 1.0;
	mPan = 0;
	mPitch = 1.0f;

	mDefaultFrequency = 44100;
	mDemoEndUpdateCount = 0;
	mDemoLooping = false;

	RehupVolume();
}

SDLSoundInstance::~SDLSoundInstance()
{
	Stop();
}

void SDLSoundInstance::RehupVolume()
{
	Mix_VolumeChunk(mMixChunk, (mBaseVolume * mVolume * mSoundManagerP->mMasterVolume) * 128);
}

void SDLSoundInstance::RehupPan()
{
}

// SDL_mixer chunk pitching
// https://gist.github.com/hydren/ea794e65e95c7713c00c88f74b71f8b1

uint16_t SDLSoundInstance::FormatSampleSize(uint16_t format)
{
	return (format & 0xFF) / 8;
}

int SDLSoundInstance::GetChunkDurationMS(int chunkSize)
{
	const uint32_t points = chunkSize / FormatSampleSize(mSoundManagerP->mMixerFormat);  // bytes / samplesize == sample points
	const uint32_t frames = (points / mSoundManagerP->mMixerChannels);  // sample points / channels == sample frames
	return ((frames * 1000) / mSoundManagerP->mMixerFreq);  // (sample frames * 1000) / frequency == play length, in ms
}

void SDLSoundInstance::CreateSoundPitchHandler(const Mix_Chunk* chunk, const float* speed, int loop, int self_halt)
{
	mPitchHandler.self = this;
	mPitchHandler.chunk = chunk;
	mPitchHandler.speed = speed;
	mPitchHandler.position = 0;
	mPitchHandler.altered = 0;
	mPitchHandler.loop = loop;
	mPitchHandler.duration = GetChunkDurationMS(chunk->alen);
	mPitchHandler.chunk_size = chunk->alen / FormatSampleSize(mSoundManagerP->mMixerFormat);
	mPitchHandler.self_halt = self_halt;
}

void SDLSoundInstance::PitchHandlerFuncCallback(int mix_channel, void* stream, int length, void* user_data)
{
	SDLSoundPitchHandler* handler = (SDLSoundPitchHandler*) user_data;
	SDLSoundInstance* pThis = handler->self;
	const int16_t* chunk_data = (int16_t*) handler->chunk->abuf;

	int16_t* buffer = (int16_t*) stream;
	const int buffer_size = length / sizeof(int16_t);  // buffer size (as array)
	const float speed_factor = *handler->speed;  // take a "snapshot" of speed factor

	// if there is still sound to be played
	if(handler->position < handler->duration || handler->loop)
	{
		const int aFrameCount = buffer_size / pThis->mSoundManagerP->mMixerChannels;
		const float delta = 1000.0 / pThis->mSoundManagerP->mMixerFreq,  // normal duration of each sample
				vdelta = delta * speed_factor;  // virtual stretched duration, scaled by 'speedFactor'

		// if playback is unaltered and pitch is required (for the first time)
		if(!handler->altered && speed_factor != 1.0f)
			handler->altered = 1;  // flags playback modification and proceed to the pitch routine

		if(handler->altered)  // if unaltered, this pitch routine is skipped
		{
			const int aChunkFrames = handler->chunk_size / pThis->mSoundManagerP->mMixerChannels;
			for (int i = 0, j = 0; j < aFrameCount; i += pThis->mSoundManagerP->mMixerChannels, j++)
			{
				const float x = handler->position + j * vdelta;
				const float aFramePosition = x / delta;
				const int k = floor(aFramePosition);
				const float prop = aFramePosition - k;
				if (k < aChunkFrames || handler->loop)
				{
					const int aBase = (k * pThis->mSoundManagerP->mMixerChannels) % handler->chunk_size;
					int aNext = aBase + pThis->mSoundManagerP->mMixerChannels;
					if (aNext == handler->chunk_size)
						aNext = 0;
					for (int c = 0; c < pThis->mSoundManagerP->mMixerChannels; c++)
					{
						const int16_t v0 = chunk_data[aBase + c];
						const int16_t v1 = chunk_data[aNext + c];
						buffer[i + c] = v0 + prop * (v1 - v0);
					}
				}
				else
				{
					for (int c = 0; c < pThis->mSoundManagerP->mMixerChannels; c++)
						buffer[i + c] = 0;
				}
			}
		}

		// update position
		handler->position += static_cast<float>(aFrameCount) * vdelta;

		// reset position if looping
		if(handler->loop) while(handler->position > handler->duration)
			handler->position -= handler->duration;
	}
	else  // if we already played the whole sound but finished earlier than expected by SDL_mixer (due to faster playback speed)
	{
		// set silence on the buffer since Mix_HaltChannel() poops out some of it for a few ms.
		for(int i = 0; i < buffer_size; i++)
			buffer[i] = 0;

		if(handler->self_halt)
			Mix_HaltChannel(mix_channel);  // XXX unsafe call, since it locks audio; but no safer solution was found yet...
	}
}

void SDLSoundInstance::Release()
{
	Stop();
	mReleased = true;
}

void SDLSoundInstance::SetBaseVolume(double theBaseVolume)
{
	mBaseVolume = theBaseVolume;
	RehupVolume();
}

void SDLSoundInstance::SetBasePan(int theBasePan)
{
	mBasePan = theBasePan;
	RehupPan();
}

void SDLSoundInstance::AdjustPitch(double theNumSteps)
{
	mPitch = powf(1.0594630943592952645618252949463, theNumSteps);
}

void SDLSoundInstance::SetVolume(double theVolume)
{
	mVolume = theVolume;
	RehupVolume();
}

void SDLSoundInstance::SetPan(int thePosition) //-hundredth db to +hundredth db = left to right
{
	mPan = thePosition;
	RehupPan();
}

bool SDLSoundInstance::Play(bool looping, bool autoRelease)
{
	Stop();

	mHasPlayed = true;
	mAutoRelease = autoRelease;

	if (gSexyAppBase->IsInDemoMode()) // demo sessions: derive playing state from game ticks, not the audio clock
	{
		mDemoLooping = looping;
		mDemoEndUpdateCount = gSexyAppBase->mUpdateCount +
			static_cast<uint>(GetChunkDurationMS(mMixChunk != nullptr ? mMixChunk->alen : 0) / (10.0 * mPitch)) + 1;
	}

	if (!mMixChunk)
		return false;

	const int aTargetChannel = (mReservedChannel >= 0 && mReservedChannel < MAX_CHANNELS) ? mReservedChannel : -1;
	mChannel = Mix_PlayChannel(aTargetChannel, mMixChunk, (looping) ? -1 : 0);
	if (mChannel != -1)
	{
		CreateSoundPitchHandler(mMixChunk, &mPitch, looping, 0);
		Mix_RegisterEffect(mChannel, PitchHandlerFuncCallback, 0, &mPitchHandler);
		return true;
	}

	mChannel = -1;
	return false;
}

void SDLSoundInstance::Stop()
{
	mDemoEndUpdateCount = 0;
	if (mChannel >= 0 && mChannel < MAX_CHANNELS)
	{
		// Detach pitch effect before halting to avoid callbacks touching a dying object.
		Mix_UnregisterAllEffects(mChannel);
		Mix_HaltChannel(mChannel);
		mChannel = -1;
	}
	mAutoRelease = false;
}

bool SDLSoundInstance::IsPlaying()
{
	if (gSexyAppBase->IsInDemoMode()) // see Play(): tick-derived playing state in demo sessions
	{
		if (!mMixChunk || !mHasPlayed || mDemoEndUpdateCount == 0)
			return false;
		if (mDemoLooping)
			return true;
		return gSexyAppBase->mUpdateCount < mDemoEndUpdateCount;
	}
	if (!mMixChunk || !mHasPlayed || mChannel < 0 || mChannel >= MAX_CHANNELS)
		return false;
	return Mix_Playing(mChannel);
}

bool SDLSoundInstance::IsReleased()
{
	if ((!mReleased) && (mAutoRelease) && (mHasPlayed) && (!IsPlaying()))
		Release();

	return mReleased;
}

double SDLSoundInstance::GetVolume()
{
	return mVolume;
}
