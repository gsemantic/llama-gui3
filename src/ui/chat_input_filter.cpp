#include "../include/ui/chat_interface.h"
#include "imgui.h"
#include <iostream>
#include <cstring>

namespace llama_gui {
namespace ui {

// CRITICAL FIX: Character filter callback to prevent control characters at input level
static int InputTextFilterCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        // Filter out all control characters except basic whitespace
        unsigned int c = static_cast<unsigned int>(data->EventChar);
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            // Remove control character
            return 1; // Return 1 to filter out the character
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        // Filter entire buffer for control characters after any edit (including paste)
        std::cout << "DEBUG: CallbackEdit triggered, BufTextLen: " << data->BufTextLen << std::endl;
        std::string original;
        for (int i = 0; i < data->BufTextLen; ++i) {
            unsigned char c = static_cast<unsigned char>(data->Buf[i]);
            original += (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
        }
        std::cout << "DEBUG: Original buffer content: '" << original << "'" << std::endl;

        std::string filtered;
        for (int i = 0; i < data->BufTextLen; ++i) {
            unsigned char c = static_cast<unsigned char>(data->Buf[i]);
            // Allow printable ASCII, whitespace, and UTF-8 bytes
            if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t' || c == ' ' || c > 127) {
                filtered += static_cast<char>(c);
            } else {
                std::cout << "DEBUG: Filtered out char 0x" << std::hex << static_cast<int>(c) << std::dec << " at position " << i << std::endl;
            }
        }
        // Copy filtered text back to buffer
        int copy_len = std::min(static_cast<int>(filtered.size()), data->BufSize - 1);
        memcpy(data->Buf, filtered.c_str(), copy_len);
        data->Buf[copy_len] = '\0';
        data->BufTextLen = copy_len;

        std::cout << "DEBUG: Filtered buffer content: '" << filtered << "', length: " << copy_len << std::endl;

        // Adjust cursor and selection positions
        data->CursorPos = std::min(data->CursorPos, copy_len);
        data->SelectionStart = std::min(data->SelectionStart, copy_len);
        data->SelectionEnd = std::min(data->SelectionEnd, copy_len);
    }
    return 0;
}

// Enhanced UTF-8 validation and repair function
std::string validate_and_fix_utf8(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        // ASCII (0xxxxxxx)
        if (c <= 0x7F) {
            output += c;
            i++;
            continue;
        }

        // 2-byte UTF-8 (110xxxxx 10xxxxxx)
        if ((c & 0xE0) == 0xC0) {
            if (i + 1 < input.size() && (input[i+1] & 0xC0) == 0x80) {
                output += input.substr(i, 2);
                i += 2;
                continue;
            }
        }
        // 3-byte UTF-8 (1110xxxx 10xxxxxx 10xxxxxx)
        else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < input.size() && 
                (input[i+1] & 0xC0) == 0x80 && 
                (input[i+2] & 0xC0) == 0x80) {
                output += input.substr(i, 3);
                i += 3;
                continue;
            }
        }
        // 4-byte UTF-8 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
        else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < input.size() && 
                (input[i+1] & 0xC0) == 0x80 && 
                (input[i+2] & 0xC0) == 0x80 && 
                (input[i+3] & 0xC0) == 0x80) {
                output += input.substr(i, 4);
                i += 4;
                continue;
            }
        }

        // Невалидный байт — заменяем на символ замены Unicode (U+FFFD)
        output += '\xEF';
        output += '\xBF';
        output += '\xBD';  // REPLACEMENT CHARACTER
        i++;
    }

    return output;
}

} // namespace ui
} // namespace llama_gui
