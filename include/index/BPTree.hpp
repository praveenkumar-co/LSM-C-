#pragma once
// BPTree.hpp - disk-based B+Tree mapping key -> vlog::IndexValueRef. See docs/PHASE2_PLAN.md section 2.
//
// - One std::shared_mutex for the whole tree: get/scan take it shared, put/erase/apply exclusive.
// - No merges: erase never touches parents or siblings; empty leaves stay in the tree and the
//   leaf chain (plan section 5).
// - Keys: 1..256 bytes, compared like std::string (memcmp, then shorter first).

#include "index/BPTreeFormat.hpp"
#include "index/Pager.hpp"
#include "vlog/VLogFormat.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bpt {

// View over the bytes of one leaf or internal page (slotted layout, plan section 1.3).
class Node {
public:
    explicit Node(uint8_t* p) : p_(p) {}

    bool isLeaf() const { return p_[offsetof(PageHeader, page_type)] == static_cast<uint8_t>(PageType::Leaf); }
    uint8_t level() const { return p_[offsetof(PageHeader, level)]; }
    uint16_t count() const { return ld<uint16_t>(offsetof(PageHeader, slot_count)); }
    uint16_t freeStart() const { return ld<uint16_t>(offsetof(PageHeader, free_start)); }
    uint16_t cellStart() const { return ld<uint16_t>(offsetof(PageHeader, cell_start)); }
    uint16_t fragBytes() const { return ld<uint16_t>(offsetof(PageHeader, frag_bytes)); }
    page_id_t rightSibling() const { return ld<uint32_t>(offsetof(PageHeader, right_sibling)); }
    page_id_t leftmostChild() const { return ld<uint32_t>(offsetof(PageHeader, leftmost_child)); }
    void setRightSibling(page_id_t id) { st<uint32_t>(offsetof(PageHeader, right_sibling), id); }
    void setLeftmostChild(page_id_t id) { st<uint32_t>(offsetof(PageHeader, leftmost_child), id); }

    uint32_t valueSize() const { return isLeaf() ? kLeafValueSize : kChildValueSize; }
    uint16_t slot(uint32_t i) const { return ld<uint16_t>(kHeaderSize + kSlotSize * i); }
    uint16_t keyLen(uint32_t i) const { return ld<uint16_t>(slot(i)); }
    std::string_view key(uint32_t i) const {
        return std::string_view(reinterpret_cast<const char*>(p_ + slot(i) + 2), keyLen(i));
    }
    uint32_t cellSize(uint32_t i) const { return 2 + keyLen(i) + valueSize(); }
    const uint8_t* valuePtr(uint32_t i) const { return p_ + slot(i) + 2 + keyLen(i); }

    vlog::IndexValueRef leafValue(uint32_t i) const {
        vlog::IndexValueRef r;
        std::memcpy(&r, valuePtr(i), sizeof(r));
        return r;
    }
    void setLeafValue(uint32_t i, const vlog::IndexValueRef& r) {
        std::memcpy(p_ + slot(i) + 2 + keyLen(i), &r, sizeof(r));
    }
    page_id_t cellChild(uint32_t i) const {
        page_id_t c;
        std::memcpy(&c, valuePtr(i), sizeof(c));
        return c;
    }
    // Child j of an internal page: j == 0 is leftmost_child, j >= 1 is the child of cell j-1.
    page_id_t childAt(uint32_t j) const { return j == 0 ? leftmostChild() : cellChild(j - 1); }

    // First index i with key(i) >= k.
    uint32_t lowerBound(std::string_view k) const {
        uint32_t lo = 0, hi = count();
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (compareKeys(key(mid), k) < 0) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    // Number of keys <= k. For an internal page this is the index of the child to follow.
    uint32_t upperBound(std::string_view k) const {
        uint32_t lo = 0, hi = count();
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (compareKeys(key(mid), k) <= 0) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    uint32_t contiguousFree() const { return static_cast<uint32_t>(cellStart() - freeStart()); }

    // Inserts a cell at slot position pos. Compacts first if that makes room.
    // Returns false (page unchanged) if the cell does not fit even after compaction.
    bool insert(uint32_t pos, std::string_view k, const void* value) {
        const uint32_t cs = 2 + static_cast<uint32_t>(k.size()) + valueSize();
        const uint32_t need = kSlotSize + cs;
        if (contiguousFree() < need) {
            if (contiguousFree() + fragBytes() < need) return false;
            compact();
        }
        const uint16_t n = count();
        const uint16_t cell = static_cast<uint16_t>(cellStart() - cs);
        const uint16_t klen = static_cast<uint16_t>(k.size());
        std::memcpy(p_ + cell, &klen, 2);
        std::memcpy(p_ + cell + 2, k.data(), k.size());
        std::memcpy(p_ + cell + 2 + k.size(), value, valueSize());
        uint8_t* slots = p_ + kHeaderSize;
        std::memmove(slots + kSlotSize * (pos + 1), slots + kSlotSize * pos, kSlotSize * (n - pos));
        std::memcpy(slots + kSlotSize * pos, &cell, 2);
        setCount(static_cast<uint16_t>(n + 1));
        setFreeStart(static_cast<uint16_t>(freeStart() + kSlotSize));
        setCellStart(cell);
        return true;
    }

    // Removes slot pos. Its cell bytes are zeroed and counted in frag_bytes.
    void remove(uint32_t pos) {
        const uint16_t n = count();
        const uint32_t cs = cellSize(pos);
        std::memset(p_ + slot(pos), 0, cs);
        uint8_t* slots = p_ + kHeaderSize;
        std::memmove(slots + kSlotSize * pos, slots + kSlotSize * (pos + 1), kSlotSize * (n - pos - 1));
        setCount(static_cast<uint16_t>(n - 1));
        setFreeStart(static_cast<uint16_t>(freeStart() - kSlotSize));
        std::memset(p_ + freeStart(), 0, kSlotSize);  // keep [free_start, cell_start) zero
        setFragBytes(static_cast<uint16_t>(fragBytes() + cs));
    }

    // Rewrites live cells contiguously at the end of the page; frag_bytes becomes 0.
    void compact() {
        uint8_t tmp[kPageSize];
        const uint16_t n = count();
        uint32_t top = kPageSize;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t cs = cellSize(i);
            top -= cs;
            std::memcpy(tmp + top, p_ + slot(i), cs);
            uint16_t off = static_cast<uint16_t>(top);
            std::memcpy(tmp + kHeaderSize + kSlotSize * i, &off, 2);
        }
        const uint32_t fs = freeStart();
        std::memcpy(p_ + kHeaderSize, tmp + kHeaderSize, kSlotSize * n);
        std::memset(p_ + fs, 0, top - fs);
        std::memcpy(p_ + top, tmp + top, kPageSize - top);
        setCellStart(static_cast<uint16_t>(top));
        setFragBytes(0);
    }

    // Removes every entry (keeps type, level, sibling, leftmost child).
    void clearEntries() {
        std::memset(p_ + kHeaderSize, 0, kPageSize - kHeaderSize);
        setCount(0);
        setFreeStart(static_cast<uint16_t>(kHeaderSize));
        setCellStart(static_cast<uint16_t>(kPageSize));
        setFragBytes(0);
    }

private:
    template <typename T> T ld(size_t off) const { T v; std::memcpy(&v, p_ + off, sizeof(v)); return v; }
    template <typename T> void st(size_t off, T v) { std::memcpy(p_ + off, &v, sizeof(v)); }
    void setCount(uint16_t v) { st<uint16_t>(offsetof(PageHeader, slot_count), v); }
    void setFreeStart(uint16_t v) { st<uint16_t>(offsetof(PageHeader, free_start), v); }
    void setCellStart(uint16_t v) { st<uint16_t>(offsetof(PageHeader, cell_start), v); }
    void setFragBytes(uint16_t v) { st<uint16_t>(offsetof(PageHeader, frag_bytes), v); }

    uint8_t* p_;
};

class BPTree {
public:
    using Ref = vlog::IndexValueRef;
    using ScanResult = std::vector<std::pair<std::string, Ref>>;

    enum class PutStatus { Inserted, Updated, InvalidKey };
    enum class ApplyResult { Applied, Skipped, InvalidKey };

    struct Shape {
        uint32_t height = 0;
        uint32_t leaves = 0;
        uint32_t internals = 0;
        uint32_t empty_leaves = 0;
        uint64_t keys = 0;
    };

    explicit BPTree(Pager& pager) : pager_(pager) {}

    PutStatus put(std::string_view key, const Ref& ref) {
        if (!validKeyLen(key.size())) return PutStatus::InvalidKey;
        std::unique_lock<std::shared_mutex> lk(mu_);
        return putLocked(key, ref);
    }

    std::optional<Ref> get(std::string_view key) const {
        if (!validKeyLen(key.size())) return std::nullopt;
        std::shared_lock<std::shared_mutex> lk(mu_);
        return getLocked(key);
    }

    // Returns true if the key was present.
    bool erase(std::string_view key) {
        if (!validKeyLen(key.size())) return false;
        std::unique_lock<std::shared_mutex> lk(mu_);
        return eraseLocked(key);
    }

    // Entries with start <= key < end, ascending, at most `limit`. start >= end or limit == 0: empty.
    ScanResult scan(std::string_view start, std::string_view end, size_t limit) const {
        ScanResult out;
        if (limit == 0 || compareKeys(start, end) >= 0) return out;
        std::shared_lock<std::shared_mutex> lk(mu_);
        scanLocked(start, &end, limit, &out);
        return out;
    }

    // Entries with key >= start (no upper bound), ascending, at most `limit`.
    ScanResult scanFrom(std::string_view start, size_t limit) const {
        ScanResult out;
        if (limit == 0) return out;
        std::shared_lock<std::shared_mutex> lk(mu_);
        scanLocked(start, nullptr, limit, &out);
        return out;
    }

    // Redo/live apply of one vLog entry under one exclusive lock (plan section 4):
    // if should_apply(...) then Full/Delta -> put {seq, kind=1, vptr}, Tombstone -> erase.
    // *had_key (optional) reports whether the key was in the index before this call.
    ApplyResult apply(std::string_view key, vlog::EntryType type, uint64_t seq, uint8_t flags, const vlog::VPtr& vptr,
                      bool* had_key = nullptr) {
        if (!validKeyLen(key.size())) return ApplyResult::InvalidKey;
        std::unique_lock<std::shared_mutex> lk(mu_);
        std::optional<Ref> cur = getLocked(key);
        if (had_key) *had_key = cur.has_value();
        if (seq > pager_.maxAppliedSeq()) pager_.setMaxAppliedSeq(seq);
        if (!vlog::should_apply(cur.has_value(), cur ? cur->seq : 0, seq, flags)) return ApplyResult::Skipped;
        if (type == vlog::EntryType::Tombstone) {
            eraseLocked(key);
        } else {
            Ref r{};
            r.seq = seq;
            r.kind = 1;
            r.vptr = vptr;
            PutStatus st = putLocked(key, r);
            if (st != PutStatus::Inserted && st != PutStatus::Updated) {
                errno = 0;
                fatal("apply: put failed for a valid key");
            }
        }
        return ApplyResult::Applied;
    }

    uint64_t size() const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return pager_.keyCount();
    }

    // Test-only: keys of every leaf, in leaf-chain order (empty leaves included).
    std::vector<std::vector<std::string>> leafKeysForTest() const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        std::vector<std::vector<std::string>> out;
        PageGuard g = pager_.fetch(pager_.rootPageId());
        while (!Node(g.data()).isLeaf()) {
            PageGuard c = pager_.fetch(Node(g.data()).childAt(0));
            g = std::move(c);
        }
        while (true) {
            Node n(g.data());
            out.emplace_back();
            for (uint32_t i = 0; i < n.count(); ++i) out.back().emplace_back(n.key(i));
            if (n.rightSibling() == kNoPage) break;
            PageGuard ng = pager_.fetch(n.rightSibling());
            g = std::move(ng);
        }
        return out;
    }

    // Test-only: walks the whole tree and checks the invariants listed in plan section 3.
    // Returns "" if all hold, else a description of the first violation.
    std::string checkInvariants(Shape* shape = nullptr) const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        Shape s;
        std::vector<page_id_t> leaves;
        std::vector<page_id_t> path;
        std::string err = checkSubtree(pager_.rootPageId(), -1, std::nullopt, std::nullopt, &leaves, &s, &path);
        if (!err.empty()) return err;
        {
            PageGuard g = pager_.fetch(pager_.rootPageId());
            s.height = Node(g.data()).level() + 1u;
        }
        if (s.height != pager_.treeHeight()) {
            return "meta tree_height " + std::to_string(pager_.treeHeight()) + " != root level + 1 = " +
                   std::to_string(s.height);
        }
        if (s.keys != pager_.keyCount()) {
            return "meta key_count " + std::to_string(pager_.keyCount()) + " != keys in leaves " + std::to_string(s.keys);
        }
        // The leaf chain must visit exactly the leaves found by the tree walk, in key order.
        page_id_t cur = leaves.front();
        for (size_t i = 0; i < leaves.size(); ++i) {
            if (cur != leaves[i]) {
                return "leaf chain position " + std::to_string(i) + " is page " + std::to_string(cur) +
                       ", tree order says " + std::to_string(leaves[i]);
            }
            PageGuard g = pager_.fetch(cur);
            cur = Node(g.data()).rightSibling();
        }
        if (cur != kNoPage) return "last leaf has right_sibling " + std::to_string(cur);
        if (shape) *shape = s;
        return "";
    }

private:
    std::optional<Ref> getLocked(std::string_view key) const {
        PageGuard g = descendToLeaf(key);
        Node n(g.data());
        uint32_t pos = n.lowerBound(key);
        if (pos < n.count() && compareKeys(n.key(pos), key) == 0) return n.leafValue(pos);
        return std::nullopt;
    }

    PageGuard descendToLeaf(std::string_view key) const {
        PageGuard g = pager_.fetch(pager_.rootPageId());
        while (!Node(g.data()).isLeaf()) {
            Node n(g.data());
            PageGuard child = pager_.fetch(n.childAt(n.upperBound(key)));  // pin child before unpinning parent
            g = std::move(child);
        }
        return g;
    }

    PutStatus putLocked(std::string_view key, const Ref& ref) {
        // Descend, keeping every internal page on the path pinned with the child index taken.
        std::vector<PageGuard> path;
        std::vector<uint32_t> path_idx;
        PageGuard leaf = pager_.fetch(pager_.rootPageId());
        while (!Node(leaf.data()).isLeaf()) {
            Node n(leaf.data());
            uint32_t j = n.upperBound(key);
            PageGuard child = pager_.fetch(n.childAt(j));
            path.push_back(std::move(leaf));
            path_idx.push_back(j);
            leaf = std::move(child);
        }
        Node n(leaf.data());
        uint32_t pos = n.lowerBound(key);
        if (pos < n.count() && compareKeys(n.key(pos), key) == 0) {
            n.setLeafValue(pos, ref);  // same cell size: an overwrite never splits
            leaf.markDirty();
            noteSeq(ref.seq);
            return PutStatus::Updated;
        }
        pager_.setKeyCount(pager_.keyCount() + 1);
        noteSeq(ref.seq);
        if (n.insert(pos, key, &ref)) {
            leaf.markDirty();
            return PutStatus::Inserted;
        }

        // Leaf split, then insert the separator into the parents, splitting them as needed.
        std::string sep;
        page_id_t new_child = splitLeaf(leaf, pos, key, ref, &sep);
        page_id_t left_child = leaf.id();
        leaf.release();
        for (size_t lvl = path.size(); lvl-- > 0;) {
            PageGuard& parent = path[lvl];
            Node pn(parent.data());
            uint32_t j = path_idx[lvl];
            if (pn.childAt(j) != left_child) fatal("split: parent does not point at the split child");
            // The new right sibling goes right after the split child: cell position j.
            if (pn.insert(j, sep, &new_child)) {
                parent.markDirty();
                return PutStatus::Inserted;
            }
            std::string up;
            new_child = splitInternal(parent, j, sep, new_child, &up);
            sep = std::move(up);
            left_child = parent.id();
            path.pop_back();  // unpin
        }
        // The root split: grow the tree by one level.
        uint32_t old_level = pager_.treeHeight() - 1;
        if (old_level >= 254) fatal("tree height limit");
        PageGuard root = pager_.allocate(PageType::Internal, static_cast<uint8_t>(old_level + 1));
        Node rn(root.data());
        rn.setLeftmostChild(left_child);
        if (!rn.insert(0, sep, &new_child)) fatal("new root cannot hold one separator");
        root.markDirty();
        pager_.setRoot(root.id(), pager_.treeHeight() + 1);
        return PutStatus::Inserted;
    }

    struct Entry {
        std::string key;
        uint8_t value[kLeafValueSize];  // leaf: IndexValueRef; internal: first 4 bytes are the child id
    };

    static std::vector<Entry> readEntries(const Node& n) {
        std::vector<Entry> out(n.count());
        for (uint32_t i = 0; i < n.count(); ++i) {
            out[i].key.assign(n.key(i));
            std::memcpy(out[i].value, n.valuePtr(i), n.valueSize());
        }
        return out;
    }

    // Split index by bytes (plan 2.4 step 2): first i where the running total of (cell + slot)
    // bytes of E[0..i) reaches half of the grand total, clamped to [lo, hi].
    static size_t splitIndex(const std::vector<Entry>& e, uint32_t value_size, size_t lo, size_t hi) {
        uint64_t total = 0;
        for (const Entry& x : e) total += kSlotSize + 2 + x.key.size() + value_size;
        uint64_t run = 0;
        size_t i = 0;
        while (i < e.size() && run * 2 < total) {
            run += kSlotSize + 2 + e[i].key.size() + value_size;
            ++i;
        }
        return std::min(std::max(i, lo), hi);
    }

    static void fillNode(Node& n, const std::vector<Entry>& e, size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            if (!n.insert(n.count(), e[i].key, e[i].value)) fatal("split half does not fit in a page");
        }
    }

    // Splits a full leaf L while inserting (key, ref) at slot pos. Returns the new right leaf R;
    // *sep = first key of R.
    page_id_t splitLeaf(PageGuard& lg, uint32_t pos, std::string_view key, const Ref& ref, std::string* sep) {
        Node l(lg.data());
        std::vector<Entry> e = readEntries(l);
        Entry ne;
        ne.key.assign(key);
        std::memcpy(ne.value, &ref, sizeof(ref));
        e.insert(e.begin() + pos, std::move(ne));
        size_t s = splitIndex(e, kLeafValueSize, 1, e.size() - 1);

        PageGuard rg = pager_.allocate(PageType::Leaf, 0);
        Node r(rg.data());
        l.clearEntries();
        fillNode(l, e, 0, s);
        fillNode(r, e, s, e.size());
        r.setRightSibling(l.rightSibling());
        l.setRightSibling(rg.id());
        lg.markDirty();
        rg.markDirty();
        *sep = e[s].key;
        return rg.id();
    }

    // Splits a full internal page P while inserting (sep, child) at cell position j.
    // P keeps C0 and E[0..m); P' gets leftmost child E[m].child and E[m+1..n); E[m].key moves up.
    page_id_t splitInternal(PageGuard& pg, uint32_t j, const std::string& sep, page_id_t child, std::string* up) {
        Node p(pg.data());
        std::vector<Entry> e = readEntries(p);
        Entry ne;
        ne.key = sep;
        std::memset(ne.value, 0, sizeof(ne.value));
        std::memcpy(ne.value, &child, sizeof(child));
        e.insert(e.begin() + j, std::move(ne));
        if (e.size() < 3) fatal("internal split with fewer than 3 entries");
        size_t m = splitIndex(e, kChildValueSize, 1, e.size() - 2);

        PageGuard qg = pager_.allocate(PageType::Internal, p.level());
        Node q(qg.data());
        page_id_t mid_child;
        std::memcpy(&mid_child, e[m].value, sizeof(mid_child));
        p.clearEntries();
        fillNode(p, e, 0, m);
        q.setLeftmostChild(mid_child);
        fillNode(q, e, m + 1, e.size());
        pg.markDirty();
        qg.markDirty();
        *up = e[m].key;
        return qg.id();
    }

    bool eraseLocked(std::string_view key) {
        PageGuard leaf = descendToLeaf(key);
        Node n(leaf.data());
        uint32_t pos = n.lowerBound(key);
        if (pos >= n.count() || compareKeys(n.key(pos), key) != 0) return false;
        n.remove(pos);
        leaf.markDirty();
        pager_.setKeyCount(pager_.keyCount() - 1);
        return true;
    }

    void scanLocked(std::string_view start, const std::string_view* end, size_t limit, ScanResult* out) const {
        PageGuard g = descendToLeaf(start);
        uint32_t pos = Node(g.data()).lowerBound(start);
        while (true) {
            Node n(g.data());
            for (; pos < n.count(); ++pos) {
                std::string_view k = n.key(pos);
                if (end && compareKeys(k, *end) >= 0) return;
                out->emplace_back(std::string(k), n.leafValue(pos));
                if (out->size() >= limit) return;
            }
            page_id_t next = n.rightSibling();
            if (next == kNoPage) return;
            PageGuard ng = pager_.fetch(next);  // pin next leaf before unpinning this one
            g = std::move(ng);
            pos = 0;
        }
    }

    void noteSeq(uint64_t seq) {
        if (seq > pager_.maxAppliedSeq()) pager_.setMaxAppliedSeq(seq);
    }

    // expected_level < 0 means "root: any level".
    std::string checkSubtree(page_id_t id, int expected_level, std::optional<std::string> lo,
                             std::optional<std::string> hi, std::vector<page_id_t>* leaves, Shape* s,
                             std::vector<page_id_t>* path) const {
        auto fail = [&](const std::string& m) { return "page " + std::to_string(id) + ": " + m; };
        if (std::find(path->begin(), path->end(), id) != path->end()) return fail("cycle in tree");
        std::vector<std::string> keys;
        std::vector<page_id_t> children;
        int level;
        bool leaf;
        {
            PageGuard g = pager_.fetch(id);
            const uint8_t* p = g.data();
            PageHeader h = loadHeader(p);
            Node n(g.data());
            if (h.magic != kPageMagic) return fail("bad magic");
            if (h.page_id != id) return fail("page_id field " + std::to_string(h.page_id));
            leaf = n.isLeaf();
            level = h.level;
            if (leaf != (h.level == 0)) return fail("leaf flag and level disagree");
            if (!leaf && h.page_type != static_cast<uint8_t>(PageType::Internal)) return fail("bad page type");
            if (expected_level >= 0 && level != expected_level) {
                return fail("level " + std::to_string(level) + ", expected " + std::to_string(expected_level));
            }
            if (h.free_start != kHeaderSize + kSlotSize * h.slot_count) return fail("free_start != 32 + 2*slot_count");
            if (h.free_start > h.cell_start || h.cell_start > kPageSize) return fail("free_start > cell_start");
            if (leaf && h.leftmost_child != 0) return fail("leaf with leftmost_child");
            if (!leaf && h.right_sibling != 0) return fail("internal page with right_sibling");
            if (!leaf && (h.leftmost_child == kNoPage || h.slot_count == 0)) return fail("internal page without children");
            std::vector<std::pair<uint32_t, uint32_t>> cells;
            uint64_t live = 0;
            for (uint32_t i = 0; i < h.slot_count; ++i) {
                uint32_t off = n.slot(i);
                if (off < h.cell_start || off + 2 > kPageSize) return fail("slot " + std::to_string(i) + " out of range");
                uint32_t klen = n.keyLen(i);
                if (!validKeyLen(klen)) return fail("bad key length " + std::to_string(klen));
                uint32_t cs = n.cellSize(i);
                if (off + cs > kPageSize) return fail("cell " + std::to_string(i) + " runs past the page");
                cells.emplace_back(off, off + cs);
                live += cs;
                keys.emplace_back(n.key(i));
                if (!leaf) children.push_back(n.cellChild(i));
            }
            std::sort(cells.begin(), cells.end());
            for (size_t i = 1; i < cells.size(); ++i) {
                if (cells[i].first < cells[i - 1].second) return fail("overlapping cells");
            }
            if (live + h.frag_bytes != kPageSize - h.cell_start) {
                return fail("live cell bytes " + std::to_string(live) + " + frag " + std::to_string(h.frag_bytes) +
                            " != 4096 - cell_start");
            }
            for (uint32_t i = h.free_start; i < h.cell_start; ++i) {
                if (p[i] != 0) return fail("nonzero byte in free gap at " + std::to_string(i));
            }
            if (!leaf) children.insert(children.begin(), h.leftmost_child);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0 && compareKeys(keys[i - 1], keys[i]) >= 0) return fail("keys not strictly ascending");
            if (lo && compareKeys(keys[i], *lo) < 0) return fail("key below the parent's lower bound");
            if (hi && compareKeys(keys[i], *hi) >= 0) return fail("key at or above the parent's upper bound");
        }
        if (leaf) {
            leaves->push_back(id);
            s->leaves++;
            s->keys += keys.size();
            if (keys.empty()) s->empty_leaves++;
            return "";
        }
        s->internals++;
        path->push_back(id);
        for (size_t j = 0; j < children.size(); ++j) {
            std::optional<std::string> clo = (j == 0) ? lo : std::optional<std::string>(keys[j - 1]);
            std::optional<std::string> chi = (j == keys.size()) ? hi : std::optional<std::string>(keys[j]);
            std::string err = checkSubtree(children[j], level - 1, clo, chi, leaves, s, path);
            if (!err.empty()) return err;
        }
        path->pop_back();
        return "";
    }

    Pager& pager_;
    mutable std::shared_mutex mu_;
};

} // namespace bpt
