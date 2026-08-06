#pragma once

#include <string>

// FontAwesome Icons definitions
// Based on FontAwesome Free Solid 6.6.0
// See: https://fontawesome.com/search?o=r&m=free

#define ICON_FA_FOLDER "\xef\x83\x87"       // U+F07B
#define ICON_FA_FOLDER_OPEN "\xef\x83\xbc"  // U+F07C
#define ICON_FA_SEARCH "\xef\x80\x82"       // U+F002
#define ICON_FA_COG "\xef\x80\x93"          // U+F013
#define ICON_FA_CHECK "\xef\x80\x8c"        // U+F00C
#define ICON_FA_TIMES "\xef\x80\x8d"        // U+F00D
#define ICON_FA_ARROW_DOWN "\xef\x81\xb8"   // U+F078
#define ICON_FA_ARROW_UP "\xef\x81\xb7"     // U+F077
#define ICON_FA_TRASH "\xef\x8b\xad"        // U+F2ED
#define ICON_FA_SAVE "\xef\x83\x87"         // U+F0C7
#define ICON_FA_PLUS "\xef\x81\xa7"         // U+F067
#define ICON_FA_MINUS "\xef\x81\xa8"        // U+F068
#define ICON_FA_PENCIL "\xef\x81\x80"       // U+F040
#define ICON_FA_CLIPBOARD "\xef\x8b\xa8"    // U+F328
#define ICON_FA_FILE "\xef\x85\x9b"         // U+F15B
#define ICON_FA_CHEVRON_LEFT "\xef\x81\x93" // U+F053
#define ICON_FA_CHEVRON_RIGHT "\xef\x81\x94"// U+F054
#define ICON_FA_DOWNLOAD "\xef\x87\x83"     // U+F019
#define ICON_FA_WINDOW_CLOSE "\xef\x90\x98" // U+F410
#define ICON_FA_GLOBE "\xef\x80\xac"        // U+F0AC

namespace llama_gui {
namespace ui {

struct FontAwesomeIcons {
    static constexpr const char* Folder = ICON_FA_FOLDER;
    static constexpr const char* FolderOpen = ICON_FA_FOLDER_OPEN;
    static constexpr const char* Search = ICON_FA_SEARCH;
    static constexpr const char* Cog = ICON_FA_COG;
    static constexpr const char* Check = ICON_FA_CHECK;
    static constexpr const char* Times = ICON_FA_TIMES;
    static constexpr const char* ArrowDown = ICON_FA_ARROW_DOWN;
    static constexpr const char* ArrowUp = ICON_FA_ARROW_UP;
    static constexpr const char* Trash = ICON_FA_TRASH;
    static constexpr const char* Save = ICON_FA_SAVE;
    static constexpr const char* Plus = ICON_FA_PLUS;
    static constexpr const char* Minus = ICON_FA_MINUS;
    static constexpr const char* Pencil = ICON_FA_PENCIL;
    static constexpr const char* Clipboard = ICON_FA_CLIPBOARD;
    static constexpr const char* File = ICON_FA_FILE;
    static constexpr const char* ChevronLeft = ICON_FA_CHEVRON_LEFT;
    static constexpr const char* ChevronRight = ICON_FA_CHEVRON_RIGHT;
    static constexpr const char* Download = ICON_FA_DOWNLOAD;
    static constexpr const char* WindowClose = ICON_FA_WINDOW_CLOSE;
    static constexpr const char* Globe = ICON_FA_GLOBE;
};

} // namespace ui
} // namespace llama_gui
