/* Exercise the actual upstream HAL implementation directly, without installing
 * a bundle or changing VirtualAudio. This does not test discovery by iOS. */
#include "../lapis_audio_hal/audio_plugin.c"

int main(void)
{
	if (initialize(NULL, NULL) != 0 || sink < 0) return 1;
	volume = 0.0f;
	volume_to_kernel();
	struct hakios_audio_volume invalid = { 961, 1 };
	errno = 0;
	int rc = ioctl(sink, HAKIOS_AUDIO_SET_VOLUME, &invalid);
	int ok = rc == -1 && errno == EINVAL;
	int16_t frames[8] = { 11, -11, 22, -22, 33, -33, 44, -44 };
	OSStatus status = do_io_operation(NULL, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 0,
	    kAudioServerPlugInIOOperationWriteMix, 4, NULL, frames, NULL);
	if (status != 0) ok = 0;
	if (!ok) (void)write(sink, frames, 4);
	close(sink);
	return ok ? 0 : 2;
}
