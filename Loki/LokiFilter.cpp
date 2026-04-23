// LokiFilter.cpp
//
// Loki text processing node: decodes text from upstream pixels,
// removes any words that appear in a user-supplied filter list
// (case-insensitive), and re-encodes to pixels.
//
// Filter list is space-delimited. Evaluates live — output updates
// whenever upstream or the filter list changes.

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Knob.h"
#include "DDImage/Format.h"
#include "DDImage/Interest.h"

#include <cstring>
#include <cctype>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_set>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;
static const int LK_MAX_TEXT = LK_W * LK_H * LK_BPP - LK_HEADER;

static const char* const CLASS = "LokiFilter";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels and removes\n"
  "any words that match the filter list (case-insensitive).\n"
  "Re-encodes the filtered result to pixels.\n\n"
  "Enter space-delimited words in the filter field.\n"
  "Output updates live.";


static std::string toLowerAscii(const std::string& s)
{
  std::string out = s;
  for (char& c : out) {
    unsigned char uc = (unsigned char)c;
    if (uc < 0x80)
      c = (char)std::tolower(uc);
  }
  return out;
}


class LokiFilter : public Iop
{
  const char* _filterKnob;
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiFilter(Node* node) : Iop(node), _filterKnob(nullptr)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiFilter() override {}

  void knobs(Knob_Callback f) override
  {
    Multiline_String_knob(f, &_filterKnob, "filter_words", "filter words");
    Tooltip(f, "Space-delimited list of words to remove (case-insensitive).\n"
               "Supports TCL expressions.");
    SetFlags(f, Knob::STARTLINE);
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

    // Build the filter set (lowercased for case-insensitive matching)
    std::unordered_set<std::string> filterSet;
    if (_filterKnob) {
      std::istringstream fiss(_filterKnob);
      std::string fw;
      while (fiss >> fw)
        filterSet.insert(toLowerAscii(fw));
    }

    // Split input on whitespace, keep words not in the filter set
    std::vector<std::string> kept;
    std::istringstream iss(decoded);
    std::string word;
    while (iss >> word) {
      if (filterSet.find(toLowerAscii(word)) == filterSet.end())
        kept.push_back(word);
    }

    // Rejoin
    std::string result;
    for (size_t i = 0; i < kept.size(); i++) {
      if (i > 0) result += ' ';
      result += kept[i];
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

Format LokiFilter::_fmt;
bool LokiFilter::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiFilter(node); }
const Iop::Description LokiFilter::description(CLASS, "Loki/LokiFilter", build);
