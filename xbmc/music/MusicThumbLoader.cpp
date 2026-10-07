/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MusicThumbLoader.h"

#include "FileItem.h"
#include "MusicDatabase.h"
#include "TextureDatabase.h"
#include "dbwrappers/dataset.h"
#include "music/infoscanner/MusicInfoScanner.h"
#include "music/tags/MusicInfoTag.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "video/VideoThumbLoader.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>
#include <vector>

using namespace MUSIC_INFO;

namespace
{
using LibraryArtBatch = std::map<int, std::vector<ArtForThumbLoader>>;
using PathArtBatch = std::map<std::string, std::map<std::string, std::string>>;

std::string BuildIdList(const std::set<int>& ids)
{
  std::string result;
  bool first = true;
  for (const int id : ids)
  {
    if (!first)
      result += ",";
    result += std::to_string(id);
    first = false;
  }
  return result;
}

class CMusicThumbLoaderDatabase : public CMusicDatabase
{
public:
  bool GetArtistArtBatch(const std::set<int>& artistIds, LibraryArtBatch& art)
  {
    if (artistIds.empty())
      return true;

    const std::string ids = BuildIdList(artistIds);
    const std::string sql =
        "SELECT art.media_id AS item_id, art.art_id AS art_id, art.media_type AS media_type, "
        "art.type AS type, '' AS prefix, art.url AS url, 0 AS iorder "
        "FROM art WHERE art.media_type='artist' AND art.media_id IN (" +
        ids + ")";
    return GetArtBatch(sql, art);
  }

  bool GetAlbumArtBatch(const std::set<int>& albumIds, LibraryArtBatch& art)
  {
    if (albumIds.empty())
      return true;

    const std::string ids = BuildIdList(albumIds);
    const std::string sql =
        "SELECT art.media_id AS item_id, art.art_id AS art_id, art.media_type AS media_type, "
        "art.type AS type, '' AS prefix, art.url AS url, 0 AS iorder "
        "FROM art WHERE art.media_type='album' AND art.media_id IN (" +
        ids + ") "
              "UNION "
              "SELECT album_artist.idAlbum AS item_id, art.art_id AS art_id, "
              "art.media_type AS media_type, art.type AS type, 'albumartist' AS prefix, "
              "art.url AS url, album_artist.iOrder AS iorder "
              "FROM art JOIN album_artist ON art.media_id=album_artist.idArtist "
              "AND art.media_type='artist' WHERE album_artist.idAlbum IN (" +
        ids + ")";
    return GetArtBatch(sql, art);
  }

  bool GetSongArtBatch(const std::set<int>& songIds, LibraryArtBatch& art)
  {
    if (songIds.empty())
      return true;

    const std::string ids = BuildIdList(songIds);
    const std::string sql =
        "SELECT art.media_id AS item_id, art.art_id AS art_id, art.media_type AS media_type, "
        "art.type AS type, '' AS prefix, art.url AS url, 0 AS iorder "
        "FROM art WHERE art.media_type='song' AND art.media_id IN (" +
        ids + ") "
              "UNION "
              "SELECT song.idSong AS item_id, art.art_id AS art_id, art.media_type AS media_type, "
              "art.type AS type, '' AS prefix, art.url AS url, 0 AS iorder "
              "FROM art JOIN song ON art.media_id=song.idAlbum AND art.media_type='album' "
              "WHERE song.idSong IN (" +
        ids + ") "
              "UNION "
              "SELECT song.idSong AS item_id, art.art_id AS art_id, art.media_type AS media_type, "
              "art.type AS type, 'albumartist' AS prefix, art.url AS url, "
              "album_artist.iOrder AS iorder "
              "FROM song JOIN album_artist ON song.idAlbum=album_artist.idAlbum "
              "JOIN art ON art.media_id=album_artist.idArtist AND art.media_type='artist' "
              "WHERE song.idSong IN (" +
        ids + ") "
              "UNION "
              "SELECT song_artist.idSong AS item_id, art.art_id AS art_id, "
              "art.media_type AS media_type, art.type AS type, 'artist' AS prefix, "
              "art.url AS url, song_artist.iOrder AS iorder "
              "FROM song_artist JOIN art ON art.media_id=song_artist.idArtist "
              "AND art.media_type='artist' WHERE song_artist.idRole=" +
        std::to_string(ROLE_ARTIST) + " AND song_artist.idSong IN (" + ids + ")";
    return GetArtBatch(sql, art);
  }

private:
  bool GetArtBatch(const std::string& sql, LibraryArtBatch& art)
  {
    if (!m_pDB || !m_pDS2)
      return false;

    try
    {
      if (!m_pDS2->query(sql))
      {
        m_pDS2->close();
        return false;
      }

      while (!m_pDS2->eof())
      {
        ArtForThumbLoader artitem;
        artitem.artType = m_pDS2->fv("type").get_asString();
        artitem.mediaType = m_pDS2->fv("media_type").get_asString();
        artitem.prefix = m_pDS2->fv("prefix").get_asString();
        artitem.url = m_pDS2->fv("url").get_asString();
        const int iOrder = m_pDS2->fv("iorder").get_asInt();
        if (iOrder > 0)
          artitem.prefix += m_pDS2->fv("iorder").get_asString();

        art[m_pDS2->fv("item_id").get_asInt()].emplace_back(std::move(artitem));
        m_pDS2->next();
      }
      m_pDS2->close();
      return true;
    }
    catch (...)
    {
      m_pDS2->close();
    }
    return false;
  }
};

class CMusicThumbLoaderTextureDatabase : public CTextureDatabase
{
public:
  bool GetPathArtBatch(const std::set<std::string>& paths, PathArtBatch& art)
  {
    if (paths.empty())
      return true;
    if (!m_pDB || !m_pDS)
      return false;

    try
    {
      std::string sql =
          "SELECT url, type, texture FROM path WHERE type IN ('thumb','fanart') AND url IN (";
      bool first = true;
      for (const auto& path : paths)
      {
        if (!first)
          sql += ",";
        sql += PrepareSQL("'%s'", path.c_str());
        first = false;
      }
      sql += ")";

      if (!m_pDS->query(sql))
      {
        m_pDS->close();
        return false;
      }

      while (!m_pDS->eof())
      {
        art[m_pDS->fv("url").get_asString()][m_pDS->fv("type").get_asString()] =
            m_pDS->fv("texture").get_asString();
        m_pDS->next();
      }
      m_pDS->close();
      return true;
    }
    catch (...)
    {
      m_pDS->close();
    }
    return false;
  }
};

bool ApplyLibraryArt(CFileItem& item, const std::vector<ArtForThumbLoader>& art)
{
  if (art.empty() || !item.HasMusicInfoTag())
    return false;

  CMusicInfoTag& tag = *item.GetMusicInfoTag();
  std::string fanartfallback;
  std::string artname;
  std::map<std::string, std::string> artmap;
  std::map<std::string, std::string> discartmap;
  for (auto artitem : art)
  {
    /* Add art to artmap, naming according to media type.
    For example: artists have "thumb", "fanart", "poster" etc.,
    albums have "thumb", "artist.thumb", "artist.fanart",... "artist1.thumb", "artist1.fanart" etc.,
    songs have "thumb", "album.thumb", "artist.thumb", "albumartist.thumb", "albumartist1.thumb" etc.
    */
    if (tag.GetType() == artitem.mediaType)
      artname = artitem.artType;
    else if (artitem.prefix.empty())
      artname = artitem.mediaType + "." + artitem.artType;
    else
    {
      if (tag.GetType() == MediaTypeAlbum)
        StringUtils::Replace(artitem.prefix, "albumartist", "artist");
      artname = artitem.prefix + "." + artitem.artType;
    }

    // Pull out album art for this specific disc e.g. "thumb2", skip art for other discs
    if (artitem.mediaType == MediaTypeAlbum && tag.GetDiscNumber() > 0)
    {
      // Find any trailing digits
      size_t startnum = artitem.artType.find_last_not_of("0123456789");
      std::string digits = artitem.artType.substr(startnum + 1);
      int num = atoi(digits.c_str());
      if (num > 0 && startnum < artitem.artType.size())
      {
        if (num == tag.GetDiscNumber())
          discartmap.insert(std::make_pair(artitem.artType.substr(0, startnum + 1), artitem.url));
        continue;
      }
    }

    artmap.insert(std::make_pair(artname, artitem.url));

    // Add fallback art for "thumb" and "fanart" art types only
    // Set album thumb as the fallback used when song thumb is missing
    if (tag.GetType() == MediaTypeSong && artitem.mediaType == MediaTypeAlbum &&
        artitem.artType == "thumb")
    {
      item.SetArtFallback(artitem.artType, artname);
    }

    // For albums and songs set fallback fanart from the artist.
    // For songs prefer primary song artist over primary albumartist fanart as fallback fanart
    if (artitem.prefix == "artist" && artitem.artType == "fanart")
      fanartfallback = artname;
    if (artitem.prefix == "albumartist" && artitem.artType == "fanart" && fanartfallback.empty())
      fanartfallback = artname;
  }
  if (!fanartfallback.empty())
    item.SetArtFallback("fanart", fanartfallback);

  // Process specific disc art when we have some
  for (const auto& discart : discartmap)
  {
    std::map<std::string, std::string>::iterator it;
    if (tag.GetType() == MediaTypeAlbum)
    {
      // Insert or replace album art with specific disc art
      it = artmap.find(discart.first);
      if (it != artmap.end())
        it->second = discart.second;
      else
        artmap.insert(discart);
    }
    else if (tag.GetType() == MediaTypeSong)
    {
      // Use disc thumb rather than album as fallback for song thumb
      // (Fallback approach is used to fill missing thumbs).
      if (discart.first == "thumb")
      {
        it = artmap.find("album.thumb");
        if (it != artmap.end())
          // Replace "album.thumb" already set as fallback
          it->second = discart.second;
        else
        {
          // Insert thumb for album and set as fallback
          artmap.insert(std::make_pair("album.thumb", discart.second));
          item.SetArtFallback("thumb", "album.thumb");
        }
      }
      else
      {
        // Apply disc art as song art when not have that type (fallback does not apply).
        // Art of other types could been set via JSON, or in future read from metadata
        it = artmap.find(discart.first);
        if (it == artmap.end())
          artmap.insert(discart);
      }
    }
  }

  item.AppendArt(artmap);
  item.SetProperty("libraryartfilled", true);
  return true;
}
} // unnamed namespace

CMusicThumbLoader::CMusicThumbLoader() : CThumbLoader()
{
  EnableJjsMusicNavDiagnostics("MusicThumbLoader");
  m_musicDatabase = new CMusicThumbLoaderDatabase;
}

CMusicThumbLoader::~CMusicThumbLoader()
{
  delete m_musicDatabase;
}

void CMusicThumbLoader::SetPrefetchItems(const CFileItemList& items)
{
  m_manualPrefetchItems.clear();
  m_manualPrefetchItems.reserve(items.Size());
  for (const auto& item : items)
    m_manualPrefetchItems.emplace_back(item);
}

const std::vector<CFileItemPtr>& CMusicThumbLoader::GetPrefetchItems() const
{
  if (!m_vecItems.empty())
    return m_vecItems;
  return m_manualPrefetchItems;
}

void CMusicThumbLoader::OnLoaderStart()
{
  const auto loaderStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicThumbLoader OnLoaderStart BEGIN items={}",
            GetPrefetchItems().size());

  const auto dbOpenStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicThumbLoader MusicDatabase.Open BEGIN");
  m_musicDatabase->Open();
  const auto dbOpenElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - dbOpenStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicThumbLoader MusicDatabase.Open END ms={}",
            dbOpenElapsed.count());

  m_albumArt.clear();
  PrefetchLibraryArt();
  PrefetchCachedImages();
  CThumbLoader::OnLoaderStart();

  const auto loaderElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - loaderStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicThumbLoader OnLoaderStart END ms={}",
            loaderElapsed.count());
}

void CMusicThumbLoader::OnLoaderFinish()
{
  const auto finishStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicThumbLoader OnLoaderFinish BEGIN");

  m_musicDatabase->Close();
  m_albumArt.clear();
  m_manualPrefetchItems.clear();
  m_cachedPathArt.clear();
  m_cachedPathArtPrefetched = false;
  CThumbLoader::OnLoaderFinish();

  const auto finishElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - finishStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicThumbLoader OnLoaderFinish END ms={}",
            finishElapsed.count());
}

void CMusicThumbLoader::PrefetchLibraryArt()
{
  const auto prefetchStart = std::chrono::steady_clock::now();
  const auto& items = GetPrefetchItems();
  std::set<int> artistIds;
  std::set<int> albumIds;
  std::set<int> songIds;

  for (const auto& item : items)
  {
    if (!item || item->m_bIsShareOrDrive || !item->HasMusicInfoTag() ||
        item->GetProperty("libraryartfilled").asBoolean())
      continue;

    const CMusicInfoTag& tag = *item->GetMusicInfoTag();
    if (tag.GetDatabaseId() < 0)
      continue;

    if (tag.GetType() == MediaTypeArtist)
      artistIds.insert(tag.GetDatabaseId());
    else if (tag.GetType() == MediaTypeAlbum)
      albumIds.insert(tag.GetDatabaseId());
    else if (tag.GetType() == MediaTypeSong)
      songIds.insert(tag.GetDatabaseId());
  }

  CLog::Log(LOGINFO,
            "[JJS-MUSIC-NAV] PrefetchLibraryArt BEGIN items={} artists={} albums={} songs={}",
            items.size(), artistIds.size(), albumIds.size(), songIds.size());

  auto* database = static_cast<CMusicThumbLoaderDatabase*>(m_musicDatabase);
  LibraryArtBatch artistArt;
  LibraryArtBatch albumArt;
  LibraryArtBatch songArt;

  bool artistPrefetched = true;
  if (!artistIds.empty())
  {
    const auto batchStart = std::chrono::steady_clock::now();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] ArtistArtBatch BEGIN ids={}", artistIds.size());
    artistPrefetched = database->GetArtistArtBatch(artistIds, artistArt);
    const auto batchElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - batchStart);
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] ArtistArtBatch END ms={} ok={} items_with_art={}",
              batchElapsed.count(), artistPrefetched, artistArt.size());
  }

  bool albumPrefetched = true;
  if (!albumIds.empty())
  {
    const auto batchStart = std::chrono::steady_clock::now();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] AlbumArtBatch BEGIN ids={}", albumIds.size());
    albumPrefetched = database->GetAlbumArtBatch(albumIds, albumArt);
    const auto batchElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - batchStart);
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] AlbumArtBatch END ms={} ok={} items_with_art={}",
              batchElapsed.count(), albumPrefetched, albumArt.size());
  }

  bool songPrefetched = true;
  if (!songIds.empty())
  {
    const auto batchStart = std::chrono::steady_clock::now();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] SongArtBatch BEGIN ids={}", songIds.size());
    songPrefetched = database->GetSongArtBatch(songIds, songArt);
    const auto batchElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - batchStart);
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] SongArtBatch END ms={} ok={} items_with_art={}",
              batchElapsed.count(), songPrefetched, songArt.size());
  }

  for (const auto& item : items)
  {
    if (!item || item->m_bIsShareOrDrive || !item->HasMusicInfoTag() ||
        item->GetProperty("libraryartfilled").asBoolean())
      continue;

    const CMusicInfoTag& tag = *item->GetMusicInfoTag();
    if (tag.GetDatabaseId() < 0)
      continue;

    const LibraryArtBatch* batch = nullptr;
    bool prefetched = false;
    if (tag.GetType() == MediaTypeArtist)
    {
      batch = &artistArt;
      prefetched = artistPrefetched;
    }
    else if (tag.GetType() == MediaTypeAlbum)
    {
      batch = &albumArt;
      prefetched = albumPrefetched;
    }
    else if (tag.GetType() == MediaTypeSong)
    {
      batch = &songArt;
      prefetched = songPrefetched;
    }

    if (!batch || !prefetched)
      continue;

    const auto artIt = batch->find(tag.GetDatabaseId());
    if (artIt != batch->end())
      ApplyLibraryArt(*item, artIt->second);

    // A successful batch lookup also caches the fact that this item has no library art.
    // If the batch itself failed, leave this unset so Kodi's existing per-item lookup is used.
    item->SetProperty("libraryartfilled", true);
  }

  const auto prefetchElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - prefetchStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] PrefetchLibraryArt END ms={}",
            prefetchElapsed.count());
}

void CMusicThumbLoader::PrefetchCachedImages()
{
  const auto prefetchStart = std::chrono::steady_clock::now();
  m_cachedPathArt.clear();
  m_cachedPathArtPrefetched = false;

  const auto& items = GetPrefetchItems();
  std::set<std::string> paths;
  for (const auto& item : items)
  {
    if (!item || item->m_bIsShareOrDrive || item->GetPath().empty())
      continue;

    if (!item->HasArt("thumb") || !item->HasArt("fanart"))
      paths.insert(item->GetPath());
  }

  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] PrefetchCachedImages BEGIN items={} paths={}",
            items.size(), paths.size());

  if (paths.empty())
  {
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] PrefetchCachedImages END ms=0 no_paths=true");
    return;
  }

  CMusicThumbLoaderTextureDatabase database;
  const auto openStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] TextureDatabase.Open BEGIN");
  const bool opened = database.Open();
  const auto openElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - openStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] TextureDatabase.Open END ms={} ok={}",
            openElapsed.count(), opened);
  if (!opened)
    return;

  const auto batchStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] TexturePathBatch BEGIN paths={}", paths.size());
  const bool prefetched = database.GetPathArtBatch(paths, m_cachedPathArt);
  const auto batchElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - batchStart);
  CLog::Log(LOGINFO,
            "[JJS-MUSIC-NAV] TexturePathBatch END ms={} ok={} paths_with_art={}",
            batchElapsed.count(), prefetched, m_cachedPathArt.size());
  database.Close();
  if (!prefetched)
  {
    m_cachedPathArt.clear();
    return;
  }

  // Keep an entry for misses too. This prevents a second per-item SQLite query for
  // paths that were part of the successful batch but had no cached thumb/fanart.
  for (const auto& path : paths)
    m_cachedPathArt[path];

  m_cachedPathArtPrefetched = true;

  const auto prefetchElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - prefetchStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] PrefetchCachedImages END ms={} cached_paths={}",
            prefetchElapsed.count(), m_cachedPathArt.size());
}

bool CMusicThumbLoader::LoadItem(CFileItem* pItem)
{
  bool result  = LoadItemCached(pItem);
       result |= LoadItemLookup(pItem);

  return result;
}

bool CMusicThumbLoader::LoadItemCached(CFileItem* pItem)
{
  if (pItem->m_bIsShareOrDrive)
    return false;

  if (pItem->HasMusicInfoTag() && !pItem->GetProperty("libraryartfilled").asBoolean())
  {
    if (FillLibraryArt(*pItem))
      return true;

    if (pItem->GetMusicInfoTag()->GetType() == MediaTypeArtist)
      return false; // No fallback
  }

  if (pItem->HasVideoInfoTag() && !pItem->HasArt("thumb"))
  { // music video
    CVideoThumbLoader loader;
    if (loader.LoadItemCached(pItem))
      return true;
  }

  // Fallback to folder thumb when path has one cached
  if (!pItem->HasArt("thumb"))
  {
    std::string art = GetCachedImage(*pItem, "thumb");
    if (!art.empty())
      pItem->SetArt("thumb", art);
  }

  // Fallback to folder fanart when path has one cached
  //! @todo Remove as "fanart" is never been cached for music folders (only for
  // artists) or start caching fanart for folders?
  if (!pItem->HasArt("fanart"))
  {
    std::string art = GetCachedImage(*pItem, "fanart");
    if (!art.empty())
    {
      pItem->SetArt("fanart", art);
    }
  }

  return false;
}

bool CMusicThumbLoader::LoadItemLookup(CFileItem* pItem)
{
  if (pItem->m_bIsShareOrDrive)
    return false;

  if (pItem->HasMusicInfoTag() && pItem->GetMusicInfoTag()->GetType() == MediaTypeArtist) // No fallback for artist
    return false;

  if (pItem->HasVideoInfoTag())
  { // music video
    CVideoThumbLoader loader;
    if (loader.LoadItemLookup(pItem))
      return true;
  }

  if (!pItem->HasArt("thumb"))
  {
    // Look for embedded art
    if (pItem->HasMusicInfoTag() && !pItem->GetMusicInfoTag()->GetCoverArtInfo().Empty())
    {
      // The item has got embedded art but user thumbs overrule, so check for those first
      if (!FillThumb(*pItem, false)) // Check for user thumbs but ignore folder thumbs
      {
        // No user thumb, use embedded art
        std::string thumb = CTextureUtils::GetWrappedImageURL(pItem->GetPath(), "music");
        pItem->SetArt("thumb", thumb);
      }
    }
    else
    {
      // Check for user thumbs
      FillThumb(*pItem, true);
    }
  }

  return true;
}

bool CMusicThumbLoader::FillThumb(CFileItem &item, bool folderThumbs /* = true */)
{
  if (item.HasArt("thumb"))
    return true;
  std::string thumb = GetCachedImage(item, "thumb");
  if (thumb.empty())
  {
    const auto userThumbStart = std::chrono::steady_clock::now();
    thumb = item.GetUserMusicThumb(false, folderThumbs);
    const auto userThumbElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - userThumbStart);
    if (userThumbElapsed.count() >= 250)
    {
      CLog::Log(LOGINFO,
                "[JJS-MUSIC-NAV] GetUserMusicThumb SLOW ms={} folder_thumbs={} item='{}'",
                userThumbElapsed.count(), folderThumbs, item.GetPath());
    }
    if (!thumb.empty())
    {
      SetCachedImage(item, "thumb", thumb);
      const auto pathIt = m_cachedPathArt.find(item.GetPath());
      if (m_cachedPathArtPrefetched && pathIt != m_cachedPathArt.end())
        pathIt->second["thumb"] = thumb;
    }
  }
  if (!thumb.empty())
    item.SetArt("thumb", thumb);
  return !thumb.empty();
}

std::string CMusicThumbLoader::GetCachedImage(const CFileItem& item, const std::string& type)
{
  if (m_cachedPathArtPrefetched && (type == "thumb" || type == "fanart"))
  {
    const auto pathIt = m_cachedPathArt.find(item.GetPath());
    if (pathIt != m_cachedPathArt.end())
    {
      const auto artIt = pathIt->second.find(type);
      if (artIt != pathIt->second.end())
        return artIt->second;
      return "";
    }
  }

  return CThumbLoader::GetCachedImage(item, type);
}

bool CMusicThumbLoader::FillLibraryArt(CFileItem &item)
{
  /* Called for any item with MusicInfoTag and no art.
     Items on Genres, Sources and Roles nodes have ID (although items on Years
     node do not) so check for song/album/artist specifically.
     Non-library songs (file view) can also have MusicInfoTag but no ID or type
  */
  const auto fillStart = std::chrono::steady_clock::now();
  bool artfound(false);
  std::vector<ArtForThumbLoader> art;
  CMusicInfoTag &tag = *item.GetMusicInfoTag();
  if (tag.GetDatabaseId() > -1 &&
      (tag.GetType() == MediaTypeSong || tag.GetType() == MediaTypeAlbum ||
       tag.GetType() == MediaTypeArtist))
  {
    // Item in music library, fetch the art
    m_musicDatabase->Open();
    if (tag.GetType() == MediaTypeSong)
      artfound = m_musicDatabase->GetArtForItem(tag.GetDatabaseId(), tag.GetAlbumId(), -1, false, art);
    else if (tag.GetType() == MediaTypeAlbum)
      artfound = m_musicDatabase->GetArtForItem(-1, tag.GetDatabaseId(), -1, false, art);
    else //Artist
      artfound = m_musicDatabase->GetArtForItem(-1, -1, tag.GetDatabaseId(), true, art);

    m_musicDatabase->Close();
  }
  else if (!tag.GetArtist().empty() &&
           (tag.GetType() == MediaTypeNone || tag.GetType() == MediaTypeSong))
  {
    /*
    Could be non-library song - has musictag but no ID or type (may have
    thumb already). Try to fetch both song artist(s) and album artist(s) art by
    artist name, e.g. "artist.thumb", "artist.fanart", "artist.clearlogo",
    "artist.banner", "artist1.thumb", "artist1.fanart", "artist1.clearlogo",
    "artist1.banner", "albumartist.thumb", "albumartist.fanart" etc.
    Set fanart as fallback.
    */
    CSong song;
    // Try to split song artist names (various tags) into artist credits
    song.SetArtistCredits(tag.GetArtist(), tag.GetMusicBrainzArtistHints(), tag.GetMusicBrainzArtistID());
    if (!song.artistCredits.empty())
    {
      tag.SetType(MediaTypeSong);  // Makes "Information" context menu visible
      m_musicDatabase->Open();
      int iOrder = 0;
      // Song artist art
      for (const auto& artistCredit : song.artistCredits)
      {
        int idArtist = m_musicDatabase->GetArtistByName(artistCredit.GetArtist());
        if (idArtist > 0)
        {
          std::vector<ArtForThumbLoader> artistart;
          if (m_musicDatabase->GetArtForItem(-1, -1, idArtist, true, artistart))
          {
            for (auto& artitem : artistart)
            {
              if (iOrder > 0)
                artitem.prefix = StringUtils::Format("artist{}", iOrder);
              else
                artitem.prefix = "artist";
            }
            art.insert(art.end(), artistart.begin(), artistart.end());
          }
        }
        ++iOrder;
      }
      // Album artist art
      if (!tag.GetAlbumArtist().empty() && tag.GetArtistString().compare(tag.GetAlbumArtistString()) != 0)
      {
        // Split song artist names correctly into artist credits from various tag
        // arrays, inc. fallback to song artist names
        CAlbum album;
        album.SetArtistCredits(tag.GetAlbumArtist(), tag.GetMusicBrainzAlbumArtistHints(), tag.GetMusicBrainzAlbumArtistID(),
          tag.GetArtist(), tag.GetMusicBrainzArtistHints(), tag.GetMusicBrainzArtistID());

        iOrder = 0;
        for (const auto& artistCredit : album.artistCredits)
        {
          int idArtist = m_musicDatabase->GetArtistByName(artistCredit.GetArtist());
          if (idArtist > 0)
          {
            std::vector<ArtForThumbLoader> artistart;
            if (m_musicDatabase->GetArtForItem(-1, -1, idArtist, true, artistart))
            {
              for (auto& artitem : artistart)
              {
                if (iOrder > 0)
                  artitem.prefix = StringUtils::Format("albumartist{}", iOrder);
                else
                  artitem.prefix = "albumartist";
              }
              art.insert(art.end(), artistart.begin(), artistart.end());
            }
          }
          ++iOrder;
        }
      }
      else
      {
        // Replicate the artist art as album artist art
        std::vector<ArtForThumbLoader> artistart;
        for (const auto& artitem : art)
        {
          ArtForThumbLoader newart;
          newart.artType = artitem.artType;
          newart.mediaType = artitem.mediaType;
          newart.prefix = "album" + artitem.prefix;
          newart.url = artitem.url;
          artistart.emplace_back(newart);
        }
        art.insert(art.end(), artistart.begin(), artistart.end());
      }
      artfound = !art.empty();
      m_musicDatabase->Close();
    }
  }

  if (artfound)
    ApplyLibraryArt(item, art);

  const auto fillElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - fillStart);
  if (fillElapsed.count() >= 250)
  {
    CLog::Log(LOGINFO,
              "[JJS-MUSIC-NAV] FillLibraryArt SLOW ms={} type='{}' id={} item='{}'",
              fillElapsed.count(), tag.GetType(), tag.GetDatabaseId(), item.GetPath());
  }

  return artfound;
}
