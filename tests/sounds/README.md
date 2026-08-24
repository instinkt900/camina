# Test sounds

`blip.ogg` is a real Ogg Vorbis file, because issue #424 cannot be tested with a
made-up one. The point of that work is that a short effect which is **not** a
WAV decodes to PCM at cook time, and only a file a decoder accepts can show it.

Every other sound in the test suite is bytes a test wrote inline. That works for
WAV, which `tests/test_cooker.cpp` builds with `wav_16_bit`, and for the streamed
path, which copies bytes through without looking at them.

It is 0.1 seconds of a 440 Hz sine, mono, at 22050 Hz, which is 3.5 KiB. Short
on purpose: it is committed, and a test fixture that decodes to PCM is held in
memory twice.

Made once with ffmpeg and committed, so no build machine needs an encoder:

```
ffmpeg -f lavfi -i "sine=frequency=440:duration=0.1:sample_rate=22050" \
    -ac 1 -c:a libvorbis -q:a 0 blip.ogg
```
