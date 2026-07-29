#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kApiUrl =
    "https://www.amiiboapi.org/api/amiibo/";

struct Record {
    std::string head;
    std::string tail;
    std::string name;
    std::string character;
    std::string gameSeries;
    std::string amiiboSeries;
    std::string type;
};

void appendUtf8(std::string& output, uint32_t value) {
    if (value <= 0x7f) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        throw std::runtime_error("invalid Unicode code point");
    }
}

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    std::vector<Record> readCatalogue() {
        skipSpace();
        expect('{');
        std::vector<Record> records;
        bool found = false;
        skipSpace();
        if (consume('}')) return records;
        while (true) {
            const std::string key = readString();
            skipSpace();
            expect(':');
            skipSpace();
            if ((key == "amiibo" || key == "amiibos") && peek() == '[') {
                auto candidate = readRecords();
                if (!candidate.empty() || !found) {
                    records = std::move(candidate);
                    found = true;
                }
            } else {
                skipValue();
            }
            skipSpace();
            if (consume('}')) break;
            expect(',');
            skipSpace();
        }
        skipSpace();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected data after JSON document");
        return records;
    }

private:
    char peek() const {
        if (position_ >= input_.size())
            throw std::runtime_error("unexpected end of JSON");
        return input_[position_];
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            throw std::runtime_error(
                std::string("expected '") + expected + "' at byte "
                + std::to_string(position_));
        }
    }

    void skipSpace() {
        while (position_ < input_.size()
               && std::isspace(static_cast<unsigned char>(input_[position_])))
            ++position_;
    }

    uint32_t readHex4() {
        if (position_ + 4 > input_.size())
            throw std::runtime_error("truncated Unicode escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned char c =
                static_cast<unsigned char>(input_[position_++]);
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else throw std::runtime_error("invalid Unicode escape");
        }
        return value;
    }

    std::string readString() {
        expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char c =
                static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return output;
            if (c < 0x20)
                throw std::runtime_error("unescaped control character");
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size())
                throw std::runtime_error("truncated string escape");
            switch (input_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                uint32_t value = readHex4();
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (position_ + 2 > input_.size()
                            || input_[position_] != '\\'
                            || input_[position_ + 1] != 'u')
                        throw std::runtime_error(
                            "Unicode high surrogate without low surrogate");
                    position_ += 2;
                    const uint32_t low = readHex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        throw std::runtime_error("invalid Unicode surrogate");
                    value = 0x10000 + ((value - 0xd800) << 10)
                        + (low - 0xdc00);
                } else if (value >= 0xdc00 && value <= 0xdfff) {
                    throw std::runtime_error("unexpected Unicode low surrogate");
                }
                appendUtf8(output, value);
                break;
            }
            default:
                throw std::runtime_error("invalid string escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    void skipLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal)
            throw std::runtime_error("invalid JSON literal");
        position_ += literal.size();
    }

    void skipNumber() {
        const size_t start = position_;
        consume('-');
        if (position_ < input_.size() && input_[position_] == '0') {
            ++position_;
        } else {
            if (position_ >= input_.size()
                    || !std::isdigit(
                        static_cast<unsigned char>(input_[position_])))
                throw std::runtime_error("invalid JSON number");
            while (position_ < input_.size()
                   && std::isdigit(
                       static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (consume('.')) {
            if (position_ >= input_.size()
                    || !std::isdigit(
                        static_cast<unsigned char>(input_[position_])))
                throw std::runtime_error("invalid JSON number fraction");
            while (position_ < input_.size()
                   && std::isdigit(
                       static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (position_ < input_.size()
                && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size()
                    && (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            if (position_ >= input_.size()
                    || !std::isdigit(
                        static_cast<unsigned char>(input_[position_])))
                throw std::runtime_error("invalid JSON number exponent");
            while (position_ < input_.size()
                   && std::isdigit(
                       static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (position_ == start)
            throw std::runtime_error("invalid JSON number");
    }

    void skipObject() {
        expect('{');
        skipSpace();
        if (consume('}')) return;
        while (true) {
            readString();
            skipSpace();
            expect(':');
            skipSpace();
            skipValue();
            skipSpace();
            if (consume('}')) return;
            expect(',');
            skipSpace();
        }
    }

    void skipArray() {
        expect('[');
        skipSpace();
        if (consume(']')) return;
        while (true) {
            skipValue();
            skipSpace();
            if (consume(']')) return;
            expect(',');
            skipSpace();
        }
    }

    void skipValue() {
        skipSpace();
        switch (peek()) {
        case '"': readString(); break;
        case '{': skipObject(); break;
        case '[': skipArray(); break;
        case 't': skipLiteral("true"); break;
        case 'f': skipLiteral("false"); break;
        case 'n': skipLiteral("null"); break;
        default: skipNumber(); break;
        }
    }

    static void assign(Record& record, std::string_view key,
                       std::string value) {
        if (key == "head") record.head = std::move(value);
        else if (key == "tail") record.tail = std::move(value);
        else if (key == "name") record.name = std::move(value);
        else if (key == "character") record.character = std::move(value);
        else if (key == "gameSeries") record.gameSeries = std::move(value);
        else if (key == "amiiboSeries") record.amiiboSeries = std::move(value);
        else if (key == "type") record.type = std::move(value);
    }

    Record readRecord() {
        Record record;
        expect('{');
        skipSpace();
        if (consume('}')) return record;
        while (true) {
            const std::string key = readString();
            skipSpace();
            expect(':');
            skipSpace();
            if (peek() == '"') assign(record, key, readString());
            else skipValue();
            skipSpace();
            if (consume('}')) break;
            expect(',');
            skipSpace();
        }
        return record;
    }

    std::vector<Record> readRecords() {
        std::vector<Record> records;
        expect('[');
        skipSpace();
        if (consume(']')) return records;
        while (true) {
            if (peek() == '{') records.push_back(readRecord());
            else skipValue();
            skipSpace();
            if (consume(']')) break;
            expect(',');
            skipSpace();
        }
        return records;
    }

    std::string_view input_;
    size_t position_ = 0;
};

std::string readFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("could not open " + path.string());
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

void appendJsonString(std::string& output, std::string_view value) {
    output.push_back('"');
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (const unsigned char c : value) {
        switch (c) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 0x20) {
                output += "\\u00";
                output.push_back(hex[c >> 4]);
                output.push_back(hex[c & 0x0f]);
            } else {
                output.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    output.push_back('"');
}

std::string asciiFold(std::string_view value) {
    std::string folded(value);
    for (char& c : folded) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z')
            c = static_cast<char>(byte - 'A' + 'a');
    }
    return folded;
}

bool validHexId(std::string_view value) {
    return value.size() == 8
        && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
}

void lowercaseHex(std::string& value) {
    for (char& c : value)
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
}

std::string makeCatalogue(std::vector<Record> records) {
    std::map<std::string, Record> unique;
    for (Record& record : records) {
        lowercaseHex(record.head);
        lowercaseHex(record.tail);
        if (!validHexId(record.head) || !validHexId(record.tail)
                || record.name.empty())
            continue;
        unique[record.head + record.tail] = std::move(record);
    }

    records.clear();
    records.reserve(unique.size());
    for (auto& [id, record] : unique)
        records.push_back(std::move(record));
    std::sort(records.begin(), records.end(), [](const Record& a,
                                                  const Record& b) {
        const auto aSeries = asciiFold(a.gameSeries);
        const auto bSeries = asciiFold(b.gameSeries);
        if (aSeries != bSeries) return aSeries < bSeries;
        const auto aName = asciiFold(a.name);
        const auto bName = asciiFold(b.name);
        if (aName != bName) return aName < bName;
        if (a.head != b.head) return a.head < b.head;
        return a.tail < b.tail;
    });

    if (records.empty())
        throw std::runtime_error(
            "AmiiboAPI response contained no valid catalogue entries");

    std::string output;
    output.reserve(records.size() * 220);
    output += "{\"source\":\"AmiiboAPI\",\"sourceUrl\":";
    appendJsonString(output, kApiUrl);
    output += ",\"amiibo\":[";
    bool first = true;
    for (const Record& record : records) {
        if (!first) output.push_back(',');
        first = false;
        output += "{\"head\":";
        appendJsonString(output, record.head);
        output += ",\"tail\":";
        appendJsonString(output, record.tail);
        output += ",\"name\":";
        appendJsonString(output, record.name);
        output += ",\"character\":";
        appendJsonString(output, record.character);
        output += ",\"gameSeries\":";
        appendJsonString(output, record.gameSeries);
        output += ",\"amiiboSeries\":";
        appendJsonString(output, record.amiiboSeries);
        output += ",\"type\":";
        appendJsonString(output, record.type);
        output.push_back('}');
    }
    output += "]}\n";
    return output;
}

void writeFile(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("could not write " + path.string());
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream)
        throw std::runtime_error("failed while writing " + path.string());
}

struct Options {
    fs::path source;
    fs::path root = fs::current_path();
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--source" && i + 1 < argc) {
            options.source = fs::path(argv[++i]);
        } else if (arg == "--root" && i + 1 < argc) {
            options.root = fs::path(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: update-amiibo-catalog --source <amiiboapi.json>"
                   " [--root <repository>]\n"
                << "The workflow downloads AmiiboAPI JSON; this tool validates"
                   " and embeds it without making runtime network requests.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument: "
                                     + std::string(arg));
        }
    }
    if (options.source.empty())
        throw std::runtime_error("--source is required");
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const std::string raw = readFile(options.source);
        const std::string catalogue =
            makeCatalogue(JsonReader(raw).readCatalogue());

        const fs::path shared =
            options.root / "shared/data/amiibo_catalog.json";
        const fs::path web =
            options.root / "webapp/data/amiibo_catalog.json";
        const fs::path browser =
            options.root / "webapp/data/amiibo_catalog.js";
        writeFile(shared, catalogue);
        writeFile(web, catalogue);
        writeFile(
            browser,
            "window.NS_AMIIBO_CATALOG="
                + catalogue.substr(0, catalogue.size() - 1) + ";\n");

        const auto marker = std::string_view{"\"head\":"};
        size_t count = 0;
        for (size_t position = 0;
             (position = catalogue.find(marker, position))
                 != std::string::npos;
             position += marker.size())
            ++count;
        std::cout << "Wrote " << count << " Amiibo to " << shared << '\n'
                  << "Wrote " << count << " Amiibo to " << web << '\n'
                  << "Wrote browser catalogue to " << browser << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "update-amiibo-catalog: " << error.what() << '\n';
        return 1;
    }
}
