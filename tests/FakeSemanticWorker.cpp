#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<unsigned char, 4> kMagic{{'V', 'S', 'W', '1'}};

bool readExact(char* data, size_t size) {
    std::cin.read(data, static_cast<std::streamsize>(size));
    return static_cast<size_t>(std::cin.gcount()) == size;
}

std::uint32_t readUint32(const std::vector<unsigned char>& data, size_t& offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("truncated integer");
    std::uint32_t value = data[offset] |
                          (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
                          (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
                          (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return value;
}

void appendUint32(std::vector<unsigned char>& data, std::uint32_t value) {
    data.push_back(static_cast<unsigned char>(value));
    data.push_back(static_cast<unsigned char>(value >> 8));
    data.push_back(static_cast<unsigned char>(value >> 16));
    data.push_back(static_cast<unsigned char>(value >> 24));
}

void appendString(std::vector<unsigned char>& data, const std::string& value) {
    appendUint32(data, static_cast<std::uint32_t>(value.size()));
    data.insert(data.end(), value.begin(), value.end());
}

std::vector<std::int8_t> encode(const std::string& text, int dimensions) {
    std::vector<double> values(static_cast<size_t>(dimensions), 0.0);
    std::istringstream words(text);
    std::string word;
    while (words >> word) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : word) {
            hash ^= static_cast<unsigned char>(std::tolower(c));
            hash *= 1099511628211ULL;
        }
        values[hash % values.size()] += (hash & 1U) ? 1.0 : -1.0;
    }
    double norm = 0.0;
    for (double value : values) norm += value * value;
    norm = std::sqrt(norm);
    if (norm == 0.0) norm = 1.0;
    std::vector<std::int8_t> result(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        result[i] = static_cast<std::int8_t>(
            std::clamp(std::lround(values[i] / norm * 127.0), -127L, 127L));
    }
    return result;
}

std::string argument(int argc, char** argv, const std::string& name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return "";
}

} // namespace

int main(int argc, char** argv) {
    const std::string modelId = argument(argc, argv, "--model-id");
    const std::string revision = argument(argc, argv, "--revision");
    const int dimensions = std::stoi(argument(argc, argv, "--dimensions"));
    while (true) {
        std::array<unsigned char, 4> header{};
        if (!readExact(reinterpret_cast<char*>(header.data()), header.size())) return 0;
        size_t headerOffset = 0;
        std::vector<unsigned char> headerVector(header.begin(), header.end());
        const std::uint32_t size = readUint32(headerVector, headerOffset);
        if (size == 0 || size > 64U * 1024U * 1024U) return 2;
        std::vector<unsigned char> request(size);
        if (!readExact(reinterpret_cast<char*>(request.data()), request.size())) return 2;
        if (request.size() < 9 ||
            !std::equal(kMagic.begin(), kMagic.end(), request.begin())) return 2;
        const unsigned char operation = request[4];
        size_t offset = 5;
        const std::uint32_t count = readUint32(request, offset);
        std::vector<std::string> texts;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t textSize = readUint32(request, offset);
            if (offset + textSize > request.size()) return 2;
            texts.emplace_back(reinterpret_cast<const char*>(request.data() + offset),
                               textSize);
            offset += textSize;
        }
        if (operation == 4) return 0;

        std::vector<unsigned char> response(kMagic.begin(), kMagic.end());
        response.push_back(0);
        appendUint32(response, static_cast<std::uint32_t>(dimensions));
        if (operation == 1) {
            appendString(response, modelId);
            appendString(response, revision);
        } else {
            appendUint32(response, count);
            for (const auto& text : texts) {
                const auto vector = encode(text, dimensions);
                response.insert(response.end(),
                                reinterpret_cast<const unsigned char*>(vector.data()),
                                reinterpret_cast<const unsigned char*>(vector.data() + vector.size()));
            }
        }
        std::vector<unsigned char> responseHeader;
        appendUint32(responseHeader, static_cast<std::uint32_t>(response.size()));
        std::cout.write(reinterpret_cast<const char*>(responseHeader.data()),
                        static_cast<std::streamsize>(responseHeader.size()));
        std::cout.write(reinterpret_cast<const char*>(response.data()),
                        static_cast<std::streamsize>(response.size()));
        std::cout.flush();
    }
}
