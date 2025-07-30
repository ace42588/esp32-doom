#ifndef WS_DEFLATE_H
#define WS_DEFLATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration of mz_stream for the interface
typedef struct mz_stream_s mz_stream;

/**
 * Compress data using RFC 7692 compliant deflate compression
 * 
 * @param input Input data to compress
 * @param input_len Length of input data
 * @param output Output buffer for compressed data
 * @param output_len On input: size of output buffer, on output: size of compressed data
 * @param stream_ctx Optional stream context for reuse (can be NULL)
 * @return 0 on success, -1 on failure
 */
int ws_deflate_compress(
    const uint8_t *input, size_t input_len,
    uint8_t *output, size_t *output_len,
    mz_stream *stream_ctx
);

/**
 * Decompress data using RFC 7692 compliant deflate decompression
 * 
 * @param input Input compressed data
 * @param input_len Length of input data
 * @param output Output buffer for decompressed data
 * @param output_len On input: size of output buffer, on output: size of decompressed data
 * @param stream_ctx Optional stream context for reuse (can be NULL)
 * @return 0 on success, -1 on failure
 */
int ws_deflate_decompress(
    const uint8_t *input, size_t input_len,
    uint8_t *output, size_t *output_len,
    mz_stream *stream_ctx
);

#ifdef __cplusplus
}
#endif

#endif // WS_DEFLATE_H 