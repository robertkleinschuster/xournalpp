/*
 * Xournal++
 *
 * Output streams for writing
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstdio>       // for FILE
#include <string>       // for string
#include <string_view>  // for string_view

#include <zlib.h>  // for gzFile

#include "filesystem.h"  // for path

class OutputStream {
public:
    OutputStream();
    virtual ~OutputStream();

public:
    void write(const char* str);
    void write(const std::string_view sv);
    void write(const std::u8string_view sv);

    virtual void write(const char* data, size_t len) = 0;
    virtual void close() = 0;
};

class GzOutputStream: public OutputStream {
public:
    GzOutputStream(fs::path file);
    ~GzOutputStream() override;

public:
    void write(const char* data, size_t len) override;

    void close() override;

    const std::string& getLastError() const;

private:
#ifdef XOJ_HAVE_LIBDEFLATE
    // Fork: the document is collected and compressed with libdeflate at once when closing
    std::FILE* out = nullptr;
    std::string buffer;
#else
    gzFile fp = nullptr;
#endif

    std::string error;
    fs::path file;
};
