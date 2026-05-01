// LokiDecode.cpp
//
// Pixel-to-text decoder for Nuke. Decodes upstream Loki-encoded pixels
// into the `prompt_text` knob.
//
// RGB passes through unchanged so downstream Loki nodes still see the
// encoded text. Alpha is filled with random noise every engine() call,
// and a monotonic counter is mixed into the hash via append(). Together
// these make the node's output genuinely change on every pull, which
// stops Nuke from caching us as a noop and short-circuiting _open()
// — without that, the decoded text would go stale when upstream changed.

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Interest.h"
#include "DDImage/Hash.h"

#include <cstring>
#include <algorithm>
#include <atomic>
#include <random>
#include <string>
#include <vector>

using namespace DD::Image;

static const int LK_W = 128;
static const int LK_H = 128;
static const int LK_BPP = 3;
static const int LK_HEADER = 4;

static const char* const CLASS = "LokiDecode";
static const char* const HELP =
  "Decodes text from upstream Loki-encoded pixels into the\n"
  "`prompt_text` knob.\n\n"
  "RGB passes through unchanged. Alpha carries random noise to\n"
  "force re-evaluation when upstream changes.\n\n"
  "Read from Python:\n"
  "  nuke.toNode('LokiDecode1')['prompt_text'].value()\n\n"
  "Read from a TCL expression:\n"
  "  [value LokiDecode1.prompt_text]";


class LokiDecode : public Iop
{
  const char* _textKnob;
  std::atomic<uint64_t> _tick;

public:

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  LokiDecode(Node* node) : Iop(node),
    _textKnob(nullptr),
    _tick(0)
  {}

  ~LokiDecode() override {}

  void knobs(Knob_Callback f) override
  {
    String_knob(f, &_textKnob, "prompt_text", "prompt text");
    Tooltip(f, "Decoded text from upstream pixels.");
    SetFlags(f, Knob::STARTLINE | Knob::INVISIBLE);
  }

  // Monotonic counter mixed into the hash so Nuke sees us as different
  // on every pull. Pattern from NDKExamples/examples/Socket.cpp.
  void append(Hash& hash) override
  {
    hash.append(_tick.fetch_add(1) + 1);
  }

  void _validate(bool for_real) override
  {
    input(0)->validate(for_real);
    copy_info();
    info_.turn_on(Mask_Alpha);
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
  {
    input(0)->request(0, 0, LK_W, LK_H, Mask_RGB, count);
  }

  // _open() runs on a worker thread, locked, before any engine() call.
  // Pixel reads from input(0) are reliable here (unlike in _validate).
  // set_text() from a worker thread is safe as long as the knob is not
  // OUTPUT_ONLY — Nuke marshals the actual mutation to the main thread.
  void _open() override
  {
    const int totalBytes = LK_W * LK_H * LK_BPP;
    std::vector<unsigned char> buf(totalBytes, 0);

    Interest interest(input0(), 0, 0, LK_W, LK_H, Mask_RGB, true);
    interest.unlock();

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

    Knob* k = knob("prompt_text");
    if (k)
      k->set_text(decoded.c_str());
  }

  // Pass RGB through. Fill alpha with random noise — see file header
  // for why this matters.
  void engine(int y, int x, int r, ChannelMask channels, Row& row) override
  {
    ChannelSet fromInput(channels);
    fromInput &= Mask_RGB;
    if (fromInput) {
      row.get(input0(), y, x, r, fromInput);
    }

    if (channels & Mask_Alpha) {
      thread_local std::mt19937 rng(
        (uint32_t)(uintptr_t)&row ^ (uint32_t)y ^ (uint32_t)_tick.load());
      std::uniform_real_distribution<float> dist(0.0f, 1.0f);

      float* dst = row.writable(Chan_Alpha) + x;
      float* end = dst + (r - x);
      while (dst < end) *dst++ = dist(rng);
    }
  }

  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Iop::Description description;
};

static Iop* build(Node* node) { return new LokiDecode(node); }
const Iop::Description LokiDecode::description(CLASS, "Loki/LokiDecode", build);
