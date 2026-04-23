// LokiSort.cpp
//
// Loki text processing node: decodes text from upstream pixels,
// splits on whitespace, sorts the words (ascending or descending,
// case-insensitive), and re-encodes to pixels.

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

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;
static const int LK_MAX_TEXT = LK_W * LK_H * LK_BPP - LK_HEADER;

static const char* const CLASS = "LokiSort";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels, splits on\n"
  "whitespace, sorts the words (case-insensitive), and re-encodes\n"
  "to pixels.";


static const char* const direction_names[] = { "ascending", "descending", nullptr };


class LokiSort : public Iop
{
  int _direction; // 0 = ascending, 1 = descending
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiSort(Node* node) : Iop(node), _direction(0)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiSort() override {}

  void knobs(Knob_Callback f) override
  {
    Enumeration_knob(f, &_direction, direction_names, "direction", "direction");
    Tooltip(f, "Sort direction (case-insensitive).");
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

    std::vector<std::string> words;
    std::istringstream iss(decoded);
    std::string word;
    while (iss >> word)
      words.push_back(word);

    // Case-insensitive comparison: compare ASCII bytes as lowercase,
    // leave non-ASCII bytes alone (just compare raw).
    auto caseInsensitiveLess = [](const std::string& a, const std::string& b) {
      size_t n = std::min(a.size(), b.size());
      for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca < 0x80) ca = (unsigned char)std::tolower(ca);
        if (cb < 0x80) cb = (unsigned char)std::tolower(cb);
        if (ca != cb) return ca < cb;
      }
      return a.size() < b.size();
    };

    if (_direction == 0)
      std::sort(words.begin(), words.end(), caseInsensitiveLess);
    else
      std::sort(words.begin(), words.end(),
                [&](const std::string& a, const std::string& b) {
                  return caseInsensitiveLess(b, a);
                });

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

Format LokiSort::_fmt;
bool LokiSort::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiSort(node); }
const Iop::Description LokiSort::description(CLASS, "Loki/LokiSort", build);
