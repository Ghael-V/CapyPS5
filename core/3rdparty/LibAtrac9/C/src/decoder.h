#pragma once

#include "error_codes.h"
#include "structures.h"

At9Status Decode(Atrac9Handle* handle, const unsigned char* audio, unsigned char* pcm, int* bytesUsed, int nointerleave);
At9Status DecodeS32(Atrac9Handle* handle, const unsigned char* audio, int* pcm, int* bytesUsed, int nointerleave);
At9Status DecodeF32(Atrac9Handle* handle, const unsigned char* audio, float* pcm, int* bytesUsed, int nointerleave);
int GetCodecInfo(Atrac9Handle* handle, CodecInfo* pCodecInfo);
