# Kodi JJS – gapless Dolby TrueHD / Atmos playback

Kodi JJS is a custom Kodi 21.2 Omega fork focused on seamless, gapless playback of
Dolby TrueHD / Dolby Atmos material when Kodi uses RAW passthrough.

The goal is to keep the existing AudioEngine / HDMI passthrough stream alive at a
track boundary instead of tearing it down and opening a new stream. This avoids
the normal receiver / HDMI resynchronization gap between compatible consecutive
tracks.

## JJS audio changes

- **Seamless RAW handover in PAPlayer**  
  The next compatible RAW decoder is prepared before the current track ends.
  At the boundary, Kodi transfers the already-running AudioEngine stream to the
  successor decoder instead of draining and recreating it. Potentially blocking
  decoder teardown and playback callbacks are moved away from the PAPlayer audio
  thread.

- **TrueHD MAT seamless-branch handling**  
  TrueHD output-timing discontinuities are handled as seamless branch points.
  MAT padding and timing alignment are carried forward instead of allowing the
  discontinuity to grow into a MAT packer reset / dropped audio. This is based on
  the newer Kodi upstream MAT handling and the LAV Filters design credited in the
  source.

- **Chapter / end-offset calculation fix**  
  The upstream Kodi fix for chaptered audio ending too early is included as a
  separate change.

- **Kodi-compatible failure fallback**  
  A RAW source is retained at a clean track boundary only while its existing
  AudioEngine buffer still contains audio. If an asynchronous successor is not
  ready before that buffer drains — for example because an SMB open/read stalls —
  Kodi JJS abandons the seamless handover and continues through Kodi's normal
  stream-finish/error path. Kodi's network retries and timeouts are not changed.

## Android identity

Android builds from this fork use:

- App name: **Kodi JJS**
- Package: **org.jjs.kodi**

This allows Kodi JJS to be installed independently from the official
`org.xbmc.kodi` application. Official Kodi updates therefore do not overwrite
this fork.

## Source and portability

The audio changes are in Kodi core code and are not hard-coded to NVIDIA Shield.
The same source tree can be used for Android ARM64 and as the Kodi source for
other build systems such as LibreELEC.

Current development branch: **21.2-Omega-jjs**

## Search terms

Kodi Atmos gapless, Kodi TrueHD gapless, Dolby Atmos gapless playback,
TrueHD seamless playback, MAT seamless transition, RAW passthrough,
Android TV, NVIDIA Shield, Kodi AudioEngine.

## Upstream

Kodi is an open-source media center developed by Team Kodi. This repository is a
fork of the official Kodi source tree and remains under Kodi's GPL licensing.
