#include "util/OutputStream.h"

#include <cassert>
#include <cerrno>
#include <cstring>      // for strlen
#include <string_view>  // for string_view
#include <utility>      // for move

#include "util/GzUtil.h"  // for GzUtil
#include "util/i18n.h"    // for FS, _F
#include "util/safe_casts.h"

OutputStream::OutputStream() = default;

OutputStream::~OutputStream() = default;

void OutputStream::write(const char* str) { write(str, std::strlen(str)); }

void OutputStream::write(const std::string_view sv) { write(sv.data(), sv.length()); }

void OutputStream::write(const std::u8string_view sv) { write(char_cast(sv.data()), sv.length()); }

////////////////////////////////////////////////////////
/// GzOutputStream /////////////////////////////////////
////////////////////////////////////////////////////////

#ifdef XOJ_HAVE_LIBDEFLATE

#include <libdeflate.h>

GzOutputStream::GzOutputStream(fs::path file): file(std::move(file)) {
#ifdef _WIN32
    this->out = _wfopen(this->file.c_str(), L"wb");
#else
    this->out = std::fopen(this->file.c_str(), "wb");
#endif
    if (this->out == nullptr) {
        this->error = FS(_F("Error opening file: \"{1}\"") % this->file.u8string());
        this->error = this->error + "\n" + std::strerror(errno);
    }
}

GzOutputStream::~GzOutputStream() {
    if (this->out) {
        close();
    }
}

auto GzOutputStream::getLastError() const -> const std::string& { return this->error; }

void GzOutputStream::write(const char* data, size_t len) {
    xoj_assert(this->out);
    this->buffer.append(data, len);
}

void GzOutputStream::close() {
    if (!this->out) {
        return;
    }
    // Level 6 like zlib's default, which it compresses about three times faster and a few percent smaller
    libdeflate_compressor* compressor = libdeflate_alloc_compressor(6);
    std::string compressed;
    size_t size = 0;
    if (compressor) {
        compressed.resize(libdeflate_gzip_compress_bound(compressor, this->buffer.size()));
        size = libdeflate_gzip_compress(compressor, this->buffer.data(), this->buffer.size(), compressed.data(),
                                        compressed.size());
        libdeflate_free_compressor(compressor);
    }
    std::string().swap(this->buffer);

    const bool written = size > 0 && std::fwrite(compressed.data(), 1, size, this->out) == size;
    const int writeErrno = errno;
    const bool closed = std::fclose(this->out) == 0;
    this->out = nullptr;

    if (!written || !closed) {
        this->error = FS(_F("Error writing data to file: \"{1}\"") % this->file.u8string());
        this->error += "\n";
        this->error += size == 0 ? "Compression failed" : std::strerror(written ? errno : writeErrno);
    }
}

#else

GzOutputStream::GzOutputStream(fs::path file): file(std::move(file)) {
    this->fp = GzUtil::openPath(this->file, "w");
    if (this->fp == nullptr) {
        this->error = FS(_F("Error opening file: \"{1}\"") % this->file.u8string());
        this->error = this->error + "\n" + std::strerror(errno);
    }
}

GzOutputStream::~GzOutputStream() {
    if (this->fp) {
        close();
    }
    this->fp = nullptr;
}

auto GzOutputStream::getLastError() const -> const std::string& { return this->error; }

void GzOutputStream::write(const char* data, size_t len) {
    xoj_assert(this->fp);
    if (len == 0) {  // e.g. an empty attribute value. gzwrite() returns 0 for it, so there is nothing to write
        return;
    }
    auto written = gzwrite(this->fp, data, strict_cast<unsigned int>(len));
    if (as_unsigned(written) != len) {
        int errnum = 0;
        const char* error = gzerror(this->fp, &errnum);
        if (errnum != Z_OK) {
            this->error = FS(_F("Error writing data to file: \"{1}\"") % this->file.u8string());
            this->error += "\n" + FS(_F("Error code {1}. Message:") % errnum) + "\n";
            if (errnum == Z_ERRNO) {
                // fs error. Fetch the precise message
                this->error += std::strerror(errno);
            } else {
                this->error += error;
            }
        }
    }
}

void GzOutputStream::close() {
    if (!this->fp) {
        return;
    }

    auto errnum = gzclose(this->fp);
    this->fp = nullptr;

    if (errnum != Z_OK) {
        this->error = FS(_F("Error occurred while closing file: \"{1}\"") % this->file.u8string());
        this->error += "\n" + FS(_F("Error code {1}") % errnum);
        if (errnum == Z_ERRNO) {
            // fs error. Fetch the precise message
            this->error = this->error + "\n" + std::strerror(errno);
        }
    }
}

#endif
