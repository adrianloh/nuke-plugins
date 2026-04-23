// webpWriter.cpp
// WebP image writer plugin for Nuke 15.2
// Default: near-lossless encoding (lossless encoder with preprocessing level 60).
// Check "lossy" to switch to lossy VP8 encoding.
// Quality slider controls near-lossless level (default) or lossy quality.
// Embeds description into both XMP and EXIF when provided.

#include "DDImage/DDWindows.h"
#include "DDImage/FileWriter.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/ARRAY.h"

#include "webp/encode.h"
#include "webp/mux.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace DD::Image;

// ---------------------------------------------------------------------------
// EXIF builder
// ---------------------------------------------------------------------------

static std::vector<uint8_t> buildExifWithDescription(const std::string& desc)
{
  uint32_t strLen = (uint32_t)desc.size() + 1;
  bool strInline = (strLen <= 4);
  uint32_t totalSize = 26 + (strInline ? 0 : strLen);
  if (totalSize & 1) totalSize++;

  std::vector<uint8_t> exif(totalSize, 0);
  uint8_t* p = exif.data();

  p[0] = 'I'; p[1] = 'I';
  p[2] = 42;  p[3] = 0;
  p[4] = 8;   p[5] = 0; p[6] = 0; p[7] = 0;
  p[8] = 1; p[9] = 0;
  p[10] = 0x0E; p[11] = 0x01;
  p[12] = 2;    p[13] = 0;
  p[14] = (uint8_t)(strLen);
  p[15] = (uint8_t)(strLen >> 8);
  p[16] = (uint8_t)(strLen >> 16);
  p[17] = (uint8_t)(strLen >> 24);

  if (strInline) {
    memcpy(p + 18, desc.c_str(), desc.size());
  }
  else {
    uint32_t strOffset = 26;
    p[18] = (uint8_t)(strOffset);
    p[19] = (uint8_t)(strOffset >> 8);
    p[20] = (uint8_t)(strOffset >> 16);
    p[21] = (uint8_t)(strOffset >> 24);
    memcpy(p + 26, desc.c_str(), desc.size());
  }

  return exif;
}

// ---------------------------------------------------------------------------
// XMP builder
// ---------------------------------------------------------------------------

static std::string buildXmpWithDescription(const std::string& desc)
{
  std::string escaped;
  escaped.reserve(desc.size());
  for (char c : desc) {
    switch (c) {
      case '&':  escaped += "&amp;";  break;
      case '<':  escaped += "&lt;";   break;
      case '>':  escaped += "&gt;";   break;
      case '"':  escaped += "&quot;"; break;
      case '\'': escaped += "&apos;"; break;
      default:   escaped += c;        break;
    }
  }

  std::string xmp;
  xmp += "<?xpacket begin='\xEF\xBB\xBF' id='W5M0MpCehiHzreSzNTczkc9d'?>\n";
  xmp += "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n";
  xmp += "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n";
  xmp += " <rdf:Description rdf:about=''\n";
  xmp += "  xmlns:dc='http://purl.org/dc/elements/1.1/'>\n";
  xmp += "  <dc:description>\n";
  xmp += "   <rdf:Alt>\n";
  xmp += "    <rdf:li xml:lang='x-default'>";
  xmp += escaped;
  xmp += "</rdf:li>\n";
  xmp += "   </rdf:Alt>\n";
  xmp += "  </dc:description>\n";
  xmp += " </rdf:Description>\n";
  xmp += "</rdf:RDF>\n";
  xmp += "</x:xmpmeta>\n";
  xmp += "<?xpacket end='w'?>";

  return xmp;
}


// ---------------------------------------------------------------------------
// Advanced WebP encode helper — supports near-lossless
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
    config.quality = quality;  // 0-100 lossy quality
  }
  else {
    // Near-lossless: lossless encoder with preprocessing
    config.lossless = 1;
    config.near_lossless = (int)(quality + 0.5f);  // 0-100 preprocessing level
    config.quality = 75;  // lossless effort level (speed/size tradeoff)
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
// webpWriter
// ---------------------------------------------------------------------------

class webpWriter : public FileWriter
{
  float _quality;
  bool  _lossy;
  const char* _description;

public:
  webpWriter(Write* iop)
    : FileWriter(iop),
      _quality(60.0f),
      _lossy(false),
      _description("")
  {}

  ~webpWriter() override {}

  void execute() override;

  void knobs(Knob_Callback f) override
  {
    Float_knob(f, &_quality, "quality", "quality");
    Tooltip(f, "In default mode: near-lossless preprocessing level (0 = most preprocessing/smallest, "
               "100 = true lossless). In lossy mode: lossy quality (0 = smallest, 100 = best).");
    SetRange(f, 0, 100);

    Bool_knob(f, &_lossy, "lossy", "lossy");
    Tooltip(f, "Switch to lossy VP8 encoding. Default is near-lossless.");

    String_knob(f, &_description, "description", "description");
    Tooltip(f, "Text to embed as image description metadata. "
               "Written to both XMP dc:description and EXIF ImageDescription. "
               "Leave empty to skip metadata embedding.");
  }

  const char* help() override { return "WebP image format (near-lossless default)"; }

  static const Writer::Description d;
};

static Writer* build(Write* iop) { return new webpWriter(iop); }
const Writer::Description webpWriter::d("webp\0", "WebP image", build);


void webpWriter::execute()
{
  if (!open())
    return;

  const int wdt = width();
  const int hgt = height();

  int depth = iop->depth();
  if (depth > 3) depth = 3;

  Channel ch[3];
  for (int i = 0; i < 3; i++)
    ch[i] = iop->channel_written_to(i);
  ChannelSet channels = channel_mask(depth);

  input0().request(0, 0, wdt, hgt, channels, 1);

  const int stride = wdt * 3;
  unsigned char* rgb = new unsigned char[stride * hgt];
  if (!rgb) {
    iop->critical("webpWriter: failed to allocate image buffer");
    close();
    return;
  }

  Row row(0, wdt);
  for (int y = 0; y < hgt; y++) {
    iop->status(double(y) / hgt);
    get(hgt - y - 1, 0, wdt, channels, row);
    if (aborted()) break;

    unsigned char* dst = rgb + y * stride;
    if (depth >= 1) to_byte(0, dst + 0, row[ch[0]], nullptr, wdt, 3);
    if (depth >= 2) to_byte(1, dst + 1, row[ch[1]], nullptr, wdt, 3);
    if (depth >= 3) to_byte(2, dst + 2, row[ch[2]], nullptr, wdt, 3);

    if (depth < 3) {
      for (int x = 0; x < wdt; x++) {
        if (depth < 2) dst[x * 3 + 1] = 0;
        if (depth < 3) dst[x * 3 + 2] = 0;
      }
    }
  }

  if (aborted()) {
    delete[] rgb;
    close();
    return;
  }

  // Encode
  float q = _quality;
  if (q < 0.0f) q = 0.0f;
  if (q > 100.0f) q = 100.0f;

  uint8_t* encoded = nullptr;
  size_t encodedSize = 0;

  bool ok = webpEncodeAdvanced(rgb, wdt, hgt, stride, _lossy, q,
                               &encoded, &encodedSize);
  delete[] rgb;

  if (!ok || encodedSize == 0 || !encoded) {
    iop->critical("webpWriter: WebP encode failed");
    if (encoded) WebPFree(encoded);
    close();
    return;
  }

  std::string desc(_description ? _description : "");

  if (!desc.empty()) {
    WebPData image;
    image.bytes = encoded;
    image.size = encodedSize;

    WebPMux* mux = WebPMuxCreate(&image, 1);
    WebPFree(encoded);
    encoded = nullptr;

    if (!mux) {
      iop->critical("webpWriter: failed to create WebP mux");
      close();
      return;
    }

    std::vector<uint8_t> exifBlob = buildExifWithDescription(desc);
    WebPData exifData;
    exifData.bytes = exifBlob.data();
    exifData.size = exifBlob.size();
    WebPMuxSetChunk(mux, "EXIF", &exifData, 1);

    std::string xmpStr = buildXmpWithDescription(desc);
    WebPData xmpData;
    xmpData.bytes = reinterpret_cast<const uint8_t*>(xmpStr.c_str());
    xmpData.size = xmpStr.size();
    WebPMuxSetChunk(mux, "XMP ", &xmpData, 1);

    WebPData output;
    WebPMuxError err = WebPMuxAssemble(mux, &output);
    WebPMuxDelete(mux);

    if (err != WEBP_MUX_OK || output.size == 0) {
      iop->critical("webpWriter: failed to assemble WebP file");
      close();
      return;
    }

    write(output.bytes, (FILE_OFFSET)output.size);
    WebPDataClear(&output);
  }
  else {
    write(encoded, (FILE_OFFSET)encodedSize);
    WebPFree(encoded);
  }

  close();
}
