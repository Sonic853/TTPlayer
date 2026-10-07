#include <ttpcomm/text.h>
#include "ttplayer/core/text.h"

#include <windows.h>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace ttplayer::core {
std::string WideToUtf8(std::wstring_view value) { return ttpcomm::text::WideToUtf8(value); }
std::wstring Utf8ToWide(std::string_view value) { return ttpcomm::text::Utf8ToWide(value); }

std::string ReadUtf8Text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open text file");
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    if (data.starts_with("\xEF\xBB\xBF")) data.erase(0, 3);
    return data;
}

void WriteUtf8Text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write text file");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}
}
