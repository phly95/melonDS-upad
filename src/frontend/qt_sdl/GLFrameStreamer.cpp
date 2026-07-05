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

#ifdef HAVE_GSTREAMER

#include "GLFrameStreamer.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "Platform.h"

GLFrameStreamer::GLFrameStreamer() = default;

GLFrameStreamer::~GLFrameStreamer()
{
    Stop();
}

bool GLFrameStreamer::IsSupported()
{
    if (!gst_is_initialized())
    {
        GError* error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &error))
        {
            if (error) g_error_free(error);
            return false;
        }
    }
    return true;
}

std::string GLFrameStreamer::GetEncoderDesc(StreamingEncoder enc, const std::string& gpu_device,
                                           uint32_t enc_bitrate)
{
    switch (enc)
    {
    case StreamingEncoder::VAAPI:
        return "vaapih264enc rate-control=cqp init-qp=22 qp-ip=1";
    case StreamingEncoder::VAAPI_LowPower:
        return "vah264lpenc rate-control=cqp init-qp=22 qp-ip=1";
    case StreamingEncoder::x264:
        return "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(enc_bitrate);
    case StreamingEncoder::OpenH264:
        return "openh264enc complexity=low bitrate=" + std::to_string(enc_bitrate);
    case StreamingEncoder::Auto:
    default:
    {
        // Try modern VA encoder first, then legacy, then software fallback
        GstElement* test = gst_element_factory_make("vah264lpenc", nullptr);
        if (test) { gst_object_unref(test); return "vah264lpenc rate-control=cqp init-qp=22 qp-ip=1"; }
        test = gst_element_factory_make("vaapih264enc", nullptr);
        if (test) { gst_object_unref(test); return "vaapih264enc rate-control=cqp init-qp=22 qp-ip=1"; }
        return "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(enc_bitrate);
    }
    }
}

bool GLFrameStreamer::IsVAAPIEncoder(StreamingEncoder enc)
{
    return enc == StreamingEncoder::VAAPI || enc == StreamingEncoder::VAAPI_LowPower;
}

bool GLFrameStreamer::InitGstPipeline(uint32_t pipeline_width, uint32_t pipeline_height)
{
    CleanupGstPipeline();

    if (!gpuDevice.empty() && !gst_is_initialized())
        g_setenv("GST_VAAPI_DRM_DEVICE", gpuDevice.c_str(), TRUE);

    std::string enc_desc = GetEncoderDesc(encoder, gpuDevice, bitrate);

    bool isLegacyVAAPI = enc_desc.find("vaapi") != std::string::npos;
    bool isNewVAAPI = enc_desc.find("vah264") != std::string::npos;
    bool isVAAPI = isLegacyVAAPI || isNewVAAPI;

    // Build pipeline with explicit caps throughout
    std::string pipeline_desc =
        "appsrc name=src is-live=true format=3 "
        "! videoconvert";

    if (isVAAPI)
    {
        pipeline_desc +=
            " ! video/x-raw,format=NV12"
            ",width=" + std::to_string(pipeline_width) +
            ",height=" + std::to_string(pipeline_height) +
            ",framerate=60/1";
    }

    pipeline_desc +=
        " ! " + enc_desc +
        " ! h264parse"
        " ! rtph264pay config-interval=1 pt=96"
        " ! udpsink host=" + targetIP +
        " port=" + std::to_string(targetPort);

    melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Pipeline: %s", pipeline_desc.c_str());

    GError* error = nullptr;
    pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
    if (error)
    {
        melonDS::Platform::Log(melonDS::Platform::Error, "GLFrameStreamer: Pipeline parse error: %s", error->message);
        g_error_free(error);
        pipeline = nullptr;
        return false;
    }

    appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (!appsrc)
    {
        melonDS::Platform::Log(melonDS::Platform::Error, "GLFrameStreamer: Could not find appsrc.");
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return false;
    }

    // Set explicit appsrc caps with framerate
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "RGBA",
        "width", G_TYPE_INT, static_cast<int>(pipeline_width),
        "height", G_TYPE_INT, static_cast<int>(pipeline_height),
        "framerate", GST_TYPE_FRACTION, 60, 1,
        nullptr);
    g_object_set(appsrc, "caps", caps, nullptr);
    gst_caps_unref(caps);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        melonDS::Platform::Log(melonDS::Platform::Error, "GLFrameStreamer: Failed to start pipeline.");
        gst_object_unref(appsrc);
        appsrc = nullptr;
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return false;
    }

    frameTimestamp = 0;
    pipelineStarted = true;
    melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Pipeline started (%ux%u).", pipeline_width, pipeline_height);
    return true;
}

void GLFrameStreamer::CleanupGstPipeline()
{
    if (pipeline)
        gst_element_set_state(pipeline, GST_STATE_NULL);
    if (appsrc) { gst_object_unref(appsrc); appsrc = nullptr; }
    if (pipeline) { gst_object_unref(pipeline); pipeline = nullptr; }
    pipelineStarted = false;
}

void GLFrameStreamer::Start(const std::string& ip, uint16_t port,
                            StreamingEncoder enc, const std::string& gpu,
                            bool custom_resolution, uint32_t stream_width, uint32_t stream_height,
                            StreamingScreen stream_screen, uint32_t stream_bitrate)
{
    if (active) Stop();

    screen = stream_screen;
    customResolution = custom_resolution;
    bitrate = stream_bitrate;
    targetIP = ip;
    targetPort = port;
    encoder = enc;
    gpuDevice = gpu;

    if (custom_resolution)
    {
        width = stream_width;
        height = stream_height;
    }
    else
    {
        // Will be determined from source texture on first PushFrame
        width = 256;
        height = 192;
    }

    glGenFramebuffers(1, &fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenBuffers(NUM_PBO_BUFFERS, pbo);
    for (int i = 0; i < NUM_PBO_BUFFERS; i++)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    pboPrimed = false;
    currentPBO = 0;

    // If custom resolution is known, start pipeline immediately.
    // Otherwise, defer until first PushFrame when we know the real size.
    if (custom_resolution)
    {
        active = InitGstPipeline(width, height);
    }
    else
    {
        active = true;
        pipelineStarted = false;
    }

    if (active)
        melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Streaming to %s:%d (%s %ux%u)",
                     ip.c_str(), port,
                     custom_resolution ? "custom" : "auto",
                     width, height);
}

void GLFrameStreamer::Stop()
{
    if (!active) return;
    CleanupGstPipeline();
    for (int i = 0; i < NUM_PBO_BUFFERS; i++)
    {
        if (pboFence[i]) { glDeleteSync(pboFence[i]); pboFence[i] = nullptr; }
    }
    if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    if (tex) { glDeleteTextures(1, &tex); tex = 0; }
    if (pbo[0] || pbo[1]) { glDeleteBuffers(NUM_PBO_BUFFERS, pbo); pbo[0] = pbo[1] = 0; }
    active = false;
    pboPrimed = false;
}

bool GLFrameStreamer::PushFrame(void* top_buffer, void* bottom_buffer, bool use_opengl_renderer)
{
    if (!active)
        return false;

    void* src_buffer = use_opengl_renderer ? top_buffer :
                       ((screen == StreamingScreen::Top) ? top_buffer : bottom_buffer);
    if (!src_buffer)
        return false;

    if (use_opengl_renderer)
    {
        GLuint srcTexId = *(GLuint*)src_buffer;
        GLint layer = (screen == StreamingScreen::Top) ? 0 : 1;

        if (!glIsTexture(srcTexId))
        {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "GLFrameStreamer: srcTexId %u is not a valid texture", srcTexId);
            return false;
        }

        GLint srcWidth = 256, srcHeight = 192;
        glBindTexture(GL_TEXTURE_2D_ARRAY, srcTexId);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &srcWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &srcHeight);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        // If custom resolution is off and we haven't started the pipeline yet, do it now
        if (!customResolution && !pipelineStarted)
        {
            width = srcWidth;
            height = srcHeight;

            // Resize GL resources to actual size
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);

            for (int i = 0; i < NUM_PBO_BUFFERS; i++)
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
                glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 4, nullptr, GL_STREAM_READ);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            pboPrimed = false;

            // Start pipeline with correct resolution
            active = InitGstPipeline(width, height);
            if (!active)
                return false;
        }
        // If custom resolution is off and resolution changed, restart pipeline
        else if (!customResolution && pipelineStarted &&
                 (static_cast<uint32_t>(srcWidth) != width || static_cast<uint32_t>(srcHeight) != height))
        {
            melonDS::Platform::Log(melonDS::Platform::Info,
                "GLFrameStreamer: Resolution changed %ux%u -> %dx%d, restarting pipeline",
                width, height, srcWidth, srcHeight);

            width = srcWidth;
            height = srcHeight;

            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);

            for (int i = 0; i < NUM_PBO_BUFFERS; i++)
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
                glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 4, nullptr, GL_STREAM_READ);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            pboPrimed = false;

            active = InitGstPipeline(width, height);
            if (!active)
                return false;
        }

        // Read frame from source texture
        if (static_cast<uint32_t>(srcWidth) == width && static_cast<uint32_t>(srcHeight) == height)
        {
            // Direct read - source matches stream size
            GLuint srcFbo;
            glGenFramebuffers(1, &srcFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, srcTexId, 0, layer);
            glReadBuffer(GL_COLOR_ATTACHMENT0);

            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[currentPBO]);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            glDeleteFramebuffers(1, &srcFbo);
        }
        else
        {
            // Blit source to target texture at stream resolution (custom resolution mode)
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);

            GLuint srcFbo;
            glGenFramebuffers(1, &srcFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, srcTexId, 0, layer);
            glReadBuffer(GL_COLOR_ATTACHMENT0);

            glBlitFramebuffer(0, 0, srcWidth, srcHeight, 0, 0, width, height,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);

            glDeleteFramebuffers(1, &srcFbo);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);

            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[currentPBO]);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        // Insert fence to track when this PBO readback completes
        if (pboFence[currentPBO]) glDeleteSync(pboFence[currentPBO]);
        pboFence[currentPBO] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    else
    {
        // Software renderer: src_buffer is CPU-side BGRA 256x192 data.
        if (!pipelineStarted)
        {
            active = InitGstPipeline(width, height);
            if (!active)
                return false;
        }

        GLuint srcTex;
        glGenTextures(1, &srcTex);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, src_buffer);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);

        GLuint srcFbo;
        glGenFramebuffers(1, &srcFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        glBlitFramebuffer(0, 0, 256, 192, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);

        glDeleteFramebuffers(1, &srcFbo);
        glDeleteTextures(1, &srcTex);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[currentPBO]);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        if (pboFence[currentPBO]) glDeleteSync(pboFence[currentPBO]);
        pboFence[currentPBO] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    // Poll bus for async errors
    if (pipeline)
    {
        GstBus* bus = gst_element_get_bus(pipeline);
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg)
        {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            melonDS::Platform::Log(melonDS::Platform::Error,
                "GLFrameStreamer: Pipeline error from %s: %s%s%s",
                GST_OBJECT_NAME(msg->src), err->message,
                dbg ? "\n" : "", dbg ? dbg : "");
            g_error_free(err);
            g_free(dbg);
            gst_message_unref(msg);
            gst_object_unref(bus);
            Stop();
            return false;
        }
        gst_object_unref(bus);
    }

    // Map previous frame's PBO and push to GStreamer
    if (pboPrimed && pipelineStarted)
    {
        int readPBO = (currentPBO + 1) % NUM_PBO_BUFFERS;

        // Wait for this PBO's readback to complete on the GPU
        if (pboFence[readPBO])
        {
            glClientWaitSync(pboFence[readPBO], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(pboFence[readPBO]);
            pboFence[readPBO] = nullptr;
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[readPBO]);
        GLubyte* pixels = (GLubyte*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                            width * height * 4, GL_MAP_READ_BIT);
        if (!pixels)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            currentPBO = (currentPBO + 1) % NUM_PBO_BUFFERS;
            return false;
        }

        std::vector<uint8_t> frame(width * height * 4);
        memcpy(frame.data(), pixels, width * height * 4);

        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        GstBuffer* buf = gst_buffer_new_allocate(nullptr, width * height * 4, nullptr);
        if (!buf)
        {
            currentPBO = (currentPBO + 1) % NUM_PBO_BUFFERS;
            return false;
        }

        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_WRITE);
        memcpy(map.data, frame.data(), width * height * 4);
        gst_buffer_unmap(buf, &map);

        GST_BUFFER_PTS(buf) = frameTimestamp;
        GST_BUFFER_DTS(buf) = frameTimestamp;
        GST_BUFFER_DURATION(buf) = gst_util_uint64_scale_int(1, GST_SECOND, 60);
        frameTimestamp += GST_BUFFER_DURATION(buf);

        GstFlowReturn ret;
        g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
        gst_buffer_unref(buf);

        if (ret != GST_FLOW_OK)
        {
            melonDS::Platform::Log(melonDS::Platform::Warn,
                "GLFrameStreamer: push failed: %d", static_cast<int>(ret));
            currentPBO = (currentPBO + 1) % NUM_PBO_BUFFERS;
            return false;
        }
    }

    currentPBO = (currentPBO + 1) % NUM_PBO_BUFFERS;
    pboPrimed = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

#endif // HAVE_GSTREAMER
