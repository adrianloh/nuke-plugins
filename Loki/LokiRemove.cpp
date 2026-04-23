// LokiRemove.cpp
//
// Loki text processing node: decodes text from upstream pixels,
// splits on whitespace, removes a fraction of the words at random,
// and re-encodes to pixels.
//
// Always keeps at least 1 word.
// Seed slider selects which words get removed deterministically.

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

static const char* const CLASS = "LokiRemove";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels, splits on\n"
  "whitespace, and removes a fraction of the words at random.\n"
  "Always keeps at least 1 word.\n\n"
  "Drag the seed slider to scrub through different removals, or\n"
  "press Remove to jump to a random seed.";


class LokiRemove : public Iop
{
  float _rate;
  int _seed;
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiRemove(Node* node) : Iop(node), _rate(0.25f), _seed(0)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiRemove() override {}

  void knobs(Knob_Callback f) override
  {
    Float_knob(f, &_rate, "rate", "removal rate");
    Tooltip(f, "Fraction of words to remove.\n"
               "0.0 = keep all, 1.0 = keep only 1 word.");
    SetRange(f, 0.0, 1.0);

    Int_knob(f, &_seed, "seed", "seed");
    Tooltip(f, "Random seed. Drag to scrub through different removals,\n"
               "or press Remove to jump to a random seed.");
    SetRange(f, 0, 1000);
    SetFlags(f, Knob::DO_NOT_WRITE | Knob::SLIDER | Knob::FORCE_RANGE);

    Button(f, "remove", "Remove");
    Tooltip(f, "Re-roll which words get removed.");
  }

  int knob_changed(Knob* k) override
  {
    if (k && std::strcmp(k->name().c_str(), "remove") == 0) {
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

    std::string decoded;
    if (totalBytes >= LK_HEADER) {
      uint32_t len = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
      if (len > 0 && len <= (uint32_t)(totalBytes - LK_HEADER)) {
        decoded.assign((const char*)(buf.data() + LK_HEADER), len);
      }
    }

    // Split on whitespace
    std::vector<std::string> words;
    std::istringstream iss(decoded);
    std::string word;
    while (iss >> word)
      words.push_back(word);

    // Compute how many words to REMOVE, then clamp so we keep at least 1.
    float rate = _rate;
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;

    int total = (int)words.size();
    int toKeep = total;
    if (total > 0) {
      int toRemove = (int)((float)total * rate);
      toKeep = total - toRemove;
      if (toKeep < 1) toKeep = 1;
    }

    if (toKeep < total && total > 0) {
      // Build an index list, shuffle it, then keep the first toKeep indices.
      // Preserve original order of the kept words by sorting indices back.
      std::vector<int> indices(total);
      for (int i = 0; i < total; i++)
        indices[i] = i;

      std::mt19937 rng((uint32_t)_seed);
      std::shuffle(indices.begin(), indices.end(), rng);
      indices.resize(toKeep);
      std::sort(indices.begin(), indices.end());

      std::vector<std::string> kept;
      kept.reserve(toKeep);
      for (int idx : indices)
        kept.push_back(words[idx]);
      words = std::move(kept);
    }

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

Format LokiRemove::_fmt;
bool LokiRemove::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiRemove(node); }
const Iop::Description LokiRemove::description(CLASS, "Loki/LokiRemove", build);
