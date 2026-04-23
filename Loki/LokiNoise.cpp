// LokiNoise.cpp
//
// Loki text processing node: decodes text from upstream pixels,
// replaces a fraction of characters with random ASCII letters,
// and re-encodes to pixels.
//
// The replacement only runs when the user presses the "Noise" button —
// between presses the output is stable (same input, same rate → same output).

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
#include <random>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;
static const int LK_MAX_TEXT = LK_W * LK_H * LK_BPP - LK_HEADER;

static const char* const CLASS = "LokiNoise";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels and replaces\n"
  "a percentage of characters with random ASCII letters.\n"
  "Re-encodes the noised result to pixels.\n\n"
  "Press the Noise button to re-roll the randomness.\n"
  "Output is stable between presses.";


class LokiNoise : public Iop
{
  float _rate;        // 0.0 .. 1.0, fraction of chars to replace
  int _seed;          // updated by the Noise button
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiNoise(Node* node) : Iop(node), _rate(0.10f), _seed(0)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiNoise() override {}

  void knobs(Knob_Callback f) override
  {
    Float_knob(f, &_rate, "rate", "replacement rate");
    Tooltip(f, "Fraction of characters to replace with random ASCII letters.\n"
               "0.0 = no replacement, 1.0 = replace every character.");
    SetRange(f, 0.0, 1.0);

    // Seed — visible so the user can scrub through variations manually.
    // The Noise button randomizes it; dragging the slider does the same
    // thing deterministically.
    Int_knob(f, &_seed, "seed", "seed");
    Tooltip(f, "Random seed. Drag to scrub through noise variations, or\n"
               "press Noise to jump to a random seed.");
    SetRange(f, 0, 1000);
    SetFlags(f, Knob::DO_NOT_WRITE | Knob::SLIDER | Knob::FORCE_RANGE);

    Button(f, "noise", "Noise");
    Tooltip(f, "Re-roll the noise with a random seed.");
  }

  int knob_changed(Knob* k) override
  {
    if (k && std::strcmp(k->name().c_str(), "noise") == 0) {
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

    std::string decoded;
    if (totalBytes >= LK_HEADER) {
      uint32_t len = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
      if (len > 0 && len <= (uint32_t)(totalBytes - LK_HEADER)) {
        decoded.assign((const char*)(buf.data() + LK_HEADER), len);
      }
    }

    // Apply noise: replace a fraction of characters with random ASCII letters.
    // Match the Python behaviour: int(n * rate) replacements, unique indices,
    // random letter from [A-Za-z] for each.
    float rate = _rate;
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;

    std::string result = decoded;
    int n = (int)result.size();
    int numToReplace = (int)((float)n * rate);

    if (n > 0 && numToReplace > 0) {
      std::mt19937 rng((uint32_t)_seed);

      // Build a shuffled index list and take the first numToReplace.
      // Equivalent to random.sample(range(n), numToReplace).
      std::vector<int> indices(n);
      for (int i = 0; i < n; i++)
        indices[i] = i;
      std::shuffle(indices.begin(), indices.end(), rng);

      // ASCII letters: A-Z (26) and a-z (26) = 52 characters
      static const char letters[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
      std::uniform_int_distribution<int> letterDist(0, 51);

      for (int i = 0; i < numToReplace; i++) {
        result[indices[i]] = letters[letterDist(rng)];
      }
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

Format LokiNoise::_fmt;
bool LokiNoise::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiNoise(node); }
const Iop::Description LokiNoise::description(CLASS, "Loki/LokiNoise", build);
