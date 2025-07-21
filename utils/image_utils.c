#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h> // Added for memcpy, strstr, strrchr, strcmp
#include <sys/time.h>

#include "im2d.h"
#include "drmrga.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "image_utils.h"
#include "file_utils.h" // Assuming this provides write_data_to_file

static const char* filter_image_names[] = {
    "jpg",
    "jpeg",
    "JPG",
    "JPEG",
    "png",
    "PNG",
    "data",
    NULL
};

#ifndef DISABLE_LIBJPEG
#include "turbojpeg.h"
static const char* subsampName[TJ_NUMSAMP] = {"4:4:4", "4:2:2", "4:2:0", "Grayscale", "4:4:0", "4:1:1"};
static const char* colorspaceName[TJ_NUMCS] = {"RGB", "YCbCr", "GRAY", "CMYK", "YCCK"};

static int read_image_jpeg(const char* path, image_buffer_t* image)
{
    printf("DEBUG: Attempting to open JPEG file: '%s'\n", path);
    FILE* jpegFile = NULL;
    unsigned long jpegSize = 0; // Initialize for safety
    int width, height;
    int origin_width, origin_height;
    unsigned char* jpegBuf = NULL;
    unsigned long size = 0; // Initialize for safety
    tjhandle handle = NULL; // Moved declaration up
    int subsample, colorspace; // Moved declaration up
    int ret = -1; // Default to error return, change to 0 on success

    // 1. Open File
    if ((jpegFile = fopen(path, "rb")) == NULL) {
        printf("ERROR: Failed to open input file '%s'\n", path);
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    printf("DEBUG: Successfully opened file '%s'.\n", path);

    // 2. Determine File Size
    if (fseek(jpegFile, 0, SEEK_END) < 0 || (size = ftell(jpegFile)) < 0 || fseek(jpegFile, 0, SEEK_SET) < 0) {
        printf("ERROR: Determining input file size failure for '%s'.\n", path);
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    if (size == 0) {
        printf("ERROR: Input file '%s' contains no data.\n", path);
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    jpegSize = (unsigned long)size;
    printf("DEBUG: JPEG file size: %lu bytes.\n", jpegSize);

    // 3. Allocate JPEG Buffer
    if ((jpegBuf = (unsigned char*)malloc(jpegSize)) == NULL) {
        printf("ERROR: Failed to allocate JPEG buffer of size %lu\n", jpegSize);
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    printf("DEBUG: Allocated jpegBuf at %p.\n", (void*)jpegBuf);

    // 4. Read JPEG Data into Buffer
    if (fread(jpegBuf, 1, jpegSize, jpegFile) < jpegSize) {
        printf("ERROR: Failed to read %lu bytes from input file '%s' into buffer.\n", jpegSize, path);
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    printf("DEBUG: Successfully read JPEG data into buffer.\n");

    // Close the file immediately after reading its content
    fclose(jpegFile);
    jpegFile = NULL; // Set to NULL to prevent double-close in cleanup

    // 5. Initialize libjpeg-turbo Decompressor
    printf("DEBUG: Attempting to initialize libjpeg-turbo decompressor.\n");
    handle = tjInitDecompress();
    if (handle == NULL) {
        printf("ERROR: tjInitDecompress failed.\n");
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    printf("DEBUG: tjInitDecompress successful, handle: %p\n", (void*)handle);

    // 6. Decompress JPEG Header (First call)
    printf("DEBUG: Calling tjDecompressHeader3 for JPEG data (size: %lu).\n", size);
    ret = tjDecompressHeader3(handle, jpegBuf, size, &origin_width, &origin_height, &subsample, &colorspace);
    if (ret < 0) {
        printf("ERROR: tjDecompressHeader3 failed for '%s'. ErrorStr: '%s', errorCode: %d\n", path, tjGetErrorStr(), tjGetErrorCode(handle)); // FIXED
        goto out; // ⚠️ CRITICAL: Exit on failure
    }
    printf("DEBUG: tjDecompressHeader3 successful. Image dimensions: %dx%d, Subsampling: %s, Colorspace: %s\n",
           origin_width, origin_height, subsampName[subsample], colorspaceName[colorspace]);

    // Use original dimensions, no need for redundant tjDecompressHeader3 call
    width = origin_width;
    height = origin_height;

    printf("DEBUG: Target image dimensions for decoding: %d x %d\n", width, height);

    // 7. Allocate Output Buffer (sw_out_buf)
    int sw_out_size = width * height * 3; // Assuming TJPF_RGB output (3 bytes per pixel)
    unsigned char* sw_out_buf = image->virt_addr; // Check if caller provided buffer

    if (sw_out_buf == NULL) {
        printf("DEBUG: image->virt_addr is NULL, attempting to malloc %d bytes for sw_out_buf.\n", sw_out_size);
        sw_out_buf = (unsigned char*)malloc(sw_out_size);
        if (sw_out_buf == NULL) {
            printf("ERROR: Failed to allocate sw_out_buf of size %d\n", sw_out_size);
            goto out; // ⚠️ CRITICAL: Exit on malloc failure
        }
        printf("DEBUG: Successfully allocated new sw_out_buf at %p.\n", (void*)sw_out_buf);
    } else {
        printf("DEBUG: Using pre-allocated sw_out_buf at %p (caller-provided size: %d).\n", (void*)sw_out_buf, image->size);
        // Optional: Add a check if image->size is sufficient
        if (image->size < sw_out_size) {
            printf("ERROR: Provided image buffer (size %d) is too small for required size (%d).\n", image->size, sw_out_size);
            // If the buffer was provided by the caller and is too small, it's an error.
            // Do NOT free sw_out_buf here, as it's not owned by this function if it was passed in.
            goto out;
        }
    }

    // 8. CRITICAL: Fill buffer with a known value for debugging
    memset(sw_out_buf, 0xCC, sw_out_size);
    printf("DEBUG: sw_out_buf initialized to 0xCC. First 9 bytes before tjDecompress2: %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
           sw_out_buf[0], sw_out_buf[1], sw_out_buf[2], sw_out_buf[3], sw_out_buf[4], sw_out_buf[5], sw_out_buf[6], sw_out_buf[7], sw_out_buf[8]);

    int pixelFormat = TJPF_RGB; // Assuming RGB output for image_buffer_t
    int flags = 0; // Set appropriate flags if needed, otherwise 0

    // 9. Call tjDecompress2
    printf("DEBUG: Calling tjDecompress2 with handle: %p, jpegBuf: %p, size: %lu, sw_out_buf: %p, width: %d, pitch: 0, height: %d, pixelFormat: TJPF_RGB, flags: %d\n",
           (void*)handle, (void*)jpegBuf, size, (void*)sw_out_buf, width, height, pixelFormat, flags);
    ret = tjDecompress2(handle, jpegBuf, size, sw_out_buf, width, 0, height, pixelFormat, flags);

    printf("DEBUG: After tjDecompress2 call: returned 'ret' value: %d\n", ret);
    printf("DEBUG: tjGetErrorCode(handle): %d, tjGetErrorStr(): '%s'\n", tjGetErrorCode(handle), tjGetErrorStr()); // FIXED

    // 10. CRITICAL: Inspect Pixels After Decompression
    printf("DEBUG: Pixels AFTER tjDecompress2, expecting RGB (0-255):\n");
    if (sw_out_buf && sw_out_size >= 9) {
        printf("Px1: R=%u, G=%u, B=%u\n", sw_out_buf[0], sw_out_buf[1], sw_out_buf[2]);
        printf("Px2: R=%u, G=%u, B=%u\n", sw_out_buf[3], sw_out_buf[4], sw_out_buf[5]);
        printf("Px3: R=%u, G=%u, B=%u\n", sw_out_buf[6], sw_out_buf[7], sw_out_buf[8]);
    } else {
        printf("Buffer is NULL or too small after tjDecompress2.\n");
    }

    // 11. Comprehensive Error Handling for tjDecompress2
    if (ret < 0) { // Only fail if tjDecompress2 returned a negative value (fatal error)
        printf("ERROR: tjDecompress2 returned a fatal error for '%s'. ErrorStr: '%s', ErrorCode: %d\n",
               path, tjGetErrorStr(), tjGetErrorCode(handle));

        if (image->virt_addr == NULL && sw_out_buf != NULL) {
            free(sw_out_buf);
            sw_out_buf = NULL;
        }
        ret = -1; // Ensure an error return
        goto out;
    } else if (tjGetErrorCode(handle) != 0) { // Log warnings but don't fail
        printf("WARNING: tjDecompress2 encountered a non-fatal issue for '%s'. ErrorStr: '%s', ErrorCode: %d\n",
               path, tjGetErrorStr(), tjGetErrorCode(handle));
    // Do NOT set ret = -1 or goto out here, as decompression was successful.
}

    // 12. Success Path: Populate image_buffer_t struct
    image->width = width;
    image->height = height;
    image->format = IMAGE_FORMAT_RGB888; // Explicitly set format
    image->virt_addr = sw_out_buf; // Assign the decoded buffer
    image->size = sw_out_size;
    ret = 0; // Set return value to success

out: // Unified cleanup label
    printf("DEBUG: Entering cleanup phase for read_image_jpeg. jpegFile: %p, jpegBuf: %p, handle: %p\n",
           (void*)jpegFile, (void*)jpegBuf, (void*)handle);

    if (jpegFile) {
        fclose(jpegFile);
        // jpegFile = NULL; // Already set to NULL after fread
    }
    if (jpegBuf) {
        free(jpegBuf);
        jpegBuf = NULL;
    }
    if (handle) {
        tjDestroy(handle);
        handle = NULL;
    }
    // IMPORTANT: sw_out_buf (image->virt_addr) is *not* freed here if `ret` is 0 (success).
    // It's now owned by the `image_buffer_t` struct, which the caller is responsible for.
    // If it was allocated locally (image->virt_addr was NULL initially) AND an error occurred AFTER
    // its allocation, it should have been freed at the point of the error, *before* goto out.

    return ret; // Return the determined success/failure status
}

static int write_image_jpeg(const char* path, int quality, const image_buffer_t* image)
{
    int ret = -1; // Default to error
    int jpegSubsamp = TJSAMP_422;
    unsigned char* jpegBuf = NULL;
    unsigned long jpegSize = 0;
    int flags = 0;
    tjhandle handle = NULL;

    const unsigned char* data = image->virt_addr;
    int width = image->width;
    int height = image->height;
    int pixelFormat;

    // Determine pixel format based on image->format
    if (image->format == IMAGE_FORMAT_RGB888) {
        pixelFormat = TJPF_RGB;
    } else {
        printf("write_image_jpeg: pixel format %d not supported for encoding.\n", image->format);
        goto write_out;
    }

    handle = tjInitCompress();
    if (handle == NULL) {
        printf("ERROR: tjInitCompress failed.\n");
        goto write_out;
    }

    ret = tjCompress2(handle, data, width, 0, height, pixelFormat, &jpegBuf, &jpegSize, jpegSubsamp, quality, flags);

    if (ret != 0 || tjGetErrorCode(handle) != 0) {
        printf("ERROR: tjCompress2 failed. ErrorStr: '%s', ErrorCode: %d\n", tjGetErrorStr(), tjGetErrorCode(handle)); // FIXED
    // ... (previous code) ...
        ret = -1; // Ensure error return
        goto write_out;
    }

    // printf("ret=%d jpegBuf=%p jpegSize=%lu\n", ret, (void*)jpegBuf, jpegSize);
    if (jpegBuf != NULL && jpegSize > 0) {
        if (write_data_to_file(path, (const char*)jpegBuf, jpegSize) != 0) {
            printf("ERROR: Failed to write compressed JPEG data to file '%s'.\n", path);
            ret = -1; // Ensure error return
        } else {
            ret = 0; // Success
        }
    } else {
        printf("ERROR: Compressed JPEG buffer is NULL or size is 0.\n");
        ret = -1; // Ensure error return
    }

write_out: // Cleanup for write_image_jpeg
    if (jpegBuf) {
        tjFree(jpegBuf); // Use tjFree for buffers allocated by libjpeg-turbo
        jpegBuf = NULL;
    }
    if (handle) {
        tjDestroy(handle);
        handle = NULL;
    }
    return ret;
}
#endif

// Original `image_file_filter`, `read_image_raw`, `read_image_stb`
// and `convert_image` related functions remain as they were,
// assuming they are correct or their issues are out of this scope.
// Only included here for completeness of the file.

static int image_file_filter(const struct dirent *entry)
{
    const char ** filter;

    for (filter = filter_image_names; *filter; ++filter) {
        if(strstr(entry->d_name, *filter) != NULL) {
            return 1;
        }
    }
    return 0;
}

static int read_image_raw(const char* path, image_buffer_t* image)
{
    FILE *fp = fopen(path, "rb");
    if(fp == NULL) {
        printf("fopen %s fail!\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);
    unsigned char *data = image->virt_addr;
    if (image->virt_addr == NULL) {
        data = (unsigned char *)malloc(file_size+1);
    }
    data[file_size] = 0; // Null-terminate, common for text/raw, but care for binary.
    fseek(fp, 0, SEEK_SET);
    if(file_size != fread(data, 1, file_size, fp)) {
        printf("fread %s fail!\n", path);
        free(data); // Free if allocation was here
        fclose(fp); // Close file on error
        return -1;
    }
    if(fp) {
        fclose(fp);
    }
    if (image->virt_addr == NULL) {
        image->virt_addr = data;
        image->size = file_size;
    }

    return 0;
}

static int read_image_stb(const char* path, image_buffer_t* image)
{
    // 默认图像为3通道
    int w, h, c;
    // STB_IMAGE loads into an RGB (or RGBA) buffer always
    unsigned char* pixeldata = stbi_load(path, &w, &h, &c, 0);
    if (!pixeldata) {
        printf("error: read image %s fail\n", path);
        return -1;
    }
    // printf("load image wxhxc=%dx%dx%d path=%s\n", w, h, c, path);
    int size = w * h * c;

    // 设置图像数据
    if (image->virt_addr != NULL) {
        memcpy(image->virt_addr, pixeldata, size);
        stbi_image_free(pixeldata); // Free STB-allocated buffer as we copied it
    } else {
        image->virt_addr = pixeldata; // Assign STB-allocated buffer
    }
    image->width = w;
    image->height = h;
    if (c == 4) {
        image->format = IMAGE_FORMAT_RGBA8888;
    } else if (c == 1) {
        image->format = IMAGE_FORMAT_GRAY8;
    } else { // Default to RGB for 3 channels
        image->format = IMAGE_FORMAT_RGB888;
    }
    image->size = size; // Set size
    return 0;
}

int read_image(const char* path, image_buffer_t* image)
{
    const char* _ext = strrchr(path, '.');
    if (!_ext) {
        // missing extension
        printf("ERROR: File '%s' has no extension.\n", path);
        return -1;
    }
    if (strcmp(_ext, ".data") == 0) {
        return read_image_raw(path, image);
#ifndef DISABLE_LIBJPEG
    } else if (strcmp(_ext, ".jpg") == 0 || strcmp(_ext, ".jpeg") == 0 || strcmp(_ext, ".JPG") == 0 ||
        strcmp(_ext, ".JPEG") == 0) {
        return read_image_jpeg(path, image);
#endif
    } else { // Fallback to STB for PNG and other formats
        return read_image_stb(path, image);
    }
}

int write_image(const char* path, const image_buffer_t* img)
{
    int ret;
    int width = img->width;
    int height = img->height;
    int channel;
    void* data = img->virt_addr;

    // Determine channel count based on format
    if (img->format == IMAGE_FORMAT_RGB888) {
        channel = 3;
    } else if (img->format == IMAGE_FORMAT_RGBA8888) {
        channel = 4;
    } else if (img->format == IMAGE_FORMAT_GRAY8) {
        channel = 1;
    } else {
        printf("write_image: Unsupported image format %d for writing.\n", img->format);
        return -1;
    }

    printf("write_image path: %s width=%d height=%d channel=%d data=%p\n",
        path, width, height, channel, data);

    const char* _ext = strrchr(path, '.');
    if (!_ext) {
        // missing extension
        return -1;
    }

    if (strcmp(_ext, ".png") == 0 || strcmp(_ext, ".PNG") == 0) { // Fixed logical OR to ||
        ret = stbi_write_png(path, width, height, channel, data, width * channel); // Added stride
    } else if (strcmp(_ext, ".jpg") == 0 || strcmp(_ext, ".jpeg") == 0 || strcmp(_ext, ".JPG") == 0 ||
        strcmp(_ext, ".JPEG") == 0) {
        int quality = 95;
#ifndef DISABLE_LIBJPEG
        ret = write_image_jpeg(path, quality, img);
#else
        ret = stbi_write_jpg(path, width, height, channel, data, quality);
#endif
    } else if (strcmp(_ext, ".data") == 0 || strcmp(_ext, ".DATA") == 0) { // Fixed logical OR to ||
        int size = get_image_size(img);
        ret = write_data_to_file(path, data, size);
    } else {
        // unknown extension type
        printf("write_image: Unsupported file extension '%s'.\n", _ext);
        return -1;
    }
    return ret;
}

static int crop_and_scale_image_c(int channel, unsigned char *src, int src_width, int src_height,
                                   int crop_x, int crop_y, int crop_width, int crop_height,
                                   unsigned char *dst, int dst_width, int dst_height,
                                   int dst_box_x, int dst_box_y, int dst_box_width, int dst_box_height) {
    if (dst == NULL || src == NULL) { // Added src == NULL check
        printf("src or dst buffer is null\n");
        return -1;
    }

    float x_ratio = (float)crop_width / (float)dst_box_width;
    float y_ratio = (float)crop_height / (float)dst_box_height;

    // printf("src_width=%d src_height=%d crop_x=%d crop_y=%d crop_width=%d crop_height=%d\n",
    //      src_width, src_height, crop_x, crop_y, crop_width, crop_height);
    // printf("dst_width=%d dst_height=%d dst_box_x=%d dst_box_y=%d dst_box_width=%d dst_box_height=%d\n",
    //      dst_width, dst_height, dst_box_x, dst_box_y, dst_box_width, dst_box_height);
    // printf("channel=%d x_ratio=%f y_ratio=%f\n", channel, x_ratio, y_ratio);

    // From original image specified area, bilinearly scale to target specified area
    for (int dst_y = dst_box_y; dst_y < dst_box_y + dst_box_height; dst_y++) {
        for (int dst_x = dst_box_x; dst_x < dst_box_x + dst_box_width; dst_x++) {
            int dst_x_offset = dst_x - dst_box_x;
            int dst_y_offset = dst_y - dst_box_y;

            // Calculate source coordinates
            float src_x_float = dst_x_offset * x_ratio;
            float src_y_float = dst_y_offset * y_ratio;

            int src_x1 = (int)src_x_float;
            int src_y1 = (int)src_y_float;
            int src_x2 = (src_x1 + 1 < crop_width) ? src_x1 + 1 : src_x1; // Clamp x2
            int src_y2 = (src_y1 + 1 < crop_height) ? src_y1 + 1 : src_y1; // Clamp y2

            float x_diff = src_x_float - src_x1;
            float y_diff = src_y_float - src_y1;

            // Calculate global source indices
            int global_src_x1 = crop_x + src_x1;
            int global_src_y1 = crop_y + src_y1;
            int global_src_x2 = crop_x + src_x2;
            int global_src_y2 = crop_y + src_y2;

            // Ensure indices are within bounds of the original source image
            global_src_x1 = fmax(0, fmin(global_src_x1, src_width - 1));
            global_src_y1 = fmax(0, fmin(global_src_y1, src_height - 1));
            global_src_x2 = fmax(0, fmin(global_src_x2, src_width - 1));
            global_src_y2 = fmax(0, fmin(global_src_y2, src_height - 1));


            int index_A = global_src_y1 * src_width * channel + global_src_x1 * channel;
            int index_B = global_src_y1 * src_width * channel + global_src_x2 * channel; // Right
            int index_C = global_src_y2 * src_width * channel + global_src_x1 * channel; // Down
            int index_D = global_src_y2 * src_width * channel + global_src_x2 * channel; // Down-Right

            for (int c = 0; c < channel; c++) {
                unsigned char A = src[index_A + c];
                unsigned char B = src[index_B + c];
                unsigned char C = src[index_C + c];
                unsigned char D = src[index_D + c];

                // Bilinear interpolation
                unsigned char pixel = (unsigned char)(
                    A * (1.0f - x_diff) * (1.0f - y_diff) +
                    B * x_diff * (1.0f - y_diff) +
                    C * (1.0f - x_diff) * y_diff +
                    D * x_diff * y_diff
                );

                dst[(dst_y * dst_width + dst_x) * channel + c] = pixel;
            }
        }
    }
    return 0;
}

static int crop_and_scale_image_yuv420sp(unsigned char *src, int src_width, int src_height,
                                         int crop_x, int crop_y, int crop_width, int crop_height,
                                         unsigned char *dst, int dst_width, int dst_height,
                                         int dst_box_x, int dst_box_y, int dst_box_width, int dst_box_height) {

    unsigned char* src_y = src;
    unsigned char* src_uv = src + src_width * src_height;

    unsigned char* dst_y = dst;
    unsigned char* dst_uv = dst + dst_width * dst_height;

    // Process Y plane (full resolution)
    crop_and_scale_image_c(1, src_y, src_width, src_height, crop_x, crop_y, crop_width, crop_height,
        dst_y, dst_width, dst_height, dst_box_x, dst_box_y, dst_box_width, dst_box_height);

    // Process UV plane (half resolution, x2 channels for UV interleaved)
    // Ensure all coordinates are even for UV plane
    crop_and_scale_image_c(2, src_uv, src_width / 2, src_height / 2,
        crop_x / 2, crop_y / 2, crop_width / 2, crop_height / 2, // Half-res coordinates
        dst_uv, dst_width / 2, dst_height / 2,
        dst_box_x / 2, dst_box_y / 2, dst_box_width / 2, dst_box_height / 2); // Half-res dest box

    return 0;
}

static int convert_image_cpu(image_buffer_t *src, image_buffer_t *dst, image_rect_t *src_box, image_rect_t *dst_box, char color) {
    int ret = 0; // Initialize ret
    if (dst->virt_addr == NULL) {
        printf("ERROR: Destination buffer is NULL.\n");
        return -1;
    }
    if (src->virt_addr == NULL) {
        printf("ERROR: Source buffer is NULL.\n");
        return -1;
    }
    if (src->format != dst->format) {
        printf("ERROR: Source and destination formats (%d vs %d) do not match for CPU conversion.\n", src->format, dst->format);
        // Note: For actual format conversion, you'd need more complex logic or RGA.
        return -1;
    }

    int src_box_x = 0;
    int src_box_y = 0;
    int src_box_w = src->width;
    int src_box_h = src->height;
    if (src_box != NULL) {
        src_box_x = src_box->left;
        src_box_y = src_box->top;
        src_box_w = src_box->right - src_box->left + 1;
        src_box_h = src_box->bottom - src_box->top + 1;
    }
    int dst_box_x = 0;
    int dst_box_y = 0;
    int dst_box_w = dst->width;
    int dst_box_h = dst->height;
    if (dst_box != NULL) {
        dst_box_x = dst_box->left;
        dst_box_y = dst_box->top;
        dst_box_w = dst_box->right - dst_box->left + 1;
        dst_box_h = dst_box->bottom - dst_box->top + 1;
    }

    // fill pad color if destination box is smaller than the whole destination image
    if (dst_box_w != dst->width || dst_box_h != dst->height || dst_box_x != 0 || dst_box_y != 0) { // Check all components
        int dst_size = get_image_size(dst);
        memset(dst->virt_addr, color, dst_size);
        printf("DEBUG: Filled destination image with pad color 0x%02X.\n", (unsigned int)color);
    }

    if (src->format == IMAGE_FORMAT_RGB888) {
        ret = crop_and_scale_image_c(3, src->virt_addr, src->width, src->height,
                                     src_box_x, src_box_y, src_box_w, src_box_h,
                                     dst->virt_addr, dst->width, dst->height,
                                     dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else if (src->format == IMAGE_FORMAT_RGBA8888) {
        ret = crop_and_scale_image_c(4, src->virt_addr, src->width, src->height,
                                     src_box_x, src_box_y, src_box_w, src_box_h,
                                     dst->virt_addr, dst->width, dst->height,
                                     dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else if (src->format == IMAGE_FORMAT_GRAY8) {
        ret = crop_and_scale_image_c(1, src->virt_addr, src->width, src->height,
                                     src_box_x, src_box_y, src_box_w, src_box_h,
                                     dst->virt_addr, dst->width, dst->height,
                                     dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else if (src->format == IMAGE_FORMAT_YUV420SP_NV12 || src->format == IMAGE_FORMAT_YUV420SP_NV21) {
        ret = crop_and_scale_image_yuv420sp(src->virt_addr, src->width, src->height,
                                            src_box_x, src_box_y, src_box_w, src_box_h,
                                            dst->virt_addr, dst->width, dst->height,
                                            dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else {
        printf("ERROR: No support for format %d in convert_image_cpu.\n", src->format);
        ret = -1; // Indicate error
    }
    if (ret != 0) {
        printf("ERROR: crop_and_scale_image_c/yuv420sp fail with code %d\n", ret);
        return -1;
    }
    printf("DEBUG: CPU image conversion finished.\n");
    return 0;
}

int get_image_size(const image_buffer_t* image)
{
    if (image == NULL) {
        return 0;
    }
    switch (image->format)
    {
    case IMAGE_FORMAT_GRAY8:
        return image->width * image->height;
    case IMAGE_FORMAT_RGB888:
        return image->width * image->height * 3;
    case IMAGE_FORMAT_RGBA8888:
        return image->width * image->height * 4;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        return image->width * image->height * 3 / 2;
    default:
        printf("WARNING: Unknown image format %d, cannot determine size.\n", image->format);
        return 0; // Return 0 or -1 for unknown format
    }
}

static int get_rga_fmt(image_format_t fmt) {
    switch (fmt)
    {
    case IMAGE_FORMAT_RGB888:
        return RK_FORMAT_RGB_888;
    case IMAGE_FORMAT_RGBA8888:
        return RK_FORMAT_RGBA_8888;
    case IMAGE_FORMAT_YUV420SP_NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case IMAGE_FORMAT_YUV420SP_NV21:
        return RK_FORMAT_YCrCb_420_SP;
    default:
        printf("ERROR: Unsupported image format %d for RGA.\n", fmt);
        return -1;
    }
}


static int convert_image_rga(image_buffer_t* src_img, image_buffer_t* dst_img, image_rect_t* src_box, image_rect_t* dst_box, char color)
{
    int ret = 0; // Default to success for RGA processing

    int srcWidth = src_img->width;
    int srcHeight = src_img->height;
    void *src = src_img->virt_addr;
    int src_fd = src_img->fd;
    void *src_phy = NULL; // Assuming this is not used or handled externally
    int srcFmt = get_rga_fmt(src_img->format);
    if (srcFmt == -1) goto err;

    int dstWidth = dst_img->width;
    int dstHeight = dst_img->height;
    void *dst = dst_img->virt_addr;
    int dst_fd = dst_img->fd;
    void *dst_phy = NULL; // Assuming this is not used or handled externally
    int dstFmt = get_rga_fmt(dst_img->format);
    if (dstFmt == -1) goto err;


    int rotate = 0; // Default rotate
    int use_handle = 0;
#if defined(LIBRGA_IM2D_HANDLE)
    use_handle = 1;
#endif

    // printf("src width=%d height=%d fmt=0x%x virAddr=0x%p fd=%d\n",
    //      srcWidth, srcHeight, srcFmt, src, src_fd);
    // printf("dst width=%d height=%d fmt=0x%x virAddr=0x%p fd=%d\n",
    //      dstWidth, dstHeight, dstFmt, dst, dst_fd);
    // printf("rotate=%d\n", rotate);

    int usage = 0;
    IM_STATUS ret_rga = IM_STATUS_NOERROR;

    // set rga usage
    usage |= rotate; // Only rotate flag used

    // set rga rect
    im_rect srect;
    im_rect drect;
    im_rect prect; // Pad rect, usually 0 for scaling ops
    memset(&prect, 0, sizeof(im_rect)); // Initialize prect

    if (src_box != NULL) {
        srect.x = src_box->left;
        srect.y = src_box->top;
        srect.width = src_box->right - src_box->left + 1;
        srect.height = src_box->bottom - src_box->top + 1;
    } else {
        srect.x = 0;
        srect.y = 0;
        srect.width = srcWidth;
        srect.height = srcHeight;
    }

    if (dst_box != NULL) {
        drect.x = dst_box->left;
        drect.y = dst_box->top;
        drect.width = dst_box->right - dst_box->left + 1;
        drect.height = dst_box->bottom - dst_box->top + 1;
    } else {
        drect.x = 0;
        drect.y = 0;
        drect.width = dstWidth;
        drect.height = dstHeight;
    }

    // set rga buffer
    rga_buffer_t rga_buf_src;
    rga_buffer_t rga_buf_dst;
    rga_buffer_t pat; // Pattern buffer, not used for simple scale/crop
    memset(&pat, 0, sizeof(rga_buffer_t)); // Initialize pat

    im_handle_param_t in_param;
    in_param.width = srcWidth;
    in_param.height = srcHeight;
    in_param.format = srcFmt;

    im_handle_param_t dst_param;
    dst_param.width = dstWidth;
    dst_param.height = dstHeight;
    dst_param.format = dstFmt;

    rga_buffer_handle_t rga_handle_src = 0; // Initialize
    rga_buffer_handle_t rga_handle_dst = 0; // Initialize

    if (use_handle) {
        if (src_phy != NULL) {
            rga_handle_src = importbuffer_physicaladdr((uint64_t)src_phy, &in_param);
        } else if (src_fd > 0) {
            rga_handle_src = importbuffer_fd(src_fd, &in_param);
        } else {
            rga_handle_src = importbuffer_virtualaddr(src, &in_param);
        }
        if (rga_handle_src <= 0) {
            printf("ERROR: src handle error %d\n", rga_handle_src);
            ret = -1;
            goto err;
        }
        rga_buf_src = wrapbuffer_handle(rga_handle_src, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
    } else {
        if (src_phy != NULL) {
            rga_buf_src = wrapbuffer_physicaladdr(src_phy, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
        } else if (src_fd > 0) {
            rga_buf_src = wrapbuffer_fd(src_fd, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
        } else {
            rga_buf_src = wrapbuffer_virtualaddr(src, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
        }
    }

    if (use_handle) {
        if (dst_phy != NULL) {
            rga_handle_dst = importbuffer_physicaladdr((uint64_t)dst_phy, &dst_param);
        } else if (dst_fd > 0) {
            rga_handle_dst = importbuffer_fd(dst_fd, &dst_param);
        } else {
            rga_handle_dst = importbuffer_virtualaddr(dst, &dst_param);
        }
        if (rga_handle_dst <= 0) {
            printf("ERROR: dst handle error %d\n", rga_handle_dst);
            ret = -1;
            goto err;
        }
        rga_buf_dst = wrapbuffer_handle(rga_handle_dst, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
    } else {
        if (dst_phy != NULL) {
            rga_buf_dst = wrapbuffer_physicaladdr(dst_phy, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
        } else if (dst_fd > 0) {
            rga_buf_dst = wrapbuffer_fd(dst_fd, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
        } else {
            rga_buf_dst = wrapbuffer_virtualaddr(dst, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
        }
    }

    // Fill pad color if destination box is smaller than the whole destination image
    if (drect.width != dstWidth || drect.height != dstHeight || drect.x != 0 || drect.y != 0) {
        im_rect dst_whole_rect = {0, 0, dstWidth, dstHeight};
        int imcolor = (color << 24) | (color << 16) | (color << 8) | color; // Assuming ARGB for RGA fill color
        printf("DEBUG: Filling dst image (x=%d y=%d w=%d h=%d) with color=0x%x\n",
            dst_whole_rect.x, dst_whole_rect.y, dst_whole_rect.width, dst_whole_rect.height, imcolor);
        ret_rga = imfill(rga_buf_dst, dst_whole_rect, imcolor);
        if (ret_rga <= 0) {
            if (dst != NULL) {
                size_t dst_size = get_image_size(dst_img);
                memset(dst, color, dst_size); // Fallback to CPU memset if RGA fill fails
                printf("WARNING: RGA imfill failed, fallback to CPU memset for padding.\n");
            } else {
                printf("WARNING: Can not fill color on target image (dst is NULL).\n");
            }
        }
    }

    // RGA process
    ret_rga = improcess(rga_buf_src, rga_buf_dst, pat, srect, drect, prect, usage);
    if (ret_rga <= 0) {
        printf("ERROR: RGA improcess failed. STATUS=%d, message: %s\n", ret_rga, imStrError((IM_STATUS)ret_rga));
        ret = -1;
    } else {
        printf("DEBUG: RGA improcess finished successfully.\n");
    }

err: // Cleanup for convert_image_rga
    if (rga_handle_src > 0) {
        releasebuffer_handle(rga_handle_src);
    }
    if (rga_handle_dst > 0) {
        releasebuffer_handle(rga_handle_dst);
    }
    return ret;
}

int convert_image(image_buffer_t* src_img, image_buffer_t* dst_img, image_rect_t* src_box, image_rect_t* dst_box, char color)
{
    int ret;
#if defined(DISABLE_RGA)
    printf("DEBUG: convert_image using CPU path (RGA disabled).\n");
    ret = convert_image_cpu(src_img, dst_img, src_box, dst_box, color);
#else // RGA is enabled
    // RGA width alignment check
#if defined(RV1106_1103)
    // RV1106/1103 might have a 4-pixel alignment requirement
    if(src_img->width % 4 == 0 && dst_img->width % 4 == 0 &&
       get_rga_fmt(src_img->format) != -1 && get_rga_fmt(dst_img->format) != -1) {
#else
    // Other platforms might have a 16-pixel alignment requirement
    if(src_img->width % 16 == 0 && dst_img->width % 16 == 0 &&
       get_rga_fmt(src_img->format) != -1 && get_rga_fmt(dst_img->format) != -1) {
#endif
        printf("DEBUG: Attempting convert_image using RGA.\n");
        ret = convert_image_rga(src_img, dst_img, src_box, dst_box, color);
        if (ret != 0) {
            printf("WARNING: RGA conversion failed (%d), falling back to CPU.\n", ret);
            ret = convert_image_cpu(src_img, dst_img, src_box, dst_box, color);
        }
    } else {
#if defined(RV1106_1103)
        printf("DEBUG: Source/Destination width not 4-aligned or unsupported format for RGA, falling back to CPU.\n");
#else
        printf("DEBUG: Source/Destination width not 16-aligned or unsupported format for RGA, falling back to CPU.\n");
#endif
        ret = convert_image_cpu(src_img, dst_img, src_box, dst_box, color);
    }
#endif
    return ret;
}

int convert_image_with_letterbox(image_buffer_t* src_image, image_buffer_t* dst_image, letterbox_t* letterbox, char color)
{
    int ret = 0;
    int allow_slight_change = 1;
    int src_w = src_image->width;
    int src_h = src_image->height;
    int dst_w = dst_image->width;
    int dst_h = dst_image->height;
    int resize_w = dst_w;
    int resize_h = dst_h;

    int padding_w = 0;
    int padding_h = 0;

    int _left_offset = 0;
    int _top_offset = 0;
    float scale = 1.0f; // Use float literal

    image_rect_t src_box;
    src_box.left = 0;
    src_box.top = 0;
    src_box.right = src_image->width - 1;
    src_box.bottom = src_image->height - 1;

    image_rect_t dst_box;
    dst_box.left = 0;
    dst_box.top = 0;
    dst_box.right = dst_image->width - 1;
    dst_box.bottom = dst_image->height - 1;

    float _scale_w = (float)dst_w / src_w;
    float _scale_h = (float)dst_h / src_h;
    if(_scale_w < _scale_h) {
        scale = _scale_w;
        resize_h = (int) (src_h * scale); // Explicit cast
    } else {
        scale = _scale_h;
        resize_w = (int) (src_w * scale); // Explicit cast
    }

    // slight change image size for align
    // Ensure resize_w is multiple of 4 for common alignment
    if (allow_slight_change == 1 && (resize_w % 4 != 0)) {
        resize_w -= resize_w % 4;
        if (resize_w < 4 && dst_w >= 4) resize_w = 4; // Ensure minimum size if clamping too much
    }
    // Ensure resize_h is multiple of 2 for common alignment (YUV)
    if (allow_slight_change == 1 && (resize_h % 2 != 0)) {
        resize_h -= resize_h % 2;
        if (resize_h < 2 && dst_h >= 2) resize_h = 2; // Ensure minimum size if clamping too much
    }

    // Recalculate padding based on potentially adjusted resize_w/h
    padding_h = dst_h - resize_h;
    padding_w = dst_w - resize_w;

    // center
    if (_scale_w < _scale_h) { // Height is constrained, pad width
        dst_box.top = padding_h / 2;
        // Ensure top offset is even for YUV formats
        if (dst_box.top % 2 != 0) {
            dst_box.top -= 1; // Round down to nearest even
            if (dst_box.top < 0) dst_box.top = 0; // Don't go below 0
        }
        dst_box.bottom = dst_box.top + resize_h - 1;
        _top_offset = dst_box.top;
        dst_box.left = (dst_w - resize_w) / 2; // Center horizontally
        if (dst_box.left % 2 != 0) { // Ensure left offset is even
            dst_box.left -= 1;
            if (dst_box.left < 0) dst_box.left = 0;
        }
        dst_box.right = dst_box.left + resize_w - 1;
        _left_offset = dst_box.left;

    } else { // Width is constrained, pad height
        dst_box.left = padding_w / 2;
        // Ensure left offset is even
        if (dst_box.left % 2 != 0) {
            dst_box.left -= 1; // Round down to nearest even
            if (dst_box.left < 0) dst_box.left = 0; // Don't go below 0
        }
        dst_box.right = dst_box.left + resize_w - 1;
        _left_offset = dst_box.left;
        dst_box.top = (dst_h - resize_h) / 2; // Center vertically
        if (dst_box.top % 2 != 0) { // Ensure top offset is even
            dst_box.top -= 1;
            if (dst_box.top < 0) dst_box.top = 0;
        }
        dst_box.bottom = dst_box.top + resize_h - 1;
        _top_offset = dst_box.top;
    }

    printf("scale=%f dst_box=(%d %d %d %d) allow_slight_change=%d _left_offset=%d _top_offset=%d padding_w=%d padding_h=%d\n",
        scale, dst_box.left, dst_box.top, dst_box.right, dst_box.bottom, allow_slight_change,
        _left_offset, _top_offset, padding_w, padding_h);

    //set offset and scale
    if(letterbox != NULL){
        letterbox->scale = scale;
        letterbox->x_pad = _left_offset;
        letterbox->y_pad = _top_offset;
    }
    // alloc memory buffer for dst image,
    // remember to free
    if (dst_image->virt_addr == NULL && dst_image->fd <= 0) {
        int dst_size = get_image_size(dst_image);
        if (dst_size == 0) {
            printf("ERROR: Cannot determine destination image size for allocation.\n");
            return -1;
        }
        dst_image->virt_addr = (uint8_t *)malloc(dst_size);
        if (dst_image->virt_addr == NULL) {
            printf("ERROR: malloc size %d error for dst_image->virt_addr\n", dst_size);
            return -1;
        }
        dst_image->size = dst_size; // Set allocated size
    } else {
        // If caller provided buffer, verify its size.
        int required_size = get_image_size(dst_image);
        if (dst_image->size < required_size) {
            printf("ERROR: Provided dst_image buffer (size %d) is smaller than required (%d).\n", dst_image->size, required_size);
            return -1;
        }
    }
    ret = convert_image(src_image, dst_image, &src_box, &dst_box, color);
    return ret;
}
