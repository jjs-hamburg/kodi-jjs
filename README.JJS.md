# Kodi JJS – gapless Dolby TrueHD / Atmos playback and targeted Kodi core fixes

Kodi JJS is a custom Kodi 21.3 Omega fork focused on seamless, gapless playback of
Dolby TrueHD / Dolby Atmos material when Kodi uses RAW passthrough.

The project also carries a small set of targeted Kodi core fixes found while using
and testing this fork. They are listed below so it is clear which behaviour differs
from stock Kodi and why.

## Kodi issues fixed in this fork

### Audio playback / PAPlayer

- **HDMI resynchronization gaps between compatible RAW tracks**  
  Standard Kodi can tear down and recreate the RAW AudioEngine stream at a track
  boundary even when the successor uses the same compatible output format. Kodi
  JJS prepares the successor in advance and hands the already-running AudioEngine
  stream over instead. This avoids the receiver / HDMI resynchronization gap.

- **TrueHD MAT discontinuity at seamless branch points**  
  TrueHD output timing can restart at a seamless branch. Stock handling can let
  that discontinuity grow into a MAT reset / dropped audio. Kodi JJS preserves the
  MAT timing relationship and carries pending padding forward across the branch.
  The implementation is based on the newer Kodi MAT handling and the LAV Filters
  design credited in the source.

- **RAW passthrough backlog lost at decoder EOF**  
  The passthrough parser can still own complete RAW/MAT output when the demuxer
  itself has already reached EOF. Kodi JJS drains that pending parser backlog before
  reporting EOF so the handover inherits the real end of the source.

- **Manual Next / Previous and incompatible audio-format transition races**  
  Manual transitions could overlap asynchronous preparation with PAPlayer shutdown
  and leave stale or orphaned AudioEngine streams behind. Kodi JJS waits for pending
  queue jobs before final stream cleanup and uses Kodi's normal reopen path only for
  a real output-format change.

- **PAPlayer restart with a stale start event**  
  A completed PAPlayer thread could leave its start event signaled and make a manual
  restart race with the newly created player thread. Kodi JJS resets that event
  before restarting the thread.

- **Playback callback blocked by audio file-state housekeeping**  
  Saving the outgoing audio file state held the application stack lock while
  potentially slow database housekeeping ran. The successor's playback callback
  needs the same lock. Kodi JJS releases that lock first for audio-only playback so
  transition callbacks are not stalled by unrelated database work.

- **Chapter / end-offset calculation**  
  The upstream Kodi fix for chaptered audio ending too early is included as a
  separate change.

- **Failure fallback remains Kodi-compatible**  
  A RAW source is retained at a clean track boundary only while its existing
  AudioEngine buffer still contains audio. If an asynchronous successor is not
  ready before that buffer drains — for example because an SMB open/read stalls —
  Kodi JJS abandons the seamless handover and continues through Kodi's normal
  stream-finish/error path. Kodi's network retries and timeouts are not changed.

### Music library

- **False music tag-rescan prompt after database open failure (JJS.006)**  
  GetMusicNeedsTagScan() can return a negative error value when the music database
  could not be opened. Stock Kodi treated every non-zero result as "tag scan
  required". Kodi JJS prompts only when the returned value is actually positive.

### Progress handling – JJS.008


- **Cancelling a music-library update can leave the progress operation alive**  
  CMusicInfoScanner starts a parallel MusicFileCounter for the percentage
  calculation. Its recursive counting path checked the stop flag too late and not
  at every loop level, so cancelling a large network library could leave
  Process() waiting in StopThread() before it reached the normal MarkFinished()
  cleanup. Kodi JJS checks the stop flag before new directory reads and inside the
  counting loops so cancellation can unwind promptly.

- **A destroyed Python DialogProgressBG can close unrelated progress displays**  
  Kodi's extended progress window is shared by multiple background-progress
  handles. The Python object's destructor closed that common window instead of
  finishing only its own handle. Other active progress operations could therefore
  disappear and only become visible again when another operation reopened the
  shared window. Kodi JJS marks only the object's own handle finished.

## Android identity

Android builds from this fork use:

- App name: **Kodi JJS**
- Package: **org.jjs.kodi**

This allows Kodi JJS to be installed independently from the official
`org.xbmc.kodi` application. Official Kodi updates therefore do not overwrite
this fork.

## Source and portability

The audio and core fixes are not hard-coded to NVIDIA Shield. The same source tree
can be used for Android ARM64 and as the Kodi source for other build systems such
as LibreELEC.

Current development branch: **21.3-Omega-jjs**

## Search terms

Kodi Atmos gapless, Kodi TrueHD gapless, Dolby Atmos gapless playback,
TrueHD seamless playback, MAT seamless transition, RAW passthrough,
Kodi progress dialog, DialogProgressBG, music library scan cancel,
Android TV, NVIDIA Shield, Kodi AudioEngine.

## Upstream

Kodi is an open-source media center developed by Team Kodi. This repository is a
fork of the official Kodi source tree and remains under Kodi's GPL licensing.
