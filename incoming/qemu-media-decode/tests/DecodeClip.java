// Copyright 2026 jinghero. SPDX-License-Identifier: Apache-2.0
import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.SystemClock;
import java.nio.ByteBuffer;
import java.util.zip.CRC32;

/** Run under app_process. The optional frame delay leaves time to save a snapshot. */
public final class DecodeClip {
    private static void hashImage(Image image, CRC32 hash) {
        for (int p = 0; p < image.getPlanes().length; ++p) {
            Image.Plane plane = image.getPlanes()[p];
            ByteBuffer buf = plane.getBuffer();
            int w = image.getWidth() >> (p == 0 ? 0 : 1);
            int h = image.getHeight() >> (p == 0 ? 0 : 1);
            for (int y = 0; y < h; ++y) {
                int row = buf.position() + y * plane.getRowStride();
                for (int x = 0; x < w; ++x) hash.update(buf.get(row + x * plane.getPixelStride()));
            }
        }
    }

    public static void main(String[] args) throws Exception {
        int delay = args.length > 2 ? Integer.parseInt(args[2]) : 0;
        MediaExtractor extractor = new MediaExtractor();
        extractor.setDataSource(args[0]);
        extractor.selectTrack(0);
        MediaFormat format = extractor.getTrackFormat(0);
        int w = format.getInteger(MediaFormat.KEY_WIDTH);
        int h = format.getInteger(MediaFormat.KEY_HEIGHT);
        CRC32 crc = new CRC32();
        try (ImageReader reader = ImageReader.newInstance(w, h, ImageFormat.YUV_420_888, 3)) {
            MediaCodec codec = MediaCodec.createByCodecName(args[1]);
            try {
                codec.configure(format, reader.getSurface(), null, 0);
                codec.start();
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                boolean sentEos = false;
                int frames = 0;
                long deadline = SystemClock.elapsedRealtime() + 15000;
                while (SystemClock.elapsedRealtime() < deadline) {
                    if (!sentEos) {
                        int index = codec.dequeueInputBuffer(10000);
                        if (index >= 0) {
                            int size = extractor.readSampleData(codec.getInputBuffer(index), 0);
                            sentEos = size < 0;
                            codec.queueInputBuffer(index, 0, Math.max(0, size),
                                    sentEos ? 0 : extractor.getSampleTime(),
                                    sentEos ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                            if (!sentEos) extractor.advance();
                        }
                    }
                    int index = codec.dequeueOutputBuffer(info, 10000);
                    if (index < 0) continue;
                    boolean render = info.size > 0;
                    codec.releaseOutputBuffer(index, render);
                    if (render) {
                        Image next;
                        while ((next = reader.acquireNextImage()) == null) {
                            if (SystemClock.elapsedRealtime() >= deadline)
                                throw new AssertionError("ImageReader timeout at frame " + frames);
                            SystemClock.sleep(2);
                        }
                        try (Image image = next) { hashImage(image, crc); }
                        System.out.printf("frame=%d pts=%d crc=%08x%n", ++frames, info.presentationTimeUs, crc.getValue());
                        if (delay > 0) SystemClock.sleep(delay);
                        deadline = SystemClock.elapsedRealtime() + 15000;
                    }
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        System.out.printf("EOS codec=%s frames=%d crc=%08x%n", codec.getName(), frames, crc.getValue());
                        return;
                    }
                }
                throw new AssertionError("codec output timeout at frame " + frames);
            } finally { codec.release(); }
        } finally { extractor.release(); }
    }
}
