/* Exercise two separated streams through the real HakiOS HAL functions. */
#include "../lapis_audio_hal/audio_plugin.c"

int main(void)
{
	if (initialize(NULL, NULL) != 0 || sink < 0) return 1;
	volume = 1.0f;
	volume_to_kernel();
	int16_t samples[960];
	for (unsigned burst = 0; burst < 2; burst++) {
		unsigned period = burst ? 96 : 48;
		for (unsigned packet = 0; packet < 100; packet++) {
			for (unsigned i = 0; i < 960; i++) samples[i] = ((i / 2) % period < period / 2) ? 16000 : -16000;
			if (do_io_operation(NULL, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 0,
			    kAudioServerPlugInIOOperationWriteMix, 480, NULL, samples, NULL) != 0) return 2;
			usleep(10000);
		}
		if (burst == 0) sleep(2);
	}
	volume = 0.0f;
	volume_to_kernel();
	close(sink);
	return 0;
}
