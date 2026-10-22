/*
 *  VSFilterMod cross-platform port -- DirectShow CCritSec / CAutoLock.
 *
 *  Recursive, because RTS takes the same lock from nested render helpers.
 */

#pragma once

#include <mutex>

class CCritSec
{
public:
    CCritSec() {}
    CCritSec(const CCritSec&) = delete;
    CCritSec& operator=(const CCritSec&) = delete;

    void Lock() { m_mutex.lock(); }
    void Unlock() { m_mutex.unlock(); }

private:
    std::recursive_mutex m_mutex;
};

class CAutoLock
{
public:
    explicit CAutoLock(CCritSec* cs) : m_cs(cs) { if (m_cs) m_cs->Lock(); }
    ~CAutoLock() { if (m_cs) m_cs->Unlock(); }

    CAutoLock(const CAutoLock&) = delete;
    CAutoLock& operator=(const CAutoLock&) = delete;

private:
    CCritSec* m_cs;
};
