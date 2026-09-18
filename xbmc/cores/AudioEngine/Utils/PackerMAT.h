/*
 *  Copyright (C) 2024 Team Kodi
 *  Copyright (C) 2010-2021 Hendrik Leppkes
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  The TrueHD seamless-branch padding carry-forward is derived from the
 *  TrueHD MAT packer in LAV Filters by Hendrik Leppkes (Nevcairiel).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

struct TrueHDMajorSyncInfo
{
  int ratebits{0};
  uint16_t outputTiming{0};
  bool outputTimingPresent{false};
  bool valid{false};
};

enum class Type
{
  PADDING,
  DATA,
};

class CTrueHDTrace
{
public:
  enum class Stage : uint8_t
  {
    DEMUX,
    DEMUX_EOF,
    PACK_IN,
    PACK_SKIP_NOSYNC,
    MAT_FLUSH,
    MAT_POP,
    MAT_TRANSFER,
    READRAW_OUT,
    READRAW_EMPTY,
    DECODER_FILL,
    DECODER_TAKE,
    DECODER_DISCARD,
    DECODER_EOF,
    AE_ADD,
    SEEK,
    HANDOVER_BEGIN,
    HANDOVER_END,
  };

  static uint32_t RegisterStream();
  static uint64_t Hash(const uint8_t* data, std::size_t size);
  static uint64_t Record(Stage stage,
                         uint32_t streamId,
                         uint64_t itemId,
                         uint32_t size,
                         uint64_t hash,
                         int64_t value1 = 0,
                         int64_t value2 = 0,
                         int64_t value3 = 0,
                         int64_t value4 = 0);
  static void DumpAround(uint64_t centerSequence,
                         uint64_t eventsBefore = 256,
                         uint64_t eventsAfter = 384);
};

class CPackerMAT
{
public:
  CPackerMAT();
  ~CPackerMAT() = default;

  bool PackTrueHD(const uint8_t* data, int size);
  std::vector<uint8_t> GetOutputFrame();

  uint32_t GetBufferedBytes() const { return m_bufferCount; }
  uint32_t GetBufferedSamples() const { return m_state.samples; }
  uint32_t GetPendingPadding() const { return m_state.padding; }
  uint32_t GetQueuedPacketCount() const
  {
    return static_cast<uint32_t>(m_outputQueue.size());
  }
  void SetTraceStreamId(uint32_t streamId) { m_traceStreamId = streamId; }

private:
  struct MATState
  {
    bool init; // differentiates the first header

    // audio_sampling_frequency:
    //  0 -> 48 kHz
    //  1 -> 96 kHz
    //  2 -> 192 kHz
    //  8 -> 44.1 kHz
    //  9 -> 88.2 kHz
    // 10 -> 176.4 kHz
    int ratebits;

    // Output timing obtained parsing TrueHD major sync headers (when available) or
    // inferred increasing a counter the rest of the time.
    uint16_t outputTiming;
    bool outputTimingValid;

    // Input timing of audio unit (obtained of each audio unit) and used to calculate padding
    // bytes. On the contrary of outputTiming, frametime is present in all audio units.
    uint16_t prevFrametime;
    bool prevFrametimeValid;

    uint32_t matFramesize; // size in bytes of current MAT frame
    uint32_t prevMatFramesize; // size in bytes of previous MAT frame

    uint32_t padding; // padding bytes pending to write

    // Frame-time to output-time offset used to keep MAT slot alignment across
    // TrueHD seamless branch points.
    int32_t nOutputTimeOffset;

    uint32_t samples; // number of samples accumulated in current MAT frame
    int numberOfSamplesOffset; // offset respect number of samples in a standard MAT frame (40 * 24)
  };

  void WriteHeader();
  void WritePadding();
  void AppendData(const uint8_t* data, int size, Type type);
  uint32_t GetCount() const { return m_bufferCount; }
  int FillDataBuffer(const uint8_t* data, int size, Type type);
  void FlushPacket();
  TrueHDMajorSyncInfo ParseTrueHDMajorSyncHeaders(const uint8_t* p, int buffsize) const;

  MATState m_state{};

  uint32_t m_bufferCount{0};
  std::vector<uint8_t> m_buffer;
  std::deque<std::vector<uint8_t>> m_outputQueue;

  struct TraceQueuedMAT
  {
    uint64_t serial{0};
    uint32_t streamId{0};
  };
  uint32_t m_traceStreamId{0};
  uint64_t m_traceFrameSeq{0};
  uint64_t m_traceMatSeq{0};
  uint64_t m_traceMatFirstFrameSeq{0};
  uint64_t m_traceMatLastFrameSeq{0};
  std::deque<TraceQueuedMAT> m_traceOutputQueue;
};

class CBitStream
{
public:
  // opens an existing byte array as bitstream
  CBitStream(const uint8_t* bytes, int _size)
  {
    data = bytes;
    size = _size;
  }

  // reads bits from bitstream
  int ReadBits(int bits)
  {
    int dat = 0;
    for (int i = index; i < index + bits; i++)
    {
      dat = dat * 2 + getbit(data[i / 8], i % 8);
    }
    index += bits;
    return dat;
  }

  // skip bits from bitstream
  void SkipBits(int bits) { index += bits; }

private:
  uint8_t getbit(uint8_t x, int y) { return (x >> (7 - y)) & 1; }

  const uint8_t* data{nullptr};
  int size{0};
  int index{0};
};
