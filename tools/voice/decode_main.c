/* decode_main.c - libg722_1 → WAV (GPL-2.0 via separate process)
 * usage: decode.exe in.raw out.wav [bitrate]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <g722_1/g722_1.h>

/* referenced by patched/debug builds of decoder.c */
int G722_DBG_ERR;
int G722_DBG_BITSLEFT;

static void put32(FILE *f, unsigned long v)
{
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}
static void put16(FILE *f, unsigned v)
{
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
}

int main(int argc, char **argv)
{
    FILE *fi, *fo;
    g722_1_decode_state_t *s;
    int bit_rate = 16000;
    int frame_bytes, done = 0, total = 0;
    unsigned char buf[512];
    short pcm[2048];
    long data_bytes = 0;
    int n;

    if (argc < 3) {
        fprintf(stderr, "usage: %s in.raw out.wav [bitrate]\n", argv[0]);
        return 1;
    }
    if (argc > 3) bit_rate = atoi(argv[3]);

    fi = fopen(argv[1], "rb");
    if (!fi) { perror("open input"); return 1; }
    fo = fopen(argv[2], "wb");
    if (!fo) { perror("open output"); return 1; }

    s = g722_1_decode_init(NULL, bit_rate, 16000);
    if (!s) { fprintf(stderr, "decode init failed for rate %d\n", bit_rate); return 1; }

    frame_bytes = (bit_rate / 50) / 8;
    fprintf(stderr, "bit_rate=%d frame_bytes=%d samples/frame=%d\n",
            bit_rate, frame_bytes, MAX_FRAME_SIZE / 2);

    fwrite("RIFF", 1, 4, fo);
    put32(fo, 0);
    fwrite("WAVEfmt ", 1, 8, fo);
    put32(fo, 16);
    put16(fo, 1);
    put16(fo, 1);
    put32(fo, 16000);
    put32(fo, 16000 * 2);
    put16(fo, 2);
    put16(fo, 16);
    fwrite("data", 1, 4, fo);
    put32(fo, 0);

    while (1) {
        n = (int)fread(buf, 1, frame_bytes, fi);
        if (n < frame_bytes) break;
        n = g722_1_decode(s, pcm, buf, frame_bytes);
        if (n > 0) {
            fwrite(pcm, sizeof(short), n, fo);
            data_bytes += n * 2;
            total += n;
        }
        done++;
    }

    fseek(fo, 4, SEEK_SET);
    put32(fo, 36 + data_bytes);
    fseek(fo, 40, SEEK_SET);
    put32(fo, data_bytes);
    fclose(fi);
    fclose(fo);
    g722_1_decode_release(s);
    fprintf(stderr, "frames=%d samples=%d\n", done, total);
    return 0;
}
