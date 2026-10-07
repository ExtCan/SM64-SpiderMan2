// Which stencil marks the game draws into its main view only while
// Spider-Man is on screen.
//
// The game marks some of what it draws in the stencil buffer (its depth
// buffer's 8 bits) and later passes read the marks: its temporal
// anti-aliasing and upscalers treat marked pixels differently (bit 0x80:
// "responsive" - less history, no trails), hair and fur draw only over their
// owner's mark. Spider-Man has his own; Mario, drawn into the same G-buffer,
// had whatever the pass he joined happened to write (0.5: the last
// pipeline's stencil writes, with the command list's current reference).
//
// The frame tracker reports the stencil writes into the main view of each
// command list (NoteList); the mod says each frame whether Spider-Man is on
// screen (EndFrame). Around each switch between him and Mario (M), the frames
// just before and just after are compared: the same place, a moment apart.
// A mark in most of the frames with him and in (almost) none of those without
// is his - not something that only happened to be on screen while he was (a
// fountain, the river). Mario gets it, and for the bits it doesn't cover, what
// every G-buffer draw writes (Decide).
//
// Platform independent (unit tested).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../common/platform.h"

namespace sm2m {

class StencilCensus {
public:
    // A stencil write: the bits written (reference & write mask), the write
    // mask, the pass operation (D3D12_STENCIL_OP), whether it was a G-buffer
    // draw (else a depth-only one into the main depth buffer), and whether it
    // was in one of the G-buffer passes Mario is drawn in.
    static uint32_t Key(uint8_t ref, uint8_t writeMask, uint8_t passOp, bool gbuffer, bool marioPass = false) {
        return uint32_t(ref & writeMask) | (uint32_t(writeMask) << 8) | (uint32_t(passOp & 15) << 16) |
               (gbuffer ? (1u << 20) : 0u) | (gbuffer && marioPass ? (1u << 21) : 0u);
    }
    static uint8_t Ref(uint32_t key) { return uint8_t(key & 0xFF); }
    static uint8_t WriteMask(uint32_t key) { return uint8_t((key >> 8) & 0xFF); }
    static uint8_t PassOp(uint32_t key) { return uint8_t((key >> 16) & 15); }
    static bool GBuffer(uint32_t key) { return (key >> 20) & 1; }
    static bool MarioPass(uint32_t key) { return (key >> 21) & 1; }

    static constexpr int kMaxKeys = 255;   // different writes remembered (the rest are counted, not kept)
    static constexpr int kHistory = 512;   // frames remembered, for the window before a switch

    // From the recording threads (any number): one command list's stencil
    // writes - its different keys, and how many draws wrote each.
    void NoteList(const uint32_t* keys, const uint32_t* draws, int n);
    void Note(uint32_t key, uint32_t draws = 1) { NoteList(&key, &draws, 1); }
    // The frame's lists are all in: a frame with Spider-Man on screen or not
    // (`skip`: neither - a menu, photo mode, a load).
    void EndFrame(bool heroVisible, bool skip = false);
    // Frames compared on either side of a switch (at most kHistory).
    void SetWindow(int frames);

    struct Finding {
        uint32_t key = 0;
        float withHero = 0, without = 0; // share of the frames it was drawn in
        uint64_t draws = 0;              // draws that wrote it, in all
    };
    // Marks in at least `minShare` of the frames with him around the switches
    // and at most `maxOther` of those without, once the windows hold
    // `minFrames` of each.
    std::vector<Finding> HeroMarks(int minFrames, float minShare = 0.8f, float maxOther = 0.05f) const;
    // Marks in at least `minShare` of all frames, with him and without (what
    // everything of a kind writes - the world's geometry, say): those in the
    // passes Mario is drawn in first, then the most drawn.
    std::vector<Finding> CommonMarks(int minFrames, float minShare = 0.9f) const;
    // The windows around the switches hold `minFrames` of each kind.
    bool Enough(int minFrames) const;
    int FramesWithHero() const; // all frames counted
    int FramesWithout() const;
    int WindowFramesWithHero() const;
    int WindowFramesWithout() const;
    int Switches() const;       // switches whose windows are in
    int Overflow() const;       // writes not kept (more than kMaxKeys different ones)

    // What Mario's pixels get: Spider-Man's own marks, and for the bits those
    // don't cover, what every G-buffer draw writes (his suit gets that too -
    // drawn into the G-buffer like everything else - before his own mark goes
    // on top). mask 0: nothing (what is behind Mario stays).
    struct Decision {
        bool ready = false;          // enough frames around switches
        uint8_t ref = 0, mask = 0;
        std::string own;             // his own marks, for the log ("" if none)
        std::string shared;          // the G-buffer's, where added
    };
    Decision Decide(int minFrames) const;
    // Every mark seen, with its shares (for the log).
    std::string Describe() const;
    void Reset();

private:
    struct Entry {
        uint32_t key = 0;
        int withHero = 0, without = 0; // frames it was drawn in (all)
        int winWith = 0, winWithout = 0; // ... in the windows around switches
        uint64_t draws = 0;
    };
    struct Past {                    // a frame, for the window before a switch
        uint8_t kind = 0;            // 1 with him, 2 without
        uint8_t n = 0;
        uint8_t idx[62] = {};        // its entries
    };
    int Find(uint32_t key);          // mu_ held; created if there is room (-1: none)
    void Commit();                   // mu_ held: the windows of the last switch
    mutable Mutex mu_;
    std::vector<int> frame_;         // this frame's entries
    std::vector<Entry> entries_;
    int withHero_ = 0, without_ = 0;
    int overflow_ = 0;
    // switches
    int window_ = 150;
    std::vector<Past> history_;      // ring
    int historyNext_ = 0, historyCount_ = 0;
    int lastKind_ = 0;
    int beforeKind_ = 0, beforeFrames_ = 0, afterKind_ = 0, afterFrames_ = 0;
    std::vector<int> beforeCount_, afterCount_; // per entry
    int winWith_ = 0, winWithout_ = 0, switches_ = 0;
};

} // namespace sm2m
