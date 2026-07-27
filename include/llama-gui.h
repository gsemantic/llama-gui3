/**
 * @file llama-gui.h
 * @brief Основной заголовочный файл для llama-gui проекта
 * @version 1.0.0
 * @date 2025-12-02
 */

#ifndef LLAMA_GUI_H
#define LLAMA_GUI_H

// Макросы версии
#define LLAMA_GUI_VERSION_MAJOR 1
#define LLAMA_GUI_VERSION_MINOR 0
#define LLAMA_GUI_VERSION_PATCH 0

#define LLAMA_GUI_VERSION_STRING "1.0.0"

// Настройки сборки
#ifdef _WIN32
    #define LLAMA_GUI_PLATFORM_WINDOWS
    #ifdef _WIN64
        #define LLAMA_GUI_PLATFORM_WINDOWS_64
    #else
        #define LLAMA_GUI_PLATFORM_WINDOWS_32
    #endif
#elif defined(__APPLE__)
    #define LLAMA_GUI_PLATFORM_MACOS
    #include <TargetConditionals.h>
    #if TARGET_OS_MAC
        #define LLAMA_GUI_PLATFORM_MACOS_DESKTOP
    #endif
#elif defined(__linux__)
    #define LLAMA_GUI_PLATFORM_LINUX
    #if defined(__x86_64__) || defined(_M_X64)
        #define LLAMA_GUI_PLATFORM_LINUX_64
    #elif defined(__i386__) || defined(_M_IX86)
        #define LLAMA_GUI_PLATFORM_LINUX_32
    #endif
#endif

// Настройки компилятора
#if defined(__GNUC__) || defined(__clang__)
    #define LLAMA_GUI_CDECL __attribute__((cdecl))
    #define LLAMA_GUI_STDCALL __attribute__((stdcall))
    #define LLAMA_GUI_FASTCALL __attribute__((fastcall))
#else
    #define LLAMA_GUI_CDECL
    #define LLAMA_GUI_STDCALL
    #define LLAMA_GUI_FASTCALL
#endif

// Макросы отладки
#ifdef _DEBUG
    #define LLAMA_GUI_DEBUG
    #ifndef LLAMA_GUI_NO_ASSERTS
        #define LLAMA_GUI_ASSERT(x) assert(x)
        #include <cassert>
    #endif
#else
    #ifndef LLAMA_GUI_NO_ASSERTS
        #define LLAMA_GUI_ASSERT(x) ((void)0)
    #endif
#endif

// Макросы для экспорта импорта DLL
#ifdef LLAMA_GUI_SHARED
    #ifdef LLAMA_GUI_EXPORTS
        #define LLAMA_GUI_API __declspec(dllexport)
    #else
        #define LLAMA_GUI_API __declspec(dllimport)
    #endif
#else
    #define LLAMA_GUI_API
#endif

// Поддержка Unicode
#ifdef UNICODE
    #define LLAMA_GUI_UNICODE
    #ifndef _UNICODE
        #define _UNICODE
    #endif
#endif

// Безопасные макросы
#ifdef _WIN32
    #define LLAMA_GUI_SAFE_DELETE(p) { if (p) { delete p; p = nullptr; } }
    #define LLAMA_GUI_SAFE_DELETE_ARRAY(p) { if (p) { delete[] p; p = nullptr; } }
#else
    #define LLAMA_GUI_SAFE_DELETE(p) { if (p) { delete p; p = nullptr; } }
    #define LLAMA_GUI_SAFE_DELETE_ARRAY(p) { if (p) { delete[] p; p = nullptr; } }
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#endif // LLAMA_GUI_H