#include "stencil_census.h"

#include <algorithm>
#include <cstdio>

namespace sm2m {

int StencilCensus::Find(uint32_t key) {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].key == key) return int(i);
    if (int(entries_.size()) >= kMaxKeys) return -1;
    Entry e;
    e.key = key;
    entries_.push_back(e);
    return int(entries_.size()) - 1;
}

void StencilCensus::NoteList(const uint32_t* keys, const uint32_t* draws, int n) {
    LockGuard lock(mu_);
    for (int i = 0; i < n; ++i) {
        const int e = Find(keys[i]);
        if (e < 0) {
            ++overflow_;
            continue;
        }
        entries_[size_t(e)].draws += draws[i];
        if (std::find(frame_.begin(), frame_.end(), e) == frame_.end()) frame_.push_back(e);
    }
}

void StencilCensus::SetWindow(int frames) {
    LockGuard lock(mu_);
    window_ = std::max(1, std::min(kHistory, frames));
}

void StencilCensus::EndFrame(bool heroVisible, bool skip) {
    LockGuard lock(mu_);
    if (skip) {
        frame_.clear();
        return;
    }
    const int kind = heroVisible ? 1 : 2;
    (heroVisible ? withHero_ : without_) += 1;
    for (int e : frame_) (heroVisible ? entries_[size_t(e)].withHero : entries_[size_t(e)].without) += 1;
    if (history_.empty()) {
        history_.resize(kHistory);
        beforeCount_.assign(kMaxKeys, 0);
        afterCount_.assign(kMaxKeys, 0);
    }
    // A switch (M): the frames just before it, from the history, and from now
    // on the ones just after it.
    if (lastKind_ != 0 && kind != lastKind_) {
        if (afterKind_ != 0) Commit(); // (the last switch's, cut short by this one)
        std::fill(beforeCount_.begin(), beforeCount_.end(), 0);
        std::fill(afterCount_.begin(), afterCount_.end(), 0);
        beforeKind_ = lastKind_;
        beforeFrames_ = 0;
        for (int k = 0; k < historyCount_ && beforeFrames_ < window_; ++k) {
            const Past& p = history_[size_t((historyNext_ - 1 - k + kHistory) % kHistory)];
            if (p.kind != lastKind_) break;
            for (int j = 0; j < p.n; ++j) ++beforeCount_[p.idx[j]];
            ++beforeFrames_;
        }
        afterKind_ = kind;
        afterFrames_ = 0;
    }
    if (afterKind_ == kind) {
        for (int e : frame_) ++afterCount_[size_t(e)];
        if (++afterFrames_ >= window_) Commit();
    }
    Past& p = history_[size_t(historyNext_)];
    p.kind = uint8_t(kind);
    p.n = 0;
    for (int e : frame_)
        if (p.n < sizeof(p.idx)) p.idx[p.n++] = uint8_t(e);
    historyNext_ = (historyNext_ + 1) % kHistory;
    historyCount_ = std::min(kHistory, historyCount_ + 1);
    lastKind_ = kind;
    frame_.clear();
}

void StencilCensus::Commit() {
    // Too little on either side (a switch right after another): not counted.
    const int need = std::max(1, window_ / 4);
    if (beforeKind_ != 0 && afterKind_ != 0 && beforeFrames_ >= need && afterFrames_ >= need) {
        const bool heroBefore = beforeKind_ == 1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            Entry& e = entries_[i];
            const int b = beforeCount_[i], a = afterCount_[i];
            e.winWith += heroBefore ? b : a;
            e.winWithout += heroBefore ? a : b;
        }
        winWith_ += heroBefore ? beforeFrames_ : afterFrames_;
        winWithout_ += heroBefore ? afterFrames_ : beforeFrames_;
        ++switches_;
    }
    beforeKind_ = afterKind_ = 0;
    beforeFrames_ = afterFrames_ = 0;
}

bool StencilCensus::Enough(int minFrames) const {
    LockGuard lock(mu_);
    return switches_ > 0 && winWith_ >= minFrames && winWithout_ >= minFrames;
}

int StencilCensus::FramesWithHero() const {
    LockGuard lock(mu_);
    return withHero_;
}

int StencilCensus::FramesWithout() const {
    LockGuard lock(mu_);
    return without_;
}

int StencilCensus::WindowFramesWithHero() const {
    LockGuard lock(mu_);
    return winWith_;
}

int StencilCensus::WindowFramesWithout() const {
    LockGuard lock(mu_);
    return winWithout_;
}

int StencilCensus::Switches() const {
    LockGuard lock(mu_);
    return switches_;
}

int StencilCensus::Overflow() const {
    LockGuard lock(mu_);
    return overflow_;
}

std::vector<StencilCensus::Finding> StencilCensus::HeroMarks(int minFrames, float minShare, float maxOther) const {
    LockGuard lock(mu_);
    std::vector<Finding> out;
    if (switches_ == 0 || winWith_ < minFrames || winWithout_ < minFrames || winWith_ <= 0 || winWithout_ <= 0)
        return out;
    for (const Entry& e : entries_) {
        Finding f;
        f.key = e.key;
        f.withHero = float(e.winWith) / float(winWith_);
        f.without = float(e.winWithout) / float(winWithout_);
        f.draws = e.draws;
        if (f.withHero >= minShare && f.without <= maxOther) out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.withHero > b.withHero; });
    return out;
}

std::vector<StencilCensus::Finding> StencilCensus::CommonMarks(int minFrames, float minShare) const {
    LockGuard lock(mu_);
    std::vector<Finding> out;
    if (withHero_ < minFrames || without_ < minFrames || withHero_ <= 0 || without_ <= 0) return out;
    for (const Entry& e : entries_) {
        Finding f;
        f.key = e.key;
        f.withHero = float(e.withHero) / float(withHero_);
        f.without = float(e.without) / float(without_);
        f.draws = e.draws;
        if (f.withHero >= minShare && f.without >= minShare) out.push_back(f);
    }
    // What the passes Mario is drawn in write first (the objects drawn with
    // him - characters, cars - not the static world, which has most draws).
    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
        if (MarioPass(a.key) != MarioPass(b.key)) return MarioPass(a.key);
        return a.draws > b.draws;
    });
    return out;
}

StencilCensus::Decision StencilCensus::Decide(int minFrames) const {
    Decision d;
    if (!Enough(minFrames)) return d;
    d.ready = true;
    char buf[160];
    for (const Finding& f : HeroMarks(minFrames)) {
        if (PassOp(f.key) != 3) continue; // (replace: a mark - not a count)
        const uint8_t m = WriteMask(f.key);
        if (!m || (d.mask & m)) continue; // (overlapping bits: the more frequent one, first, wins)
        d.ref |= Ref(f.key) & m;
        d.mask |= m;
        std::snprintf(buf, sizeof(buf), "%s0x%02x under mask 0x%02x (%.0f%% of frames with him, %.0f%% without)",
                      d.own.empty() ? "" : ", ", Ref(f.key), m, double(f.withHero) * 100.0, double(f.without) * 100.0);
        d.own += buf;
    }
    for (const Finding& f : CommonMarks(minFrames)) {
        if (PassOp(f.key) != 3 || !GBuffer(f.key)) continue;
        const uint8_t m = WriteMask(f.key) & uint8_t(~d.mask);
        if (!m) continue;
        d.ref |= Ref(f.key) & m;
        d.mask |= m;
        std::snprintf(buf, sizeof(buf), "%s0x%02x under mask 0x%02x", d.shared.empty() ? "" : ", ", Ref(f.key) & m, m);
        d.shared += buf;
    }
    return d;
}

std::string StencilCensus::Describe() const {
    LockGuard lock(mu_);
    static const char* const kOps[] = {"?", "keep", "zero", "replace", "incr-sat", "decr-sat", "invert", "incr", "decr"};
    // The most frequent first, at most 24 (a game writing an id per object
    // would make a line nobody can read).
    std::vector<const Entry*> order;
    for (const Entry& e : entries_) order.push_back(&e);
    std::sort(order.begin(), order.end(),
              [](const Entry* a, const Entry* b) { return a->withHero + a->without > b->withHero + b->without; });
    std::string out;
    size_t shown = 0;
    for (const Entry* e : order) {
        if (shown == 24) break;
        ++shown;
        char buf[224];
        const uint8_t op = PassOp(e->key);
        int n = std::snprintf(buf, sizeof(buf), "%s%s 0x%02x (mask 0x%02x, %s) in %d%% of frames with Spider-Man, %d%% without",
                              out.empty() ? "" : "; ", op < 9 ? kOps[op] : "?", Ref(e->key), WriteMask(e->key),
                              !GBuffer(e->key) ? "depth-only" : (MarioPass(e->key) ? "G-buffer, Mario's passes" : "G-buffer"),
                              withHero_ ? e->withHero * 100 / withHero_ : 0, without_ ? e->without * 100 / without_ : 0);
        if (switches_ > 0 && n > 0 && size_t(n) < sizeof(buf))
            std::snprintf(buf + n, sizeof(buf) - size_t(n), " (around M: %d%% / %d%%)",
                          winWith_ ? e->winWith * 100 / winWith_ : 0, winWithout_ ? e->winWithout * 100 / winWithout_ : 0);
        out += buf;
    }
    if (order.size() > shown) out += "; +" + std::to_string(order.size() - shown) + " more";
    if (overflow_) out += "; " + std::to_string(overflow_) + " write(s) of more kinds not kept";
    return out.empty() ? std::string("no stencil writes into the main view") : out;
}

void StencilCensus::Reset() {
    LockGuard lock(mu_);
    frame_.clear();
    entries_.clear();
    withHero_ = without_ = 0;
    overflow_ = 0;
    history_.clear();
    historyNext_ = historyCount_ = 0;
    lastKind_ = 0;
    beforeKind_ = beforeFrames_ = afterKind_ = afterFrames_ = 0;
    beforeCount_.clear();
    afterCount_.clear();
    winWith_ = winWithout_ = switches_ = 0;
}

} // namespace sm2m
