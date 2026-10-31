#include "TaskFiles.h"

#include <algorithm>  // for sort
#include <cctype>     // for isxdigit
#include <cstdio>     // for snprintf
#include <fstream>    // for ifstream, ofstream
#include <iterator>   // for istreambuf_iterator

#include "util/PathUtil.h"  // for toUtf8, isChildOrEquivalent

namespace {

constexpr int MAX_DEPTH = 8;

struct Content {
    std::vector<std::string> lines;
    std::string newline = "\n";
    bool finalNewline = true;
};

auto read(const fs::path& path) -> std::optional<Content> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Content content;
    if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        data.erase(0, 3);  // UTF-8 byte order mark, written by some Windows editors
    }
    if (data.find("\r\n") != std::string::npos) {
        content.newline = "\r\n";
    }
    content.finalNewline = data.empty() || data.back() == '\n';
    size_t start = 0;
    while (start < data.size()) {
        size_t end = data.find('\n', start);
        if (end == std::string::npos) {
            end = data.size();
        }
        std::string line = data.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        content.lines.push_back(std::move(line));
        start = end + 1;
    }
    return content;
}

auto write(const fs::path& path, const Content& content) -> bool {
    std::string data;
    for (size_t i = 0; i < content.lines.size(); i++) {
        data += content.lines[i];
        if (i + 1 < content.lines.size() || content.finalNewline) {
            data += content.newline;
        }
    }
    // Write next to it and replace it, so a crash never leaves half a file
    auto part = path;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (out.fail()) {
            std::error_code error;
            fs::remove(part, error);
            return false;
        }
    }
    std::error_code error;
    fs::rename(part, path, error);
    if (error) {
        fs::remove(part, error);
        return false;
    }
    return true;
}

}  // namespace

auto TaskFiles::isTaskFile(const fs::path& path) -> bool {
    const auto name = Util::toUtf8(path.filename());
    constexpr std::string_view suffix = ".todo.txt";
    return name == "todo.txt" ||
           (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
}

auto TaskFiles::find(const fs::path& root) -> std::vector<fs::path> {
    std::vector<fs::path> files;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
    for (; !error && it != end; it.increment(error)) {
        const auto name = Util::toUtf8(it->path().filename());
        std::error_code typeError;
        if (it->is_directory(typeError)) {
            if ((!name.empty() && name.front() == '.') || it.depth() >= MAX_DEPTH) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (isTaskFile(it->path()) && it->is_regular_file(typeError)) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

auto TaskFiles::readLines(const fs::path& path) -> std::optional<std::vector<std::string>> {
    auto content = read(path);
    if (!content) {
        return std::nullopt;
    }
    return std::move(content->lines);
}

auto TaskFiles::replaceLine(const fs::path& path, const std::string& oldLine, const std::vector<std::string>& newLines)
        -> bool {
    auto content = read(path);
    if (!content) {
        return false;
    }
    auto& lines = content->lines;
    auto it = std::find(lines.begin(), lines.end(), oldLine);
    if (it == lines.end()) {
        return false;
    }
    it = lines.erase(it);
    lines.insert(it, newLines.begin(), newLines.end());
    return write(path, *content);
}

auto TaskFiles::appendLine(const fs::path& path, const std::string& line) -> bool {
    std::error_code error;
    Content content;
    if (fs::exists(path, error)) {
        auto existing = read(path);
        if (!existing) {
            return false;
        }
        content = std::move(*existing);
        // A last empty line (from a final line break) stays the end of the file
        while (!content.lines.empty() && content.lines.back().empty()) {
            content.lines.pop_back();
        }
        content.finalNewline = true;
    }
    content.lines.push_back(line);
    return write(path, content);
}

auto TaskFiles::fileFor(const fs::path& document, const fs::path& root) -> fs::path {
    std::error_code error;
    if (!document.empty() && Util::isChildOrEquivalent(document, root)) {
        for (auto dir = document.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            if (fs::exists(dir / "todo.txt", error)) {
                return dir / "todo.txt";
            }
            if (fs::equivalent(dir, root, error) || dir == dir.parent_path()) {
                break;
            }
        }
    }
    return root / "todo.txt";
}

auto TaskFiles::documentLink(const fs::path& taskFile, const fs::path& document, int page) -> std::string {
    auto relative = document.lexically_relative(taskFile.parent_path());
    if (relative.empty()) {
        relative = document;
    }
    const auto generic = relative.generic_u8string();  // forward slashes, as in other apps
    std::string link;
    for (char8_t c: generic) {
        // Spaces would end the tag; % and # have a meaning in the link
        if (c == ' ' || c == '%' || c == '#') {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", static_cast<unsigned char>(c));
            link += buffer;
        } else {
            link += static_cast<char>(c);
        }
    }
    if (page >= 0) {
        link += "#p" + std::to_string(page + 1);
    }
    return link;
}

auto TaskFiles::resolveDocumentLink(const fs::path& taskFile, const std::string& value) -> std::optional<DocumentLink> {
    DocumentLink link;
    std::string path = value;
    if (const size_t hash = path.rfind("#p"); hash != std::string::npos) {
        try {
            const int page = std::stoi(path.substr(hash + 2));
            if (page > 0) {
                link.page = page - 1;
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        path.erase(hash);
    }
    std::string decoded;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
            decoded += static_cast<char>(std::stoi(path.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += path[i];
        }
    }
    if (decoded.empty()) {
        return std::nullopt;
    }
    auto document = Util::fromUtf8(decoded);
    link.document = document.is_absolute() ? document : (taskFile.parent_path() / document).lexically_normal();
    return link;
}
