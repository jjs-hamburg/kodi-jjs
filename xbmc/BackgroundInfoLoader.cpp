/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BackgroundInfoLoader.h"

#include "FileItem.h"
#include "URL.h"
#include "threads/Thread.h"
#include "utils/log.h"

#include <chrono>
#include <mutex>

namespace
{
const char* JjsLoaderStageName(int stage)
{
  switch (stage)
  {
    case 1:
      return "OnLoaderStart";
    case 2:
      return "LoadItemCached";
    case 3:
      return "LoadItemLookup";
    case 4:
      return "OnLoaderFinish";
    default:
      return "idle";
  }
}
} // unnamed namespace

CBackgroundInfoLoader::CBackgroundInfoLoader() = default;

CBackgroundInfoLoader::~CBackgroundInfoLoader()
{
  StopThread();
}

void CBackgroundInfoLoader::Reset()
{
  m_jjsCurrentItem.store(nullptr, std::memory_order_relaxed);
  m_jjsStage.store(0, std::memory_order_relaxed);
  m_pVecItems = nullptr;
  m_vecItems.clear();
  m_bIsLoading = false;
}

void CBackgroundInfoLoader::Run()
{
  try
  {
    if (!m_vecItems.empty())
    {
      m_jjsStage.store(1, std::memory_order_relaxed);
      OnLoaderStart();
      m_jjsStage.store(0, std::memory_order_relaxed);

      // Stage 1: All "fast" stuff we have already cached
      for (std::vector<CFileItemPtr>::const_iterator iter = m_vecItems.begin(); iter != m_vecItems.end(); ++iter)
      {
        const CFileItemPtr& pItem = *iter;

        // Ask the callback if we should abort
        if ((m_pProgressCallback && m_pProgressCallback->Abort()) || m_bStop)
          break;

        m_jjsCurrentItem.store(pItem.get(), std::memory_order_relaxed);
        m_jjsStage.store(2, std::memory_order_relaxed);
        try
        {
          if (LoadItemCached(pItem.get()) && m_pObserver)
            m_pObserver->OnItemLoaded(pItem.get());
        }
        catch (...)
        {
          CLog::Log(LOGERROR,
                    "CBackgroundInfoLoader::LoadItemCached - Unhandled exception for item {}",
                    CURL::GetRedacted(pItem->GetPath()));
        }
        m_jjsCurrentItem.store(nullptr, std::memory_order_relaxed);
        m_jjsStage.store(0, std::memory_order_relaxed);
      }

      // Stage 2: All "slow" stuff that we need to lookup
      for (std::vector<CFileItemPtr>::const_iterator iter = m_vecItems.begin(); iter != m_vecItems.end(); ++iter)
      {
        const CFileItemPtr& pItem = *iter;

        // Ask the callback if we should abort
        if ((m_pProgressCallback && m_pProgressCallback->Abort()) || m_bStop)
          break;

        m_jjsCurrentItem.store(pItem.get(), std::memory_order_relaxed);
        m_jjsStage.store(3, std::memory_order_relaxed);
        try
        {
          if (LoadItemLookup(pItem.get()) && m_pObserver)
            m_pObserver->OnItemLoaded(pItem.get());
        }
        catch (...)
        {
          CLog::Log(LOGERROR,
                    "CBackgroundInfoLoader::LoadItemLookup - Unhandled exception for item {}",
                    CURL::GetRedacted(pItem->GetPath()));
        }
        m_jjsCurrentItem.store(nullptr, std::memory_order_relaxed);
        m_jjsStage.store(0, std::memory_order_relaxed);
      }
    }

    m_jjsStage.store(4, std::memory_order_relaxed);
    OnLoaderFinish();
    m_jjsStage.store(0, std::memory_order_relaxed);
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "{} - Unhandled exception", __FUNCTION__);
  }

  Reset();
}

void CBackgroundInfoLoader::Load(CFileItemList& items)
{
  StopThread();

  if (items.IsEmpty())
    return;

  std::unique_lock<CCriticalSection> lock(m_lock);

  for (int nItem=0; nItem < items.Size(); nItem++)
    m_vecItems.push_back(items[nItem]);

  m_pVecItems = &items;
  m_bStop = false;
  m_bIsLoading = true;

  m_thread = new CThread(this, "BackgroundLoader");
  m_thread->Create();
  m_thread->SetPriority(ThreadPriority::BELOW_NORMAL);
}

void CBackgroundInfoLoader::StopAsync()
{
  m_bStop = true;
}


void CBackgroundInfoLoader::StopThread()
{
  const bool traceWait = m_jjsDiagnostics && m_thread != nullptr;
  const auto waitStart = std::chrono::steady_clock::now();

  if (traceWait)
  {
    CFileItem* currentItem = m_jjsCurrentItem.load(std::memory_order_relaxed);
    const std::string currentPath =
        currentItem ? CURL::GetRedacted(currentItem->GetPath()) : std::string{};
    CLog::Log(LOGINFO,
              "[JJS-MUSIC-NAV] {} StopThread BEGIN stage={} item='{}' items={} loading={}",
              m_jjsDiagnosticName, JjsLoaderStageName(m_jjsStage.load(std::memory_order_relaxed)),
              currentPath, m_vecItems.size(), static_cast<bool>(m_bIsLoading));
  }

  StopAsync();

  if (m_thread)
  {
    m_thread->StopThread();
    delete m_thread;
    m_thread = NULL;
  }

  if (traceWait)
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - waitStart);
    CLog::Log(LOGINFO, "[JJS-MUSIC-NAV] {} StopThread END ms={}", m_jjsDiagnosticName,
              elapsed.count());
  }

  Reset();
}

bool CBackgroundInfoLoader::IsLoading()
{
  return m_bIsLoading;
}

void CBackgroundInfoLoader::SetObserver(IBackgroundLoaderObserver* pObserver)
{
  m_pObserver = pObserver;
}

void CBackgroundInfoLoader::SetProgressCallback(IProgressCallback* pCallback)
{
  m_pProgressCallback = pCallback;
}

