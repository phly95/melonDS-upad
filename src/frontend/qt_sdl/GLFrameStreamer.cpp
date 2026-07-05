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

std::string GLFrameStreamer::GetEncoderDesc(StreamingEncoder encoder, const std::string& gpu_device,
                                           uint32_t bitrate)
{
    switch (encoder)
    {
    case StreamingEncoder::VAAPI:
        return "vaapih264enc rate-control=cqp init-qp=22 qp-ip=1";
    case StreamingEncoder::VAAPI_LowPower:
        return "vah264lpenc rate-control=cqp init-qp=22 qp-ip=1";
    case StreamingEncoder::x264:
        return "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(bitrate);
    case StreamingEncoder::OpenH264:
        return "openh264enc complexity=low bitrate=" + std::to_string(bitrate);
    case StreamingEncoder::Auto:
    default:
    {
        GstElement* test = gst_element_factory_make("vaapih264enc", nullptr);
        if (test)
        {
            gst_object_unref(test);
            return "vaapih264enc rate-control=cqp init-qp=22 qp-ip=1";
        }
        return "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(bitrate);
    }
    }
}

void GLFrameStreamer::InitGstPipeline(const std::string& target_ip, uint16_t target_port,
                                       StreamingEncoder encoder, const std::string& gpu_device,
                                       uint32_t bitrate)
{
    if (!gst_is_initialized())
    {
        if (!gpu_device.empty())
            g_setenv("GST_VAAPI_DRM_DEVICE", gpu_device.c_str(), TRUE);
        gst_init(nullptr, nullptr);
    }

    std::string enc_desc = GetEncoderDesc(encoder, gpu_device, bitrate);

    std::string pipeline_desc = "appsrc name=src is-live=true format=3 "
                                "! videoconvert ! " + enc_desc +
                                " ! h264parse "
                                "! rtph264pay config-interval=1 pt=96 "
                                "! udpsink host=" + target_ip +
                                " port=" + std::to_string(target_port);

    melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Pipeline: %s", pipeline_desc.c_str());

    GError* error = nullptr;
    pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
    if (error)
    {
        melonDS::Platform::Log(melonDS::Platform::Error, "GLFrameStreamer: Pipeline parse error: %s", error->message);
        g_error_free(error);
        pipeline = nullptr;
        return;
    }

    appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (!appsrc)
    {
        melonDS::Platform::Log(melonDS::Platform::Error, "GLFrameStreamer: Could not find appsrc.");
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return;
    }

    GstVideoInfo vinfo;
    gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, width, height);
    GstCaps* caps = gst_video_info_to_caps(&vinfo);
    g_object_set(appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        melonDS::Platform::Log(melonDS::Platform::Error, "GLFrameStreamer: Failed to start pipeline.");
        gst_object_unref(appsrc);
        appsrc = nullptr;
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return;
    }

    frameTimestamp = 0;
    melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Pipeline started.");
}

void GLFrameStreamer::CleanupGstPipeline()
{
    if (pipeline)
        gst_element_set_state(pipeline, GST_STATE_NULL);
    if (appsrc) { gst_object_unref(appsrc); appsrc = nullptr; }
    if (pipeline) { gst_object_unref(pipeline); pipeline = nullptr; }
}

void GLFrameStreamer::Start(const std::string& target_ip, uint16_t target_port,
                            StreamingEncoder encoder, const std::string& gpu_device,
                            bool custom_resolution, uint32_t stream_width, uint32_t stream_height,
                            StreamingScreen stream_screen, uint32_t stream_bitrate)
{
    if (active) Stop();

    screen = stream_screen;
    customResolution = custom_resolution;
    bitrate = stream_bitrate;

    if (custom_resolution)
    {
        width = stream_width;
        height = stream_height;
    }
    else
    {
        // Will be determined dynamically from source texture in PushFrame
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

    allocWidth = width;
    allocHeight = height;

    glGenBuffers(NUM_PBO_BUFFERS, pbo);
    for (int i = 0; i < NUM_PBO_BUFFERS; i++)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    pboPrimed = false;
    currentPBO = 0;

    InitGstPipeline(target_ip, target_port, encoder, gpu_device, bitrate);
    active = (pipeline != nullptr);

    if (active)
        melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Streaming to %s:%d (%ux%u)",
                     target_ip.c_str(), target_port, width, height);
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
    if (!active || !appsrc || !pipeline || !fbo || !tex)
        return false;

    // Poll bus for async errors
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

    void* src_buffer = use_opengl_renderer ? top_buffer :
                       ((screen == StreamingScreen::Top) ? top_buffer : bottom_buffer);
    if (!src_buffer)
        return false;

    if (use_opengl_renderer)
    {
        // OpenGL renderer: src_buffer is a pointer to a GLuint texture ID
        // for a GL_TEXTURE_2D_ARRAY: top=layer0, bottom=layer1.
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

        // When custom resolution is off, resize stream to match renderer output
        if (!customResolution && (static_cast<uint32_t>(srcWidth) != width || static_cast<uint32_t>(srcHeight) != height))
        {
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

            allocWidth = width;
            allocHeight = height;

            // Update appsrc caps to match new resolution
            GstVideoInfo vinfo;
            gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, width, height);
            GstCaps* caps = gst_video_info_to_caps(&vinfo);
            g_object_set(appsrc, "caps", caps, NULL);
            gst_caps_unref(caps);

            melonDS::Platform::Log(melonDS::Platform::Info, "GLFrameStreamer: Resized to %ux%u", width, height);
        }

        // Read directly from source texture layer into PBO (no intermediate blit)
        GLuint srcFbo;
        glGenFramebuffers(1, &srcFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, srcTexId, 0, layer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[currentPBO]);
        glReadPixels(0, 0, srcWidth, srcHeight, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        // Insert fence to track when this PBO readback completes
        if (pboFence[currentPBO]) glDeleteSync(pboFence[currentPBO]);
        pboFence[currentPBO] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        glDeleteFramebuffers(1, &srcFbo);
    }
    else
    {
        // Software renderer: src_buffer is CPU-side BGRA 256x192 data.
        // Upload to temp texture, blit to target texture at stream resolution.
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

        // Read from target texture into PBO
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[currentPBO]);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        // Insert fence to track when this PBO readback completes
        if (pboFence[currentPBO]) glDeleteSync(pboFence[currentPBO]);
        pboFence[currentPBO] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    // Map previous frame's PBO and push to GStreamer
    if (pboPrimed)
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
