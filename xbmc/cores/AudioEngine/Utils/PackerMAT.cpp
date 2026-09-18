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

#include "PackerMAT.h"

#include "utils/log.h"

#include <algorithm>
#include <array>
#include <assert.h>
#include <atomic>
#include <utility>

extern "C"
{
#include <libavutil/common.h>
#include <libavutil/intreadwrite.h>
}

namespace
{
constexpr uint32_t FORMAT_MAJOR_SYNC = 0xf8726fba;

constexpr auto BURST_HEADER_SIZE = 8;
constexpr auto MAT_BUFFER_SIZE = 61440;
constexpr auto MAT_BUFFER_LIMIT = MAT_BUFFER_SIZE - 24; // MAT end code size
constexpr auto MAT_POS_MIDDLE = 30708 + BURST_HEADER_SIZE; // middle point + IEC header in front

// magic MAT format values, meaning is unknown at this point
constexpr std::array<uint8_t, 20> mat_start_code = {0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01,
                                                    0x01, 0x80, 0x00, 0x56, 0xA5, 0x3B, 0xF4,
                                                    0x81, 0x83, 0x49, 0x80, 0x77, 0xE0};

constexpr std::array<uint8_t, 12> mat_middle_code = {0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA,
                                                     0x82, 0x83, 0x49, 0x80, 0x77, 0xE0};

constexpr std::array<uint8_t, 24> mat_end_code = {0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

constexpr std::size_t TRUEHD_TRACE_RING_SIZE = 16384;

struct TrueHDTraceEvent
{
  uint64_t sequence{0};
  CTrueHDTrace::Stage stage{CTrueHDTrace::Stage::DEMUX};
  uint32_t streamId{0};
  uint64_t itemId{0};
  uint32_t size{0};
  uint64_t hash{0};
  int64_t value1{0};
  int64_t value2{0};
  int64_t value3{0};
  int64_t value4{0};
};

struct TrueHDTraceSlot
{
  std::atomic<uint64_t> committed{0};
  TrueHDTraceEvent event{};
};

std::array<TrueHDTraceSlot, TRUEHD_TRACE_RING_SIZE> g_trueHDTrace;
std::atomic<uint64_t> g_trueHDTraceSequence{0};
std::atomic<uint32_t> g_trueHDTraceStreamId{0};

const char* TrueHDTraceStageName(CTrueHDTrace::Stage stage)
{
  switch (stage)
  {
    case CTrueHDTrace::Stage::DEMUX:
      return "DEMUX";
    case CTrueHDTrace::Stage::DEMUX_EOF:
      return "DEMUX_EOF";
    case CTrueHDTrace::Stage::PACK_IN:
      return "PACK_IN";
    case CTrueHDTrace::Stage::PACK_SKIP_NOSYNC:
      return "PACK_SKIP_NOSYNC";
    case CTrueHDTrace::Stage::MAT_FLUSH:
      return "MAT_FLUSH";
    case CTrueHDTrace::Stage::MAT_POP:
      return "MAT_POP";
    case CTrueHDTrace::Stage::MAT_TRANSFER:
      return "MAT_TRANSFER";
    case CTrueHDTrace::Stage::READRAW_OUT:
      return "READRAW_OUT";
    case CTrueHDTrace::Stage::READRAW_EMPTY:
      return "READRAW_EMPTY";
    case CTrueHDTrace::Stage::DECODER_FILL:
      return "DECODER_FILL";
    case CTrueHDTrace::Stage::DECODER_TAKE:
      return "DECODER_TAKE";
    case CTrueHDTrace::Stage::DECODER_DISCARD:
      return "DECODER_DISCARD";
    case CTrueHDTrace::Stage::DECODER_EOF:
      return "DECODER_EOF";
    case CTrueHDTrace::Stage::AE_ADD:
      return "AE_ADD";
    case CTrueHDTrace::Stage::SEEK:
      return "SEEK";
    case CTrueHDTrace::Stage::HANDOVER_BEGIN:
      return "HANDOVER_BEGIN";
    case CTrueHDTrace::Stage::HANDOVER_END:
      return "HANDOVER_END";
  }
  return "UNKNOWN";
}
} // namespace

uint32_t CTrueHDTrace::RegisterStream()
{
  return g_trueHDTraceStreamId.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t CTrueHDTrace::Hash(const uint8_t* data, std::size_t size)
{
  if (!data || size == 0)
    return 0;

  uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t CTrueHDTrace::Record(Stage stage,
                              uint32_t streamId,
                              uint64_t itemId,
                              uint32_t size,
                              uint64_t hash,
                              int64_t value1,
                              int64_t value2,
                              int64_t value3,
                              int64_t value4)
{
  const uint64_t sequence =
      g_trueHDTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  TrueHDTraceSlot& slot = g_trueHDTrace[sequence % TRUEHD_TRACE_RING_SIZE];
  slot.committed.store(0, std::memory_order_relaxed);
  slot.event = {sequence, stage, streamId, itemId, size, hash, value1, value2, value3, value4};
  slot.committed.store(sequence, std::memory_order_release);
  return sequence;
}

void CTrueHDTrace::DumpAround(uint64_t centerSequence,
                              uint64_t eventsBefore,
                              uint64_t eventsAfter)
{
  if (centerSequence == 0)
    return;

  const uint64_t first = centerSequence > eventsBefore ? centerSequence - eventsBefore : 1;
  const uint64_t lastRequested = centerSequence + eventsAfter;
  const uint64_t lastRecorded = g_trueHDTraceSequence.load(std::memory_order_acquire);
  const uint64_t last = std::min(lastRequested, lastRecorded);

  CLog::Log(LOGINFO,
            "TRUEHDTRACE BEGIN center={} range={}..{} last-recorded={}",
            centerSequence, first, last, lastRecorded);

  for (uint64_t sequence = first; sequence <= last; ++sequence)
  {
    TrueHDTraceSlot& slot = g_trueHDTrace[sequence % TRUEHD_TRACE_RING_SIZE];
    if (slot.committed.load(std::memory_order_acquire) != sequence)
      continue;

    const TrueHDTraceEvent event = slot.event;
    if (slot.committed.load(std::memory_order_acquire) != sequence)
      continue;

    CLog::Log(LOGINFO,
              "TRUEHDTRACE seq={} stage={} stream={} item={} size={} hash={:016x} "
              "v1={} v2={} v3={} v4={}",
              event.sequence, TrueHDTraceStageName(event.stage), event.streamId,
              event.itemId, event.size, event.hash, event.value1, event.value2,
              event.value3, event.value4);
  }

  CLog::Log(LOGINFO, "TRUEHDTRACE END center={}", centerSequence);
}

CPackerMAT::CPackerMAT()
{
  m_buffer.reserve(MAT_BUFFER_SIZE);
}

// On a high level, a MAT frame consists of a sequence of padded TrueHD frames
// The size of the padded frame can be determined from the frame time/sequence code in the frame header,
// since it varies to accommodate spikes in bitrate.
// In average all frames are always padded to 2560 bytes, so that 24 frames fit in one MAT frame, however
// due to bitrate spikes single sync frames have been observed to use up to twice that size, in which
// case they'll be preceded by smaller frames to keep the average bitrate constant.
// A constant padding to 2560 bytes can work (this is how the ffmpeg spdifenc module works), however
// high-bitrate streams can overshoot this size and therefor require proper handling of dynamic padding.
bool CPackerMAT::PackTrueHD(const uint8_t* data, int size)
{
  // Too small to contain the TrueHD timing/sync fields used below.
  if (size < 10)
    return false;

  TrueHDMajorSyncInfo info;
  const bool isMajorSync = (AV_RB32(data + 4) == FORMAT_MAJOR_SYNC);
  const uint16_t frameTime = AV_RB16(data + 2);
  const uint64_t traceFrameSeq = ++m_traceFrameSeq;
  const uint64_t traceHash =
      m_traceStreamId ? CTrueHDTrace::Hash(data, static_cast<std::size_t>(size)) : 0;

  // Get ratebits and output timing from the sync frame. If the extended
  // header parse fails, keep the frame and fall back to the basic ratebits
  // field; seamless-branch detection is simply unavailable for that frame.
  if (isMajorSync)
  {
    info = ParseTrueHDMajorSyncHeaders(data, size);
    m_state.ratebits = info.valid ? info.ratebits : (data[8] >> 4);
  }
  else if (m_state.prevFrametimeValid == false)
  {
    if (m_traceStreamId)
    {
      CTrueHDTrace::Record(CTrueHDTrace::Stage::PACK_SKIP_NOSYNC, m_traceStreamId,
                           traceFrameSeq, static_cast<uint32_t>(size), traceHash,
                           frameTime, -1, GetCount(), m_state.padding);
    }
    // only start streaming on a major sync frame
    m_state.numberOfSamplesOffset = 0;
    return false;
  }

  uint32_t spaceSize = 0;
  const uint16_t frameSamples = 40 << (m_state.ratebits & 7);
  if (m_traceStreamId)
  {
    CTrueHDTrace::Record(CTrueHDTrace::Stage::PACK_IN, m_traceStreamId, traceFrameSeq,
                         static_cast<uint32_t>(size), traceHash, frameTime,
                         info.outputTimingPresent ? info.outputTiming : -1, GetCount(),
                         m_state.padding);
  }
  m_state.outputTiming += frameSamples;

  if (info.outputTimingPresent)
  {
    if (m_state.outputTimingValid && (info.outputTiming != m_state.outputTiming))
    {
      CLog::Log(LOGINFO,
                "CPackerMAT::PackTrueHD: seamless branch detected -> output timing "
                "expected: {}, found: {}",
                m_state.outputTiming, info.outputTiming);

      // At a TrueHD seamless branch the frame timing restarts. Preserve the
      // existing MAT stream and carry the required padding forward instead of
      // letting the discontinuity grow into a packer reset/drop.
      m_state.prevFrametimeValid = false;
      spaceSize = frameSamples * (64 >> (m_state.ratebits & 7));

      uint32_t prevOutput = static_cast<uint16_t>(info.outputTiming - frameSamples);
      if (prevOutput < frameTime)
        prevOutput += 0x10000u;

      const int32_t currentFrameOutputOffset =
          static_cast<int32_t>(prevOutput - frameTime);

      if (m_state.nOutputTimeOffset >= currentFrameOutputOffset)
      {
        m_state.padding +=
            (m_state.nOutputTimeOffset - currentFrameOutputOffset) *
            (64 >> (m_state.ratebits & 7));
      }

      CLog::Log(LOGINFO,
                "CPackerMAT::PackTrueHD: seamless branch carrying forward {} bytes padding "
                "(offset {} -> {})",
                m_state.padding, m_state.nOutputTimeOffset, currentFrameOutputOffset);
    }
    m_state.outputTiming = info.outputTiming;
    m_state.outputTimingValid = true;
  }

  // compute final padded size for the previous frame, if any
  if (m_state.prevFrametimeValid)
    spaceSize = uint16_t(frameTime - m_state.prevFrametime) * (64 >> (m_state.ratebits & 7));

  // compute padding (ie. difference to the size of the previous frame)
  assert(!m_state.prevFrametimeValid || spaceSize >= m_state.prevMatFramesize);

  // if for some reason the spaceSize fails, align the actual frame size
  if (spaceSize < m_state.prevMatFramesize)
    spaceSize = FFALIGN(m_state.prevMatFramesize, (64 >> (m_state.ratebits & 7)));

  m_state.padding += (spaceSize - m_state.prevMatFramesize);

  // detect seeks and re-initialize internal state i.e. skip stream
  // until the next major sync frame
  if (m_state.padding > MAT_BUFFER_SIZE * 5)
  {
    CLog::Log(LOGINFO, "CPackerMAT::PackTrueHD: seek detected, re-initializing MAT packer state");
    m_state = {};
    m_state.init = true;
    m_buffer.clear();
    m_bufferCount = 0;
    return false;
  }

  // Remember the relation between TrueHD frame time and output timing. At a
  // later seamless branch this lets us preserve the MAT slot alignment.
  if (m_state.outputTimingValid)
  {
    uint32_t prevOutput = static_cast<uint16_t>(m_state.outputTiming - frameSamples);
    if (prevOutput < frameTime)
      prevOutput += 0x10000u;

    m_state.nOutputTimeOffset = static_cast<int32_t>(prevOutput - frameTime);
  }

  // store frame time of the previous frame
  m_state.prevFrametime = frameTime;
  m_state.prevFrametimeValid = true;

  // Write the MAT header into the fresh buffer
  if (GetCount() == 0)
  {
    WriteHeader();

    // initial header, don't count it for the frame size
    if (m_state.init == false)
    {
      m_state.init = true;
      m_state.matFramesize = 0;
    }
  }

  // write padding of the previous frame (if any)
  while (m_state.padding > 0)
  {
    WritePadding();

    assert(m_state.padding == 0 || GetCount() == MAT_BUFFER_SIZE);

    // Buffer is full, submit it
    if (GetCount() == MAT_BUFFER_SIZE)
    {
      FlushPacket();

      // and setup a new buffer
      WriteHeader();
    }
  }

  // count the number of samples in this frame
  m_state.samples += frameSamples;

  // write actual audio data to the buffer
  if (m_traceMatFirstFrameSeq == 0)
    m_traceMatFirstFrameSeq = traceFrameSeq;
  m_traceMatLastFrameSeq = traceFrameSeq;
  int remaining = FillDataBuffer(data, size, Type::DATA);

  // not all data could be written, or the buffer is full
  if (remaining || GetCount() == MAT_BUFFER_SIZE)
  {
    // flush out old data
    FlushPacket();

    if (remaining)
    {
      // setup a new buffer
      WriteHeader();
      m_traceMatFirstFrameSeq = traceFrameSeq;
      m_traceMatLastFrameSeq = traceFrameSeq;

      // and write the remaining data
      remaining = FillDataBuffer(data + (size - remaining), remaining, Type::DATA);

      assert(remaining == 0);
    }
  }

  // store the size of the current MAT frame, so we can add padding later
  m_state.prevMatFramesize = m_state.matFramesize;
  m_state.matFramesize = 0;

  // return true if have MAT packet
  return !m_outputQueue.empty();
}

std::vector<uint8_t> CPackerMAT::GetOutputFrame()
{
  std::vector<uint8_t> buffer;

  if (m_outputQueue.empty())
    return {};

  TraceQueuedMAT traceMeta;
  if (!m_traceOutputQueue.empty())
  {
    traceMeta = m_traceOutputQueue.front();
    m_traceOutputQueue.pop_front();
  }

  buffer = std::move(m_outputQueue.front());
  m_outputQueue.pop_front();

  if (traceMeta.streamId)
  {
    CTrueHDTrace::Record(CTrueHDTrace::Stage::MAT_POP, traceMeta.streamId,
                         traceMeta.serial, static_cast<uint32_t>(buffer.size()),
                         CTrueHDTrace::Hash(buffer.data(), buffer.size()),
                         static_cast<int64_t>(m_outputQueue.size()));
  }

  return buffer;
}

void CPackerMAT::WriteHeader()
{
  m_buffer.resize(MAT_BUFFER_SIZE);

  // reserve size for the IEC header and the MAT start code
  const size_t size = BURST_HEADER_SIZE + mat_start_code.size();

  // write MAT start code. IEC header written later, skip space only
  memcpy(m_buffer.data() + BURST_HEADER_SIZE, mat_start_code.data(), mat_start_code.size());
  m_bufferCount = size;

  // unless the start code falls into the padding, it's considered part of the current MAT frame
  // Note that audio frames are not always aligned with MAT frames, so we might already have a partial
  // frame at this point
  m_state.matFramesize += size;

  // The MAT metadata counts as padding, if we're scheduled to write any, which mean the start bytes
  // should reduce any further padding.
  if (m_state.padding > 0)
  {
    // if the header fits into the padding of the last frame, just reduce the amount of needed padding
    if (m_state.padding > size)
    {
      m_state.padding -= size;
      m_state.matFramesize = 0;
    }
    else
    {
      // otherwise, consume all padding and set the size of the next MAT frame to the remaining data
      m_state.matFramesize = (size - m_state.padding);
      m_state.padding = 0;
    }
  }
}

void CPackerMAT::WritePadding()
{
  if (m_state.padding == 0)
    return;

  // for padding not writes any data (nullptr) as buffer is already zeroed
  // only counts/skip bytes
  const int remaining = FillDataBuffer(nullptr, m_state.padding, Type::PADDING);

  // not all padding could be written to the buffer, write it later
  if (remaining >= 0)
  {
    m_state.padding = remaining;
    m_state.matFramesize = 0;
  }
  else
  {
    // more padding then requested was written, eg. there was a MAT middle/end marker
    // that needed to be written
    m_state.padding = 0;
    m_state.matFramesize = -remaining;
  }
}

void CPackerMAT::AppendData(const uint8_t* data, int size, Type type)
{
  // for padding not write anything, only skip bytes
  if (type == Type::DATA)
    memcpy(m_buffer.data() + m_bufferCount, data, size);

  m_state.matFramesize += size;
  m_bufferCount += size;
}

int CPackerMAT::FillDataBuffer(const uint8_t* data, int size, Type type)
{
  if (GetCount() >= MAT_BUFFER_LIMIT)
    return size;

  int remaining = size;

  // Write MAT middle marker, if needed
  // The MAT middle marker always needs to be in the exact same spot, any audio data will be split.
  // If we're currently writing padding, then the marker will be considered as padding data and
  // reduce the amount of padding still required.
  if (GetCount() <= MAT_POS_MIDDLE && GetCount() + size > MAT_POS_MIDDLE)
  {
    // write as much data before the middle code as we can
    int nBytesBefore = MAT_POS_MIDDLE - GetCount();
    AppendData(data, nBytesBefore, type);
    remaining -= nBytesBefore;

    // write the MAT middle code
    AppendData(mat_middle_code.data(), mat_middle_code.size(), Type::DATA);

    // if we're writing padding, deduct the size of the code from it
    if (type == Type::PADDING)
      remaining -= mat_middle_code.size();

    // write remaining data after the MAT marker. For padding, data is nullptr;
    // pointer arithmetic on nullptr is undefined, so keep it nullptr.
    if (remaining > 0)
      remaining = FillDataBuffer(data ? data + nBytesBefore : nullptr, remaining, type);

    return remaining;
  }

  // not enough room in the buffer to write all the data,
  // write as much as we can and add the MAT footer
  if (GetCount() + size >= MAT_BUFFER_LIMIT)
  {
    // write as much data before the middle code as we can
    int nBytesBefore = MAT_BUFFER_LIMIT - GetCount();
    AppendData(data, nBytesBefore, type);
    remaining -= nBytesBefore;

    // write the MAT end code
    AppendData(mat_end_code.data(), mat_end_code.size(), Type::DATA);

    assert(GetCount() == MAT_BUFFER_SIZE);

    // MAT markers don't displace padding, so reduce the amount of padding
    if (type == Type::PADDING)
      remaining -= mat_end_code.size();

    // any remaining data will be written in future calls
    return remaining;
  }

  AppendData(data, size, type);

  return 0;
}

void CPackerMAT::FlushPacket()
{
  if (GetCount() == 0)
    return;

  assert(GetCount() == MAT_BUFFER_SIZE);

  // normal number of samples per frame
  const uint16_t frameSamples = 40 << (m_state.ratebits & 7);
  const uint32_t MATSamples = (frameSamples * 24);

  const uint64_t traceMatSerial = ++m_traceMatSeq;
  if (m_traceStreamId)
  {
    const uint64_t traceHash = CTrueHDTrace::Hash(m_buffer.data(), m_buffer.size());
    CTrueHDTrace::Record(CTrueHDTrace::Stage::MAT_FLUSH, m_traceStreamId,
                         traceMatSerial, static_cast<uint32_t>(m_buffer.size()), traceHash,
                         static_cast<int64_t>(m_traceMatFirstFrameSeq),
                         static_cast<int64_t>(m_traceMatLastFrameSeq), m_state.samples,
                         m_state.numberOfSamplesOffset);
  }

  // push MAT packet to output queue
  m_outputQueue.emplace_back(std::move(m_buffer));
  m_traceOutputQueue.push_back({traceMatSerial, m_traceStreamId});

  // we expect 24 frames per MAT frame, so calculate an offset from that
  // this is done after delivery, because it modifies the duration of the frame,
  //  eg. the start of the next frame
  if (MATSamples != m_state.samples)
    m_state.numberOfSamplesOffset += m_state.samples - MATSamples;

  m_state.samples = 0;

  m_buffer.clear();
  m_bufferCount = 0;
  m_traceMatFirstFrameSeq = 0;
  m_traceMatLastFrameSeq = 0;
}

TrueHDMajorSyncInfo CPackerMAT::ParseTrueHDMajorSyncHeaders(const uint8_t* p, int buffsize) const
{
  TrueHDMajorSyncInfo info;

  if (buffsize < 32)
    return {};

  // parse major sync and look for a restart header
  int majorSyncSize = 28;
  if (p[29] & 1) // restart header exists
  {
    int extensionSize = p[30] >> 4; // calculate headers size
    majorSyncSize += 2 + extensionSize * 2;
  }

  if (majorSyncSize > buffsize)
    return {};

  CBitStream bs(p + 4, buffsize - 4);

  bs.SkipBits(32); // format_sync

  info.ratebits = bs.ReadBits(4); // ratebits
  info.valid = true;

  //  (1) 6ch_multichannel_type
  //  (1) 8ch_multichannel_type
  //  (2) reserved
  //  (2) 2ch_presentation_channel_modifier
  //  (2) 6ch_presentation_channel_modifier
  //  (5) 6ch_presentation_channel_assignment
  //  (2) 8ch_presentation_channel_modifier
  // (13) 8ch_presentation_channel_assignment
  // (16) signature
  // (16) flags
  // (16) reserved
  //  (1) variable_rate
  // (15) peak_data_rate
  bs.SkipBits(1 + 1 + 2 + 2 + 2 + 5 + 2 + 13 + 16 + 16 + 16 + 1 + 15);

  const int numSubstreams = bs.ReadBits(4);

  bs.SkipBits(4 + (majorSyncSize - 17) * 8);

  // substream directory
  for (int i = 0; i < numSubstreams; i++)
  {
    int extraSubstreamWord = bs.ReadBits(1);
    //  (1) restart_nonexistent
    //  (1) crc_present
    //  (1) reserved
    // (12) substream_end_ptr
    bs.SkipBits(15);
    if (extraSubstreamWord)
      bs.SkipBits(16); // drc_gain_update, drc_time_update, reserved
  }

  // substream segments
  for (int i = 0; i < numSubstreams; i++)
  {
    if (bs.ReadBits(1))
    { // block_header_exists
      if (bs.ReadBits(1))
      { // restart_header_exists
        bs.SkipBits(14); // restart_sync_word
        info.outputTiming = bs.ReadBits(16);
        info.outputTimingPresent = true;
        // XXX: restart header
      }
      // XXX: Block header
    }
    // XXX: All blocks, all substreams?
    break;
  }

  return info;
}
