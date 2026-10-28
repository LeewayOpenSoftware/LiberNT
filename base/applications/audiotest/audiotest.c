/*
 * PROJECT:     LiberNT audio test harness
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     440 Hz sine through waveOut with reserved engine options
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SAMPLE_RATE 48000
#define PLAY_SECONDS 2
#define CHANNELS 2
#define BITS_PER_SAMPLE 16
#define SINE_FREQ 440.0
#define FRAMES (SAMPLE_RATE * PLAY_SECONDS)

static SHORT SineBuffer[FRAMES * CHANNELS];

static int Report(const char *result, int code)
{
    FILE *f = fopen("audiotest-result.txt", "w");
    if (f)
    {
        fprintf(f, "result=%s code=%d device=waveOut freq=%d rate=%d\n",
                result, code, (int)SINE_FREQ, SAMPLE_RATE);
        fclose(f);
    }
    printf("audiotest: %s (code %d)\n", result, code);
    return code;
}

int main(int argc, char **argv)
{
    WAVEFORMATEX format;
    HWAVEOUT device;
    WAVEHDR header;
    MMRESULT res;
    int i, code;

    if (argc > 1)
    {
        if (!strcmp(argv[1], "--ping"))
            return Report("skip --ping reserved for audiodg IPC (phase 3)", 2);
        if (!strcmp(argv[1], "--wasapi"))
            return Report("skip --wasapi reserved for the shared engine (phase 4)", 2);
        if (!strcmp(argv[1], "--capture"))
            return Report("skip --capture reserved for shared capture (phase 8)", 2);
        printf("usage: audiotest [--ping | --wasapi | --capture]\n");
        return 64;
    }

    for (i = 0; i < FRAMES; i++)
    {
        double s = sin(2.0 * 3.14159265358979323846 * SINE_FREQ * i / SAMPLE_RATE);
        SHORT v = (SHORT)(s * 26000.0);
        SineBuffer[i * CHANNELS] = v;
        SineBuffer[i * CHANNELS + 1] = v;
    }

    ZeroMemory(&format, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = CHANNELS;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = BITS_PER_SAMPLE;
    format.nBlockAlign = CHANNELS * BITS_PER_SAMPLE / 8;
    format.nAvgBytesPerSec = SAMPLE_RATE * format.nBlockAlign;

    res = waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR)
        return Report("fail waveOutOpen", 1);

    ZeroMemory(&header, sizeof(header));
    header.lpData = (LPSTR)SineBuffer;
    header.dwBufferLength = sizeof(SineBuffer);
    res = waveOutPrepareHeader(device, &header, sizeof(header));
    if (res == MMSYSERR_NOERROR)
        res = waveOutWrite(device, &header, sizeof(header));
    if (res == MMSYSERR_NOERROR)
    {
        while (!(header.dwFlags & WHDR_DONE))
            Sleep(10);
    }
    code = (res == MMSYSERR_NOERROR && (header.dwFlags & WHDR_DONE)) ? 0 : 1;
    if (res == MMSYSERR_NOERROR)
        waveOutUnprepareHeader(device, &header, sizeof(header));
    waveOutClose(device);
    return Report(code ? "fail playback" : "pass", code);
}
