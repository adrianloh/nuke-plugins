// BaseSixFour.cpp
// Nuke NDK plugin: input pixels -> encoded image -> base64 string.
// Supports WebP (near-lossless default), JPEG, and PNG.
//
// Python usage:
//   nuke.execute(node, nuke.frame(), nuke.frame())
//   b64 = node['_output'].value()

#include "DDImage/DDWindows.h"
#include "DDImage/Iop.h"
#include "DDImage/NukeWrapper.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Executable.h"
#include "DDImage/Interest.h"

#include "webp/encode.h"
#include "stb_image_write.h"

#include <string>
#include <vector>
#include <cstring>
#include <cmath>

using namespace DD::Image;

// ---------------------------------------------------------------------------
// Base64 encoder
// ---------------------------------------------------------------------------

static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len)
{
  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = (uint32_t)data[i] << 16;
    if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len) n |= (uint32_t)data[i + 2];

    out += b64_table[(n >> 18) & 0x3F];
    out += b64_table[(n >> 12) & 0x3F];
    out += (i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? b64_table[n & 0x3F] : '=';
  }
  return out;
}

// ---------------------------------------------------------------------------
// Linear to sRGB conversion
// ---------------------------------------------------------------------------

static inline float linear_to_srgb(float c)
{
  if (c <= 0.0f) return 0.0f;
  if (c >= 1.0f) return 1.0f;
  if (c <= 0.0031308f)
    return c * 12.92f;
  else
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

// ---------------------------------------------------------------------------
// stb_image_write callback
// ---------------------------------------------------------------------------

static void stbi_write_callback(void* context, void* data, int size)
{
  auto* buf = reinterpret_cast<std::vector<uint8_t>*>(context);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
  buf->insert(buf->end(), src, src + size);
}

// ---------------------------------------------------------------------------
// Advanced WebP encode — supports near-lossless
// ---------------------------------------------------------------------------

static bool webpEncodeAdvanced(const uint8_t* rgb, int w, int h, int stride,
                               bool lossy, float quality,
                               uint8_t** outData, size_t* outSize)
{
  WebPConfig config;
  if (!WebPConfigInit(&config))
    return false;

  if (lossy) {
    config.lossless = 0;
    config.quality = quality;
  }
  else {
    config.lossless = 1;
    config.near_lossless = (int)(quality + 0.5f);
    config.quality = 75;
  }

  if (!WebPValidateConfig(&config))
    return false;

  WebPPicture pic;
  if (!WebPPictureInit(&pic))
    return false;

  pic.width = w;
  pic.height = h;
  pic.use_argb = 1;

  if (!WebPPictureImportRGB(&pic, rgb, stride)) {
    WebPPictureFree(&pic);
    return false;
  }

  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &writer;

  int ok = WebPEncode(&config, &pic);
  WebPPictureFree(&pic);

  if (!ok) {
    WebPMemoryWriterClear(&writer);
    return false;
  }

  *outData = writer.mem;
  *outSize = writer.size;
  return true;
}

// ---------------------------------------------------------------------------
// Format enum
// ---------------------------------------------------------------------------

enum OutputFormat {
  FORMAT_WEBP = 0,
  FORMAT_JPEG = 1,
  FORMAT_PNG  = 2
};

static const char* const format_names[] = { "webp", "jpeg", "png", nullptr };

// ---------------------------------------------------------------------------
// BaseSixFour node
// ---------------------------------------------------------------------------

static const char* const CLASS = "BaseSixFour";
static const char* const HELP =
  "Reads input image, encodes to WebP/JPEG/PNG, and outputs as base64 string.\n\n"
  "Default: WebP near-lossless (quality = preprocessing level).\n"
  "Check 'lossy' to switch WebP to lossy VP8 encoding.\n\n"
  "Python usage:\n"
  "  nuke.execute(node, nuke.frame(), nuke.frame())\n"
  "  b64 = node['_output'].value()";

#ifdef FN_OS_WINDOWS
#pragma warning(disable:4355)
#endif

class BaseSixFour : public Iop, Executable
{
  float _quality;
  bool  _lossy;
  int   _format;
  int   _size;
  const char* _output;
  std::string _outputStorage;

public:
  int maximum_inputs() const { return 1; }
  int minimum_inputs() const { return 1; }

  BaseSixFour(Node* node) : Iop(node), Executable(this)
  {
    _quality = 60.0f;
    _lossy = false;
    _format = FORMAT_WEBP;
    _size = 0;
    _output = "";
  }

  ~BaseSixFour() {}

  void _validate(bool for_real)
  {
    copy_info();
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count)
  {
    input(0)->request(input0().info().channels(), count);
  }

  void engine(int y, int x, int r, ChannelMask channels, Row& row)
  {
    row.get(input0(), y, x, r, channels);
  }

  virtual Executable* executable() { return this; }

  void beginExecuting()
  {
    _outputStorage.clear();
  }

  void execute()
  {
    Format format = input0().format();
    const int fx = format.x();
    const int fy = format.y();
    const int fr = format.r();
    const int ft = format.t();
    const int w = fr - fx;
    const int h = ft - fy;

    if (w <= 0 || h <= 0) {
      error("BaseSixFour: invalid image dimensions");
      return;
    }

    ChannelSet readChannels = input0().info().channels();

    Channel chanR = Chan_Red;
    Channel chanG = Chan_Green;
    Channel chanB = Chan_Blue;
    bool hasR = readChannels.contains(chanR);
    bool hasG = readChannels.contains(chanG);
    bool hasB = readChannels.contains(chanB);

    const int stride = w * 3;
    std::vector<uint8_t> rgb(stride * h, 0);

    Interest interest(input0(), fx, fy, fr, ft, readChannels, true);
    interest.unlock();

    for (int ry = fy; ry < ft; ry++) {
      progressFraction(ry - fy, h);

      Row row(fx, fr);
      row.get(input0(), ry, fx, fr, readChannels);
      if (aborted())
        return;

      int dsty = (ft - 1 - ry);
      uint8_t* dst = rgb.data() + dsty * stride;

      const float* rPtr = hasR ? row[chanR] + fx : nullptr;
      const float* gPtr = hasG ? row[chanG] + fx : nullptr;
      const float* bPtr = hasB ? row[chanB] + fx : nullptr;

      for (int x = 0; x < w; x++) {
        float rv = rPtr ? rPtr[x] : 0.0f;
        float gv = gPtr ? gPtr[x] : 0.0f;
        float bv = bPtr ? bPtr[x] : 0.0f;

        dst[x * 3 + 0] = (uint8_t)(linear_to_srgb(rv) * 255.0f + 0.5f);
        dst[x * 3 + 1] = (uint8_t)(linear_to_srgb(gv) * 255.0f + 0.5f);
        dst[x * 3 + 2] = (uint8_t)(linear_to_srgb(bv) * 255.0f + 0.5f);
      }
    }

    if (aborted())
      return;

    // --- Encode ---

    std::vector<uint8_t> encodedBuf;
    bool encodeOK = false;

    float q = _quality;
    if (q < 0.0f) q = 0.0f;
    if (q > 100.0f) q = 100.0f;

    switch (_format) {
      case FORMAT_WEBP: {
        uint8_t* webpOut = nullptr;
        size_t webpSize = 0;

        encodeOK = webpEncodeAdvanced(rgb.data(), w, h, stride,
                                      _lossy, q, &webpOut, &webpSize);
        if (encodeOK && webpOut && webpSize > 0) {
          encodedBuf.assign(webpOut, webpOut + webpSize);
        }
        else {
          encodeOK = false;
        }
        if (webpOut) WebPFree(webpOut);
        break;
      }

      case FORMAT_JPEG: {
        int jpegQuality = (int)(q + 0.5f);
        if (jpegQuality < 1) jpegQuality = 1;
        if (jpegQuality > 100) jpegQuality = 100;

        encodeOK = stbi_write_jpg_to_func(
          stbi_write_callback, &encodedBuf,
          w, h, 3, rgb.data(), jpegQuality) != 0;
        break;
      }

      case FORMAT_PNG: {
        stbi_write_png_compression_level = 8;
        encodeOK = stbi_write_png_to_func(
          stbi_write_callback, &encodedBuf,
          w, h, 3, rgb.data(), stride) != 0;
        break;
      }
    }

    if (!encodeOK || encodedBuf.empty()) {
      error("BaseSixFour: encode failed");
      return;
    }

    _outputStorage = base64_encode(encodedBuf.data(), encodedBuf.size());
  }

  void endExecuting()
  {
    if (!_outputStorage.empty()) {
      knob("_output")->set_text(_outputStorage.c_str());
      knob("size")->set_value((int)_outputStorage.size());
    }
    else {
      knob("size")->set_value(0);
    }
  }

  // --- Knobs ---

  void knobs(Knob_Callback f)
  {
    Enumeration_knob(f, &_format, format_names, "format", "format");
    Tooltip(f, "Output image format.\n"
               "WebP: near-lossless default, smallest files.\n"
               "JPEG: widely compatible, lossy.\n"
               "PNG: lossless, largest files.");

    Float_knob(f, &_quality, "quality", "quality");
    Tooltip(f, "WebP default mode: near-lossless preprocessing level "
               "(0 = most preprocessing/smallest, 60 = default, 100 = true lossless).\n"
               "WebP lossy mode: lossy quality (0 = smallest, 100 = best).\n"
               "JPEG: lossy quality. PNG: ignored.");
    SetRange(f, 0, 100);

    Bool_knob(f, &_lossy, "lossy", "lossy");
    Tooltip(f, "WebP: switch to lossy VP8 encoding. Default is near-lossless.\n"
               "Has no effect on JPEG or PNG.");

    Divider(f);

    // Read-only size indicator
    Int_knob(f, &_size, "size", "size");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION | Knob::DO_NOT_WRITE);
    Tooltip(f, "Size of the base64 output in bytes. 0 = empty buffer.");

    Divider(f);

    // Usage hint
    Text_knob(f, "nuke.execute(node, f, f)  \xe2\x86\x92  node['_output'].value()");

    Divider(f);

    // Invisible output knob
    String_knob(f, &_output, "_output", "_output");
    SetFlags(f, Knob::INVISIBLE | Knob::DO_NOT_WRITE);

    // Render and Copy buttons
    const char* renderScript =
      "node = nuke.thisNode()\n"
      "nuke.execute(node, nuke.frame(), nuke.frame())\n";
    PyScript_knob(f, renderScript, "render", "Render");
    Tooltip(f, "Execute at current frame: read pixels, encode, base64.");

    const char* copyScript =
      "from PySide2.QtWidgets import QApplication\n"
      "node = nuke.thisNode()\n"
      "data = node['_output'].value()\n"
      "if data:\n"
      "    QApplication.clipboard().setText(data)\n"
      "    nuke.message('Copied %d bytes to clipboard' % len(data))\n"
      "else:\n"
      "    nuke.message('Nothing to copy. Hit Render first.')\n";
    PyScript_knob(f, copyScript, "copy", "Copy");
    Tooltip(f, "Copy the base64 string to the system clipboard.");
  }

  const char* Class() const { return CLASS; }
  const char* node_help() const { return HELP; }
  const char* node_label() const { return ""; }

  static const Iop::Description description;
};


static Iop* BaseSixFourCreate(Node* node)
{
  return new BaseSixFour(node);
}

const Iop::Description BaseSixFour::description(CLASS, "Image/BaseSixFour",
                                                 BaseSixFourCreate);
