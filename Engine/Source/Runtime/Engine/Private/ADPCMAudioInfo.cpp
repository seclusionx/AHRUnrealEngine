// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.


#include "EnginePrivate.h"
#include "ADPCMAudioInfo.h"
#include "IAudioFormat.h"


namespace ADPCM
{
	void DecodeBlock(const uint8* EncodedADPCMBlock, int32 BlockSize, int16* DecodedPCMData);
	void DecodeBlockStereo(const uint8* EncodedADPCMBlockLeft, const uint8* EncodedADPCMBlockRight, int32 BlockSize, int16* DecodedPCMData);
}

FADPCMAudioInfo::FADPCMAudioInfo(void)
{

}

FADPCMAudioInfo::~FADPCMAudioInfo(void)
{

}

bool FADPCMAudioInfo::ReadCompressedInfo(const uint8* InSrcBufferData, uint32 InSrcBufferDataSize, struct FSoundQualityInfo* QualityInfo)
{
	check(InSrcBufferData);
	check(QualityInfo);

	SrcBufferData = InSrcBufferData;
	SrcBufferDataSize = InSrcBufferDataSize;
	SrcBufferOffset = 0;

	if (!WaveInfo.ReadWaveInfo((uint8*)SrcBufferData, SrcBufferDataSize))
	{
		UE_LOG(LogAudio, Warning, TEXT("WaveInfo.ReadWaveInfo Failed"));
		return false;
	}

	const uint32 PreambleSize = 7;
	BlockSize = *WaveInfo.pBlockAlign;

	// (BlockSize - PreambleSize) * 2 (samples per byte) + 2 (preamble samples)
	UncompressedBlockSize = (2 + (BlockSize - PreambleSize) * 2) * sizeof(int16);
	CompressedBlockSize = BlockSize;

	// Calculate the preferred mono buffer size here based on the same calculations as we do for Vorbis but with our blocksizes taken into account
	const uint32 uncompressedBlockSamples = (2 + (BlockSize - PreambleSize) * 2);
	const uint32 targetBlocks = MONO_PCM_BUFFER_SAMPLES / uncompressedBlockSamples;
	StreamBufferSize = targetBlocks * UncompressedBlockSize;
	StreamBufferSizeInBlocks = CompressedBlockSize * targetBlocks;

	if (QualityInfo)
	{
		QualityInfo->SampleRate = *WaveInfo.pSamplesPerSec;
		QualityInfo->NumChannels = *WaveInfo.pChannels;
		QualityInfo->SampleDataSize = (WaveInfo.SampleDataSize / CompressedBlockSize) * UncompressedBlockSize;
		//QualityInfo->Duration = (float)TrueSampleCount / QualityInfo->SampleRate;
	}

	return true;
}

bool FADPCMAudioInfo::ReadCompressedData(uint8* Destination, bool bLooping, uint32 BufferSize)
{
	check(Destination);

	const uint8* EncodedADPCMBlockStart = reinterpret_cast<uint8*>(WaveInfo.SampleDataStart);
	uint8* EncodedADPCMBlock = (uint8*)EncodedADPCMBlockStart + SrcBufferOffset;
	const uint8* EncodedDataEnd = EncodedADPCMBlockStart + WaveInfo.SampleDataSize;

	// See if we are still completely inside the source data..
	if (EncodedADPCMBlock + StreamBufferSizeInBlocks < EncodedDataEnd)
	{
		// and if so do a straight decode
		while (BufferSize)
		{
			ADPCM::DecodeBlock(EncodedADPCMBlock, CompressedBlockSize, (int16*)Destination);
			EncodedADPCMBlock += CompressedBlockSize;
			SrcBufferOffset += CompressedBlockSize;
			Destination += UncompressedBlockSize;
			BufferSize -= UncompressedBlockSize;
		}
	}
	else
	{
		// otherwise we copy what we have left

		while (EncodedADPCMBlock < EncodedDataEnd)
		{
			ADPCM::DecodeBlock(EncodedADPCMBlock, CompressedBlockSize, (int16*)Destination);
			EncodedADPCMBlock += CompressedBlockSize;
			Destination += UncompressedBlockSize;
			BufferSize -= UncompressedBlockSize;
		}

		// and then decide if we zero pad or reset and loop
		if (bLooping)
		{
			EncodedADPCMBlock = (uint8*)EncodedADPCMBlockStart;
			SrcBufferOffset = 0;
			while (BufferSize)
			{
				ADPCM::DecodeBlock(EncodedADPCMBlock, CompressedBlockSize, (int16*)Destination);
				EncodedADPCMBlock += CompressedBlockSize;
				SrcBufferOffset += CompressedBlockSize;
				Destination += UncompressedBlockSize;
				BufferSize -= UncompressedBlockSize;
			}
		}
		else
		{
			FMemory::Memzero(Destination, BufferSize);
		}
	}

	return true;
}

void FADPCMAudioInfo::ExpandFile(uint8* DstBuffer, struct FSoundQualityInfo* QualityInfo)
{
	check(DstBuffer);

	if (*WaveInfo.pChannels == 1)
	{
		const uint8* EncodedADPCMBlockStart = reinterpret_cast<uint8*>(WaveInfo.SampleDataStart);
		uint8 * EncodedADPCMBlock = (uint8*)EncodedADPCMBlockStart;
		while (EncodedADPCMBlock < EncodedADPCMBlockStart + (WaveInfo.SampleDataSize))
		{
			ADPCM::DecodeBlock(EncodedADPCMBlock, CompressedBlockSize, (int16*)DstBuffer);
			EncodedADPCMBlock += CompressedBlockSize;
			DstBuffer += UncompressedBlockSize;
		}
	}
	else
	{
		const uint8* EncodedADPCMBlockLeftStart = reinterpret_cast<uint8*>(WaveInfo.SampleDataStart);
		const uint8* EncodedADPCMBlockRightStart = reinterpret_cast<uint8*>(WaveInfo.SampleDataStart + (WaveInfo.SampleDataSize / 2));
		uint8 * EncodedADPCMLeftBlock = (uint8*)EncodedADPCMBlockLeftStart;
		uint8 * EncodedADPCMRightBlock = (uint8*)EncodedADPCMBlockRightStart;
		while (EncodedADPCMLeftBlock < EncodedADPCMBlockLeftStart + (WaveInfo.SampleDataSize / 2))
		{
			ADPCM::DecodeBlockStereo(EncodedADPCMLeftBlock, EncodedADPCMRightBlock, CompressedBlockSize, (int16*)DstBuffer);
			EncodedADPCMLeftBlock += CompressedBlockSize;
			EncodedADPCMRightBlock += CompressedBlockSize;
			DstBuffer += (UncompressedBlockSize * 2);
		}
	}
}

int FADPCMAudioInfo::GetStreamBufferSize() const
{
	return StreamBufferSize;
}