// LokiDecode.cpp
//
// Pixel-to-text decoder for Nuke. Terminal/inspection node.
// Takes encoded pixels from upstream, passes them through unchanged,
// and decodes the text into a read-only knob.
//
// The Viewer pulls through this node, so the decoded text is always
// current when upstream changes.
//
// Encoding expected: first 4 bytes = string length (LE uint32),
// then raw string bytes packed R-G-B sequentially across pixels,
// row by row top-down.

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Interest.h"
#include "DDImage/Hash.h"

#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <chrono>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;

static const char* const CLASS = "LokiDecode";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels.\n\n"
  "Passes pixels through unchanged. The decoded text appears in\n"
  "the in-panel watcher widget and is accessible via Python:\n"
  "  nuke.toNode('LokiDecode1')['prompt_text'].value()";


class LokiDecode : public Iop
{
  const char* _textKnob;
  std::string _decodedText;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiDecode(Node* node) : Iop(node),
    _textKnob(nullptr)
  {}

  ~LokiDecode() override {}

  void knobs(Knob_Callback f) override
  {
    // Single-line knob. Populated by _open() from upstream pixels.
    // OUTPUT_ONLY so set_text() never affects the node hash.
    // Hidden (INVISIBLE) because humans should watch via the custom
    // Qt widget added by Python on node creation — this knob is the
    // data source for your pipeline, not something to look at.
    String_knob(f, &_textKnob, "prompt_text", "prompt text");
    Tooltip(f, "Decoded text from upstream pixels.\n"
               "Populated by the C++ decoder. Access via Python:\n"
               "  node['prompt_text'].value()");
    SetFlags(f, Knob::STARTLINE | Knob::OUTPUT_ONLY | Knob::INVISIBLE);
  }

  // Ensure _open() always runs by preventing stale cache hits.
  // Time-based hash means Nuke always sees us as "different."
  void append(Hash& hash) override
  {
    hash.append((U64)std::chrono::steady_clock::now().time_since_epoch().count());
  }

  void _validate(bool for_real) override
  {
    copy_info();
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
  {
    input(0)->request(0, 0, LK_W, LK_H, Mask_RGB, count);
  }

  // _open() runs after _request(), on a worker thread, locked,
  // before any engine() calls. Decode the full upstream image here.
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

    _decodedText.clear();
    if (totalBytes >= LK_HEADER) {
      uint32_t len = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
      if (len > 0 && len <= (uint32_t)(totalBytes - LK_HEADER)) {
        _decodedText.assign((const char*)(buf.data() + LK_HEADER), len);
      }
    }

    Knob* k = knob("prompt_text");
    if (k)
      k->set_text(_decodedText.c_str());
  }

  // Pass input pixels through unchanged
  void engine(int y, int x, int r, ChannelMask channels, Row& row) override
  {
    row.get(input0(), y, x, r, channels);
  }

  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Iop::Description description;
};

static Iop* build(Node* node) { return new LokiDecode(node); }
const Iop::Description LokiDecode::description(CLASS, "Loki/LokiDecode", build);
