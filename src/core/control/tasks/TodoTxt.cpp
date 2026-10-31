#include "TodoTxt.h"

#include <algorithm>  // for find_if
#include <array>      // for array
#include <cctype>     // for isdigit
#include <charconv>   // for from_chars
#include <cstdio>     // for snprintf

#include <glib.h>  // for g_utf8_strdown

using namespace std::chrono;

namespace TodoTxt {

namespace {

auto isSpace(char c) -> bool { return c == ' ' || c == '\t'; }

/// Splits at spaces, keeping each word's position in text
auto words(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> result;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && isSpace(text[pos])) {
            pos++;
        }
        const size_t start = pos;
        while (pos < text.size() && !isSpace(text[pos])) {
            pos++;
        }
        if (pos > start) {
            result.push_back(text.substr(start, pos - start));
        }
    }
    return result;
}

/// A key:value tag (not a URL like https://...)
auto splitTag(std::string_view word) -> std::optional<std::pair<std::string_view, std::string_view>> {
    const size_t colon = word.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= word.size() || word[colon + 1] == '/') {
        return std::nullopt;
    }
    for (char c: word.substr(0, colon)) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            return std::nullopt;
        }
    }
    return std::pair{word.substr(0, colon), word.substr(colon + 1)};
}

auto toInt(std::string_view text) -> std::optional<int> {
    int value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

auto lower(std::string_view text) -> std::string {
    gchar* down = g_utf8_strdown(text.data(), static_cast<gssize>(text.size()));
    std::string result(down);
    g_free(down);
    return result;
}

auto plusDays(const Date& date, int days) -> Date { return Date(sys_days(date) + std::chrono::days(days)); }

}  // namespace

auto parseDate(std::string_view text) -> std::optional<Date> {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    auto y = toInt(text.substr(0, 4)), m = toInt(text.substr(5, 2)), d = toInt(text.substr(8, 2));
    if (!y || !m || !d) {
        return std::nullopt;
    }
    Date date{year(*y), month(static_cast<unsigned>(*m)), day(static_cast<unsigned>(*d))};
    return date.ok() ? std::optional(date) : std::nullopt;
}

auto formatDate(const Date& date) -> std::string {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    return buffer;
}

auto parse(std::string_view line) -> std::optional<Task> {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || isSpace(line.back()))) {
        line.remove_suffix(1);
    }
    while (!line.empty() && isSpace(line.front())) {
        line.remove_prefix(1);
    }
    if (line.empty()) {
        return std::nullopt;
    }
    Task task;
    auto takeWord = [&line](size_t length) {
        line.remove_prefix(std::min(length + 1, line.size()));  // the word and the following space
    };
    auto nextWord = [&line]() {
        const size_t end = line.find(' ');
        return line.substr(0, end);
    };
    if (line.size() >= 2 && line[0] == 'x' && line[1] == ' ') {
        task.done = true;
        takeWord(1);
        if (auto date = parseDate(nextWord())) {
            task.completed = date;
            takeWord(10);
        }
    } else if (line.size() >= 4 && line[0] == '(' && line[1] >= 'A' && line[1] <= 'Z' && line[2] == ')' &&
               line[3] == ' ') {
        task.priority = line[1];
        takeWord(3);
    }
    if (auto date = parseDate(nextWord())) {
        task.created = date;
        takeWord(10);
    }
    task.description = std::string(line);
    return task;
}

auto format(const Task& task) -> std::string {
    std::string line;
    if (task.done) {
        line += "x ";
        if (task.completed) {
            line += formatDate(*task.completed) + " ";
        }
    } else if (task.priority) {
        line += std::string("(") + task.priority + ") ";
    }
    if (task.created) {
        line += formatDate(*task.created) + " ";
    }
    return line + task.description;
}

auto Task::tag(std::string_view key) const -> std::optional<std::string> {
    for (auto word: words(description)) {
        if (auto tag = splitTag(word); tag && tag->first == key) {
            return std::string(tag->second);
        }
    }
    return std::nullopt;
}

auto Task::due() const -> std::optional<Date> {
    auto value = tag("due");
    return value ? parseDate(*value) : std::nullopt;
}

auto Task::threshold() const -> std::optional<Date> {
    auto value = tag("t");
    return value ? parseDate(*value) : std::nullopt;
}

auto Task::time() const -> std::optional<minutes> {
    auto value = tag("time");
    if (!value) {
        return std::nullopt;
    }
    const size_t colon = value->find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    auto h = toInt(std::string_view(*value).substr(0, colon));
    auto m = toInt(std::string_view(*value).substr(colon + 1));
    if (!h || !m || *h < 0 || *h > 23 || *m < 0 || *m > 59) {
        return std::nullopt;
    }
    return hours(*h) + minutes(*m);
}

auto Task::projects() const -> std::vector<std::string> {
    std::vector<std::string> result;
    for (auto word: words(description)) {
        if (word.size() > 1 && word[0] == '+') {
            result.emplace_back(word.substr(1));
        }
    }
    return result;
}

auto Task::contexts() const -> std::vector<std::string> {
    std::vector<std::string> result;
    for (auto word: words(description)) {
        if (word.size() > 1 && word[0] == '@') {
            result.emplace_back(word.substr(1));
        }
    }
    return result;
}

auto Task::title() const -> std::string {
    std::string result;
    for (auto word: words(description)) {
        if (splitTag(word)) {
            continue;
        }
        if (!result.empty()) {
            result += ' ';
        }
        result += word;
    }
    return result;
}

void setTag(Task& task, std::string_view key, const std::optional<std::string>& value) {
    auto& text = task.description;
    for (auto word: words(text)) {
        auto tag = splitTag(word);
        if (!tag || tag->first != key) {
            continue;
        }
        const size_t start = static_cast<size_t>(word.data() - text.data());
        if (value) {
            text.replace(start, word.size(), std::string(key) + ":" + *value);
        } else {
            // Remove the word with one space next to it
            size_t from = start;
            size_t to = start + word.size();
            if (to < text.size() && isSpace(text[to])) {
                to++;
            } else if (from > 0 && isSpace(text[from - 1])) {
                from--;
            }
            text.erase(from, to - from);
        }
        return;
    }
    if (value) {
        if (!text.empty() && !isSpace(text.back())) {
            text += ' ';
        }
        text += std::string(key) + ":" + *value;
    }
}

auto parseRecurrence(std::string_view text) -> std::optional<Recurrence> {
    Recurrence recurrence;
    if (!text.empty() && text.front() == '+') {
        recurrence.strict = true;
        text.remove_prefix(1);
    }
    if (text.size() < 2) {
        return std::nullopt;
    }
    recurrence.unit = text.back();
    if (recurrence.unit != 'd' && recurrence.unit != 'w' && recurrence.unit != 'm' && recurrence.unit != 'y') {
        return std::nullopt;
    }
    auto count = toInt(text.substr(0, text.size() - 1));
    if (!count || *count < 1 || *count > 1000) {
        return std::nullopt;
    }
    recurrence.count = *count;
    return recurrence;
}

auto addInterval(const Date& date, const Recurrence& interval) -> Date {
    switch (interval.unit) {
        case 'w':
            return plusDays(date, 7 * interval.count);
        case 'm':
        case 'y': {
            auto moved = interval.unit == 'm' ? date + months(interval.count) : date + years(interval.count);
            // 31 January + 1 month: the last day of February
            return moved.ok() ? moved : Date(moved.year() / moved.month() / last);
        }
        default:
            return plusDays(date, interval.count);
    }
}

auto complete(Task& task, const Date& today) -> std::optional<Task> {
    if (task.done) {
        return std::nullopt;
    }
    std::optional<Task> next;
    if (auto rec = task.tag("rec"); rec) {
        if (auto interval = parseRecurrence(*rec)) {
            next = task;
            const auto due = task.due();
            const Date base = interval->strict && due ? *due : today;
            const Date nextDue = addInterval(base, *interval);
            if (due || !task.threshold()) {
                setTag(*next, "due", formatDate(nextDue));
            }
            if (auto threshold = task.threshold()) {
                // Keep the distance between the start and the due date
                const auto offset = due ? sys_days(*due) - sys_days(*threshold) : std::chrono::days(0);
                const Date nextThreshold = due ? Date(sys_days(nextDue) - offset) : addInterval(base, *interval);
                setTag(*next, "t", formatDate(nextThreshold));
            }
            if (next->created) {
                next->created = today;
            }
        }
    }
    task.done = true;
    task.completed = today;
    if (task.priority) {
        setTag(task, "pri", std::string(1, task.priority));
        task.priority = 0;
    }
    return next;
}

void reopen(Task& task) {
    task.done = false;
    task.completed.reset();
    if (auto pri = task.tag("pri"); pri && pri->size() == 1 && (*pri)[0] >= 'A' && (*pri)[0] <= 'Z') {
        task.priority = (*pri)[0];
        setTag(task, "pri", std::nullopt);
    }
}

auto groupOf(const Task& task, const Date& today) -> Group {
    if (task.done) {
        return Group::Done;
    }
    if (auto threshold = task.threshold(); threshold && sys_days(*threshold) > sys_days(today)) {
        return Group::Later;
    }
    auto due = task.due();
    if (!due) {
        return Group::NoDate;
    }
    if (sys_days(*due) < sys_days(today)) {
        return Group::Overdue;
    }
    return sys_days(*due) == sys_days(today) ? Group::Today : Group::Upcoming;
}

auto quickAdd(std::string_view text, const Date& today) -> Task {
    Task task;
    task.created = today;
    std::optional<Date> due;
    std::optional<std::string> time;

    // Priority as in todo.txt: "(A) ..."
    if (text.size() >= 4 && text[0] == '(' && text[1] >= 'A' && text[1] <= 'Z' && text[2] == ')' && text[3] == ' ') {
        task.priority = text[1];
        text.remove_prefix(4);
    }

    static constexpr std::array<const char*, 7> germanDays = {"montag",  "dienstag", "mittwoch", "donnerstag",
                                                              "freitag", "samstag",  "sonntag"};
    static constexpr std::array<const char*, 7> englishDays = {"monday", "tuesday",  "wednesday", "thursday",
                                                               "friday", "saturday", "sunday"};
    const auto weekdayOf = [](const std::string& word) -> std::optional<unsigned> {
        for (unsigned i = 0; i < 7; i++) {
            if (word == germanDays[i] || word == englishDays[i]) {
                return i + 1;  // ISO: Monday = 1
            }
        }
        return std::nullopt;
    };
    const auto parseTime = [](const std::string& word) -> std::optional<std::string> {
        const size_t colon = word.find(':');
        std::optional<int> h, m;
        if (colon != std::string::npos) {
            h = toInt(std::string_view(word).substr(0, colon));
            m = toInt(std::string_view(word).substr(colon + 1));
        } else if (word.size() > 3 && word.compare(word.size() - 3, 3, "uhr") == 0) {
            h = toInt(std::string_view(word).substr(0, word.size() - 3));
            m = 0;
        }
        if (!h || !m || *h < 0 || *h > 23 || *m < 0 || *m > 59) {
            return std::nullopt;
        }
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", *h, *m);
        return std::string(buffer);
    };
    /// "5.11.", "05.11.2026", "5.11.26"; not "3.5", which is rather a number than a date
    const auto parseGermanDate = [&today](const std::string& word) -> std::optional<Date> {
        std::string_view rest(word);
        if (!rest.empty() && rest.back() == '.') {
            rest.remove_suffix(1);
        } else if (std::count(rest.begin(), rest.end(), '.') < 2) {
            return std::nullopt;
        }
        const size_t dot1 = rest.find('.');
        if (dot1 == std::string_view::npos) {
            return std::nullopt;
        }
        const size_t dot2 = rest.find('.', dot1 + 1);
        auto d = toInt(rest.substr(0, dot1));
        auto m =
                toInt(rest.substr(dot1 + 1, dot2 == std::string_view::npos ? std::string_view::npos : dot2 - dot1 - 1));
        std::optional<int> y;
        if (dot2 != std::string_view::npos) {
            y = toInt(rest.substr(dot2 + 1));
            if (!y) {
                return std::nullopt;
            }
            if (*y < 100) {
                *y += 2000;
            }
        }
        if (!d || !m) {
            return std::nullopt;
        }
        Date date{year(y.value_or(static_cast<int>(today.year()))), month(static_cast<unsigned>(*m)),
                  day(static_cast<unsigned>(*d))};
        if (!date.ok()) {
            return std::nullopt;
        }
        if (!y && sys_days(date) < sys_days(today)) {
            date = Date{date.year() + years(1), date.month(), date.day()};  // "5.1." in December: next year
        }
        return date;
    };

    auto list = words(text);
    std::vector<bool> used(list.size(), false);
    for (size_t i = 0; i < list.size(); i++) {
        const std::string word = lower(list[i]);
        const std::string next = i + 1 < list.size() ? lower(list[i + 1]) : std::string();
        auto use = [&](size_t count) {
            for (size_t k = i; k < i + count && k < list.size(); k++) {
                used[k] = true;
            }
            i += count - 1;
        };
        if (word == "heute" || word == "today") {
            due = today;
            use(1);
        } else if (word == "morgen" || word == "tomorrow") {
            due = plusDays(today, 1);
            use(1);
        } else if (word == "übermorgen") {
            due = plusDays(today, 2);
            use(1);
        } else if (auto weekday = weekdayOf(word)) {
            const unsigned current = year_month_weekday(sys_days(today)).weekday().iso_encoding();
            int delta = static_cast<int>(*weekday) - static_cast<int>(current);
            if (delta <= 0) {
                delta += 7;
            }
            due = plusDays(today, delta);
            use(1);
        } else if ((word == "nächste" || word == "nächsten" || word == "next") && (next == "woche" || next == "week")) {
            due = plusDays(today, 7);
            use(2);
        } else if (word == "in" && i + 2 < list.size() && toInt(next)) {
            const std::string unit = lower(list[i + 2]);
            const int count = *toInt(next);
            if (unit == "tag" || unit == "tagen" || unit == "day" || unit == "days") {
                due = plusDays(today, count);
                use(3);
            } else if (unit == "woche" || unit == "wochen" || unit == "week" || unit == "weeks") {
                due = plusDays(today, 7 * count);
                use(3);
            }
        } else if (auto iso = parseDate(word)) {
            due = iso;
            use(1);
        } else if (auto german = parseGermanDate(word)) {
            due = german;
            use(1);
        } else if (auto t = parseTime(word)) {
            time = t;
            use(1);
        } else if (toInt(word) && (next == "uhr")) {
            if (auto t2 = parseTime(word + "uhr")) {
                time = t2;
                use(2);
            }
        }
    }
    // Prepositions that only belonged to a date or time ("bis Freitag", "um 14 Uhr", "am 5.11.")
    for (size_t i = 0; i + 1 < list.size(); i++) {
        const std::string word = lower(list[i]);
        if (!used[i] && used[i + 1] &&
            (word == "bis" || word == "am" || word == "um" || word == "ab" || word == "by" || word == "on" ||
             word == "at" || word == "until")) {
            used[i] = true;
        }
    }
    for (size_t i = 0; i < list.size(); i++) {
        if (!used[i]) {
            if (!task.description.empty()) {
                task.description += ' ';
            }
            task.description += list[i];
        }
    }
    if (time && !due) {
        due = today;
    }
    if (due) {
        setTag(task, "due", formatDate(*due));
    }
    if (time) {
        setTag(task, "time", *time);
    }
    return task;
}

}  // namespace TodoTxt
