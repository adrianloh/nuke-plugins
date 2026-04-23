// LokiShuffle.cpp
//
// Loki text processing node: decodes text from upstream pixels,
// strips punctuation, splits on whitespace, and shuffles word order.
// Re-encodes the result to pixels.
//
// Shuffle only happens when the user presses the "Shuffle" button.
// Between presses the output is stable (same input → same output).

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Knob.h"
#include "DDImage/Format.h"
#include "DDImage/Interest.h"

#include <cstring>
#include <cctype>
#include <climits>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <random>
#include <chrono>
#include <unordered_set>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;
static const int LK_MAX_TEXT = LK_W * LK_H * LK_BPP - LK_HEADER;

static const char* const CLASS = "LokiShuffle";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels, strips\n"
  "punctuation, splits on whitespace, and shuffles the words.\n"
  "Re-encodes the result to pixels.\n\n"
  "Press the Shuffle button to produce a new random order.\n"
  "Output is stable between presses.";


class LokiShuffle : public Iop
{
  int _seed;    // updated by the shuffle button; part of the hash
  bool _unique; // if true, remove duplicate words (case-insensitive)
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiShuffle(Node* node) : Iop(node), _seed(0), _unique(false)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiShuffle() override {}

  void knobs(Knob_Callback f) override
  {
    Bool_knob(f, &_unique, "unique", "unique");
    Tooltip(f, "Remove duplicate words from the output (case-insensitive).\n"
               "First occurrence of each word is kept.");

    // Seed — visible so the user can scrub through shuffle orders.
    Int_knob(f, &_seed, "seed", "seed");
    Tooltip(f, "Random seed for shuffle. Drag to scrub, or press\n"
               "Shuffle to jump to a random seed.");
    SetRange(f, 0, 1000);
    SetFlags(f, Knob::DO_NOT_WRITE | Knob::SLIDER | Knob::FORCE_RANGE);

    Button(f, "shuffle", "Shuffle");
    Tooltip(f, "Re-roll the shuffle order with a random seed.");
  }

  int knob_changed(Knob* k) override
  {
    if (k && std::strcmp(k->name().c_str(), "shuffle") == 0) {
      // Keep the random seed within the slider range so the visible
      // slider reflects the actual seed after the button is pressed.
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<int> dist(0, 1000);
      int newSeed = dist(gen);
      knob("seed")->set_value((double)newSeed);
      return 1;
    }
    return Iop::knob_changed(k);
  }

  void _validate(bool for_real) override
  {
    input(0)->validate(for_real);

    info_.format(_fmt);
    info_.full_size_format(_fmt);
    info_.channels(Mask_RGB);
    info_.set(0, 0, LK_W, LK_H);
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
  {
    input(0)->request(0, 0, LK_W, LK_H, Mask_RGB, count);
  }

  void _open() override
  {
    // Decode upstream pixels
    Interest interest(input0(), 0, 0, LK_W, LK_H, Mask_RGB, true);
    interest.unlock();

    const int totalBytes = LK_W * LK_H * LK_BPP;
    std::vector<unsigned char> buf(totalBytes, 0);

    for (int y = 0; y < LK_H; y++) {
      Row row(0, LK_W);
      row.get(input0(), y, 0, LK_W, Mask_RGB);

      int imgRow = (LK_H - 1) - y;
      int offset = imgRow * LK_W * LK_BPP;

      for (int x = 0; x < LK_W; x++) {
        int px = offset + x * LK_BPP;
        buf[px + 0] = (unsigned char)(std::min(std::max(row[Chan_Red][x]   * 255.0f + 0.5f, 0.0f), 255.0f));
        buf[px + 1] = (unsigned char)(std::min(std::max(row[Chan_Green][x] * 255.0f + 0.5f, 0.0f), 255.0f));
        buf[px + 2] = (unsigned char)(std::min(std::max(row[Chan_Blue][x]  * 255.0f + 0.5f, 0.0f), 255.0f));
      }
    }

    // Decode text
    std::string decoded;
    if (totalBytes >= LK_HEADER) {
      uint32_t len = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
      if (len > 0 && len <= (uint32_t)(totalBytes - LK_HEADER)) {
        decoded.assign((const char*)(buf.data() + LK_HEADER), len);
      }
    }

    // Strip ASCII punctuation only: drop bytes in the ASCII range
    // (0x00-0x7F) that aren't letters, digits, or whitespace.
    // Bytes with the high bit set (0x80+) are UTF-8 continuation or
    // lead bytes and are preserved intact, so multi-byte characters
    // like em-dash, curly quotes, accented letters etc. survive.
    std::string cleaned;
    cleaned.reserve(decoded.size());
    for (unsigned char c : decoded) {
      if (c >= 0x80) {
        // Non-ASCII byte (part of a UTF-8 sequence) — keep as-is
        cleaned += (char)c;
      }
      else if (std::isalnum(c) || std::isspace(c)) {
        // ASCII letter/digit/whitespace — keep
        cleaned += (char)c;
      }
      // else: ASCII punctuation — drop
    }

    // Split on whitespace, lowercasing as we go (ASCII only — UTF-8
    // multi-byte sequences pass through untouched)
    std::vector<std::string> words;
    std::istringstream iss(cleaned);
    std::string word;
    while (iss >> word) {
      for (char& c : word) {
        unsigned char uc = (unsigned char)c;
        if (uc < 0x80)
          c = (char)std::tolower(uc);
      }
      words.push_back(word);
    }

    // Optionally remove duplicates. Since we've already lowercased
    // every word, comparisons are straightforward.
    if (_unique && !words.empty()) {
      std::vector<std::string> deduped;
      deduped.reserve(words.size());
      std::unordered_set<std::string> seen;
      for (const std::string& w : words) {
        if (seen.insert(w).second)
          deduped.push_back(w);
      }
      words = std::move(deduped);
    }

    // Shuffle using the seed knob (stable between Shuffle presses)
    std::mt19937 rng((uint32_t)_seed);
    std::shuffle(words.begin(), words.end(), rng);

    // Rejoin
    std::string result;
    for (size_t i = 0; i < words.size(); i++) {
      if (i > 0) result += ' ';
      result += words[i];
    }

    _encodeText = result;
  }

  void engine(int y, int x, int xr, ChannelMask channels, Row& row) override
  {
    int imgRow = (LK_H - 1) - y;
    int rowByteStart = imgRow * LK_W * LK_BPP;

    const std::string& text = _encodeText;
    uint32_t textLen = (uint32_t)std::min((int)text.size(), LK_MAX_TEXT);

    auto getByte = [&](int byteIndex) -> unsigned char {
      if (byteIndex < LK_HEADER)
        return (unsigned char)((textLen >> (byteIndex * 8)) & 0xFF);
      int ti = byteIndex - LK_HEADER;
      if (ti >= 0 && ti < (int)textLen)
        return (unsigned char)text[ti];
      return 0;
    };

    Channel chanList[3] = { Chan_Red, Chan_Green, Chan_Blue };
    for (int ci = 0; ci < 3; ci++) {
      Channel z = chanList[ci];
      if (!(channels & z))
        continue;

      float* dst = row.writable(z) + x;
      for (int px = x; px < xr; px++) {
        int byteIndex = rowByteStart + px * LK_BPP + ci;
        *dst++ = getByte(byteIndex) / 255.0f;
      }
    }
  }

  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Iop::Description description;
};

Format LokiShuffle::_fmt;
bool LokiShuffle::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiShuffle(node); }
const Iop::Description LokiShuffle::description(CLASS, "Loki/LokiShuffle", build);
