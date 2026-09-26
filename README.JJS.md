# Kodi JJS – gapless Dolby TrueHD / Atmos playback

Kodi JJS is a custom Kodi 21.3 Omega fork focused on seamless, gapless playback of
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

- **Manual PAPlayer transition lifecycle fix (JJS.004)**  
  Manual track changes could leave the previous PAPlayer stream active while the
  newly selected item was being prepared. Depending on timing, this could either
  make AudioEngine reject the new stream and skip through several tracks, or leave
  the new track displayed with a stale elapsed time while no audio started.

  Kodi's PAPlayer uses an asynchronous queue job plus a worker thread gated by
  `m_startEvent`. During repeated manual transitions, a start signal from the
  previous PAPlayer run could remain pending after that thread had already passed
  its initial wait. A newly created PAPlayer thread could then consume the stale
  signal and enter before the new `OpenFile()` call had set `m_isPlaying=true`;
  the thread exited immediately, even though the new decoder and AudioEngine stream
  were subsequently prepared successfully.

  JJS.004 makes manual/select-item transitions explicitly stop the previous
  PAPlayer worker, wait for outstanding queue jobs and close the old streams before
  opening the replacement item. When a PAPlayer worker must be recreated,
  `m_startEvent.Reset()` clears any stale start signal before `Create()`. The
  normal compatible RAW-to-RAW seamless handover remains unchanged.

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

Current development branch: **21.3-Omega-jjs**

## Search terms

Kodi Atmos gapless, Kodi TrueHD gapless, Dolby Atmos gapless playback,
TrueHD seamless playback, MAT seamless transition, RAW passthrough,
Android TV, NVIDIA Shield, Kodi AudioEngine.

## Upstream

Kodi is an open-source media center developed by Team Kodi. This repository is a
fork of the official Kodi source tree and remains under Kodi's GPL licensing.
