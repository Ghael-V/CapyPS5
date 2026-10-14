"""Regenerate the synthetic one-second fixtures with PyAV 18.1.0."""
from fractions import Fraction
from pathlib import Path
import math
import struct
import av


for filename, video_codec, audio_codec in (
    ("vp9-opus.webm", "libvpx-vp9", "libopus"),
    ("vp8-vorbis.webm", "libvpx", "vorbis"),
):
    with av.open(str(Path(__file__).with_name(filename)), "w") as container:
        video = container.add_stream(video_codec, rate=5)
        video.width, video.height, video.pix_fmt = 64, 48, "yuv420p"
        audio = container.add_stream(audio_codec, rate=48000)
        audio.layout = "stereo"
        audio.options = {"strict": "-2"}
        for index in range(5):
            frame = av.VideoFrame(64, 48, "yuv420p")
            for plane_index, plane in enumerate(frame.planes):
                plane.update(bytes([32 + index * 24 if plane_index == 0 else 128]) * plane.buffer_size)
            frame.pts, frame.time_base = index, Fraction(1, 5)
            container.mux(video.encode(frame))
        container.mux(video.encode())
        for start in range(0, 48000, 960):
            frame = av.AudioFrame(format="fltp", layout="stereo", samples=960)
            samples = struct.pack("<960f", *(
                0.1 * math.sin(2 * math.pi * 440 * (start + i) / 48000) for i in range(960)
            ))
            for plane in frame.planes:
                plane.update(samples)
            frame.sample_rate, frame.pts, frame.time_base = 48000, start, Fraction(1, 48000)
            container.mux(audio.encode(frame))
        container.mux(audio.encode())
