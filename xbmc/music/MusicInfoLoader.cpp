/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MusicInfoLoader.h"

#include "Album.h"
#include "Artist.h"
#include "FileItem.h"
#include "MusicDatabase.h"
#include "MusicThumbLoader.h"
#include "ServiceBroker.h"
#include "filesystem/File.h"
#include "filesystem/MusicDatabaseDirectory/DirectoryNode.h"
#include "filesystem/MusicDatabaseDirectory/QueryParams.h"
#include "music/tags/MusicInfoTag.h"
#include "music/tags/MusicInfoTagLoaderFactory.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/Archive.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <chrono>

using namespace XFILE;
using namespace MUSIC_INFO;

// HACK until we make this threadable - specify 1 thread only for now
CMusicInfoLoader::CMusicInfoLoader() : CBackgroundInfoLoader()
{
  EnableJjsMusicNavDiagnostics("MusicInfoLoader");
  m_mapFileItems = new CFileItemList;

  m_thumbLoader = new CMusicThumbLoader();
}

CMusicInfoLoader::~CMusicInfoLoader()
{
  StopThread();
  delete m_mapFileItems;
  delete m_thumbLoader;
}

void CMusicInfoLoader::OnLoaderStart()
{
  const auto loaderStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader OnLoaderStart BEGIN items={}",
            m_pVecItems ? m_pVecItems->Size() : 0);

  const auto cacheStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader CacheLoad BEGIN mode={}",
            m_strCacheFileName.empty() ? "directory" : "file");

  // Load previously cached items from HD
  if (!m_strCacheFileName.empty())
    LoadCache(m_strCacheFileName, *m_mapFileItems);
  else
  {
    m_mapFileItems->SetPath(m_pVecItems->GetPath());
    m_mapFileItems->Load();
    m_mapFileItems->SetFastLookup(true);
  }

  const auto cacheElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - cacheStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader CacheLoad END ms={} cached_items={}",
            cacheElapsed.count(), m_mapFileItems->Size());

  m_strPrevPath.clear();

  m_databaseHits = m_tagReads = 0;

  if (m_pProgressCallback)
    m_pProgressCallback->SetProgressMax(m_pVecItems->GetFileCount());

  const auto dbOpenStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader MusicDatabase.Open BEGIN");
  m_musicDatabase.Open();
  const auto dbOpenElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - dbOpenStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader MusicDatabase.Open END ms={}",
            dbOpenElapsed.count());

  if (m_thumbLoader)
  {
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader ThumbLoaderStart BEGIN");
    m_thumbLoader->SetPrefetchItems(*m_pVecItems);
    m_thumbLoader->OnLoaderStart();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader ThumbLoaderStart END");
  }

  const auto loaderElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - loaderStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader OnLoaderStart END ms={}",
            loaderElapsed.count());
}

bool CMusicInfoLoader::LoadAdditionalTagInfo(CFileItem* pItem)
{
  if (!pItem || (pItem->m_bIsFolder && !pItem->IsAudio()) ||
      pItem->IsPlayList() || pItem->IsNFO() || pItem->IsInternetStream())
    return false;

  if (pItem->GetProperty("hasfullmusictag") == "true")
    return false; // already have the information

  std::string path(pItem->GetPath());
  // For songs in library set the (primary) song artist and album properties
  // Use song Id (not path) as called for items from either library or file view,
  // but could also be listitem with tag loaded by a script
  if (pItem->HasMusicInfoTag() &&
      pItem->GetMusicInfoTag()->GetType() == MediaTypeSong &&
      pItem->GetMusicInfoTag()->GetDatabaseId() > 0)
  {
    CMusicDatabase database;
    database.Open();
    // May already have song artist ids as item property set when data read from
    // db, but check property is valid array (scripts could set item properties
    // incorrectly), otherwise fetch artist using song id.
    CArtist artist;
    bool artistfound = false;
    if (pItem->HasProperty("artistid") && pItem->GetProperty("artistid").isArray())
    {
      CVariant::const_iterator_array varid = pItem->GetProperty("artistid").begin_array();
      int idArtist = static_cast<int>(varid->asInteger());
      artistfound = database.GetArtist(idArtist, artist, false);
    }
    else
      artistfound = database.GetArtistFromSong(pItem->GetMusicInfoTag()->GetDatabaseId(), artist);
    if (artistfound)
      CMusicDatabase::SetPropertiesFromArtist(*pItem, artist);

    // May already have album id, otherwise fetch album from song id
    CAlbum album;
    bool albumfound = false;
    int idAlbum = pItem->GetMusicInfoTag()->GetAlbumId();
    if (idAlbum > 0)
      albumfound = database.GetAlbum(idAlbum, album, false);
    else
      albumfound = database.GetAlbumFromSong(pItem->GetMusicInfoTag()->GetDatabaseId(), album);
    if (albumfound)
      CMusicDatabase::SetPropertiesFromAlbum(*pItem, album);

    path = pItem->GetMusicInfoTag()->GetURL();
  }

  CLog::Log(LOGDEBUG, "Loading additional tag info for file {}", path);

  // we load up the actual tag for this file in order to
  // fetch the lyrics and add it to the current music info tag
  CFileItem tempItem(path, false);
  std::unique_ptr<IMusicInfoTagLoader> pLoader (CMusicInfoTagLoaderFactory::CreateLoader(tempItem));
  if (nullptr != pLoader)
  {
    CMusicInfoTag tag;
    pLoader->Load(path, tag);
    pItem->GetMusicInfoTag()->SetLyrics(tag.GetLyrics());
    pItem->SetProperty("hasfullmusictag", "true");
    return true;
  }
  return false;
}

bool CMusicInfoLoader::LoadItem(CFileItem* pItem)
{
  bool result  = LoadItemCached(pItem);
       result |= LoadItemLookup(pItem);

  return result;
}

bool CMusicInfoLoader::LoadItemCached(CFileItem* pItem)
{
  if ((pItem->m_bIsFolder && !pItem->IsAudio()) ||
      pItem->IsPlayList() || pItem->IsSmartPlayList() ||
      StringUtils::StartsWithNoCase(pItem->GetPath(), "newplaylist://") ||
      StringUtils::StartsWithNoCase(pItem->GetPath(), "newsmartplaylist://") ||
      pItem->IsNFO() || (pItem->IsInternetStream() && !pItem->IsMusicDb()))
    return false;

  // Get thumb for item
  const auto thumbStart = std::chrono::steady_clock::now();
  m_thumbLoader->LoadItem(pItem);
  const auto thumbElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - thumbStart);
  if (thumbElapsed.count() >= 250)
  {
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader ThumbLoad SLOW ms={} item='{}'",
              thumbElapsed.count(), pItem->GetPath());
  }

  return true;
}

bool CMusicInfoLoader::LoadItemLookup(CFileItem* pItem)
{
  if (m_pProgressCallback && !pItem->m_bIsFolder)
    m_pProgressCallback->SetProgressAdvance();

  if ((pItem->m_bIsFolder && !pItem->IsAudio()) || //
      pItem->IsPlayList() || pItem->IsSmartPlayList() || //
      StringUtils::StartsWithNoCase(pItem->GetPath(), "newplaylist://") || //
      StringUtils::StartsWithNoCase(pItem->GetPath(), "newsmartplaylist://") || //
      pItem->IsNFO() || (pItem->IsInternetStream() && !pItem->IsMusicDb()))
    return false;

  if ((!pItem->HasMusicInfoTag() || !pItem->GetMusicInfoTag()->Loaded()) && pItem->IsAudio())
  {
    // first check the cached item
    CFileItemPtr mapItem = (*m_mapFileItems)[pItem->GetPath()];
    if (mapItem && mapItem->m_dateTime==pItem->m_dateTime && mapItem->HasMusicInfoTag() && mapItem->GetMusicInfoTag()->Loaded())
    { // Query map if we previously cached the file on HD
      *pItem->GetMusicInfoTag() = *mapItem->GetMusicInfoTag();
      if (mapItem->HasArt("thumb"))
        pItem->SetArt("thumb", mapItem->GetArt("thumb"));
    }
    else
    {
      std::string strPath = URIUtils::GetDirectory(pItem->GetPath());
      URIUtils::AddSlashAtEnd(strPath);
      if (strPath!=m_strPrevPath)
      {
        // The item is from another directory as the last one,
        // query the database for the new directory...
        const auto songsByPathStart = std::chrono::steady_clock::now();
        CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader GetSongsByPath BEGIN path='{}'",
                  strPath);
        m_musicDatabase.GetSongsByPath(strPath, m_songsMap);
        const auto songsByPathElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - songsByPathStart);
        CLog::Log(LOGINFO,
                  "[JJS-MUSIC-NAV] MusicInfoLoader GetSongsByPath END ms={} entries={} path='{}'",
                  songsByPathElapsed.count(), m_songsMap.size(), strPath);
        m_databaseHits++;
      }

      /*
      This only loads the item with the song from the database when it maps to a single song,
      it can not load song data for items with cuesheets that expand to multiple songs.
      For songs from embedded or separate cuesheets strFileName is not unique, so the song map for
      the path will have the list of songs from that file. But items with cuesheets are expanded
      (replacing each item with items for every track) elsewhere. When the item we are looking up
      has a cuesheet document or is a music file with a cuesheet embedded in the tags, and it maps
      to more than one song then we can not fill the tag data and thumb from the database.
      */
      MAPSONGS::iterator it = m_songsMap.find(pItem->GetPath()); // Find file in song map
      if (it != m_songsMap.end() && it->second.size() == 1)
      {
        // Have we loaded this item from database before,
        // and even if it has a cuesheet it has only one song
        pItem->GetMusicInfoTag()->SetSong(it->second[0]);
        if (!it->second[0].strThumb.empty())
          pItem->SetArt("thumb", it->second[0].strThumb);
      }
      else if (pItem->IsMusicDb())
      { // a music db item that doesn't have tag loaded - grab details from the database
        XFILE::MUSICDATABASEDIRECTORY::CQueryParams param;
        XFILE::MUSICDATABASEDIRECTORY::CDirectoryNode::GetDatabaseInfo(pItem->GetPath(),param);
        CSong song;
        if (m_musicDatabase.GetSong(param.GetSongId(), song))
        {
          pItem->GetMusicInfoTag()->SetSong(song);
          if (!song.strThumb.empty())
            pItem->SetArt("thumb", song.strThumb);
        }
      }
      else if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_MUSICFILES_USETAGS) || pItem->IsCDDA())
      { // Nothing found, load tag from file,
        // always try to load cddb info
        // get correct tag parser
        std::unique_ptr<IMusicInfoTagLoader> pLoader (CMusicInfoTagLoaderFactory::CreateLoader(*pItem));
        if (nullptr != pLoader)
        {
          const auto tagStart = std::chrono::steady_clock::now();
          // get tag
          pLoader->Load(pItem->GetPath(), *pItem->GetMusicInfoTag());
          const auto tagElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - tagStart);
          if (tagElapsed.count() >= 250)
          {
            CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader TagLoad SLOW ms={} item='{}'",
                      tagElapsed.count(), pItem->GetPath());
          }
        }
        m_tagReads++;
      }

      m_strPrevPath = strPath;
    }
  }

  return true;
}

void CMusicInfoLoader::OnLoaderFinish()
{
  const auto finishStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO,
            "[JJS-MUSIC-NAV] MusicInfoLoader OnLoaderFinish BEGIN stopped={} db_hits={} tag_reads={}",
            static_cast<bool>(m_bStop), m_databaseHits, m_tagReads);

  // cleanup last loaded songs from database
  m_songsMap.clear();

  // cleanup cache loaded from HD
  m_mapFileItems->Clear();

  // Save loaded items to HD
  if (!m_strCacheFileName.empty())
  {
    const auto saveStart = std::chrono::steady_clock::now();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader SaveCache BEGIN file='{}'",
              m_strCacheFileName);
    SaveCache(m_strCacheFileName, *m_pVecItems);
    const auto saveElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - saveStart);
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader SaveCache END ms={}",
              saveElapsed.count());
  }
  else if (!m_bStop && (m_databaseHits > 1 || m_tagReads > 0))
  {
    const auto saveStart = std::chrono::steady_clock::now();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader DirectoryCacheSave BEGIN items={}",
              m_pVecItems ? m_pVecItems->Size() : 0);
    m_pVecItems->Save();
    const auto saveElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - saveStart);
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader DirectoryCacheSave END ms={}",
              saveElapsed.count());
  }

  const auto closeStart = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader MusicDatabase.Close BEGIN");
  m_musicDatabase.Close();
  const auto closeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - closeStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader MusicDatabase.Close END ms={}",
            closeElapsed.count());

  if (m_thumbLoader)
  {
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader ThumbLoaderFinish BEGIN");
    m_thumbLoader->OnLoaderFinish();
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader ThumbLoaderFinish END");
  }

  const auto finishElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - finishStart);
  CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] MusicInfoLoader OnLoaderFinish END ms={}",
            finishElapsed.count());
}

void CMusicInfoLoader::UseCacheOnHD(const std::string& strFileName)
{
  m_strCacheFileName = strFileName;
}

void CMusicInfoLoader::LoadCache(const std::string& strFileName, CFileItemList& items)
{
  CFile file;

  if (file.Open(strFileName))
  {
    CArchive ar(&file, CArchive::load);
    int iSize = 0;
    ar >> iSize;
    for (int i = 0; i < iSize; i++)
    {
      CFileItemPtr pItem(new CFileItem());
      ar >> *pItem;
      items.Add(pItem);
    }
    ar.Close();
    file.Close();
    items.SetFastLookup(true);
  }
}

void CMusicInfoLoader::SaveCache(const std::string& strFileName, CFileItemList& items)
{
  int iSize = items.Size();

  if (iSize <= 0)
    return ;

  CFile file;

  if (file.OpenForWrite(strFileName))
  {
    CArchive ar(&file, CArchive::store);
    ar << items.Size();
    for (int i = 0; i < iSize; i++)
    {
      CFileItemPtr pItem = items[i];
      ar << *pItem;
    }
    ar.Close();
    file.Close();
  }

}
