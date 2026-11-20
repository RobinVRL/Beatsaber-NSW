#include "../source/android_image_bridge.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const uint8_t pixels[32] = {
        1,2,3,255, 4,5,6,255, 0,0,0,0, 0,0,0,0,
        7,8,9,255, 10,11,12,255, 0,0,0,0, 0,0,0,0
    };
    uint8_t *data = NULL;
    int data_length = -1;
    int32_t width = -1, height = -1, row_stride = -1;
    int64_t timestamp = -1;
    AImage *image = bs_android_image_from_rgba(pixels, 2, 2, 16, 123456);
    assert(image);
    assert(AImage_getWidth(image, &width) == 0 && width == 2);
    assert(AImage_getHeight(image, &height) == 0 && height == 2);
    assert(AImage_getTimestamp(image, &timestamp) == 0 && timestamp == 123456);
    assert(AImage_getPlaneData(image, 0, &data, &data_length) == 0);
    assert(data_length == 32 && data && data != pixels);
    assert(memcmp(data, pixels, sizeof pixels) == 0);
    assert(AImage_getPlaneRowStride(image, 0, &row_stride) == 0 && row_stride == 16);
    assert(AImage_getPlaneData(image, 1, &data, &data_length) == -10004);
    assert(AImage_getWidth(image, NULL) == -10004);
    assert(!bs_android_image_from_rgba(pixels, 2, 2, 4, 0));
    AImage_delete(image);
    assert(AImage_getWidth(image, &width) == -10003);
    assert(AImage_getWidth(NULL, &width) == -10003);
    AImage_delete(image);
    puts("PASS: real copied CPU AImage and invalid-object guards");
    return 0;
}
