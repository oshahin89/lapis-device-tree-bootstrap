/* Bounded audible test through upstream HakiOS HAL functions, not discovery. */
#include "../lapis_audio_hal/audio_plugin.c"

int main(void)
{
	if (initialize(NULL, NULL) != 0 || sink < 0) return 1;
	int16_t samples[960];
	for (unsigned packet = 0; packet < 200; packet++) {
		if (packet == 0 || packet == 50 || packet == 150) {
			volume = packet == 50 ? 1.0f : 0.0f;
			volume_to_kernel();
		}
		unsigned period = packet < 100 ? 48 : 96;
		for (unsigned i = 0; i < 960; i++) samples[i] = ((i / 2) % period < period / 2) ? 16000 : -16000;
		if (do_io_operation(NULL, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 0,
		    kAudioServerPlugInIOOperationWriteMix, 480, NULL, samples, NULL) != 0) return 2;
		usleep(10000);
	}
	close(sink);
	return 0;
}
