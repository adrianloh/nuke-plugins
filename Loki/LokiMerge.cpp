// LokiMerge.cpp
//
// Loki two-input merge.
//
// Modes:
//   append : A + " " + B   (default, simple concatenation)
//   lerp   : per-word random pick from A or B, weighted by 'mix'.
//            mix=0 → all A, mix=1 → all B, mix=0.5 → ~half each.
//            The specific picks are deterministic given a seed.

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Knob.h"
#include "DDImage/Format.h"
#include "DDImage/Interest.h"

#include <cstring>
#include <climits>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <random>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;
static const int LK_MAX_TEXT = LK_W * LK_H * LK_BPP - LK_HEADER;

static const char* const CLASS = "LokiMerge";
static const char* const HELP =
  "Combines text from two inputs.\n\n"
  "Mode:\n"
  "  append : A + \" \" + B (default)\n"
  "  lerp   : pick each word from A or B based on mix & seed.\n"
  "           mix=0 -> all A, mix=1 -> all B, in between -> blend.\n"
  "\n"
  "A (input 0): first text\n"
  "B (input 1): second text";

static const char* const mode_names[] = { "append", "lerp", nullptr };


// Decode a 128x128 RGB image into a text string.
// Reads one full frame from the given Iop and returns the decoded text.
static std::string decodeFromInput(Iop* src)
{
  Interest interest(*src, 0, 0, LK_W, LK_H, Mask_RGB, true);
  interest.unlock();

  const int totalBytes = LK_W * LK_H * LK_BPP;
  std::vector<unsigned char> buf(totalBytes, 0);

  for (int y = 0; y < LK_H; y++) {
    Row row(0, LK_W);
    row.get(*src, y, 0, LK_W, Mask_RGB);

    int imgRow = (LK_H - 1) - y;
    int offset = imgRow * LK_W * LK_BPP;

    for (int x = 0; x < LK_W; x++) {
      int px = offset + x * LK_BPP;
      buf[px + 0] = (unsigned char)(std::min(std::max(row[Chan_Red][x]   * 255.0f + 0.5f, 0.0f), 255.0f));
      buf[px + 1] = (unsigned char)(std::min(std::max(row[Chan_Green][x] * 255.0f + 0.5f, 0.0f), 255.0f));
      buf[px + 2] = (unsigned char)(std::min(std::max(row[Chan_Blue][x]  * 255.0f + 0.5f, 0.0f), 255.0f));
    }
  }

  std::string decoded;
  if (totalBytes >= LK_HEADER) {
    uint32_t len = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    if (len > 0 && len <= (uint32_t)(totalBytes - LK_HEADER)) {
      decoded.assign((const char*)(buf.data() + LK_HEADER), len);
    }
  }
  return decoded;
}


static std::vector<std::string> splitWords(const std::string& s)
{
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string w;
  while (iss >> w)
    out.push_back(w);
  return out;
}


class LokiMerge : public Iop
{
  int _mode;        // 0 = append, 1 = lerp
  float _mix;       // 0..1 for lerp mode
  int _seed;        // seed for lerp word selection
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 2; }
  int maximum_inputs() const override { return 2; }

  // Label inputs "A" and "B" like Nuke's native Merge node
  const char* input_label(int n, char*) const override
  {
    switch (n) {
      case 0: return "A";
      case 1: return "B";
      default: return "";
    }
  }

  LokiMerge(Node* node) : Iop(node),
    _mode(0),
    _mix(0.5f),
    _seed(0)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiMerge() override {}

  void knobs(Knob_Callback f) override
  {
    Enumeration_knob(f, &_mode, mode_names, "mode", "mode");
    Tooltip(f, "append: concatenate A and B with a space.\n"
               "lerp: randomly pick each word from A or B.");

    Divider(f, "Lerp controls");

    Float_knob(f, &_mix, "mix", "mix");
    Tooltip(f, "Balance between A and B in lerp mode.\n"
               "0 = all A, 1 = all B, 0.5 = about half each.");
    SetRange(f, 0.0, 1.0);

    Int_knob(f, &_seed, "seed", "seed");
    Tooltip(f, "Seed for the lerp word selection. Drag to scrub through\n"
               "different combinations at the same mix level, or press\n"
               "Randomize to jump to a new seed.");
    SetRange(f, 0, 1000);
    SetFlags(f, Knob::DO_NOT_WRITE | Knob::SLIDER | Knob::FORCE_RANGE);

    Button(f, "randomize", "Randomize");
    Tooltip(f, "Re-roll the lerp seed with a random value.");
  }

  int knob_changed(Knob* k) override
  {
    if (k && std::strcmp(k->name().c_str(), "randomize") == 0) {
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
    input(1)->validate(for_real);

    info_.format(_fmt);
    info_.full_size_format(_fmt);
    info_.channels(Mask_RGB);
    info_.set(0, 0, LK_W, LK_H);
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
  {
    input(0)->request(0, 0, LK_W, LK_H, Mask_RGB, count);
    input(1)->request(0, 0, LK_W, LK_H, Mask_RGB, count);
  }

  void _open() override
  {
    std::string a = decodeFromInput(input(0));
    std::string b = decodeFromInput(input(1));

    if (_mode == 0) {
      // Append: A + " " + B, skipping separator if either side is empty
      if (a.empty())
        _encodeText = b;
      else if (b.empty())
        _encodeText = a;
      else
        _encodeText = a + " " + b;
      return;
    }

    // Lerp: walk through positions and pick a word from A or B at each one.
    // We take the longer of the two word lists as the output length, and
    // at each position coin-flip (weighted by mix) between A and B. If the
    // chosen side has run out of words, fall back to the other side.
    std::vector<std::string> wordsA = splitWords(a);
    std::vector<std::string> wordsB = splitWords(b);

    if (wordsA.empty() && wordsB.empty()) {
      _encodeText.clear();
      return;
    }

    float mix = _mix;
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    size_t outLen = std::max(wordsA.size(), wordsB.size());

    std::mt19937 rng((uint32_t)_seed);
    std::uniform_real_distribution<float> coin(0.0f, 1.0f);

    std::vector<std::string> result;
    result.reserve(outLen);

    for (size_t i = 0; i < outLen; i++) {
      bool pickB = (coin(rng) < mix);
      bool haveA = (i < wordsA.size());
      bool haveB = (i < wordsB.size());

      if (pickB && haveB)
        result.push_back(wordsB[i]);
      else if (!pickB && haveA)
        result.push_back(wordsA[i]);
      else if (haveA)
        result.push_back(wordsA[i]);
      else if (haveB)
        result.push_back(wordsB[i]);
      // else: shouldn't happen given outLen is the max
    }

    std::string joined;
    for (size_t i = 0; i < result.size(); i++) {
      if (i > 0) joined += ' ';
      joined += result[i];
    }
    _encodeText = joined;
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

Format LokiMerge::_fmt;
bool LokiMerge::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiMerge(node); }
const Iop::Description LokiMerge::description(CLASS, "Loki/LokiMerge", build);
