/*
    Copyright 2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef GLFRAMESTREAMER_H
#define GLFRAMESTREAMER_H

#ifdef HAVE_GSTREAMER

#include <string>
#include <atomic>
#include <memory>
#include <vector>

#include <cstdint>

#include "glad/glad.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

enum class StreamingEncoder : int
{
    Auto = 0,
    VAAPI = 1,
    VAAPI_LowPower = 2,
    x264 = 3,
    OpenH264 = 4,
};

enum class StreamingScreen : int
{
    Top = 0,
    Bottom = 1,
};

class GLFrameStreamer
{
public:
    GLFrameStreamer();
    ~GLFrameStreamer();

    bool IsSupported();
    bool IsActive() const { return active; }

    void Start(const std::string& target_ip, uint16_t target_port,
               StreamingEncoder encoder, const std::string& gpu_device,
               bool custom_resolution, uint32_t width, uint32_t height,
               StreamingScreen screen, uint32_t bitrate);
    void Stop();

    bool PushFrame(void* top_buffer, void* bottom_buffer, bool use_opengl_renderer);

    uint32_t GetWidth() const { return width; }
    uint32_t GetHeight() const { return height; }

private:
    void InitGstPipeline(const std::string& target_ip, uint16_t target_port,
                         StreamingEncoder encoder, const std::string& gpu_device,
                         uint32_t bitrate);
    void CleanupGstPipeline();
    std::string GetEncoderDesc(StreamingEncoder encoder, const std::string& gpu_device,
                               uint32_t bitrate);
    bool IsVAAPIEncoder(StreamingEncoder encoder);

    bool active = false;
    bool customResolution = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitrate = 4000;
    StreamingScreen screen = StreamingScreen::Top;

    // Track actual allocated sizes for dynamic resize when custom resolution is off
    uint32_t allocWidth = 0;
    uint32_t allocHeight = 0;

    GLuint fbo = 0;
    GLuint tex = 0;

    static constexpr int NUM_PBO_BUFFERS = 2;
    GLuint pbo[NUM_PBO_BUFFERS] = {};
    GLsync pboFence[NUM_PBO_BUFFERS] = {};
    int currentPBO = 0;
    bool pboPrimed = false;

    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstClockTime frameTimestamp = 0;
};

#endif // HAVE_GSTREAMER
#endif // GLFRAMESTREAMER_H
