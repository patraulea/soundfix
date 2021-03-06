#include <stdio.h>

#include "wav.h"

#pragma warning(disable:4996)

bool read_wav(const wchar_t *fname, short **samples, int *len)
{
	FILE *f = NULL; 
	short *buf = NULL;

	WavHeader hdr;
	WavFmt fmt;
	WavData data;

	int line = 0;
	#define BREAK { line = __LINE__; break; }

	do {
        f = _wfopen(fname, L"rb");
		if (!f) BREAK;

		if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
            BREAK;
		if (hdr.ChunkID != 0x46464952)
			BREAK;
		if (hdr.Format != 0x45564157)
			BREAK;

		if (fread(&fmt, 1, sizeof(fmt), f) != sizeof(fmt))
			BREAK;
		if (fmt.SubchunkID != 0x20746d66)
			BREAK;
        if (fmt.SubchunkSize != 16 &&
            fmt.SubchunkSize != 18) // ffmpeg from winff-1.4.2 uses this size
			BREAK;
		if (fmt.AudioFormat != 1)
			BREAK;
		if (fmt.NumChannels != 1)
			BREAK;
		if (fmt.SampleRate != 44100)
			BREAK;
		if (fmt.BitsPerSample != 16)
			BREAK;

        int fmtJunk = fmt.SubchunkSize - 16;
        if (fmtJunk)
            fseek(f, fmtJunk, SEEK_CUR);

        for (;;) {
            if (fread(&data, 1, sizeof(data), f) != sizeof(data))
                BREAK;

            if (data.SubchunkID == 0x5453494c) { // LIST
                fseek(f, data.SubchunkSize, SEEK_CUR);
                continue;
            } else {
                break;
            }
        }

        if (data.SubchunkID != 0x61746164) // data
            BREAK;

		int count = data.SubchunkSize / 2;
		if (count <= 0 || count > 100000000)
			BREAK;
		
		buf = new short[count];
		if (!buf) BREAK;

		if (fread(buf, 1, count*2, f) != count*2)
			BREAK;

		fclose(f);
		*samples = buf;
		*len = count;
		return true;
	} while (0);

	delete buf;
	if (f) fclose(f);
	printf("%s: load failed at line %d\n", fname, line);
	return false;
}
