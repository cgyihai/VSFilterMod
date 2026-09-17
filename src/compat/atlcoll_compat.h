/*
 *  VSFilterMod cross-platform port -- ATL collection replacements.
 *
 *  CAtlArray  -> std::vector with ATL's method names and its "grow on SetAt
 *                past the end" behaviour.
 *  CAtlList   -> std::list; POSITION is the address of a node, matching the
 *                GetNext(pos)/GetHeadPosition() iteration idiom used all over
 *                RTS.cpp, including the "pos becomes NULL at the end" test.
 *  CAtlMap    -> insertion-ordered map. ATL's is a hash table with unspecified
 *                iteration order; the subtitle code walks it when writing
 *                styles back out, so preserving insertion order makes output
 *                deterministic instead of merely matching one hash layout.
 */

#pragma once

#include "platform.h"
#include "cstring_compat.h"

#include <vector>
#include <list>
#include <map>
#include <utility>

typedef void* POSITION;

// ---------------------------------------------------------------------------
// CAtlArray
// ---------------------------------------------------------------------------

template <typename E>
class CAtlArray
{
public:
    CAtlArray() {}

    size_t GetCount() const { return m_v.size(); }
    bool IsEmpty() const { return m_v.empty(); }

    void RemoveAll() { m_v.clear(); }
    void SetCount(size_t n) { m_v.resize(n); }
    void SetCount(size_t n, int /*nGrowBy*/) { m_v.resize(n); }

    size_t Add() { m_v.emplace_back(); return m_v.size() - 1; }
    size_t Add(const E& e) { m_v.push_back(e); return m_v.size() - 1; }

    void InsertAt(size_t i, const E& e, size_t count = 1)
    {
        if (i > m_v.size()) i = m_v.size();
        m_v.insert(m_v.begin() + (ptrdiff_t)i, count, e);
    }
    void RemoveAt(size_t i, size_t count = 1)
    {
        if (i >= m_v.size()) return;
        if (i + count > m_v.size()) count = m_v.size() - i;
        m_v.erase(m_v.begin() + (ptrdiff_t)i, m_v.begin() + (ptrdiff_t)(i + count));
    }

    const E& GetAt(size_t i) const { return m_v[i]; }
    E& GetAt(size_t i) { return m_v[i]; }
    void SetAt(size_t i, const E& e)
    {
        if (i >= m_v.size()) m_v.resize(i + 1);
        m_v[i] = e;
    }
    void SetAtGrow(size_t i, const E& e) { SetAt(i, e); }

    const E& operator[](size_t i) const { return m_v[i]; }
    E& operator[](size_t i) { return m_v[i]; }

    const E* GetData() const { return m_v.empty() ? nullptr : &m_v[0]; }
    E* GetData() { return m_v.empty() ? nullptr : &m_v[0]; }

    void Copy(const CAtlArray& o) { m_v = o.m_v; }
    void Append(const CAtlArray& o) { m_v.insert(m_v.end(), o.m_v.begin(), o.m_v.end()); }

    /* Sorting helpers used by the segment builder. */
    std::vector<E>& raw() { return m_v; }
    const std::vector<E>& raw() const { return m_v; }

private:
    std::vector<E> m_v;
};

// ---------------------------------------------------------------------------
// CAtlList
// ---------------------------------------------------------------------------

template <typename E>
class CAtlList
{
    struct Node
    {
        E value;
        Node* prev;
        Node* next;
        Node(const E& v) : value(v), prev(nullptr), next(nullptr) {}
    };

public:
    CAtlList() : m_head(nullptr), m_tail(nullptr), m_count(0) {}
    CAtlList(const CAtlList& o) : m_head(nullptr), m_tail(nullptr), m_count(0)
    {
        for (Node* n = o.m_head; n; n = n->next) AddTail(n->value);
    }
    CAtlList& operator=(const CAtlList& o)
    {
        if (this != &o) {
            RemoveAll();
            for (Node* n = o.m_head; n; n = n->next) AddTail(n->value);
        }
        return *this;
    }
    ~CAtlList() { RemoveAll(); }

    size_t GetCount() const { return m_count; }
    bool IsEmpty() const { return m_count == 0; }

    void RemoveAll()
    {
        Node* n = m_head;
        while (n) { Node* nx = n->next; delete n; n = nx; }
        m_head = m_tail = nullptr;
        m_count = 0;
    }

    POSITION AddTail(const E& e)
    {
        Node* n = new Node(e);
        n->prev = m_tail;
        if (m_tail) m_tail->next = n; else m_head = n;
        m_tail = n;
        ++m_count;
        return (POSITION)n;
    }
    POSITION AddHead(const E& e)
    {
        Node* n = new Node(e);
        n->next = m_head;
        if (m_head) m_head->prev = n; else m_tail = n;
        m_head = n;
        ++m_count;
        return (POSITION)n;
    }
    void AddTailList(const CAtlList* o)
    {
        if (!o) return;
        for (Node* n = o->m_head; n; n = n->next) AddTail(n->value);
    }

    E RemoveHead()
    {
        E v = m_head->value;
        Unlink(m_head);
        return v;
    }
    E RemoveTail()
    {
        E v = m_tail->value;
        Unlink(m_tail);
        return v;
    }
    void RemoveHeadNoReturn() { Unlink(m_head); }

    void RemoveAt(POSITION pos) { if (pos) Unlink((Node*)pos); }

    E& GetHead() { return m_head->value; }
    const E& GetHead() const { return m_head->value; }
    E& GetTail() { return m_tail->value; }
    const E& GetTail() const { return m_tail->value; }

    POSITION GetHeadPosition() const { return (POSITION)m_head; }
    POSITION GetTailPosition() const { return (POSITION)m_tail; }

    E& GetNext(POSITION& pos)
    {
        Node* n = (Node*)pos;
        pos = (POSITION)n->next;
        return n->value;
    }
    const E& GetNext(POSITION& pos) const
    {
        Node* n = (Node*)pos;
        pos = (POSITION)n->next;
        return n->value;
    }
    E& GetPrev(POSITION& pos)
    {
        Node* n = (Node*)pos;
        pos = (POSITION)n->prev;
        return n->value;
    }
    const E& GetPrev(POSITION& pos) const
    {
        Node* n = (Node*)pos;
        pos = (POSITION)n->prev;
        return n->value;
    }

    E& GetAt(POSITION pos) { return ((Node*)pos)->value; }
    const E& GetAt(POSITION pos) const { return ((Node*)pos)->value; }
    void SetAt(POSITION pos, const E& e) { ((Node*)pos)->value = e; }

    POSITION Find(const E& e, POSITION startAfter = nullptr) const
    {
        Node* n = startAfter ? ((Node*)startAfter)->next : m_head;
        for (; n; n = n->next) if (n->value == e) return (POSITION)n;
        return nullptr;
    }

    POSITION InsertBefore(POSITION pos, const E& e)
    {
        if (!pos) return AddHead(e);
        Node* at = (Node*)pos;
        Node* n = new Node(e);
        n->prev = at->prev;
        n->next = at;
        if (at->prev) at->prev->next = n; else m_head = n;
        at->prev = n;
        ++m_count;
        return (POSITION)n;
    }
    POSITION InsertAfter(POSITION pos, const E& e)
    {
        if (!pos) return AddTail(e);
        Node* at = (Node*)pos;
        Node* n = new Node(e);
        n->next = at->next;
        n->prev = at;
        if (at->next) at->next->prev = n; else m_tail = n;
        at->next = n;
        ++m_count;
        return (POSITION)n;
    }

    void MoveToTail(POSITION pos)
    {
        Node* n = (Node*)pos;
        if (!n || n == m_tail) return;
        if (n->prev) n->prev->next = n->next; else m_head = n->next;
        if (n->next) n->next->prev = n->prev;
        n->prev = m_tail;
        n->next = nullptr;
        if (m_tail) m_tail->next = n;
        m_tail = n;
        if (!m_head) m_head = n;
    }

    void SwapElements(POSITION a, POSITION b)
    {
        if (!a || !b || a == b) return;
        std::swap(((Node*)a)->value, ((Node*)b)->value);
    }

private:
    void Unlink(Node* n)
    {
        if (!n) return;
        if (n->prev) n->prev->next = n->next; else m_head = n->next;
        if (n->next) n->next->prev = n->prev; else m_tail = n->prev;
        delete n;
        --m_count;
    }

    Node* m_head;
    Node* m_tail;
    size_t m_count;
};

// ---------------------------------------------------------------------------
// CAtlMap (insertion ordered)
// ---------------------------------------------------------------------------

template <typename K> struct CElementTraits
{
    static bool Equal(const K& a, const K& b) { return a == b; }
};

/* Present only so the CAtlMap<..., CStringElementTraits<CString> > spellings in
 * STS.h keep compiling; comparison is case-sensitive, as in ATL. */
template <typename S> struct CStringElementTraits
{
    static bool Equal(const S& a, const S& b) { return a == b; }
};

template <typename K> struct MapKeyLess
{
    bool operator()(const K& a, const K& b) const { return a < b; }
};

template <typename K, typename V, typename Traits = CElementTraits<K> >
class CAtlMap
{
    /* Intrusive nodes so POSITION -> next is O(1); walking the map is on the
     * hot path when RTS sweeps its subtitle cache. */
    struct Node
    {
        K key;
        V value;
        Node* next;
        Node(const K& k, const V& v) : key(k), value(v), next(nullptr) {}
    };

public:
    CAtlMap() : m_head(nullptr), m_tail(nullptr) {}
    CAtlMap(const CAtlMap& o) : m_head(nullptr), m_tail(nullptr) { CopyFrom(o); }
    CAtlMap& operator=(const CAtlMap& o)
    {
        if (this != &o) { RemoveAll(); CopyFrom(o); }
        return *this;
    }
    ~CAtlMap() { RemoveAll(); }

    size_t GetCount() const { return m_index.size(); }
    bool IsEmpty() const { return m_index.empty(); }

    void RemoveAll()
    {
        Node* n = m_head;
        while (n) { Node* nx = n->next; delete n; n = nx; }
        m_head = m_tail = nullptr;
        m_index.clear();
    }

    void SetAt(const K& k, const V& v)
    {
        auto it = m_index.find(k);
        if (it != m_index.end()) { it->second->value = v; return; }
        Link(new Node(k, v));
    }
    V& operator[](const K& k)
    {
        auto it = m_index.find(k);
        if (it != m_index.end()) return it->second->value;
        Node* n = new Node(k, V());
        Link(n);
        return n->value;
    }

    bool Lookup(const K& k, V& v) const
    {
        auto it = m_index.find(k);
        if (it == m_index.end()) return false;
        v = it->second->value;
        return true;
    }
    bool Lookup(const K& k) const { return m_index.find(k) != m_index.end(); }

    bool RemoveKey(const K& k)
    {
        auto it = m_index.find(k);
        if (it == m_index.end()) return false;
        Unlink(it->second);
        m_index.erase(it);
        return true;
    }

    POSITION GetStartPosition() const { return (POSITION)m_head; }

    void GetNextAssoc(POSITION& pos, K& k, V& v) const
    {
        Node* n = (Node*)pos;
        k = n->key;
        v = n->value;
        pos = (POSITION)n->next;
    }

    const K& GetKeyAt(POSITION pos) const { return ((Node*)pos)->key; }
    const V& GetValueAt(POSITION pos) const { return ((Node*)pos)->value; }
    V& GetValueAt(POSITION pos) { return ((Node*)pos)->value; }

private:
    void Link(Node* n)
    {
        if (m_tail) m_tail->next = n; else m_head = n;
        m_tail = n;
        m_index[n->key] = n;
    }
    void Unlink(Node* target)
    {
        Node* prev = nullptr;
        for (Node* n = m_head; n; prev = n, n = n->next) {
            if (n != target) continue;
            if (prev) prev->next = n->next; else m_head = n->next;
            if (n == m_tail) m_tail = prev;
            delete n;
            return;
        }
    }
    void CopyFrom(const CAtlMap& o)
    {
        for (Node* n = o.m_head; n; n = n->next) SetAt(n->key, n->value);
    }

    Node* m_head;
    Node* m_tail;
    std::map<K, Node*, MapKeyLess<K> > m_index;
};
