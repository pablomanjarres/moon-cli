#ifndef HUFFMAN_H
#define HUFFMAN_H

typedef struct HuffmanJob HuffmanJob;

typedef struct {
    unsigned long long completed, total;
    int done, error, cancelled, snapshot_ready;
} HuffmanStatus;

HuffmanJob *huffman_start(int input_fd, const char *output, int decompress,
                         unsigned workers);
int huffman_event_fd(HuffmanJob *job);
HuffmanStatus huffman_status(HuffmanJob *job);
void huffman_cancel(HuffmanJob *job);
void huffman_destroy(HuffmanJob *job);

#endif
