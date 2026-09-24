package org.jjs.iecreplay;

import android.Manifest;
import android.app.Activity;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

public class MainActivity extends Activity {
    private static final String TAG = "JJS-IEC-Replay";
    private static final int SAMPLE_RATE = 192000;
    private static final int CHANNEL_MASK = AudioFormat.CHANNEL_OUT_7POINT1_SURROUND;
    private static final int ENCODING = AudioFormat.ENCODING_IEC61937;
    private static final int BURST_BYTES = 61440;

    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        status = new TextView(this);
        status.setTextSize(24);
        status.setPadding(32, 32, 32, 32);
        status.setText("JJS IEC Replay\nStarting...");
        setContentView(status);

        new Thread(this::runReplay, "iec-replay").start();
    }

    private void setStatus(String text) {
        runOnUiThread(() -> status.setText(text));
    }

    private void runReplay() {
        File file = new File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                "pf-transition-10s.raw");

        Log.i(TAG, "Input: " + file.getAbsolutePath() + " size=" + file.length());

        if (!file.isFile()) {
            fail("RAW file not found:\n" + file.getAbsolutePath());
            return;
        }

        int minBuffer = AudioTrack.getMinBufferSize(SAMPLE_RATE, CHANNEL_MASK, ENCODING);
        int bufferSize = minBuffer > 0 ? minBuffer * 4 : 393728;
        Log.i(TAG, "minBufferSize=" + minBuffer + " bufferSize=" + bufferSize +
                " sampleRate=" + SAMPLE_RATE + " channelMask=" + CHANNEL_MASK +
                " encoding=" + ENCODING);

        AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();

        AudioFormat format = new AudioFormat.Builder()
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(CHANNEL_MASK)
                .setEncoding(ENCODING)
                .build();

        AudioTrack track;
        try {
            track = new AudioTrack(
                    attributes,
                    format,
                    bufferSize,
                    AudioTrack.MODE_STREAM,
                    AudioManager.AUDIO_SESSION_ID_GENERATE);
        } catch (Exception e) {
            fail("AudioTrack creation failed:\n" + e);
            return;
        }

        Log.i(TAG, "AudioTrack state=" + track.getState());
        if (track.getState() != AudioTrack.STATE_INITIALIZED) {
            track.release();
            fail("AudioTrack not initialized");
            return;
        }

        long totalBytes = 0;
        int writes = 0;

        try (FileInputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[BURST_BYTES];
            short[] shorts = new short[BURST_BYTES / 2];

            track.play();
            setStatus("Playing exact IEC/MAT RAW...\n" +
                    "192000 Hz / 7.1 / IEC61937\n" +
                    "Buffer: " + bufferSize + " bytes");

            while (true) {
                int got = readFullyUpTo(in, bytes);
                if (got <= 0)
                    break;

                if ((got & 1) != 0)
                    throw new IOException("Odd byte count: " + got);

                int shortCount = got / 2;
                for (int i = 0; i < shortCount; ++i) {
                    int lo = bytes[i * 2] & 0xff;
                    int hi = bytes[i * 2 + 1] & 0xff;
                    shorts[i] = (short) (lo | (hi << 8));
                }

                int off = 0;
                while (off < shortCount) {
                    int written = track.write(
                            shorts,
                            off,
                            shortCount - off,
                            AudioTrack.WRITE_BLOCKING);
                    Log.i(TAG, "write #" + (writes + 1) +
                            " requestedShorts=" + (shortCount - off) +
                            " writtenShorts=" + written);
                    if (written <= 0)
                        throw new IOException("AudioTrack.write failed: " + written);
                    off += written;
                    writes++;
                }

                totalBytes += got;
            }

            Thread.sleep(1500);
            Log.i(TAG, "DONE bytes=" + totalBytes + " writes=" + writes +
                    " playbackHead=" + track.getPlaybackHeadPosition());
            setStatus("Finished\nBytes: " + totalBytes + "\nWrites: " + writes);
        } catch (Exception e) {
            Log.e(TAG, "Replay failed", e);
            fail("Replay failed:\n" + e);
        } finally {
            try { track.stop(); } catch (Exception ignored) {}
            track.release();
        }
    }

    private static int readFullyUpTo(FileInputStream in, byte[] buffer) throws IOException {
        int total = 0;
        while (total < buffer.length) {
            int n = in.read(buffer, total, buffer.length - total);
            if (n < 0)
                break;
            total += n;
        }
        return total;
    }

    private void fail(String message) {
        Log.e(TAG, message);
        setStatus(message);
    }
}
