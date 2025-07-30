#include "miniz.h"
#include "esp_log.h"
#include <string.h>

#define TAG "ws_deflate"
#define DEFLATE_TRAILER_LEN 4

// RFC 7692 requires a final block trailer (empty uncompressed block, BFINAL=1)
static const uint8_t rfc7692_trailer[DEFLATE_TRAILER_LEN] = { 0x00, 0x00, 0xFF, 0xFF };

int ws_deflate_compress(
    const uint8_t *input, size_t input_len,
    uint8_t *output, size_t *output_len,
    mz_stream *stream_ctx  // optional, for context reuse
) {
    if (!input || !output || !output_len) return -1;

    mz_stream *stream;
    int init_result;
    
    if (stream_ctx) {
        // Use existing stream context
        stream = stream_ctx;
        // Reset the stream for new data
        mz_deflateReset(stream);
    } else {
        // Create a temporary stream for one-time use
        static mz_stream temp_stream;
        memset(&temp_stream, 0, sizeof(temp_stream));
        init_result = mz_deflateInit2(&temp_stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, -15, 8, MZ_DEFAULT_STRATEGY);
        if (init_result != MZ_OK) {
            ESP_LOGE(TAG, "Deflate init failed: %d", init_result);
            return -1;
        }
        stream = &temp_stream;
    }

    stream->next_in = (unsigned char *)input;
    stream->avail_in = input_len;
    stream->next_out = output;
    stream->avail_out = *output_len - DEFLATE_TRAILER_LEN;

    int status = mz_deflate(stream, MZ_FINISH);
    if (status != MZ_STREAM_END) {
        ESP_LOGE(TAG, "Compression failed: %d", status);
        if (!stream_ctx) {
            mz_deflateEnd(stream);
        }
        return -1;
    }

    // Append trailer
    memcpy(stream->next_out, rfc7692_trailer, DEFLATE_TRAILER_LEN);
    *output_len = stream->total_out + DEFLATE_TRAILER_LEN;

    if (!stream_ctx) {
        mz_deflateEnd(stream);
    }
    return 0;
}

int ws_deflate_decompress(
    const uint8_t *input, size_t input_len,
    uint8_t *output, size_t *output_len,
    mz_stream *stream_ctx  // optional, for context reuse
) {
    if (!input || !output || !output_len) return -1;

    // Check for RFC 7692 trailer and remove it
    if (input_len >= DEFLATE_TRAILER_LEN) {
        if (memcmp(input + input_len - DEFLATE_TRAILER_LEN, rfc7692_trailer, DEFLATE_TRAILER_LEN) == 0) {
            input_len -= DEFLATE_TRAILER_LEN;
        }
    }

    mz_stream *stream;
    int init_result;
    
    if (stream_ctx) {
        // Use existing stream context
        stream = stream_ctx;
        // Reset the stream for new data
        mz_inflateReset(stream);
    } else {
        // Create a temporary stream for one-time use
        static mz_stream temp_stream;
        memset(&temp_stream, 0, sizeof(temp_stream));
        init_result = mz_inflateInit2(&temp_stream, -15);
        if (init_result != MZ_OK) {
            ESP_LOGE(TAG, "Inflate init failed: %d", init_result);
            return -1;
        }
        stream = &temp_stream;
    }

    stream->next_in = (unsigned char *)input;
    stream->avail_in = input_len;
    stream->next_out = output;
    stream->avail_out = *output_len;

    int status = mz_inflate(stream, MZ_FINISH);
    if (status != MZ_STREAM_END) {
        ESP_LOGE(TAG, "Decompression failed: %d", status);
        if (!stream_ctx) {
            mz_inflateEnd(stream);
        }
        return -1;
    }

    *output_len = stream->total_out;
    if (!stream_ctx) {
        mz_inflateEnd(stream);
    }
    return 0;
}
