#pragma once

/**
 * @file version.h
 * @brief Version information for Llama GUI
 * 
 * Version scheme (SemVer 2.0.0):
 *   MAJOR.MINOR.PATCH-PRERELEASE+BUILD
 * 
 * Examples:
 *   0.1.0-alpha.1  - First alpha release
 *   0.1.0-beta.1   - First beta release
 *   1.0.0          - Stable release
 */

// Version components (from CMake)
#ifndef LLAMA_GUI_VERSION_MAJOR
#define LLAMA_GUI_VERSION_MAJOR 0
#endif

#ifndef LLAMA_GUI_VERSION_MINOR
#define LLAMA_GUI_VERSION_MINOR 1
#endif

#ifndef LLAMA_GUI_VERSION_PATCH
#define LLAMA_GUI_VERSION_PATCH 0
#endif

#ifndef LLAMA_GUI_VERSION_SUFFIX
#define LLAMA_GUI_VERSION_SUFFIX "alpha.2"
#endif

#ifndef LLAMA_GUI_VERSION_CODENAME
#define LLAMA_GUI_VERSION_CODENAME "Archimedes"
#endif

// Build information (from CMake)
#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif

#ifndef BUILD_YEAR
#define BUILD_YEAR "unknown"
#endif

#ifndef BUILD_TYPE
#define BUILD_TYPE "unknown"
#endif

#ifndef COMPILER_VERSION
#define COMPILER_VERSION "unknown"
#endif

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "unknown"
#endif

#ifndef GIT_DESCRIBE
#define GIT_DESCRIBE "unknown"
#endif

// Precomputed version strings
#ifndef LLAMA_GUI_VERSION_STRING
#define LLAMA_GUI_VERSION_STRING "0.1.0-alpha.2"
#endif

#ifndef LLAMA_GUI_VERSION_FULL
#define LLAMA_GUI_VERSION_FULL "0.1.0-alpha.2 Archimedes"
#endif

namespace llama_gui {
namespace core {

/**
 * @brief Get version major number
 */
constexpr int getVersionMajor() {
    return LLAMA_GUI_VERSION_MAJOR;
}

/**
 * @brief Get version minor number
 */
constexpr int getVersionMinor() {
    return LLAMA_GUI_VERSION_MINOR;
}

/**
 * @brief Get version patch number
 */
constexpr int getVersionPatch() {
    return LLAMA_GUI_VERSION_PATCH;
}

/**
 * @brief Get version suffix (e.g., "alpha.2", "beta.1")
 */
constexpr const char* getVersionSuffix() {
    return LLAMA_GUI_VERSION_SUFFIX;
}

/**
 * @brief Get version codename (e.g., "Archimedes")
 */
constexpr const char* getVersionCodename() {
    return LLAMA_GUI_VERSION_CODENAME;
}

/**
 * @brief Get short version string (e.g., "0.1.0-alpha.2")
 */
constexpr const char* getVersionString() {
    return LLAMA_GUI_VERSION_STRING;
}

/**
 * @brief Get full version string (e.g., "0.1.0-alpha.2 Archimedes")
 */
constexpr const char* getVersionFull() {
    return LLAMA_GUI_VERSION_FULL;
}

/**
 * @brief Get build date
 */
constexpr const char* getBuildDate() {
    return BUILD_DATE;
}

/**
 * @brief Get build time
 */
constexpr const char* getBuildTime() {
    return BUILD_TIME;
}

/**
 * @brief Get build year
 */
constexpr const char* getBuildYear() {
    return BUILD_YEAR;
}

/**
 * @brief Get Git commit hash
 */
constexpr const char* getGitCommitHash() {
    return GIT_COMMIT_HASH;
}

/**
 * @brief Get Git describe output
 */
constexpr const char* getGitDescribe() {
    return GIT_DESCRIBE;
}

/**
 * @brief Check if this is a pre-release version
 */
constexpr bool isPrerelease() {
    return LLAMA_GUI_VERSION_MAJOR == 0;
}

/**
 * @brief Get version as a single integer for comparison
 *        Format: MMmmpp (e.g., 0.1.0 -> 100)
 */
constexpr int getVersionInt() {
    return LLAMA_GUI_VERSION_MAJOR * 10000 + LLAMA_GUI_VERSION_MINOR * 100 + LLAMA_GUI_VERSION_PATCH;
}

} // namespace core
} // namespace llama_gui
