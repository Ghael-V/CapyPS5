#include "decinit.h"
#include "decoder.h"
#include "libatrac9.h"
#include "structures.h"
#include <stdlib.h>
#include <string.h>

void* Atrac9GetHandle()
{
	return calloc(1, sizeof(Atrac9Handle));
}

void Atrac9ReleaseHandle(void* handle)
{
	free(handle);
}

int Atrac9InitDecoder(void* handle, unsigned char * pConfigData)
{
	return InitDecoder(handle, pConfigData, 16);
}

int Atrac9Decode(void* handle, const unsigned char *pAtrac9Buffer, short *pPcmBuffer, int *pNBytesUsed, int nointerleave)
{
	return Decode(handle, pAtrac9Buffer, (unsigned char*)pPcmBuffer, pNBytesUsed, nointerleave);
}

int Atrac9DecodeS32(void* handle, const unsigned char *pAtrac9Buffer, int *pPcmBuffer, int *pNBytesUsed, int nointerleave)
{
	return DecodeS32(handle, pAtrac9Buffer, pPcmBuffer, pNBytesUsed, nointerleave);
}

int Atrac9DecodeF32(void* handle, const unsigned char *pAtrac9Buffer, float *pPcmBuffer, int *pNBytesUsed, int nointerleave)
{
	return DecodeF32(handle, pAtrac9Buffer, pPcmBuffer, pNBytesUsed, nointerleave);
}

int Atrac9GetCodecInfo(void* handle, Atrac9CodecInfo * pCodecInfo)
{
	return GetCodecInfo(handle, (CodecInfo*)pCodecInfo);
}
