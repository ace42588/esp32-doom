#include "miniz.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

#define TAG "ws_deflate"

int ws_deflate_compress(
    const uint8_t *input, size_t input_len,
    uint8_t *output, size_t *output_len,
    mz_stream *stream_ctx  // optional, for context reuse
) {
    if (!input || !output || !output_len) return -1;

    mz_stream *stream;
    int init_result;
    mz_stream temp_stream;
    
    if (stream_ctx) {
        // Use existing stream context
        stream = stream_ctx;
        // Reset the stream for new data
        mz_deflateReset(stream);
    } else {
        // Create a temporary stream for one-time use
        memset(&temp_stream, 0, sizeof(temp_stream));
        init_result = mz_deflateInit2(&temp_stream, 1, MZ_DEFLATED, -15, 8, MZ_DEFAULT_STRATEGY);
        if (init_result != MZ_OK) {
            ESP_LOGE(TAG, "Deflate init failed: %d", init_result);
            return -1;
        }
        stream = &temp_stream;
    }

    stream->next_in = (unsigned char *)input;
    stream->avail_in = input_len;
    stream->next_out = output;
    stream->avail_out = *output_len;

    int status = mz_deflate(stream, MZ_FINISH);
    if (status != MZ_STREAM_END) {
        ESP_LOGE(TAG, "Compression failed: %d", status);
        // Don't call mz_deflateEnd on temp stream - it will be cleaned up automatically
        return -1;
    }

    // miniz already adds the proper final block, no need for manual trailer
    *output_len = stream->total_out;

    // Don't call mz_deflateEnd on temp stream - it will be cleaned up automatically
    return 0;
}

int ws_deflate_decompress(
    const uint8_t *input, size_t input_len,
    uint8_t *output, size_t *output_len,
    mz_stream *stream_ctx  // optional, for context reuse
) {
    if (!input || !output || !output_len) return -1;

    // No need to check for RFC 7692 trailer - miniz handles it automatically

    mz_stream *stream;
    int init_result;
    mz_stream temp_stream;
    
    if (stream_ctx) {
        // Use existing stream context
        stream = stream_ctx;
        // Reset the stream for new data
        mz_inflateReset(stream);
    } else {
        // Create a temporary stream for one-time use
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
        // Don't call mz_inflateEnd on temp stream - it will be cleaned up automatically
        return -1;
    }

    *output_len = stream->total_out;
    // Don't call mz_inflateEnd on temp stream - it will be cleaned up automatically
    return 0;
}
