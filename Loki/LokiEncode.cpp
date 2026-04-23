// LokiEncode.cpp
//
// Text-to-pixel encoder for Nuke. Generator node (no inputs).
// Takes a string from a text knob (supports TCL expressions),
// encodes it into a 128x128 RGB image.
//
// Encoding: first 4 bytes = string length (LE uint32),
// then raw string bytes packed R-G-B sequentially across pixels,
// row by row top-down. Remaining pixels are zero (black).
//
// Max capacity: 128 * 128 * 3 - 4 = 49148 bytes of text.

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Format.h"

#include <cstring>
#include <algorithm>
#include <string>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;
static const int LK_MAX_TEXT = LK_W * LK_H * LK_BPP - LK_HEADER;

static const char* const CLASS = "LokiEncode";
static const char* const HELP =
  "Encodes text into a 128x128 RGB image.\n\n"
  "Type text directly or use a TCL expression like\n"
  "[value NoOp1.label] to pull from another node.\n\n"
  "Connect downstream to LokiDecode or any Loki\n"
  "text processing node.";


class LokiEncode : public Iop
{
  const char* _textKnob;
  std::string _encodeText;

  static Format _fmt;
  static bool _fmtRegistered;

public:

  int minimum_inputs() const override { return 0; }
  int maximum_inputs() const override { return 0; }

  LokiEncode(Node* node) : Iop(node),
    _textKnob(nullptr)
  {
    if (!_fmtRegistered) {
      _fmt = Format(LK_W, LK_H, 1.0);
      _fmt.add("Loki128");
      _fmtRegistered = true;
    }
  }

  ~LokiEncode() override {}

  void knobs(Knob_Callback f) override
  {
    Multiline_String_knob(f, &_textKnob, "prompt_text", "prompt_text");
    Tooltip(f, "Text to encode into pixels.\n"
               "Supports TCL expressions, e.g.:\n"
               "  [value NoOp1.label]");
    SetFlags(f, Knob::STARTLINE);
  }

  void _validate(bool for_real) override
  {
    info_.format(_fmt);
    info_.full_size_format(_fmt);
    info_.channels(Mask_RGB);
    info_.set(0, 0, LK_W, LK_H);

    _encodeText = _textKnob ? _textKnob : "";
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
  {
    // Generator — nothing to request
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

Format LokiEncode::_fmt;
bool LokiEncode::_fmtRegistered = false;

static Iop* build(Node* node) { return new LokiEncode(node); }
const Iop::Description LokiEncode::description(CLASS, "Loki/LokiEncode", build);
