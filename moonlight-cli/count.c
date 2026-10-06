/*
 * A video "decoder" for the fake platform that counts what arrives.
 * Upstream's fake platform drops every frame, so a test could only
 * tell that the first packet came in. MOONLIGHT_DUMP=<file> also keeps
 * the raw stream.
 */
#include <Limelight.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static FILE *dump;
static unsigned long frames, bytes;
static double first, last;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int count_setup(int format, int width, int height, int rate, void *context, int flags)
{
	const char *path = getenv("MOONLIGHT_DUMP");
	dump = path ? fopen(path, "wb") : NULL;
	frames = bytes = 0;
	return 0;
}

static void count_cleanup(void)
{
	double span = frames > 1 ? last - first : 0;
	if (dump)
		fclose(dump);
	printf("Video frames received: %lu in %.1f s, %.1f fps, %lu kB\n", frames, span,
	       span > 0 ? (frames - 1) / span : 0.0, bytes / 1000);
	fflush(stdout);
}

static int count_submit(PDECODE_UNIT unit)
{
	last = now();
	if (!frames++)
		first = last;
	for (PLENTRY entry = unit->bufferList; entry; entry = entry->next) {
		bytes += entry->length;
		if (dump)
			fwrite(entry->data, 1, entry->length, dump);
	}
	return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_count = {
	.setup = count_setup,
	.cleanup = count_cleanup,
	.submitDecodeUnit = count_submit,
	.capabilities = CAPABILITY_DIRECT_SUBMIT,
};
