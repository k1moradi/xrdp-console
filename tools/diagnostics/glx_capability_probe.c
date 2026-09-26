// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES 1

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
    OPENGL_TARGET_MAJOR = 3,
    OPENGL_TARGET_MINOR = 3,
    PROBE_WIDTH_PIXELS = 1366,
    PROBE_HEIGHT_PIXELS = 768,
    READBACK_SAMPLE_COUNT = 40,
    TIMER_QUERY_POLL_LIMIT = 1000
};

static int glx_error_code;

static int
capture_x_error(Display *display, XErrorEvent *event)
{
    (void)display;
    glx_error_code = event->error_code;
    return 0;
}

static double
monotonic_seconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return -1.0;
    }
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}

static int
version_at_least(int major, int minor, int required_major,
                 int required_minor)
{
    return major > required_major ||
           (major == required_major && minor >= required_minor);
}

static int
has_space_extension(const char *extensions, const char *wanted)
{
    if (extensions == NULL || wanted == NULL)
    {
        return 0;
    }
    const size_t wanted_length = strlen(wanted);
    const char *position = extensions;
    while ((position = strstr(position, wanted)) != NULL)
    {
        const int begins_token = position == extensions || position[-1] == ' ';
        const char after = position[wanted_length];
        if (begins_token && (after == '\0' || after == ' '))
        {
            return 1;
        }
        position += wanted_length;
    }
    return 0;
}

static int
has_gl_extension(const char *wanted, int major, int minor)
{
    if (wanted == NULL)
    {
        return 0;
    }

    if (version_at_least(major, minor, 3, 0))
    {
        PFNGLGETSTRINGIPROC get_string_i =
            (PFNGLGETSTRINGIPROC)glXGetProcAddressARB(
                (const GLubyte *)"glGetStringi");
        GLint extension_count = 0;
        if (get_string_i != NULL)
        {
            glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
            for (GLint index = 0; index < extension_count; ++index)
            {
                const char *extension =
                    (const char *)get_string_i(GL_EXTENSIONS, (GLuint)index);
                if (extension != NULL && strcmp(extension, wanted) == 0)
                {
                    return 1;
                }
            }
            return 0;
        }
    }

    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (extensions == NULL)
    {
        return 0;
    }
    const size_t wanted_length = strlen(wanted);
    const char *position = extensions;
    while ((position = strstr(position, wanted)) != NULL)
    {
        const int begins_token = position == extensions || position[-1] == ' ';
        const char after = position[wanted_length];
        if (begins_token && (after == '\0' || after == ' '))
        {
            return 1;
        }
        position += wanted_length;
    }
    return 0;
}

static GLXContext
create_profile_context(Display *display, GLXFBConfig config,
                       GLXPbuffer pbuffer, int profile_mask,
                       int requested_major, int requested_minor)
{
    PFNGLXCREATECONTEXTATTRIBSARBPROC create_context =
        (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB(
            (const GLubyte *)"glXCreateContextAttribsARB");
    if (create_context == NULL)
    {
        return NULL;
    }

    const int attributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, requested_major,
        GLX_CONTEXT_MINOR_VERSION_ARB, requested_minor,
        GLX_CONTEXT_PROFILE_MASK_ARB, profile_mask,
        None
    };
    glx_error_code = 0;
    XErrorHandler previous_handler = XSetErrorHandler(capture_x_error);
    GLXContext context = create_context(display, config, NULL, True, attributes);
    XSync(display, False);
    XSetErrorHandler(previous_handler);
    if (glx_error_code != 0)
    {
        if (context != NULL)
        {
            glXDestroyContext(display, context);
        }
        return NULL;
    }
    if (context == NULL ||
        !glXMakeContextCurrent(display, pbuffer, pbuffer, context))
    {
        if (context != NULL)
        {
            glXDestroyContext(display, context);
        }
        return NULL;
    }
    return context;
}

static int
find_highest_core_context(Display *display, GLXFBConfig config,
                          GLXPbuffer pbuffer, GLXContext baseline,
                          int *highest_major, int *highest_minor)
{
    static const int versions[][2] = {
        {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
        {3, 3}
    };

    if (highest_major == NULL || highest_minor == NULL)
    {
        return 0;
    }
    *highest_major = 0;
    *highest_minor = 0;

    for (size_t index = 0; index < sizeof(versions) / sizeof(versions[0]);
         ++index)
    {
        GLXContext candidate = create_profile_context(
            display, config, pbuffer, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            versions[index][0], versions[index][1]);
        if (candidate == NULL)
        {
            continue;
        }

        GLint reported_major = 0;
        GLint reported_minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &reported_major);
        glGetIntegerv(GL_MINOR_VERSION, &reported_minor);
        glXMakeContextCurrent(display, None, None, NULL);
        glXDestroyContext(display, candidate);
        if (reported_major >= versions[index][0] &&
            (reported_major > versions[index][0] ||
             reported_minor >= versions[index][1]))
        {
            *highest_major = reported_major;
            *highest_minor = reported_minor;
            break;
        }
    }

    return glXMakeContextCurrent(display, pbuffer, pbuffer, baseline) != 0;
}

static int
probe_timestamp_query(PFNGLGENQUERIESPROC gen_queries,
                      PFNGLDELETEQUERIESPROC delete_queries,
                      PFNGLQUERYCOUNTERPROC query_counter,
                      PFNGLGETQUERYIVPROC get_query,
                      PFNGLGETQUERYOBJECTIVPROC get_query_object_iv,
                      PFNGLGETQUERYOBJECTUI64VPROC get_query_object_ui64v,
                      GLint *counter_bits, GLuint64 *elapsed_ns,
                      const char **failure_reason)
{
    GLuint queries[2] = {0, 0};
    GLint available = GL_FALSE;
    GLuint64 start_ns = 0;
    GLuint64 end_ns = 0;

    if (gen_queries == NULL || delete_queries == NULL ||
        query_counter == NULL || get_query == NULL ||
        get_query_object_iv == NULL || get_query_object_ui64v == NULL ||
        counter_bits == NULL || elapsed_ns == NULL ||
        failure_reason == NULL)
    {
        return 0;
    }

    *failure_reason = "unknown";
    while (glGetError() != GL_NO_ERROR)
    {
    }
    get_query(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, counter_bits);
    if (glGetError() != GL_NO_ERROR)
    {
        *failure_reason = "counter_bits_query_error";
        return 0;
    }
    if (*counter_bits <= 0)
    {
        *failure_reason = "no_timestamp_counter_bits";
        return 0;
    }

    while (glGetError() != GL_NO_ERROR)
    {
    }
    gen_queries(2, queries);
    if (queries[0] == 0 || queries[1] == 0 || glGetError() != GL_NO_ERROR)
    {
        delete_queries(2, queries);
        *failure_reason = "query_object_creation_error";
        return 0;
    }

    query_counter(queries[0], GL_TIMESTAMP);
    glClear(GL_COLOR_BUFFER_BIT);
    query_counter(queries[1], GL_TIMESTAMP);
    glFlush();

    for (int attempt = 0; attempt < TIMER_QUERY_POLL_LIMIT; ++attempt)
    {
        get_query_object_iv(queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (glGetError() != GL_NO_ERROR)
        {
            delete_queries(2, queries);
            *failure_reason = "availability_poll_error";
            return 0;
        }
        if (available == GL_TRUE)
        {
            get_query_object_ui64v(queries[0], GL_QUERY_RESULT, &start_ns);
            get_query_object_ui64v(queries[1], GL_QUERY_RESULT, &end_ns);
            const GLenum query_error = glGetError();
            delete_queries(2, queries);
            if (query_error != GL_NO_ERROR || end_ns < start_ns)
            {
                *failure_reason = query_error != GL_NO_ERROR
                                      ? "query_result_error"
                                      : "timestamp_order_invalid";
                return 0;
            }
            *elapsed_ns = end_ns - start_ns;
            return 1;
        }

        const struct timespec pause = {0, 1000000L};
        (void)nanosleep(&pause, NULL);
    }

    delete_queries(2, queries);
    *failure_reason = "availability_timeout";
    return 0;
}

static int
probe_sampler_objects(PFNGLGENSAMPLERSPROC gen_samplers,
                      PFNGLDELETESAMPLERSPROC delete_samplers,
                      PFNGLSAMPLERPARAMETERIPROC sampler_parameter_i,
                      PFNGLBINDSAMPLERPROC bind_sampler)
{
    GLuint sampler = 0;
    if (gen_samplers == NULL || delete_samplers == NULL ||
        sampler_parameter_i == NULL || bind_sampler == NULL)
    {
        return 0;
    }

    while (glGetError() != GL_NO_ERROR)
    {
    }
    gen_samplers(1, &sampler);
    if (sampler == 0 || glGetError() != GL_NO_ERROR)
    {
        if (sampler != 0)
        {
            delete_samplers(1, &sampler);
        }
        return 0;
    }
    sampler_parameter_i(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    sampler_parameter_i(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    bind_sampler(0, sampler);
    bind_sampler(0, 0);
    const GLenum operation_error = glGetError();
    delete_samplers(1, &sampler);
    return operation_error == GL_NO_ERROR && glGetError() == GL_NO_ERROR;
}

static int
framebuffer_format_complete(GLint internal_format, GLenum external_format)
{
    GLuint texture = 0;
    GLuint framebuffer = 0;
    while (glGetError() != GL_NO_ERROR)
    {
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, 16, 16, 0,
                 external_format, GL_UNSIGNED_BYTE, NULL);
    if (glGetError() != GL_NO_ERROR)
    {
        glDeleteTextures(1, &texture);
        return 0;
    }

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    const int complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    return complete && glGetError() == GL_NO_ERROR;
}

static int
compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void
report_readback_format(const char *name, GLenum format, GLuint framebuffer,
                       unsigned char *pixels, size_t bytes)
{
    double samples[READBACK_SAMPLE_COUNT];
    size_t recorded = 0;
    uint64_t checksum = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    while (glGetError() != GL_NO_ERROR)
    {
    }

    for (int index = 0; index < READBACK_SAMPLE_COUNT + 1; ++index)
    {
        const double start = monotonic_seconds();
        glReadPixels(0, 0, PROBE_WIDTH_PIXELS, PROBE_HEIGHT_PIXELS,
                     format, GL_UNSIGNED_BYTE, pixels);
        const GLenum error = glGetError();
        const double finish = monotonic_seconds();
        if (start < 0.0 || finish < start || error != GL_NO_ERROR)
        {
            printf("readback_format=%s status=unsupported gl_error=0x%x\n",
                   name, (unsigned int)error);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return;
        }
        checksum += pixels[(size_t)index % bytes];
        if (index != 0)
        {
            samples[recorded++] = (finish - start) * 1e6;
        }
    }

    qsort(samples, recorded, sizeof(samples[0]), compare_double);
    double sum_us = 0.0;
    for (size_t index = 0; index < recorded; ++index)
    {
        sum_us += samples[index];
    }
    const double average_us = sum_us / (double)recorded;
    const size_t p50 = (recorded * 50U + 99U) / 100U - 1U;
    const size_t p95 = (recorded * 95U + 99U) / 100U - 1U;
    const size_t p99 = (recorded * 99U + 99U) / 100U - 1U;
    printf("readback_format=%s samples=%zu bytes=%zu avg_us=%.3f "
           "p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f "
           "effective_mib_s=%.1f checksum=%llu synchronization=glReadPixels\n",
           name, recorded, bytes, average_us, samples[p50], samples[p95],
           samples[p99], samples[recorded - 1],
           (double)bytes * 1e6 / average_us / 1048576.0,
           (unsigned long long)checksum);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void
report_samples(const char *name, double *samples, size_t count)
{
    qsort(samples, count, sizeof(samples[0]), compare_double);
    double sum = 0.0;
    for (size_t index = 0; index < count; ++index)
    {
        sum += samples[index];
    }
    const double average = sum / (double)count;
    const size_t p50 = (count * 50U + 99U) / 100U - 1U;
    const size_t p95 = (count * 95U + 99U) / 100U - 1U;
    const size_t p99 = (count * 99U + 99U) / 100U - 1U;
    printf("%s_samples=%zu %s_avg_us=%.3f %s_p50_us=%.3f "
           "%s_p95_us=%.3f %s_p99_us=%.3f %s_max_us=%.3f\n",
           name, count, name, average, name, samples[p50], name,
           samples[p95], name, samples[p99], name, samples[count - 1U]);
}

static int
complete_pbo(GLuint pbo, GLsync fence, PFNGLBINDBUFFERPROC bind_buffer,
             PFNGLCLIENTWAITSYNCPROC client_wait_sync,
             PFNGLDELETESYNCPROC delete_sync,
             PFNGLMAPBUFFERRANGEPROC map_buffer_range,
             PFNGLUNMAPBUFFERPROC unmap_buffer, size_t bytes,
             double submitted_at, int collect, double *completion_samples,
             double *wait_samples, size_t *recorded, uint64_t *checksum)
{
    const double wait_start = monotonic_seconds();
    const GLenum wait_result = client_wait_sync(
        fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
    const double wait_end = monotonic_seconds();
    if (wait_start < 0.0 || wait_end < wait_start ||
        (wait_result != GL_ALREADY_SIGNALED &&
         wait_result != GL_CONDITION_SATISFIED))
    {
        delete_sync(fence);
        return 0;
    }
    if (collect)
    {
        completion_samples[*recorded] = (wait_end - submitted_at) * 1e6;
        wait_samples[*recorded] = (wait_end - wait_start) * 1e6;
        ++*recorded;
    }

    bind_buffer(GL_PIXEL_PACK_BUFFER, pbo);
    void *mapped = map_buffer_range(GL_PIXEL_PACK_BUFFER, 0,
                                    (GLsizeiptr)bytes, GL_MAP_READ_BIT);
    if (mapped == NULL)
    {
        delete_sync(fence);
        return 0;
    }
    *checksum += ((const unsigned char *)mapped)[0];
    const GLboolean unmapped = unmap_buffer(GL_PIXEL_PACK_BUFFER);
    delete_sync(fence);
    return unmapped == GL_TRUE;
}

static int
report_pbo_readback(GLuint framebuffer, GLenum format, size_t bytes,
                    PFNGLBINDBUFFERPROC bind_buffer,
                    PFNGLBUFFERDATAPROC buffer_data,
                    PFNGLFENCESYNCPROC fence_sync,
                    PFNGLCLIENTWAITSYNCPROC client_wait_sync,
                    PFNGLDELETESYNCPROC delete_sync,
                    PFNGLMAPBUFFERRANGEPROC map_buffer_range,
                    PFNGLUNMAPBUFFERPROC unmap_buffer)
{
    enum { PBO_RING_SIZE = 3, CPU_OVERLAP_BYTES = 65536 };
    int success = 0;
    GLuint pbos[PBO_RING_SIZE] = {0};
    GLsync fences[PBO_RING_SIZE] = {NULL};
    double submitted_at[PBO_RING_SIZE] = {0.0};
    double submit_samples[READBACK_SAMPLE_COUNT];
    double completion_samples[READBACK_SAMPLE_COUNT];
    double wait_samples[READBACK_SAMPLE_COUNT];
    unsigned char cpu_source[CPU_OVERLAP_BYTES];
    unsigned char cpu_destination[CPU_OVERLAP_BYTES];
    size_t completed_samples = 0;
    uint64_t checksum = 0;
    memset(cpu_source, 0x5a, sizeof(cpu_source));
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    glGenBuffers(PBO_RING_SIZE, pbos);
    for (int index = 0; index < PBO_RING_SIZE; ++index)
    {
        bind_buffer(GL_PIXEL_PACK_BUFFER, pbos[index]);
        buffer_data(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL,
                    GL_STREAM_READ);
    }
    GLenum setup_error = glGetError();
    if (setup_error != GL_NO_ERROR)
    {
        printf("pbo_readback=status_unsupported reason=buffer_setup "
               "gl_error=0x%x\n", (unsigned int)setup_error);
        goto cleanup;
    }

    for (int job = 0; job < READBACK_SAMPLE_COUNT + PBO_RING_SIZE; ++job)
    {
        const int slot = job % PBO_RING_SIZE;
        if (fences[slot] != NULL)
        {
            const int collect = job - PBO_RING_SIZE >= PBO_RING_SIZE;
            bind_buffer(GL_PIXEL_PACK_BUFFER, pbos[slot]);
            if (!complete_pbo(pbos[slot], fences[slot], bind_buffer,
                              client_wait_sync,
                              delete_sync, map_buffer_range, unmap_buffer,
                              bytes, submitted_at[slot], collect,
                              completion_samples, wait_samples,
                              &completed_samples, &checksum))
            {
                printf("pbo_readback=status_failed reason=fence_or_map\n");
                goto cleanup;
            }
            fences[slot] = NULL;
        }

        bind_buffer(GL_PIXEL_PACK_BUFFER, pbos[slot]);
        const double submit_start = monotonic_seconds();
        glReadPixels(0, 0, PROBE_WIDTH_PIXELS, PROBE_HEIGHT_PIXELS, format,
                     GL_UNSIGNED_BYTE, NULL);
        fences[slot] = fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        const GLenum submit_error = glGetError();
        const double submit_end = monotonic_seconds();
        if (submit_start < 0.0 || submit_end < submit_start ||
            fences[slot] == NULL || submit_error != GL_NO_ERROR)
        {
            printf("pbo_readback=status_failed reason=submit gl_error=0x%x\n",
                   (unsigned int)submit_error);
            goto cleanup;
        }
        submitted_at[slot] = submit_start;
        if (job >= PBO_RING_SIZE)
        {
            submit_samples[job - PBO_RING_SIZE] =
                (submit_end - submit_start) * 1e6;
        }

        memcpy(cpu_destination, cpu_source, sizeof(cpu_destination));
        checksum += cpu_destination[(size_t)job % CPU_OVERLAP_BYTES];
    }

    for (int job = READBACK_SAMPLE_COUNT;
         job < READBACK_SAMPLE_COUNT + PBO_RING_SIZE; ++job)
    {
        const int slot = job % PBO_RING_SIZE;
        const int collect = job >= PBO_RING_SIZE;
        if (fences[slot] != NULL)
        {
            bind_buffer(GL_PIXEL_PACK_BUFFER, pbos[slot]);
            if (!complete_pbo(pbos[slot], fences[slot], bind_buffer,
                              client_wait_sync,
                              delete_sync, map_buffer_range, unmap_buffer,
                              bytes, submitted_at[slot], collect,
                              completion_samples, wait_samples,
                              &completed_samples, &checksum))
            {
                printf("pbo_readback=status_failed reason=drain\n");
                goto cleanup;
            }
            fences[slot] = NULL;
        }
    }

    if (completed_samples != READBACK_SAMPLE_COUNT)
    {
        printf("pbo_readback=status_failed reason=sample_count "
               "completed=%zu\n", completed_samples);
        goto cleanup;
    }
    printf("pbo_ring_depth=%d synthetic_cpu_copy_bytes_per_job=%d "
           "pbo_framebuffer_readback_bytes=%zu pbo_format=%s "
           "pbo_no_glFinish=1\n",
           PBO_RING_SIZE, CPU_OVERLAP_BYTES, bytes,
           format == GL_BGRA ? "BGRA_UBYTE" : "RGBA_UBYTE");
    report_samples("pbo_submit_call", submit_samples, READBACK_SAMPLE_COUNT);
    report_samples("pbo_submit_to_reuse_poll", completion_samples,
                   READBACK_SAMPLE_COUNT);
    report_samples("pbo_client_wait", wait_samples, READBACK_SAMPLE_COUNT);
    printf("pbo_checksum=%llu\n", (unsigned long long)checksum);
    success = 1;

cleanup:
    for (int index = 0; index < PBO_RING_SIZE; ++index)
    {
        if (fences[index] != NULL)
        {
            delete_sync(fences[index]);
        }
    }
    bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(PBO_RING_SIZE, pbos);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return success;
}

int
main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        fputs("RESULT=SKIP: XOpenDisplay failed\n", stderr);
        return 77;
    }

    const int screen = DefaultScreen(display);
    int glx_major = 0;
    int glx_minor = 0;
    if (!glXQueryVersion(display, &glx_major, &glx_minor))
    {
        fputs("RESULT=SKIP: GLX is unavailable\n", stderr);
        XCloseDisplay(display);
        return 77;
    }
    const char *glx_extensions = glXQueryExtensionsString(display, screen);
    const int glx_create_context = has_space_extension(
        glx_extensions, "GLX_ARB_create_context");
    if (!glx_create_context)
    {
        fputs("RESULT=SKIP: GLX_ARB_create_context is unavailable\n", stderr);
        XCloseDisplay(display);
        return 77;
    }

    const int config_attributes[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        None
    };
    int config_count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(
        display, screen, config_attributes, &config_count);
    if (configs == NULL || config_count == 0)
    {
        fputs("RESULT=SKIP: no GLX pbuffer framebuffer configuration\n",
              stderr);
        XFree(configs);
        XCloseDisplay(display);
        return 77;
    }
    const int pbuffer_attributes[] = {
        GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None
    };
    GLXPbuffer pbuffer = glXCreatePbuffer(display, configs[0],
                                           pbuffer_attributes);
    if (pbuffer == None)
    {
        fputs("RESULT=SKIP: GLX pbuffer creation failed\n", stderr);
        XFree(configs);
        XCloseDisplay(display);
        return 77;
    }

    GLXContext core = create_profile_context(
        display, configs[0], pbuffer, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        OPENGL_TARGET_MAJOR, OPENGL_TARGET_MINOR);
    GLXContext compatibility = create_profile_context(
        display, configs[0], pbuffer,
        GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
        OPENGL_TARGET_MAJOR, OPENGL_TARGET_MINOR);
    if (core == NULL)
    {
        fputs("RESULT=SKIP: OpenGL 3.3 core context unavailable\n", stderr);
        if (compatibility != NULL)
        {
            glXDestroyContext(display, compatibility);
        }
        glXDestroyPbuffer(display, pbuffer);
        XFree(configs);
        XCloseDisplay(display);
        return 77;
    }
    if (!glXMakeContextCurrent(display, pbuffer, pbuffer, core))
    {
        fputs("RESULT=FAIL: could not activate the probe context\n", stderr);
        glXDestroyContext(display, core);
        if (compatibility != NULL)
        {
            glXDestroyContext(display, compatibility);
        }
        glXDestroyPbuffer(display, pbuffer);
        XFree(configs);
        XCloseDisplay(display);
        return 1;
    }

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (!version_at_least(major, minor, OPENGL_TARGET_MAJOR,
                          OPENGL_TARGET_MINOR))
    {
        fprintf(stderr,
                "RESULT=SKIP: created core context reports only %d.%d\n",
                major, minor);
        glXDestroyContext(display, core);
        if (compatibility != NULL)
        {
            glXDestroyContext(display, compatibility);
        }
        glXDestroyPbuffer(display, pbuffer);
        XFree(configs);
        XCloseDisplay(display);
        return 77;
    }
    GLint max_texture_size = 0;
    GLint max_color_attachments = 0;
    GLint max_draw_buffers = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    if (version_at_least(major, minor, 3, 0))
    {
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max_color_attachments);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
    }
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl =
        (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    int maximum_core_major = 0;
    int maximum_core_minor = 0;
    if (!find_highest_core_context(display, configs[0], pbuffer, core,
                                   &maximum_core_major,
                                   &maximum_core_minor))
    {
        fputs("RESULT=FAIL: could not restore the 3.3 core context\n", stderr);
        glXDestroyContext(display, core);
        if (compatibility != NULL)
        {
            glXDestroyContext(display, compatibility);
        }
        glXDestroyPbuffer(display, pbuffer);
        XFree(configs);
        XCloseDisplay(display);
        return 1;
    }
    int composite_opcode = 0;
    int composite_event = 0;
    int composite_error = 0;
    const int composite_available = XQueryExtension(
        display, "Composite", &composite_opcode, &composite_event,
        &composite_error);
    const int has_texture_from_pixmap = has_space_extension(
        glx_extensions, "GLX_EXT_texture_from_pixmap");
    const int has_fbo = version_at_least(major, minor, 3, 0) ||
                        has_gl_extension("GL_ARB_framebuffer_object", major,
                                         minor) ||
                        has_gl_extension("GL_EXT_framebuffer_object", major,
                                         minor);
    const int has_pbo = version_at_least(major, minor, 2, 1) ||
                        has_gl_extension("GL_ARB_pixel_buffer_object", major,
                                         minor);
    const int has_map_buffer_range =
        version_at_least(major, minor, 3, 0) ||
        has_gl_extension("GL_ARB_map_buffer_range", major, minor);
    const int has_sync = version_at_least(major, minor, 3, 2) ||
                         has_gl_extension("GL_ARB_sync", major, minor);
    const int has_texture_rectangle =
        has_gl_extension("GL_ARB_texture_rectangle", major, minor) ||
        has_gl_extension("GL_EXT_texture_rectangle", major, minor) ||
        has_gl_extension("GL_NV_texture_rectangle", major, minor);
    const int has_timer_query =
        version_at_least(major, minor, 3, 3) ||
        has_gl_extension("GL_ARB_timer_query", major, minor) ||
        has_gl_extension("GL_EXT_timer_query", major, minor);

    PFNGLFENCESYNCPROC fence_sync =
        (PFNGLFENCESYNCPROC)glXGetProcAddressARB(
            (const GLubyte *)"glFenceSync");
    PFNGLMAPBUFFERRANGEPROC map_buffer_range =
        (PFNGLMAPBUFFERRANGEPROC)glXGetProcAddressARB(
            (const GLubyte *)"glMapBufferRange");
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers =
        (PFNGLGENFRAMEBUFFERSPROC)glXGetProcAddressARB(
            (const GLubyte *)"glGenFramebuffers");
    PFNGLBINDBUFFERPROC bind_buffer =
        (PFNGLBINDBUFFERPROC)glXGetProcAddressARB(
            (const GLubyte *)"glBindBuffer");
    PFNGLBUFFERDATAPROC buffer_data =
        (PFNGLBUFFERDATAPROC)glXGetProcAddressARB(
            (const GLubyte *)"glBufferData");
    PFNGLCLIENTWAITSYNCPROC client_wait_sync =
        (PFNGLCLIENTWAITSYNCPROC)glXGetProcAddressARB(
            (const GLubyte *)"glClientWaitSync");
    PFNGLDELETESYNCPROC delete_sync =
        (PFNGLDELETESYNCPROC)glXGetProcAddressARB(
            (const GLubyte *)"glDeleteSync");
    PFNGLUNMAPBUFFERPROC unmap_buffer =
        (PFNGLUNMAPBUFFERPROC)glXGetProcAddressARB(
            (const GLubyte *)"glUnmapBuffer");
    PFNGLGENQUERIESPROC gen_queries =
        (PFNGLGENQUERIESPROC)glXGetProcAddressARB(
            (const GLubyte *)"glGenQueries");
    PFNGLDELETEQUERIESPROC delete_queries =
        (PFNGLDELETEQUERIESPROC)glXGetProcAddressARB(
            (const GLubyte *)"glDeleteQueries");
    PFNGLQUERYCOUNTERPROC query_counter =
        (PFNGLQUERYCOUNTERPROC)glXGetProcAddressARB(
            (const GLubyte *)"glQueryCounter");
    PFNGLGETQUERYIVPROC get_query =
        (PFNGLGETQUERYIVPROC)glXGetProcAddressARB(
            (const GLubyte *)"glGetQueryiv");
    PFNGLGETQUERYOBJECTIVPROC get_query_object_iv =
        (PFNGLGETQUERYOBJECTIVPROC)glXGetProcAddressARB(
            (const GLubyte *)"glGetQueryObjectiv");
    PFNGLGETQUERYOBJECTUI64VPROC get_query_object_ui64v =
        (PFNGLGETQUERYOBJECTUI64VPROC)glXGetProcAddressARB(
            (const GLubyte *)"glGetQueryObjectui64v");
    PFNGLGENSAMPLERSPROC gen_samplers =
        (PFNGLGENSAMPLERSPROC)glXGetProcAddressARB(
            (const GLubyte *)"glGenSamplers");
    PFNGLDELETESAMPLERSPROC delete_samplers =
        (PFNGLDELETESAMPLERSPROC)glXGetProcAddressARB(
            (const GLubyte *)"glDeleteSamplers");
    PFNGLSAMPLERPARAMETERIPROC sampler_parameter_i =
        (PFNGLSAMPLERPARAMETERIPROC)glXGetProcAddressARB(
            (const GLubyte *)"glSamplerParameteri");
    PFNGLBINDSAMPLERPROC bind_sampler =
        (PFNGLBINDSAMPLERPROC)glXGetProcAddressARB(
            (const GLubyte *)"glBindSampler");

    printf("GLX_server_version=%d.%d\n", glx_major, glx_minor);
    printf("GL_vendor=%s\nGL_renderer=%s\nGL_version=%s\nGLSL_version=%s\n",
           vendor != NULL ? vendor : "unknown",
           renderer != NULL ? renderer : "unknown",
           version != NULL ? version : "unknown",
           glsl != NULL ? glsl : "unknown");
    printf("GL_target=3.3_core\nGL_context_request_3_3_core=%d\n"
           "GL_context_request_3_3_compatibility=%d\n"
           "GL_reported_major=%d\nGL_reported_minor=%d\n"
           "GL_highest_core_context_observed=%d.%d\n"
           "GL_max_texture_size=%d\nGL_max_color_attachments=%d\n"
           "GL_max_draw_buffers=%d\n",
           core != NULL, compatibility != NULL, major, minor,
           maximum_core_major, maximum_core_minor,
           max_texture_size, max_color_attachments, max_draw_buffers);
    printf("GL_FBO=%d\nGL_FBO_functions=%d\nGL_PBO=%d\n"
           "GL_map_buffer_range=%d\nGL_map_buffer_range_function=%d\n"
           "GL_sync_fence=%d\nGL_sync_fence_function=%d\n"
           "GL_texture_rectangle=%d\nGL_timer_query_core_or_extension=%d\n"
           "GL_timer_query_functions=%d\n"
           "GL_sampler_object_functions=%d\n"
           "GLX_EXT_texture_from_pixmap=%d\nXComposite=%d\n",
           has_fbo, gen_framebuffers != NULL, has_pbo && bind_buffer != NULL,
           has_map_buffer_range,
           map_buffer_range != NULL, has_sync, fence_sync != NULL,
           has_texture_rectangle, has_timer_query,
           gen_queries != NULL && delete_queries != NULL &&
               query_counter != NULL && get_query != NULL &&
               get_query_object_iv != NULL &&
               get_query_object_ui64v != NULL,
           gen_samplers != NULL && delete_samplers != NULL &&
               sampler_parameter_i != NULL && bind_sampler != NULL,
           has_texture_from_pixmap, composite_available);

    const int format_r8 = framebuffer_format_complete(GL_R8, GL_RED);
    const int format_rg8 = framebuffer_format_complete(GL_RG8, GL_RG);
    const int format_rgba8 = framebuffer_format_complete(GL_RGBA8, GL_RGBA);
    printf("FBO_R8_complete=%d\nFBO_RG8_complete=%d\n"
           "FBO_RGBA8_complete=%d\n",
           format_r8, format_rg8, format_rgba8);

    const size_t readback_bytes =
        (size_t)PROBE_WIDTH_PIXELS * PROBE_HEIGHT_PIXELS * 4U;
    unsigned char *readback = (unsigned char *)malloc(readback_bytes);
    if (readback == NULL)
    {
        fputs("RESULT=FAIL: readback allocation failed\n", stderr);
        if (core != NULL) glXDestroyContext(display, core);
        if (compatibility != NULL) glXDestroyContext(display, compatibility);
        glXDestroyPbuffer(display, pbuffer);
        XFree(configs);
        XCloseDisplay(display);
        return 1;
    }

    GLuint texture = 0;
    GLuint framebuffer = 0;
    int pbo_failed = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PROBE_WIDTH_PIXELS,
                 PROBE_HEIGHT_PIXELS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glGetError() == GL_NO_ERROR)
    {
        glViewport(0, 0, PROBE_WIDTH_PIXELS, PROBE_HEIGHT_PIXELS);
        glClearColor(0.31f, 0.57f, 0.83f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLint timestamp_counter_bits = 0;
        GLuint64 timer_query_elapsed_ns = 0;
        const char *timer_query_failure_reason = "unknown";
        const int timestamp_query_available = probe_timestamp_query(
            gen_queries, delete_queries, query_counter, get_query,
            get_query_object_iv, get_query_object_ui64v,
            &timestamp_counter_bits, &timer_query_elapsed_ns,
            &timer_query_failure_reason);
        printf("GL_timestamp_counter_bits=%d\n"
               "GL_timestamp_query_smoke_status=%s reason=%s\n",
               timestamp_counter_bits,
               timestamp_query_available ? "available" : "unavailable",
               timestamp_query_available ? "none"
                                         : timer_query_failure_reason);
        if (timestamp_query_available)
        {
            printf("GL_timestamp_query_smoke_elapsed_ns=%llu "
                   "measurement_scope=single_glClear_not_pipeline_timing\n",
                   (unsigned long long)timer_query_elapsed_ns);
        }
        printf("GL_sampler_object_smoke_status=%s\n",
               probe_sampler_objects(gen_samplers, delete_samplers,
                                     sampler_parameter_i, bind_sampler)
                   ? "available"
                   : "unavailable");
        report_readback_format("RGBA_UBYTE", GL_RGBA, framebuffer, readback,
                               readback_bytes);
        report_readback_format("BGRA_UBYTE", GL_BGRA, framebuffer, readback,
                               readback_bytes);
        if (has_pbo && has_sync && bind_buffer != NULL &&
            buffer_data != NULL && fence_sync != NULL &&
            client_wait_sync != NULL && delete_sync != NULL &&
            map_buffer_range != NULL && unmap_buffer != NULL)
        {
            pbo_failed = !report_pbo_readback(
                framebuffer, GL_BGRA, readback_bytes, bind_buffer,
                buffer_data, fence_sync, client_wait_sync, delete_sync,
                map_buffer_range, unmap_buffer);
        }
        else
        {
            printf("pbo_readback=status_unsupported "
                   "reason=required_PBO_or_sync_function_missing\n");
        }
    }
    else
    {
        printf("readback_benchmark=status_unsupported reason=fbo_setup_failed "
               "gl_error=0x%x\n", (unsigned int)glGetError());
    }

    free(readback);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glXMakeContextCurrent(display, None, None, NULL);
    if (core != NULL) glXDestroyContext(display, core);
    if (compatibility != NULL) glXDestroyContext(display, compatibility);
    glXDestroyPbuffer(display, pbuffer);
    XFree(configs);
    XCloseDisplay(display);
    if (pbo_failed)
    {
        fputs("RESULT=FAIL: PBO readback timing did not complete\n", stderr);
        return 1;
    }
    puts("RESULT=PASS");
    return 0;
}
