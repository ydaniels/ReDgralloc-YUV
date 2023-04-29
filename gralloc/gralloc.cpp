/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cutils/ashmem.h>
#include <cutils/atomic.h>
#include <log/log.h>

#include <hardware/gralloc.h>
#include <hardware/hardware.h>

#include "gralloc_priv.h"
#include "gr.h"

/*****************************************************************************/

struct gralloc_context_t {
    alloc_device_t  device;
    /* our private data here */
};

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle);

/*****************************************************************************/

int fb_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

static int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

extern int gralloc_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr);

extern int gralloc_lock_ycbcr(gralloc_module_t const* module,
                              buffer_handle_t handle, int usage,
                              int l, int t, int w, int h,
                              android_ycbcr *ycbcr) ;



extern int gralloc_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle);

extern int gralloc_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

extern int gralloc_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

/*****************************************************************************/

static struct hw_module_methods_t gralloc_module_methods = {
        .open = gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .version_major = GRALLOC_MODULE_API_VERSION_0_2,
            .version_minor = 0,
            .id = GRALLOC_HARDWARE_MODULE_ID,
            .name = "Graphics Memory Allocator Module",
            .author = "The Android Open Source Project",
            .methods = &gralloc_module_methods
        },
        .registerBuffer = gralloc_register_buffer,
        .unregisterBuffer = gralloc_unregister_buffer,
        .lock = gralloc_lock,
        .unlock = gralloc_unlock,
        .perform = NULL,
        .lock_ycbcr = gralloc_lock_ycbcr,

    },
    .framebuffer = 0,
    .flags = 0,
    .numBuffers = 0,
    .bufferMask = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .currentBuffer = 0,
};

/*****************************************************************************/

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int /*usage*/, buffer_handle_t* pHandle)
{

    int err = 0;
    int fd = -1;

    size = roundUpToPageSize(size);
    
    fd = ashmem_create_region("gralloc-buffer", size);
    if (fd < 0) {
        ALOGE(" gralloc couldn't create ashmem (%s)", strerror(-errno));
        err = -errno;
    }
    // reference format when creating new buffer
    if (err == 0) {
        private_handle_t* hnd = new private_handle_t(fd, size, 0);
//         hnd->setYUVData( format, reqFormat, width, height);
        gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>(
                dev->common.module);
        err = mapBuffer(module, hnd);
        if (err == 0) {
            *pHandle = hnd;
        }
    }
    
    ALOGE_IF(err, "gralloc failed err=%s", strerror(-err));
    
    return err;
}

/*****************************************************************************/

inline size_t align(size_t value, size_t alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}




static int gralloc_alloc(alloc_device_t* dev,
        int width, int height, int format, int usage,
        buffer_handle_t* pHandle, int* pStride)
{
    if (!pHandle || !pStride)
        return -EINVAL;

    int bytesPerPixel = 0;
    int pixelAlign = 0;
    int frameworkFormat = format;

    ALOGE("[Gralloc]: Requested  "
          "specific format for this usage: %d x %d, format %d usage %x",
          width, height, format, usage);

    if (format == HAL_PIXEL_FORMAT_YCbCr_420_888) {

        if (usage & GRALLOC_USAGE_HW_CAMERA_WRITE) {

            ALOGE("[Gralloc]: Swapping Requested frame  "
                  "specific format for this usage: %d x %d, format %d usage %x",
                  width, height, format, usage);

            format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
        }
        if (format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
            ALOGE("[Gralloc]: Requested YCbCr_420_888, but no known "
                  "specific format for this usage: %d x %d, usage %x",
                  width, height, usage);
        }
    }

    bool yuv_format = false;
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_FP16:
            bytesPerPixel = 8;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            bytesPerPixel = 4;
            ALOGE("[Gralloc]: setting bpp to 4  ");
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            bytesPerPixel = 3;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW16:
        case HAL_PIXEL_FORMAT_YV12:
            bytesPerPixel = 2;
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            bytesPerPixel = 1; // per-channel bpp
            pixelAlign = 1;
            yuv_format = true;
            // Not expecting to actually create any GL surfaces for this
            ALOGE("[Gralloc]: setting bpp to 1  ");
            break;
        default:
            return -EINVAL;
    }

    const size_t tileWidth = 2;
    const size_t tileHeight = 2;
    size_t size;
    size_t stride;
   // ALOGE("[Gralloc]: running yuv %d ", static_cast<int>(yuv_format));
    if (yuv_format) {
        size_t yStride = (width * bytesPerPixel + (pixelAlign - 1)) & ~(pixelAlign-1);
        size_t uvStride = (yStride / 2 + (pixelAlign - 1)) & ~(pixelAlign-1);
        size_t uvHeight = height / 2;
         size = yStride * height + 2 * (uvHeight * uvStride);
          stride = yStride / bytesPerPixel;
     //   ALOGE("[Gralloc]: Calculated yuv size and stride "
       //      "specific format for this usage: %d x %d, usage %x size  %zu and stride  %zu",
          //    width, height, usage, size, stride);
    } else  {
      //  ALOGE("[Gralloc]: aligning yuv stride and size");
        stride = align(width, tileWidth);
        size = align(height, tileHeight) * stride * bytesPerPixel + 4;

    }
   // ALOGE("[Gralloc]: will now allocated ");
    int err = gralloc_alloc_buffer(dev, size, usage, pHandle);
   // ALOGE("[Gralloc]: allocated gralloc %d ", err);
    if (err < 0) {
        ALOGE("[Gralloc]: scannot gralloc allocate buffer ");
        return err;
    }
    if (frameworkFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) {
     //   ALOGE("[Gralloc]: setting stride to 0 as require by framework "
       //       "specific format for this usage: %d x %d, usage %x size  %zu and stride  %zu",
         //     width, height, usage, size, stride);
        *pStride = 0;
    } else {
       // ALOGE("[Gralloc]: set stride ");
        *pStride = stride;
    }

    return 0;
}

static int gralloc_free(alloc_device_t* dev,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(handle);
    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
        // free this buffer
        private_module_t* m = reinterpret_cast<private_module_t*>(
                dev->common.module);
        const size_t bufferSize = m->finfo.line_length * m->info.yres;
        int index = (hnd->base - m->framebuffer->base) / bufferSize;
        m->bufferMask &= ~(1<<index); 
    } else { 
        gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>(
                dev->common.module);
        terminateBuffer(module, const_cast<private_handle_t*>(hnd));
    }

    close(hnd->fd);
    delete hnd;
    return 0;
}

/*****************************************************************************/

static int gralloc_close(struct hw_device_t *dev)
{
    gralloc_context_t* ctx = reinterpret_cast<gralloc_context_t*>(dev);
    if (ctx) {
        /* TODO: keep a list of all buffer_handle_t created, and free them
         * all here.
         */
        free(ctx);
    }
    return 0;
}

int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        gralloc_context_t *dev;
        dev = (gralloc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = gralloc_close;

        dev->device.alloc   = gralloc_alloc;
        dev->device.free    = gralloc_free;

        *device = &dev->device.common;
        status = 0;
    } else {
        status = fb_device_open(module, name, device);
    }
    return status;
}
