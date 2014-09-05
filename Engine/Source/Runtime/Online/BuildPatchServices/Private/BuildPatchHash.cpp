// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	BuildPatchHash.cpp: Implements the static struct FRollingHashConst
=============================================================================*/

#include "BuildPatchServicesPrivatePCH.h"

// We'll use the commonly used in CRC64, ECMA polynomial defined in ECMA 182.
static const uint64 HashPoly64 = 0xC96C5795D7870F42;

uint64 FRollingHashConst::HashTable[ 256 ] = { 0 };

void FRollingHashConst::Init()
{
	for( uint32 TableIdx = 0; TableIdx < 256; ++TableIdx )
	{
		uint64 val = TableIdx;
		for( uint32 ShiftCount = 0; ShiftCount < 8; ++ShiftCount )
		{
			if( ( val & 1 ) == 1 )
			{
				val >>= 1;
				val ^= HashPoly64;
			}
			else
			{
				val >>= 1;
			}
		}
		HashTable[ TableIdx ] = val;
	}
}