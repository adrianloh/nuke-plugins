// webpReader.cpp
// WebP image reader plugin for Nuke 15.2
// Reads lossy/lossless WebP files as RGB 8-bit.
// Extracts metadata:
//   - XMP dc:description      -> "webp/description"
//   - EXIF UserComment        -> "webp/usercomment"
//   - EXIF ImageDescription   -> "exif/ImageDescription"

#include "DDImage/DDWindows.h"
#include "DDImage/Reader.h"
#include "DDImage/Row.h"
#include "DDImage/Thread.h"
#include "DDImage/MetaData.h"

#include "webp/decode.h"
#include "webp/demux.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace DD::Image;

// ---------------------------------------------------------------------------
// XMP parser — extract dc:description from the rdf:li x-default element
// ---------------------------------------------------------------------------

static std::string parseXmpDescription(const uint8_t* data, size_t size)
{
  std::string xmp(reinterpret_cast<const char*>(data), size);

  // Look for <rdf:li xml:lang='x-default'> or xml:lang="x-default"
  // This handles both single and double quote variants.
  std::string desc;

  // Find dc:description block first to avoid matching other rdf:li elements
  size_t dcPos = xmp.find("dc:description");
  if (dcPos == std::string::npos)
    return {};

  // Now find the rdf:li with x-default within that block
  size_t liPos = xmp.find("x-default", dcPos);
  if (liPos == std::string::npos)
    return {};

  // Find the '>' that closes the <rdf:li ...> tag
  size_t start = xmp.find('>', liPos);
  if (start == std::string::npos)
    return {};
  start++; // skip the '>'

  // Find the closing </rdf:li>
  size_t end = xmp.find("</rdf:li>", start);
  if (end == std::string::npos)
    return {};

  return xmp.substr(start, end - start);
}


// ---------------------------------------------------------------------------
// EXIF parser — extracts fields from TIFF IFD structures
// Handles the Exif\0\0 prefix, follows SubIFD pointers.
// ---------------------------------------------------------------------------

struct ExifFields {
  std::string imageDescription;  // tag 0x010E in IFD0
  std::string userComment;       // tag 0x9286 in Exif SubIFD
};

static ExifFields parseExifFields(const uint8_t* data, size_t size)
{
  ExifFields result;

  if (size < 8)
    return result;

  // Skip optional "Exif\0\0" prefix
  size_t offset = 0;
  if (size >= 6 && data[0] == 'E' && data[1] == 'x' && data[2] == 'i' &&
      data[3] == 'f' && data[4] == 0 && data[5] == 0) {
    offset = 6;
  }

  if (size - offset < 8)
    return result;

  const uint8_t* tiff = data + offset;
  size_t tiffSize = size - offset;

  bool le = (tiff[0] == 'I' && tiff[1] == 'I');
  bool be = (tiff[0] == 'M' && tiff[1] == 'M');
  if (!le && !be)
    return result;

  auto read16 = [le](const uint8_t* p) -> uint16_t {
    if (le) return p[0] | (p[1] << 8);
    else    return (p[0] << 8) | p[1];
  };
  auto read32 = [le](const uint8_t* p) -> uint32_t {
    if (le) return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    else    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  };

  if (read16(tiff + 2) != 42)
    return result;

  // Helper: scan an IFD for specific tags
  auto scanIFD = [&](uint32_t ifdOffset, uint16_t targetTag) -> std::string {
    if (ifdOffset + 2 > tiffSize)
      return {};
    const uint8_t* ifd = tiff + ifdOffset;
    uint16_t numEntries = read16(ifd);
    ifd += 2;

    for (uint16_t i = 0; i < numEntries; i++) {
      if ((size_t)(ifd - tiff) + 12 > tiffSize)
        break;

      uint16_t tag   = read16(ifd);
      uint16_t type  = read16(ifd + 2);
      uint32_t count = read32(ifd + 4);

      if (tag == targetTag && count > 0) {
        // For ASCII (type 2): straightforward string
        if (type == 2) {
          const uint8_t* strData;
          if (count <= 4) {
            strData = ifd + 8;
          }
          else {
            uint32_t valOffset = read32(ifd + 8);
            if (valOffset + count > tiffSize) return {};
            strData = tiff + valOffset;
          }
          size_t len = count;
          while (len > 0 && strData[len - 1] == 0) len--;
          return std::string(reinterpret_cast<const char*>(strData), len);
        }

        // For UNDEFINED (type 7): used by UserComment (0x9286)
        // Format: 8 bytes charset ID + rest is the string
        if (type == 7 && count > 8) {
          uint32_t valOffset = read32(ifd + 8);
          if (valOffset + count > tiffSize) return {};
          const uint8_t* raw = tiff + valOffset;
          // Skip 8-byte charset header ("ASCII\0\0\0" or "UNICODE\0" etc)
          const uint8_t* strStart = raw + 8;
          size_t strLen = count - 8;
          // Trim trailing nulls and spaces
          while (strLen > 0 && (strStart[strLen - 1] == 0 || strStart[strLen - 1] == ' '))
            strLen--;
          if (strLen > 0)
            return std::string(reinterpret_cast<const char*>(strStart), strLen);
        }
      }
      ifd += 12;
    }
    return {};
  };

  // Helper: find a LONG (type 4) tag value in an IFD — used for SubIFD pointers
  auto findLongTag = [&](uint32_t ifdOffset, uint16_t targetTag) -> uint32_t {
    if (ifdOffset + 2 > tiffSize)
      return 0;
    const uint8_t* ifd = tiff + ifdOffset;
    uint16_t numEntries = read16(ifd);
    ifd += 2;

    for (uint16_t i = 0; i < numEntries; i++) {
      if ((size_t)(ifd - tiff) + 12 > tiffSize)
        break;
      uint16_t tag  = read16(ifd);
      uint16_t type = read16(ifd + 2);
      if (tag == targetTag && (type == 3 || type == 4)) {
        return read32(ifd + 8);
      }
      ifd += 12;
    }
    return 0;
  };

  uint32_t ifd0Offset = read32(tiff + 4);

  // Look for ImageDescription (0x010E) in IFD0
  result.imageDescription = scanIFD(ifd0Offset, 0x010E);

  // Follow Exif SubIFD pointer (tag 0x8769) to find UserComment (0x9286)
  uint32_t exifSubIFDOffset = findLongTag(ifd0Offset, 0x8769);
  if (exifSubIFDOffset > 0) {
    result.userComment = scanIFD(exifSubIFDOffset, 0x9286);
  }

  return result;
}


// ---------------------------------------------------------------------------
// webpReader
// ---------------------------------------------------------------------------

class webpReader : public Reader
{
  unsigned char* _pixels;
  int _width;
  int _height;
  unsigned char* _fileData;
  size_t _fileSize;
  int _fd;

  MetaData::Bundle _meta;

  bool readFileData();

public:
  const MetaData::Bundle& fetchMetaData(const char* key) override
  {
    return _meta;
  }

  webpReader(Read* r, int fd, const unsigned char* buf, int bufsize);
  ~webpReader() override;

  void open() override;
  void engine(int y, int x, int r, ChannelMask channels, Row& row) override;

  static const Description d;
};


static bool test(int fd, const unsigned char* block, int length)
{
  if (length < 12)
    return false;
  return (block[0] == 'R' && block[1] == 'I' && block[2] == 'F' && block[3] == 'F' &&
          block[8] == 'W' && block[9] == 'E' && block[10] == 'B' && block[11] == 'P');
}

static Reader* build(Read* iop, int fd, const unsigned char* b, int n)
{
  return new webpReader(iop, fd, b, n);
}

const Reader::Description webpReader::d("webp\0", "WebP image", build, test);


bool webpReader::readFileData()
{
  if (_fileData)
    return true;

#ifdef _WIN32
  _lseek(_fd, 0, SEEK_END);
  __int64 fsize = _telli64(_fd);
  _lseek(_fd, 0, SEEK_SET);

  if (fsize <= 0 || fsize > ((__int64)512 << 20)) {
    iop->error("webpReader: file too large or unreadable");
    return false;
  }

  _fileSize = (size_t)fsize;
  _fileData = new unsigned char[_fileSize];

  size_t totalRead = 0;
  while (totalRead < _fileSize) {
    int chunk = (int)((_fileSize - totalRead) > 0x7FFFFFFF ? 0x7FFFFFFF : (_fileSize - totalRead));
    int got = _read(_fd, _fileData + totalRead, chunk);
    if (got <= 0) break;
    totalRead += got;
  }
  _close(_fd);
  _fd = -1;
#else
  struct stat st;
  if (fstat(_fd, &st) != 0 || st.st_size <= 0) {
    iop->error("webpReader: cannot stat file");
    return false;
  }
  _fileSize = st.st_size;
  _fileData = new unsigned char[_fileSize];

  lseek(_fd, 0, SEEK_SET);
  size_t totalRead = 0;
  while (totalRead < _fileSize) {
    ssize_t got = read(_fd, _fileData + totalRead, _fileSize - totalRead);
    if (got <= 0) break;
    totalRead += got;
  }
  close(_fd);
  _fd = -1;
#endif

  if (totalRead < _fileSize) {
    iop->error("webpReader: short read on file");
    delete[] _fileData;
    _fileData = nullptr;
    _fileSize = 0;
    return false;
  }
  return true;
}


webpReader::webpReader(Read* r, int fd, const unsigned char* buf, int bufsize)
  : Reader(r),
    _pixels(nullptr),
    _fileData(nullptr),
    _fileSize(0),
    _fd(fd),
    _width(0),
    _height(0)
{
  int w = 0, h = 0;
  if (!WebPGetInfo(buf, bufsize, &w, &h)) {
    iop->error("webpReader: not a valid WebP file");
    return;
  }

  _width = w;
  _height = h;
  set_info(_width, _height, 3);
  _meta.setData(MetaData::DEPTH, MetaData::DEPTH_FIXED(8));

  if (!readFileData())
    return;

  // --- Extract metadata via WebP Demux ---
  WebPData webpData;
  webpData.bytes = _fileData;
  webpData.size = _fileSize;

  WebPDemuxer* demux = WebPDemux(&webpData);
  if (demux) {
    uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);

    // Extract XMP dc:description
    if (flags & XMP_FLAG) {
      WebPChunkIterator chunkIter;
      if (WebPDemuxGetChunk(demux, "XMP ", 1, &chunkIter)) {
        std::string desc = parseXmpDescription(chunkIter.chunk.bytes,
                                               chunkIter.chunk.size);
        if (!desc.empty()) {
          _meta.setData("webp/description", desc);
        }
        WebPDemuxReleaseChunkIterator(&chunkIter);
      }
    }

    // Extract EXIF fields
    if (flags & EXIF_FLAG) {
      WebPChunkIterator chunkIter;
      if (WebPDemuxGetChunk(demux, "EXIF", 1, &chunkIter)) {
        ExifFields fields = parseExifFields(chunkIter.chunk.bytes,
                                            chunkIter.chunk.size);
        if (!fields.imageDescription.empty()) {
          _meta.setData("exif/ImageDescription", fields.imageDescription);
        }
        if (!fields.userComment.empty()) {
          _meta.setData("webp/usercomment", fields.userComment);
        }
        WebPDemuxReleaseChunkIterator(&chunkIter);
      }
    }

    WebPDemuxDelete(demux);
  }
}


void webpReader::open()
{
  if (_pixels)
    return;

  if (_width == 0 || _height == 0)
    return;

  if (!readFileData())
    return;

  _pixels = WebPDecodeRGB(_fileData, _fileSize, &_width, &_height);

  delete[] _fileData;
  _fileData = nullptr;
  _fileSize = 0;

  if (!_pixels)
    iop->error("webpReader: WebP decode failed");
}


webpReader::~webpReader()
{
  if (_pixels)
    WebPFree(_pixels);

  delete[] _fileData;

#ifdef _WIN32
  if (_fd >= 0) _close(_fd);
#else
  if (_fd >= 0) close(_fd);
#endif
}


void webpReader::engine(int y, int x, int xr, ChannelMask channels, Row& row)
{
  if (!_pixels)
    return;

  int picy = _height - y - 1;
  if (picy < 0 || picy >= _height)
    return;

  const int stride = _width * 3;
  const unsigned char* src = _pixels + picy * stride;
  const int n = xr - x;

  if (channels & Mask_Red) {
    float* dst = row.writable(Chan_Red);
    if (dst)
      from_byte(Chan_Red, dst + x, src + x * 3, nullptr, n, 3);
  }
  if (channels & Mask_Green) {
    float* dst = row.writable(Chan_Green);
    if (dst)
      from_byte(Chan_Green, dst + x, src + x * 3 + 1, nullptr, n, 3);
  }
  if (channels & Mask_Blue) {
    float* dst = row.writable(Chan_Blue);
    if (dst)
      from_byte(Chan_Blue, dst + x, src + x * 3 + 2, nullptr, n, 3);
  }
}
