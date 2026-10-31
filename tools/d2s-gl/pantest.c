/*
 * pantest: GPU step 4a on the d2s. Mesa's panfrost Gallium driver, over
 * d2sdrm and D2SGPU, clears an IOSurface; the CPU checks the pixels through
 * the IOSurface's own mapping.
 *
 * Built as pantest.dylib and inserted into backboardd with the d2senv
 * boot-arg (like tools/d2s-gpu's test): it runs once, on its own thread, a
 * few seconds after load, and reports in the kernel log
 * ("D2S GPU UC: backboardd: pantest: ...").
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOSurface/IOSurfaceRef.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/driconf.h"
#include "util/os_time.h"
#include "util/u_inlines.h"
#include "util/xmlconfig.h"
#include "panfrost/pan_public.h"
#include "d2sdrm.h"

static const driOptionDescription gallium_conf[] = {
#include "driinfo_gallium.h"
};

static const driOptionDescription pan_conf[] = {
#include "panfrost/driinfo_panfrost.h"
};

static int fd = -1;
#define say(...) d2sdrm_log(fd, "pantest: " __VA_ARGS__)

static IOSurfaceRef
make_surface(int w, int h)
{
	int bpe = 4, fmt = 0x42475241;	/* 'BGRA' */
	const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
		               kIOSurfacePixelFormat };
	CFNumberRef vals[] = {
		CFNumberCreate(NULL, kCFNumberIntType, &w), CFNumberCreate(NULL, kCFNumberIntType, &h),
		CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberIntType, &fmt),
	};
	CFDictionaryRef props = CFDictionaryCreate(NULL, keys, (const void **)vals, 4,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(props);
	for (int i = 0; i < 4; i++) {
		CFRelease(vals[i]);
	}
	return s;
}

/* Clear to colour c; count pixels within 1 of the expected BGRA bytes. */
static int
clear_and_check(struct pipe_screen *screen, struct pipe_context *pipe, struct pipe_resource *res,
    IOSurfaceRef surf, int w, int h, const float c[4])
{
	struct pipe_framebuffer_state fb;
	memset(&fb, 0, sizeof(fb));
	fb.width = (uint32_t)w;
	fb.height = (uint32_t)h;
	fb.layers = 1;
	fb.nr_cbufs = 1;
	fb.cbufs[0].texture = res;
	fb.cbufs[0].format = res->format;
	pipe->set_framebuffer_state(pipe, &fb);

	union pipe_color_union col;
	memcpy(col.f, c, sizeof(col.f));
	pipe->clear(pipe, PIPE_CLEAR_COLOR0, 0xffffffffu, 0xff, NULL, &col, 0.0, 0);
	struct pipe_fence_handle *fence = NULL;
	uint64_t t0 = os_time_get_nano();
	pipe->flush(pipe, &fence, 0);
	bool done = fence != NULL && screen->fence_finish(screen, NULL, fence, OS_TIMEOUT_INFINITE);
	uint64_t t1 = os_time_get_nano();
	screen->fence_reference(screen, &fence, NULL);

	const uint8_t want[4] = { (uint8_t)(c[2] * 255 + 0.5f), (uint8_t)(c[1] * 255 + 0.5f),
		                  (uint8_t)(c[0] * 255 + 0.5f), (uint8_t)(c[3] * 255 + 0.5f) };
	IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *base = IOSurfaceGetBaseAddress(surf);
	size_t bpr = IOSurfaceGetBytesPerRow(surf);
	int good = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *p = base + (size_t)y * bpr + (size_t)x * 4;
			int ok = 1;
			for (int k = 0; k < 4; k++) {
				ok &= abs((int)p[k] - (int)want[k]) <= 1;
			}
			good += ok;
		}
	}
	uint32_t first, mid;
	memcpy(&first, base, 4);
	memcpy(&mid, base + (size_t)(h / 2) * bpr + (size_t)(w / 2) * 4, 4);
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	say("clear %.2f,%.2f,%.2f,%.2f: fence %s in %llu us, %d/%d pixels right, first %#x mid %#x",
	    c[0], c[1], c[2], c[3], done ? "done" : "NOT DONE",
	    (unsigned long long)((t1 - t0) / 1000), good, w * h, first, mid);
	return done && good == w * h;
}

static void
run(void)
{
	fd = d2sdrm_open();
	if (fd < 0) {
		return;		/* no D2SGPU: nothing to report to */
	}
	say("start (Mesa panfrost over d2sdrm)");

	unsigned n = (unsigned)(ARRAY_SIZE(gallium_conf) + ARRAY_SIZE(pan_conf));
	driOptionDescription *conf = malloc(n * sizeof(*conf));
	memcpy(conf, gallium_conf, sizeof(gallium_conf));
	memcpy(conf + ARRAY_SIZE(gallium_conf), pan_conf, sizeof(pan_conf));
	driOptionCache info, cache;
	driParseOptionInfo(&info, conf, n);
	driParseConfigFiles(&cache, &info, &(driConfigFileParseParams){ .driverName = "panfrost" });
	free(conf);
	struct pipe_screen_config cfg = { .options = &cache, .options_info = &info };

	struct pipe_screen *screen = panfrost_create_screen(fd, &cfg, NULL);
	if (screen == NULL) {
		say("PANTEST FAIL: panfrost_create_screen returned NULL");
		return;
	}
	say("screen: %s / %s", screen->get_vendor(screen), screen->get_name(screen));
	struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
	if (pipe == NULL) {
		say("PANTEST FAIL: context_create returned NULL");
		return;
	}

	const int w = 64, h = 64;
	IOSurfaceRef surf = make_surface(w, h);
	if (surf == NULL) {
		say("PANTEST FAIL: IOSurfaceCreate");
		return;
	}
	int pfd = d2sdrm_surface_fd(fd, IOSurfaceGetID(surf));
	struct pipe_resource templ;
	memset(&templ, 0, sizeof(templ));
	templ.target = PIPE_TEXTURE_2D;
	templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
	templ.width0 = (uint32_t)w;
	templ.height0 = (uint16_t)h;
	templ.depth0 = 1;
	templ.array_size = 1;
	templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED;
	struct winsys_handle wh;
	memset(&wh, 0, sizeof(wh));
	wh.type = WINSYS_HANDLE_TYPE_FD;
	wh.handle = (unsigned)pfd;
	wh.stride = (unsigned)IOSurfaceGetBytesPerRow(surf);
	wh.offset = 0;
	wh.modifier = DRM_FORMAT_MOD_LINEAR;
	struct pipe_resource *res = pfd >= 0 ?
	    screen->resource_from_handle(screen, &templ, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE) : NULL;
	if (pfd >= 0) {
		close(pfd);
	}
	if (res == NULL) {
		say("PANTEST FAIL: resource_from_handle (IOSurface %u, %zu bytes/row)",
		    IOSurfaceGetID(surf), IOSurfaceGetBytesPerRow(surf));
		return;
	}
	say("IOSurface %u imported as a %ux%u render target", IOSurfaceGetID(surf), w, h);

	const float a[4] = { 0.0f, 0.5f, 1.0f, 1.0f }, b[4] = { 1.0f, 0.25f, 0.0f, 1.0f };
	int ok_a = clear_and_check(screen, pipe, res, surf, w, h, a);
	int ok_b = clear_and_check(screen, pipe, res, surf, w, h, b);
	say("PANTEST %s (clear 1 %s, clear 2 %s)", ok_a && ok_b ? "OK" : "FAIL",
	    ok_a ? "ok" : "FAIL", ok_b ? "ok" : "FAIL");

	pipe_resource_reference(&res, NULL);
	pipe->destroy(pipe);
	screen->destroy(screen);
	CFRelease(surf);
}

static void *
test_thread(void *arg)
{
	(void)arg;
	sleep(5);
	run();
	return NULL;
}

__attribute__((constructor)) static void
pantest_load(void)
{
	const char *name = getprogname();
	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	pthread_t t;
	if (pthread_create(&t, NULL, test_thread, NULL) == 0) {
		pthread_detach(t);
	}
}
