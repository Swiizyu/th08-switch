// switch_compat.cpp — подмножество Win32 поверх SDL2 + libnx (порт linux_compat.cpp).
//
// Отличия от linux_compat.cpp (по отчёту о порте):
//  • поиск данных на SD: корни (sdmc:/switch, sdmc:/, sdmc:/games, sdmc:/roms) ×
//    имена папок; первая с th08.dat, иначе первая существующая, иначе
//    sdmc:/switch/th08;
//  • TranslatePath(): относительные пути → <папка данных>/путь (с учётом
//    виртуального текущего каталога от _chdir), абсолютные проходят как есть;
//  • FindFirstFileA: opendir/readdir + собственный матчинг '?*' без учёта
//    регистра + сортировка;
//  • CP932→Unicode по сгенерированной таблице (iconv нет);
//  • шрифт ищется в папке данных (fontconfig нет);
//  • ввод с геймпада (мёртвая зона стика 8000), заполняет и VK-, и DIK-таблицы;
//  • окно 1280×720, контекст GLES 3 (ES-профиль, major 3);
//  • тайминг на clock_gettime(CLOCK_MONOTONIC);
//  • аудио: SDL-колбэк 44100/S16/стерео, samples 1024, allowed_changes=0.
//
// Фиксы аудита r2 (помечены АУДИТ):
//  • GetStartupInfoA заполняет lpTitle существующим файлом, отличным от
//    GetModuleFileNameA → usesRelativePath → disableVsync → бут-бенчмарк
//    CheckFps() пропускается, dummyMidiTimerEnabled=false (как при запуске
//    с Проводника);
//  • CreateThread даёт потокам честный стек (игра всегда передаёт 0).
#include "switch_compat.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <dinput.h>
#include <dsound.h>

#include "cp932_table.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <map>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" const char *th08_switch_data_dir();

namespace
{
enum HandleKind { HANDLE_FILE, HANDLE_THREAD, HANDLE_EVENT, HANDLE_MUTEX, HANDLE_FIND };

struct SwitchHandle
{
    explicit SwitchHandle(HandleKind value) : kind(value) {}
    virtual ~SwitchHandle() {}
    HandleKind kind;
};

struct FileHandle : SwitchHandle
{
    explicit FileHandle(int value) : SwitchHandle(HANDLE_FILE), fd(value) {}
    ~FileHandle() { if (fd >= 0) close(fd); }
    int fd;
};

struct ThreadHandle : SwitchHandle
{
    ThreadHandle() : SwitchHandle(HANDLE_THREAD), finished(false), joined(false), result(0), id(0) {}
    pthread_t thread;
    volatile bool finished;
    bool joined;
    DWORD result;
    DWORD id;
    LPTHREAD_START_ROUTINE start;
    LPVOID parameter;
};

struct EventHandle : SwitchHandle
{
    EventHandle(bool manualReset, bool initial)
        : SwitchHandle(HANDLE_EVENT), manual(manualReset), signaled(initial)
    {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&condition, NULL);
    }
    ~EventHandle()
    {
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool manual;
    bool signaled;
};

struct MutexHandle : SwitchHandle
{
    MutexHandle() : SwitchHandle(HANDLE_MUTEX) { pthread_mutex_init(&mutex, NULL); }
    ~MutexHandle() { pthread_mutex_destroy(&mutex); }
    pthread_mutex_t mutex;
};

struct FindHandle : SwitchHandle
{
    FindHandle() : SwitchHandle(HANDLE_FIND), index(0) {}
    std::vector<std::string> paths;
    size_t index;
};

struct GdiObject
{
    enum Kind { BITMAP, FONT } kind;
    virtual ~GdiObject() {}
};

struct GdiBitmap : GdiObject
{
    GdiBitmap(int width_, int height_, int bits_)
        : width(width_), height(height_ < 0 ? -height_ : height_), bits(bits_)
    {
        kind = BITMAP;
        pitch = ((width * bits + 31) / 32) * 4;
        // SWITCH-ФИКС: InvertAlpha сканирует fontHeight*2+6 > 64 строк DIB
        // 1024×64 — переброс гасим запасом (как CRT-куча Windows).
        pixels.resize(pitch * height + 256 * 1024);
    }
    int width, height, bits, pitch;
    std::vector<BYTE> pixels;
};

struct GdiFont : GdiObject
{
    GdiFont() : font(NULL) { kind = FONT; }
    explicit GdiFont(TTF_Font *font_) : font(font_) { kind = FONT; }
    ~GdiFont() { if (font != NULL) TTF_CloseFont(font); }
    TTF_Font *font;
};

struct GdiDc
{
    GdiDc() : bitmap(NULL), font(&stockFont), color(0xffffffff) {}
    GdiBitmap *bitmap;
    GdiFont stockFont;
    GdiFont *font;
    COLORREF color;
};

DWORD g_lastError;
WNDPROC g_windowProcedure;
SDL_Window *g_window;
SDL_GameController *g_gamepad;
std::map<DWORD, std::vector<MSG> > g_threadMessages;
pthread_mutex_t g_messageMutex = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Папка данных и трансляция путей
// ---------------------------------------------------------------------------

const char *kSdRoots[] = {"sdmc:/switch", "sdmc:/", "sdmc:/games", "sdmc:/roms"};
const char *kFolderNames[] = {
    "th08", "touhou8", "touhou 8", "th08-switch", "touhou8-switch",
    "imperishable night", "imperishablenight", "in",
};

std::string g_dataDir;
bool g_dataDirResolved;
std::string g_virtualCwd; // "" либо "backup"/"replay"

bool PathExists(const std::string &path, bool wantDirectory)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0)
        return false;
    return wantDirectory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode);
}

void ResolveDataDir()
{
    if (g_dataDirResolved)
        return;
    g_dataDirResolved = true;

    std::string firstExisting;
    for (size_t root = 0; root < sizeof(kSdRoots) / sizeof(kSdRoots[0]); ++root)
    {
        for (size_t name = 0; name < sizeof(kFolderNames) / sizeof(kFolderNames[0]); ++name)
        {
            std::string candidate = std::string(kSdRoots[root]) + "/" + kFolderNames[name];
            if (!PathExists(candidate, true))
                continue;
            if (firstExisting.empty())
                firstExisting = candidate;
            if (PathExists(candidate + "/th08.dat", false))
            {
                g_dataDir = candidate;
                return;
            }
        }
    }
    g_dataDir = firstExisting.empty() ? "sdmc:/switch/th08" : firstExisting;
}

std::string NormalizePath(const std::string &path)
{
    std::vector<std::string> parts;
    size_t index = 0;
    bool absolute = !path.empty() && (path[0] == '/');
    while (index < path.size())
    {
        size_t separator = path.find('/', index);
        if (separator == std::string::npos)
            separator = path.size();
        std::string part = path.substr(index, separator - index);
        index = separator + 1;
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
        {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back("..");
            continue;
        }
        parts.push_back(part);
    }
    std::string result;
    for (size_t part = 0; part < parts.size(); ++part)
    {
        result += parts[part];
        if (part + 1 < parts.size())
            result += '/';
    }
    if (absolute)
        result = "/" + result;
    return result.empty() && !path.empty() ? "." : result;
}

bool IsAbsolutePath(const std::string &path)
{
    return !path.empty() && (path[0] == '/' || path.compare(0, 5, "sdmc:") == 0 ||
                             path.compare(0, 6, "romfs:") == 0);
}

// Все относительные пути (включая "./thbgm.dat") транслируются в
// <папка данных>/[<виртуальный cwd>/]путь; абсолютные проходят как есть.
std::string TranslatePath(const char *path)
{
    if (path == NULL)
        return std::string(".");
    std::string cleaned = path;
    for (size_t index = 0; index < cleaned.size(); ++index)
        if (cleaned[index] == '\\')
            cleaned[index] = '/';
    if (cleaned.empty())
        return std::string(".");
    if (IsAbsolutePath(cleaned))
        return cleaned;
    ResolveDataDir();
    std::string full = g_dataDir;
    if (!g_virtualCwd.empty())
        full += "/" + g_virtualCwd;
    full += "/" + cleaned;
    return NormalizePath(full);
}

int SetVirtualCwd(const char *path)
{
    if (path == NULL)
        return -1;
    std::string cleaned = path;
    for (size_t index = 0; index < cleaned.size(); ++index)
        if (cleaned[index] == '\\')
            cleaned[index] = '/';
    if (IsAbsolutePath(cleaned))
    {
        // sdmc:/... трактуем относительно папки данных
        ResolveDataDir();
        if (cleaned.compare(0, g_dataDir.size(), g_dataDir) == 0)
            cleaned = cleaned.substr(g_dataDir.size());
        else
            return -1;
    }
    std::string combined = g_virtualCwd.empty() ? cleaned : g_virtualCwd + "/" + cleaned;
    g_virtualCwd = NormalizePath(combined);
    if (g_virtualCwd == ".")
        g_virtualCwd.clear();
    return 0;
}

// ---------------------------------------------------------------------------
// CP932 → Unicode/UTF-8 (iconv на свитче нет)
// ---------------------------------------------------------------------------

unsigned short LookupCp932(unsigned short code)
{
    const th08_switch::Cp932Entry *table = th08_switch::g_Cp932Table;
    int low = 0, high = th08_switch::g_Cp932TableSize - 1;
    while (low <= high)
    {
        const int middle = (low + high) / 2;
        if (table[middle].code == code)
            return table[middle].unicode;
        if (table[middle].code < code)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return 0xFFFD;
}

void DecodeCp932ToWide(const char *text, size_t length, std::vector<wchar_t> &out)
{
    out.clear();
    for (size_t index = 0; index < length; ++index)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80)
        {
            out.push_back(static_cast<wchar_t>(lead));
        }
        else if (lead >= 0xA1 && lead <= 0xDF)
        {
            out.push_back(static_cast<wchar_t>(LookupCp932(lead)));
        }
        else if (index + 1 < length)
        {
            const unsigned char trail = static_cast<unsigned char>(text[index + 1]);
            index += 1;
            out.push_back(static_cast<wchar_t>(LookupCp932((lead << 8) | trail)));
        }
        else
        {
            out.push_back(0xFFFD);
        }
    }
}

void AppendUtf8(std::string &out, wchar_t value)
{
    if (value < 0x80)
    {
        out += static_cast<char>(value);
    }
    else if (value < 0x800)
    {
        out += static_cast<char>(0xC0 | (value >> 6));
        out += static_cast<char>(0x80 | (value & 0x3F));
    }
    else if (value < 0x10000)
    {
        out += static_cast<char>(0xE0 | (value >> 12));
        out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (value & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (value >> 18));
        out += static_cast<char>(0x80 | ((value >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (value & 0x3F));
    }
}

std::string ConvertCp932ToUtf8(const char *text, size_t length)
{
    if (text == NULL || length == 0)
        return std::string();
    std::vector<wchar_t> wide;
    DecodeCp932ToWide(text, length, wide);
    std::string utf8;
    utf8.reserve(wide.size() * 2);
    for (size_t index = 0; index < wide.size(); ++index)
        AppendUtf8(utf8, wide[index]);
    return utf8;
}

// ---------------------------------------------------------------------------
// Шрифт (fontconfig нет: msgothic.ttc → meiryo → любая *.ttf/*.ttc в данных)
// ---------------------------------------------------------------------------

bool HasFontExtension(const std::string &name)
{
    const size_t length = name.size();
    if (length < 4)
        return false;
    std::string tail = name.substr(length - 4);
    for (size_t index = 0; index < tail.size(); ++index)
        tail[index] = static_cast<char>(tolower(tail[index]));
    return tail == ".ttf" || tail == ".ttc";
}

std::string Lowercase(const std::string &value)
{
    std::string result = value;
    for (size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<char>(tolower(result[index]));
    return result;
}

const char *ResolveJapaneseFont()
{
    static std::string path;
    static bool resolved;
    if (resolved) return path.empty() ? NULL : path.c_str();
    resolved = true;

    const char *overridePath = getenv("TH08_FONT");
    if (overridePath != NULL && access(overridePath, R_OK) == 0)
    {
        path = overridePath;
        return path.c_str();
    }

    ResolveDataDir();
    const std::string preferred[] = {"msgothic.ttc", "msgothic.ttf"};
    for (size_t index = 0; index < sizeof(preferred) / sizeof(preferred[0]); ++index)
    {
        std::string candidate = g_dataDir + "/" + preferred[index];
        if (access(candidate.c_str(), R_OK) == 0)
        {
            path = candidate;
            return path.c_str();
        }
    }

    DIR *directory = opendir(g_dataDir.c_str());
    if (directory == NULL)
        return NULL;
    std::string anyFont;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        std::string name = entry->d_name;
        if (!HasFontExtension(name))
            continue;
        std::string lower = Lowercase(name);
        if (lower.compare(0, 6, "meiryo") == 0)
        {
            path = g_dataDir + "/" + name;
            break;
        }
        if (anyFont.empty())
            anyFont = name;
    }
    closedir(directory);
    if (path.empty() && !anyFont.empty())
        path = g_dataDir + "/" + anyFont;
    return path.empty() ? NULL : path.c_str();
}

// ---------------------------------------------------------------------------
// GDI-текст (перенесено из linux-версии без изменений)
// ---------------------------------------------------------------------------

void PutGdiTextPixel(GdiBitmap *bitmap, int x, int y, COLORREF color, BYTE coverage)
{
    if (bitmap == NULL || x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height || coverage == 0) return;
    const int red = color & 0xff;
    const int green = (color >> 8) & 0xff;
    const int blue = (color >> 16) & 0xff;
    BYTE *pixel = &bitmap->pixels[y * bitmap->pitch + x * bitmap->bits / 8];
    if (bitmap->bits == 16)
    {
        uint16_t packed;
        memcpy(&packed, pixel, sizeof(packed));
        int oldRed = ((packed >> 10) & 0x1f) * 255 / 31;
        int oldGreen = ((packed >> 5) & 0x1f) * 255 / 31;
        int oldBlue = (packed & 0x1f) * 255 / 31;
        oldRed = (oldRed * (255 - coverage) + red * coverage) / 255;
        oldGreen = (oldGreen * (255 - coverage) + green * coverage) / 255;
        oldBlue = (oldBlue * (255 - coverage) + blue * coverage) / 255;
        packed = static_cast<uint16_t>(((oldRed >> 3) << 10) | ((oldGreen >> 3) << 5) | (oldBlue >> 3));
        memcpy(pixel, &packed, sizeof(packed));
    }
    else if (bitmap->bits == 32)
    {
        pixel[0] = static_cast<BYTE>((pixel[0] * (255 - coverage) + blue * coverage) / 255);
        pixel[1] = static_cast<BYTE>((pixel[1] * (255 - coverage) + green * coverage) / 255);
        pixel[2] = static_cast<BYTE>((pixel[2] * (255 - coverage) + red * coverage) / 255);
        pixel[3] = 0;
    }
}

// ---------------------------------------------------------------------------
// Потоки
// ---------------------------------------------------------------------------

DWORD CurrentThreadIdImpl()
{
    // pthread_t в newlib — указатель.
    return static_cast<DWORD>(reinterpret_cast<uintptr_t>(pthread_self()));
}

void *ThreadTrampoline(void *opaque)
{
    ThreadHandle *handle = static_cast<ThreadHandle *>(opaque);
    handle->id = CurrentThreadIdImpl();
    handle->result = handle->start(handle->parameter);
    handle->finished = true;
    return NULL;
}

// ---------------------------------------------------------------------------
// Поиск файлов
// ---------------------------------------------------------------------------

bool PatternMatch(const char *name, const char *pattern)
{
    // '?' — ровно один символ, '*' — любая последовательность, без учёта регистра.
    if (*pattern == '\0')
        return *name == '\0';
    if (*pattern == '*')
    {
        for (const char *candidate = name; ; ++candidate)
        {
            if (PatternMatch(candidate, pattern + 1))
                return true;
            if (*candidate == '\0')
                return false;
        }
    }
    if (*name == '\0')
        return false;
    if (*pattern == '?' || tolower(*pattern) == tolower(*name))
        return PatternMatch(name + 1, pattern + 1);
    return false;
}

bool FillFindData(FindHandle *handle, WIN32_FIND_DATAA *data)
{
    if (handle->index >= handle->paths.size())
        return false;
    memset(data, 0, sizeof(*data));
    const std::string &path = handle->paths[handle->index++];
    const char *name = strrchr(path.c_str(), '/');
    strncpy(data->cFileName, name != NULL ? name + 1 : path.c_str(), MAX_PATH - 1);
    struct stat info;
    if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
        data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    return true;
}

// ---------------------------------------------------------------------------
// SDL-события и геймпад
// ---------------------------------------------------------------------------

void OpenGamepad()
{
    if (g_gamepad != NULL)
        return;
    const int count = SDL_NumJoysticks();
    for (int index = 0; index < count; ++index)
    {
        if (!SDL_IsGameController(index))
            continue;
        g_gamepad = SDL_GameControllerOpen(index);
        if (g_gamepad != NULL)
            break;
    }
}

void PumpSdlEvents(MSG *message, bool *hasMessage)
{
    *hasMessage = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
        return;
    SDL_Event event;
    if (!SDL_PollEvent(&event))
        return;
    memset(message, 0, sizeof(*message));
    message->hwnd = reinterpret_cast<HWND>(g_window);
    if (event.type == SDL_QUIT)
        message->message = WM_CLOSE;
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = TRUE;
    }
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = FALSE;
    }
    else if (event.type == SDL_CONTROLLERDEVICEADDED)
    {
        OpenGamepad();
        message->message = 0;
    }
    else
        message->message = 0;
    *hasMessage = true;
}

struct GamepadButtons
{
    bool up, down, left, right;
    bool shot, bomb, focus, skip, menu;
};

void ReadGamepad(GamepadButtons &buttons)
{
    memset(&buttons, 0, sizeof(buttons));
    SDL_PumpEvents();
    if (g_gamepad == NULL)
        OpenGamepad();
    if (g_gamepad == NULL)
        return;

    const Sint16 axisX = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 axisY = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY);
    if (axisX < -8000) buttons.left = true;
    if (axisX > 8000) buttons.right = true;
    if (axisY < -8000) buttons.up = true;
    if (axisY > 8000) buttons.down = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons.up = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons.down = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons.left = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons.right = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_A)) buttons.shot = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_B)) buttons.bomb = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) buttons.focus = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons.skip = true;
    if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_START)) buttons.menu = true;
}

// Заполняет И VK-таблицу (GetKeyboardState), И DIK-таблицу (DirectInput):
// игра ходит обоими путями.
void FillKeyboard(BYTE *state, bool directInput)
{
    memset(state, 0, 256);
    GamepadButtons buttons;
    ReadGamepad(buttons);
#define SET_KEY(code) state[(code)] = 0x80
    if (directInput)
    {
        if (buttons.up) SET_KEY(DIK_UP);
        if (buttons.down) SET_KEY(DIK_DOWN);
        if (buttons.left) SET_KEY(DIK_LEFT);
        if (buttons.right) SET_KEY(DIK_RIGHT);
        if (buttons.shot) SET_KEY(DIK_Z);
        if (buttons.bomb) SET_KEY(DIK_X);
        if (buttons.focus) SET_KEY(DIK_LSHIFT);
        if (buttons.skip) SET_KEY(DIK_LCONTROL);
        if (buttons.menu) SET_KEY(DIK_ESCAPE);
    }
    else
    {
        if (buttons.up) SET_KEY(VK_UP);
        if (buttons.down) SET_KEY(VK_DOWN);
        if (buttons.left) SET_KEY(VK_LEFT);
        if (buttons.right) SET_KEY(VK_RIGHT);
        if (buttons.shot) SET_KEY('Z');
        if (buttons.bomb) SET_KEY('X');
        if (buttons.focus) SET_KEY(VK_SHIFT);
        if (buttons.skip) SET_KEY(VK_CONTROL);
        if (buttons.menu) SET_KEY(VK_ESCAPE);
    }
#undef SET_KEY
}
} // namespace

extern "C" const char *th08_switch_data_dir()
{
    ResolveDataDir();
    return g_dataDir.c_str();
}

extern "C" int th08_switch_mkdir(const char *path)
{
    std::string translated = TranslatePath(path);
    if (mkdir(translated.c_str(), 0777) == 0)
        return 0;
    return errno == EEXIST ? 0 : -1; // _mkdir("backup") на повторных запусках не ошибка
}

extern "C" int th08_switch_chdir(const char *path)
{
    return SetVirtualCwd(path);
}

extern "C" int th08_switch_rename(const char *oldPath, const char *newPath)
{
    std::string translatedOld = TranslatePath(oldPath);
    std::string translatedNew = TranslatePath(newPath);
    return rename(translatedOld.c_str(), translatedNew.c_str());
}

extern "C" SDL_Window *th08_linux_get_window() { return g_window; }

extern "C" {
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, LPVOID, DWORD disposition, DWORD, HANDLE)
{
    std::string translated = TranslatePath(path != NULL ? path : "");
    int flags = (access & (GENERIC_WRITE | FILE_APPEND_DATA)) ? O_WRONLY : O_RDONLY;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) flags = O_RDWR;
    if (access & FILE_APPEND_DATA) flags |= O_APPEND;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    if (disposition == OPEN_ALWAYS) flags |= O_CREAT;
    int fd = open(translated.c_str(), flags, 0666);
    if (fd < 0) { g_lastError = errno; return INVALID_HANDLE_VALUE; }
    return new FileHandle(fd);
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPVOID security, DWORD disposition, DWORD attrs, HANDLE templ)
{
    if (path == NULL)
        return INVALID_HANDLE_VALUE;
    std::string narrow;
    for (size_t index = 0; path[index] != 0; ++index)
    {
        wchar_t value = path[index];
        narrow += static_cast<char>(value < 0x100 ? value : '?');
    }
    return CreateFileA(narrow.c_str(), access, share, security, disposition, attrs, templ);
}

BOOL ReadFile(HANDLE raw, LPVOID data, DWORD size, LPDWORD readSize, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
    ssize_t result = read(static_cast<FileHandle *>(raw)->fd, data, size);
    if (readSize != NULL) *readSize = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}

BOOL WriteFile(HANDLE raw, LPCVOID data, DWORD size, LPDWORD written, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
    ssize_t result = write(static_cast<FileHandle *>(raw)->fd, data, size);
    if (written != NULL) *written = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}

DWORD SetFilePointer(HANDLE raw, LONG offset, LONG *, DWORD origin)
{
    int whence = origin == FILE_BEGIN ? SEEK_SET : origin == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t result = lseek(static_cast<FileHandle *>(raw)->fd, offset, whence);
    return result < 0 ? static_cast<DWORD>(-1) : static_cast<DWORD>(result);
}

DWORD GetFileSize(HANDLE raw, LPDWORD high)
{
    struct stat info;
    if (fstat(static_cast<FileHandle *>(raw)->fd, &info) != 0) return static_cast<DWORD>(-1);
    if (high != NULL) *high = static_cast<DWORD>(static_cast<unsigned long long>(info.st_size) >> 32);
    return static_cast<DWORD>(info.st_size);
}

BOOL CloseHandle(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE) return FALSE;
    SwitchHandle *handle = static_cast<SwitchHandle *>(raw);
    if (handle->kind == HANDLE_THREAD)
    {
        ThreadHandle *thread = static_cast<ThreadHandle *>(handle);
        if (!thread->finished) return TRUE;
        if (!thread->joined) pthread_join(thread->thread, NULL);
    }
    delete handle;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE raw) { return fsync(static_cast<FileHandle *>(raw)->fd) == 0; }
BOOL DeleteFileA(LPCSTR path) { return unlink(TranslatePath(path).c_str()) == 0; }

DWORD GetFileAttributesW(LPCWSTR path)
{
    if (path == NULL) return INVALID_FILE_ATTRIBUTES;
    std::string narrow;
    for (size_t index = 0; path[index] != 0; ++index)
    {
        wchar_t value = path[index];
        narrow += static_cast<char>(value < 0x100 ? value : '?');
    }
    struct stat info;
    std::string translated = TranslatePath(narrow.c_str());
    if (stat(translated.c_str(), &info) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL SetCurrentDirectoryW(LPCWSTR path)
{
    if (path == NULL) return FALSE;
    std::string narrow;
    for (size_t index = 0; path[index] != 0; ++index)
    {
        wchar_t value = path[index];
        narrow += static_cast<char>(value < 0x100 ? value : '?');
    }
    return SetVirtualCwd(narrow.c_str()) == 0;
}

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data)
{
    if (pattern == NULL) return INVALID_HANDLE_VALUE;
    std::string directory = ".";
    std::string mask = pattern;
    const char *lastSlash = strrchr(pattern, '/');
    const char *lastBackslash = strrchr(pattern, '\\');
    const char *separator = lastSlash != NULL && (lastBackslash == NULL || lastSlash > lastBackslash)
                                ? lastSlash
                                : lastBackslash;
    if (separator != NULL)
    {
        directory = std::string(pattern, separator - pattern);
        if (directory.empty())
            directory = "/";
        mask = separator + 1;
    }

    std::string realDirectory = TranslatePath(directory.c_str());
    DIR *opened = opendir(realDirectory.c_str());
    if (opened == NULL) return INVALID_HANDLE_VALUE;

    std::vector<std::string> names;
    struct dirent *entry;
    while ((entry = readdir(opened)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (PatternMatch(entry->d_name, mask.c_str()))
            names.push_back(entry->d_name);
    }
    closedir(opened);
    if (names.empty()) return INVALID_HANDLE_VALUE;

    FindHandle *handle = new FindHandle();
    for (size_t index = 0; index < names.size(); ++index)
        handle->paths.push_back(realDirectory + "/" + names[index]);
    if (!FillFindData(handle, data)) { delete handle; return INVALID_HANDLE_VALUE; }
    return handle;
}

BOOL FindNextFileA(HANDLE raw, WIN32_FIND_DATAA *data) { return FillFindData(static_cast<FindHandle *>(raw), data); }
BOOL FindClose(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE)
        return FALSE;
    SwitchHandle *handle = static_cast<SwitchHandle *>(raw);
    if (handle->kind != HANDLE_FIND)
        return FALSE;
    delete static_cast<FindHandle *>(handle);
    return TRUE;
}
void Sleep(DWORD milliseconds) { usleep(static_cast<useconds_t>(milliseconds) * 1000); }

DWORD timeGetTime(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<DWORD>(value.tv_sec * 1000ULL + value.tv_nsec / 1000000);
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *value) { value->QuadPart = 1000000000; return TRUE; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *value)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    value->QuadPart = time.tv_sec * 1000000000LL + time.tv_nsec; return TRUE;
}
DWORD GetCurrentThreadId(void) { return CurrentThreadIdImpl(); }

HANDLE CreateThread(LPVOID, size_t, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD, LPDWORD id)
{
    ThreadHandle *handle = new ThreadHandle(); handle->start = start; handle->parameter = parameter;
    // АУДИТ-ФИКС (баг №2): игра везде передаёт dwStackSize=0 («по умолчанию»),
    // а дефолтный стек pthread на libnx мал — даём честный мегабайт.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    if (pthread_create(&handle->thread, &attributes, ThreadTrampoline, handle) != 0)
    {
        pthread_attr_destroy(&attributes);
        delete handle;
        return NULL;
    }
    pthread_attr_destroy(&attributes);
    while (handle->id == 0) usleep(100);
    if (id != NULL) *id = handle->id;
    return handle;
}

BOOL PostThreadMessageA(DWORD id, UINT message, WPARAM wparam, LPARAM lparam)
{
    MSG value; memset(&value, 0, sizeof(value)); value.message = message; value.wParam = wparam; value.lParam = lparam;
    pthread_mutex_lock(&g_messageMutex); g_threadMessages[id].push_back(value); pthread_mutex_unlock(&g_messageMutex);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE raw, DWORD timeout)
{
    SwitchHandle *base = static_cast<SwitchHandle *>(raw);
    DWORD start = timeGetTime();
    for (;;)
    {
        if (base->kind == HANDLE_THREAD && static_cast<ThreadHandle *>(base)->finished) return WAIT_OBJECT_0;
        if (base->kind == HANDLE_EVENT)
        {
            EventHandle *event = static_cast<EventHandle *>(base);
            pthread_mutex_lock(&event->mutex);
            if (event->signaled)
            {
                if (!event->manual) event->signaled = false;
                pthread_mutex_unlock(&event->mutex); return WAIT_OBJECT_0;
            }
            pthread_mutex_unlock(&event->mutex);
        }
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL, DWORD timeout, DWORD)
{
    DWORD start = timeGetTime();
    for (;;)
    {
        for (DWORD i = 0; i < count; ++i) if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) return i;
        pthread_mutex_lock(&g_messageMutex);
        bool hasMessages = !g_threadMessages[CurrentThreadIdImpl()].empty();
        pthread_mutex_unlock(&g_messageMutex);
        if (hasMessages) return count;
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

HANDLE CreateEventA(LPVOID, BOOL manual, BOOL initial, LPCSTR) { return new EventHandle(manual != FALSE, initial != FALSE); }
BOOL SetEvent(HANDLE raw)
{
    EventHandle *event = static_cast<EventHandle *>(raw); pthread_mutex_lock(&event->mutex);
    event->signaled = true; pthread_cond_broadcast(&event->condition); pthread_mutex_unlock(&event->mutex); return TRUE;
}
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void *) { return id != 0 ? id : 1; }
BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }
HANDLE CreateMutexA(LPVOID, BOOL, LPCSTR) { g_lastError = 0; return new MutexHandle(); }
DWORD GetLastError(void) { return g_lastError; }
void InitializeCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_init(value, NULL); }
void DeleteCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_destroy(value); }
void EnterCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_lock(value); }
void LeaveCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_unlock(value); }

BOOL PeekMessageA(MSG *message, HWND, UINT, UINT, UINT)
{
    pthread_mutex_lock(&g_messageMutex);
    std::vector<MSG> &queue = g_threadMessages[CurrentThreadIdImpl()];
    if (!queue.empty()) { *message = queue.front(); queue.erase(queue.begin()); pthread_mutex_unlock(&g_messageMutex); return TRUE; }
    pthread_mutex_unlock(&g_messageMutex);
    bool hasMessage; PumpSdlEvents(message, &hasMessage); return hasMessage;
}
BOOL TranslateMessage(const MSG *) { return TRUE; }
LRESULT DispatchMessageA(const MSG *message) { return g_windowProcedure != NULL ? g_windowProcedure(message->hwnd, message->message, message->wParam, message->lParam) : 0; }
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
BOOL RegisterClassA(const WNDCLASSA *value) { g_windowProcedure = value->lpfnWndProc; return TRUE; }

HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR title, DWORD style, int, int, int width, int height, HWND, HANDLE, HINSTANCE, LPVOID)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    { fprintf(stderr, "th08-switch: SDL_Init failed: %s\n", SDL_GetError()); return NULL; }
    // До окна — атрибуты контекста: GLES 3 (ES-профиль). SDL2-бэкенд devkitPro
    // при major=3 запрашивает EGL_OPENGL_ES3_BIT.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (style == WS_OVERLAPPEDWINDOW) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    width = 1280;
    height = 720;
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (g_window == NULL)
        fprintf(stderr, "th08-switch: SDL_CreateWindow failed: %s\n", SDL_GetError());
    else
        OpenGamepad();
    return reinterpret_cast<HWND>(g_window);
}

BOOL DestroyWindow(HWND) { if (g_window != NULL) SDL_DestroyWindow(g_window); g_window = NULL; SDL_Quit(); return TRUE; }
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL MoveWindow(HWND, int, int, int, int, BOOL) { return TRUE; }
int ShowCursor(BOOL show) { return SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }
HCURSOR SetCursor(HCURSOR value) { return value; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HCURSOR>(1); }
HGDIOBJ GetStockObject(int) { return NULL; }
int GetSystemMetrics(int metric) { return metric == SM_CYCAPTION ? 24 : 4; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID value, UINT) { if (value != NULL) *static_cast<BOOL *>(value) = FALSE; return TRUE; }
HWND GetForegroundWindow(void) { return reinterpret_cast<HWND>(g_window); }
DWORD GetWindowThreadProcessId(HWND, LPDWORD process) { if (process != NULL) *process = getpid(); return GetCurrentThreadId(); }
BOOL AttachThreadInput(DWORD, DWORD, BOOL) { return TRUE; }
HWND SetActiveWindow(HWND window) { if (g_window != NULL) SDL_RaiseWindow(g_window); return window; }
LONG GetWindowLongA(HWND, int) { return 0; }
BOOL WINNLSEnableIME(HWND, BOOL) { return TRUE; }
BOOL GetKeyboardState(BYTE *state) { FillKeyboard(state, false); return TRUE; }
BOOL SetKeyboardState(const BYTE *) { return TRUE; }
int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) { fprintf(stderr, "%s: %s\n", title ? title : "TH08", text ? text : ""); return 0; }
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT)
{
    if (title != NULL) fwprintf(stderr, L"%ls: ", title);
    if (text != NULL) fwprintf(stderr, L"%ls\n", text);
    return 0;
}

DWORD GetModuleFileNameA(HMODULE, LPSTR buffer, DWORD size)
{
    if (buffer == NULL || size == 0) return 0;
    const char *dataDir = th08_switch_data_dir();
    strncpy(buffer, dataDir, size - 1);
    buffer[size - 1] = 0;
    return static_cast<DWORD>(strlen(buffer));
}
DWORD GetConsoleTitleA(LPSTR buffer, DWORD size) { if (size) buffer[0] = 0; return 0; }

void GetStartupInfoA(STARTUPINFOA *value)
{
    // АУДИТ-ФИКС (баг №1): lpTitle указывает на СУЩЕСТВУЮЩИЙ файл с расширением,
    // отличный от того, что возвращает GetModuleFileNameA (папка данных).
    // Тогда main.cpp: usesRelativePath=true → disableVsync → бут-бенчмарк
    // CheckFps() пропускается целиком, а dummyMidiTimerEnabled=false — как при
    // запуске с Проводника. Это легальная семантика «относительного запуска»,
    // встроенная в саму игру.
    if (value == NULL) return;
    DWORD size = value->cb;
    memset(value, 0, sizeof(STARTUPINFOA));
    value->cb = size;
    static char title[MAX_PATH + 1];
    static bool titleReady;
    if (!titleReady)
    {
        snprintf(title, sizeof(title), "%s/th08.dat", th08_switch_data_dir());
        titleReady = true;
    }
    value->lpTitle = title;
}

int MultiByteToWideChar(UINT, DWORD, LPCSTR source, int sourceSize, LPWSTR dest, int destSize)
{
    if (source == NULL) return 0;
    const bool includeNull = sourceSize < 0;
    size_t length = includeNull ? strlen(source) : static_cast<size_t>(sourceSize);
    std::vector<wchar_t> wide;
    DecodeCp932ToWide(source, length, wide);
    if (dest != NULL)
    {
        if (destSize <= 0 || static_cast<size_t>(destSize) < wide.size() + (includeNull ? 1 : 0))
            return 0;
        for (size_t index = 0; index < wide.size(); ++index)
            dest[index] = wide[index];
        if (includeNull)
            dest[wide.size()] = 0;
    }
    return static_cast<int>(wide.size() + (includeNull ? 1 : 0));
}
DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD error, DWORD, LPSTR buffer, DWORD size, va_list *)
{
    const char *message = strerror(error); if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) *reinterpret_cast<char **>(buffer) = strdup(message);
    else if (size) { strncpy(buffer, message, size - 1); buffer[size - 1] = 0; } return strlen(message);
}
LPVOID LocalFree(LPVOID value) { free(value); return NULL; }
HGLOBAL GlobalAlloc(UINT, size_t size) { return calloc(1, size); }
HGLOBAL GlobalFree(HGLOBAL value) { free(value); return NULL; }
HMODULE LoadLibraryA(LPCSTR) { return NULL; }
void *GetProcAddress(HMODULE, LPCSTR) { return NULL; }
HDC CreateCompatibleDC(HDC) { return new GdiDc(); }
BOOL DeleteDC(HDC value) { delete static_cast<GdiDc *>(value); return TRUE; }
HGDIOBJ SelectObject(HDC dcRaw, HGDIOBJ objectRaw)
{
    if (dcRaw == NULL || objectRaw == NULL) return NULL;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    GdiObject *object = static_cast<GdiObject *>(objectRaw);
    if (object->kind == GdiObject::BITMAP)
    {
        GdiBitmap *old = dc->bitmap;
        dc->bitmap = static_cast<GdiBitmap *>(object);
        return old;
    }
    GdiFont *old = dc->font;
    dc->font = static_cast<GdiFont *>(object);
    return old;
}
BOOL DeleteObject(HGDIOBJ value) { delete static_cast<GdiObject *>(value); return TRUE; }
int SetBkMode(HDC, int mode) { return mode; }
COLORREF SetTextColor(HDC raw, COLORREF color) { GdiDc *dc = static_cast<GdiDc *>(raw); COLORREF old = dc->color; dc->color = color; return old; }
HFONT CreateFontA(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR)
{
    const char *path = ResolveJapaneseFont();
    if (path == NULL) return new GdiFont();
    if (!TTF_WasInit() && TTF_Init() != 0)
    {
        fprintf(stderr, "th08-switch: SDL_ttf initialization failed: %s\n", TTF_GetError());
        return new GdiFont();
    }
    TTF_Font *font = TTF_OpenFont(path, height < 0 ? -height : height);
    if (font == NULL)
    {
        fprintf(stderr, "th08-switch: unable to load Japanese font %s: %s\n", path, TTF_GetError());
        return new GdiFont();
    }
    if (weight >= FW_SEMIBOLD) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    return new GdiFont(font);
}
HBITMAP CreateDIBSection(HDC, const void *infoRaw, UINT, VOID **pixels, HANDLE, DWORD)
{
    const BITMAPINFO *info = static_cast<const BITMAPINFO *>(infoRaw); GdiBitmap *bitmap = new GdiBitmap(info->bmiHeader.biWidth, info->bmiHeader.biHeight, info->bmiHeader.biBitCount);
    *pixels = bitmap->pixels.empty() ? NULL : &bitmap->pixels[0]; return bitmap;
}
BOOL TextOutA(HDC dcRaw, int x, int y, LPCSTR text, int length)
{
    if (dcRaw == NULL || text == NULL || length <= 0) return FALSE;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    if (dc->bitmap == NULL || dc->font == NULL || dc->font->font == NULL) return FALSE;
    std::string utf8 = ConvertCp932ToUtf8(text, static_cast<size_t>(length));
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *rendered = TTF_RenderUTF8_Blended(dc->font->font, utf8.c_str(), white);
    if (rendered == NULL) return FALSE;
    SDL_Surface *glyph = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(rendered);
    if (glyph == NULL) return FALSE;
    if (SDL_MUSTLOCK(glyph)) SDL_LockSurface(glyph);
    for (int row = 0; row < glyph->h; ++row)
    {
        const BYTE *source = static_cast<const BYTE *>(glyph->pixels) + row * glyph->pitch;
        for (int column = 0; column < glyph->w; ++column)
            PutGdiTextPixel(dc->bitmap, x + column, y + row, dc->color, source[column * 4 + 3]);
    }
    if (SDL_MUSTLOCK(glyph)) SDL_UnlockSurface(glyph);
    SDL_FreeSurface(glyph);
    return TRUE;
}
HRESULT CoInitialize(LPVOID) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFGUID, LPVOID, DWORD, REFIID, LPVOID *) { return E_NOTIMPL; }
} // extern C

extern const GUID CLSID_ShellLink = {0};
extern const GUID IID_IShellLink = {1};
extern const GUID IID_IPersistFile = {2};
const GUID GUID_NULL = {0};
const GUID IID_IDirectSoundNotify = {3};
const GUID IID_IDirectInput8A = {4};
const GUID GUID_SysKeyboard = {5};
const GUID DIPROP_RANGE = {6};
const DIDATAFORMAT c_dfDIKeyboard = {sizeof(DIDATAFORMAT)};
const DIDATAFORMAT c_dfDIJoystick = {sizeof(DIDATAFORMAT)};

MMRESULT timeGetDevCaps(TIMECAPS *caps, UINT) { caps->wPeriodMin = 1; caps->wPeriodMax = 1000; return 0; }
MMRESULT timeBeginPeriod(UINT) { return 0; }
MMRESULT timeEndPeriod(UINT) { return 0; }
UINT timeSetEvent(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT) { static UINT id = 1; return id++; }
MMRESULT timeKillEvent(UINT) { return 0; }
MMRESULT midiOutOpen(HMIDIOUT *handle, UINT, DWORD_PTR, DWORD_PTR, DWORD) { *handle = reinterpret_cast<HMIDIOUT>(1); return 0; }
MMRESULT midiOutClose(HMIDIOUT) { return 0; }
MMRESULT midiOutReset(HMIDIOUT) { return 0; }
MMRESULT midiOutPrepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutUnprepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutLongMsg(HMIDIOUT, LPMIDIHDR header, UINT) { header->dwFlags |= 1; return 0; }
MMRESULT midiOutShortMsg(HMIDIOUT, DWORD) { return 0; }
MMRESULT joyGetPosEx(UINT, JOYINFOEX *) { return 1; }
MMRESULT joyGetDevCapsA(UINT_PTR, JOYCAPSA *caps, UINT) { memset(caps, 0, sizeof(*caps)); caps->wXmax = caps->wYmax = 65535; return 1; }

class SwitchInputDevice : public IDirectInputDevice8A
{
  public:
    explicit SwitchInputDevice(bool keyboard_) : refs(1), keyboard(keyboard_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT GetCapabilities(DIDEVCAPS *caps) { caps->dwAxes = keyboard ? 0 : 2; caps->dwButtons = keyboard ? 0 : 32; return S_OK; }
    HRESULT EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) { return S_OK; }
    HRESULT GetDeviceState(DWORD size, LPVOID data)
    {
        if (keyboard && size >= 256) FillKeyboard(static_cast<BYTE *>(data), true);
        else memset(data, 0, size); return S_OK;
    }
    HRESULT SetDataFormat(const DIDATAFORMAT *) { return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
    HRESULT SetProperty(REFGUID, const DIPROPHEADER *) { return S_OK; }
    HRESULT Acquire() { return S_OK; }
    HRESULT Unacquire() { return S_OK; }
    HRESULT Poll() { return S_OK; }
  private:
    ULONG refs; bool keyboard;
};

class SwitchDirectInput : public IDirectInput8A
{
  public:
    SwitchDirectInput() : refs(1) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateDevice(REFGUID guid, IDirectInputDevice8A **device, LPVOID)
    { *device = new SwitchInputDevice(guid.Data1 == GUID_SysKeyboard.Data1); return S_OK; }
    HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *out, LPVOID) { *out = new SwitchDirectInput(); return S_OK; }

class SwitchSoundBuffer;

SDL_AudioDeviceID g_audioDevice;
std::vector<SwitchSoundBuffer *> g_soundBuffers;

void LockAudio()
{
    if (g_audioDevice != 0) SDL_LockAudioDevice(g_audioDevice);
}

void UnlockAudio()
{
    if (g_audioDevice != 0) SDL_UnlockAudioDevice(g_audioDevice);
}

class SwitchSoundNotify : public IDirectSoundNotify
{
  public:
    explicit SwitchSoundNotify(SwitchSoundBuffer *buffer_) : refs(1), buffer(buffer_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions);
  private: ULONG refs; SwitchSoundBuffer *buffer;
};

class SwitchSoundBuffer : public IDirectSoundBuffer
{
  public:
    explicit SwitchSoundBuffer(const DSBUFFERDESC *desc)
        : refs(1), playing(false), looping(false), position(0), cursorFrame(0.0), volume(0), pan(0),
          hasFormat(false), locked(false)
    {
        memset(&format, 0, sizeof(format));
        if (desc != NULL)
        {
            bytes.resize(desc->dwBufferBytes);
            if (desc->lpwfxFormat != NULL) { format = *desc->lpwfxFormat; hasFormat = true; }
        }
        LockAudio(); g_soundBuffers.push_back(this); UnlockAudio();
    }
    SwitchSoundBuffer(const SwitchSoundBuffer &other)
        : refs(1), bytes(other.bytes), playing(false), looping(false), position(0), cursorFrame(0.0),
          volume(other.volume), pan(other.pan), format(other.format), hasFormat(other.hasFormat), locked(false)
    { LockAudio(); g_soundBuffers.push_back(this); UnlockAudio(); }
    ~SwitchSoundBuffer()
    {
        LockAudio();
        for (std::vector<SwitchSoundBuffer *>::iterator it = g_soundBuffers.begin(); it != g_soundBuffers.end(); ++it)
            if (*it == this) { g_soundBuffers.erase(it); break; }
        UnlockAudio();
    }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT QueryInterface(REFIID, void **out) { *out = new SwitchSoundNotify(this); return S_OK; }
    HRESULT GetCurrentPosition(LPDWORD play, LPDWORD write)
    {
        LockAudio();
        if (play) *play = position;
        if (write) *write = position;
        UnlockAudio();
        return S_OK;
    }
    HRESULT GetStatus(LPDWORD status)
    { LockAudio(); *status = playing ? DSBSTATUS_PLAYING : 0; UnlockAudio(); return S_OK; }
    HRESULT Initialize(void *, const DSBUFFERDESC *) { return S_OK; }
    HRESULT Lock(DWORD offset, DWORD length, LPVOID *first, LPDWORD firstSize, LPVOID *second, LPDWORD secondSize, DWORD)
    {
        LockAudio(); locked = true;
        if (bytes.empty()) bytes.resize(length ? length : 1); offset %= bytes.size(); if (length == 0 || length > bytes.size()) length = bytes.size();
        DWORD contiguous = static_cast<DWORD>(bytes.size() - offset); if (contiguous > length) contiguous = length;
        *first = &bytes[offset]; *firstSize = contiguous; if (second) *second = length > contiguous ? &bytes[0] : NULL; if (secondSize) *secondSize = length - contiguous; return S_OK;
    }
    HRESULT Play(DWORD, DWORD, DWORD flags)
    { LockAudio(); playing = true; looping = (flags & DSBPLAY_LOOPING) != 0; UnlockAudio(); return S_OK; }
    HRESULT SetCurrentPosition(DWORD value)
    {
        LockAudio();
        position = bytes.empty() ? 0 : value % bytes.size();
        cursorFrame = FrameBytes() != 0 ? static_cast<double>(position / FrameBytes()) : 0.0;
        UnlockAudio();
        return S_OK;
    }
    HRESULT SetFormat(const WAVEFORMATEX *value)
    { if (value != NULL) { LockAudio(); format = *value; hasFormat = true; UnlockAudio(); } return S_OK; }
    HRESULT SetVolume(LONG value) { LockAudio(); volume = value; UnlockAudio(); return S_OK; }
    HRESULT SetPan(LONG value) { LockAudio(); pan = value; UnlockAudio(); return S_OK; }
    HRESULT Stop() { LockAudio(); playing = false; UnlockAudio(); return S_OK; }
    HRESULT Unlock(LPVOID, DWORD, LPVOID, DWORD)
    { if (locked) { locked = false; UnlockAudio(); } return S_OK; }
    HRESULT Restore() { return S_OK; }
    void SetNotifications(DWORD count, const DSBPOSITIONNOTIFY *positions)
    {
        LockAudio();
        if (count == 0)
            notifications.clear();
        else
            notifications.assign(positions, positions + count);
        UnlockAudio();
    }
    void Mix(Sint16 *output, int outputFrames)
    {
        const DWORD frameBytes = FrameBytes();
        if (!playing || !hasFormat || bytes.empty() || frameBytes == 0 || format.wFormatTag != WAVE_FORMAT_PCM)
            return;
        const DWORD sourceFrames = static_cast<DWORD>(bytes.size() / frameBytes);
        if (sourceFrames == 0) return;
        const double step = static_cast<double>(format.nSamplesPerSec) / 44100.0;
        const float gain = volume <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, static_cast<float>(volume) / 2000.0f);
        const float panValue = pan < -10000 ? -1.0f : pan > 10000 ? 1.0f : static_cast<float>(pan) / 10000.0f;
        const float leftGain = gain * (panValue > 0.0f ? 1.0f - panValue : 1.0f);
        const float rightGain = gain * (panValue < 0.0f ? 1.0f + panValue : 1.0f);
        const DWORD oldPosition = position;
        bool wrapped = false;

        for (int frame = 0; frame < outputFrames && playing; ++frame)
        {
            DWORD sourceFrame = static_cast<DWORD>(cursorFrame);
            if (sourceFrame >= sourceFrames)
            {
                if (!looping) { playing = false; break; }
                cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
            }
            const BYTE *source = &bytes[sourceFrame * frameBytes];
            int left, right;
            if (format.wBitsPerSample == 8)
            {
                left = (static_cast<int>(source[0]) - 128) << 8;
                right = format.nChannels > 1 ? (static_cast<int>(source[1]) - 128) << 8 : left;
            }
            else if (format.wBitsPerSample == 16)
            {
                INT16 leftSample, rightSample;
                memcpy(&leftSample, source, sizeof(leftSample));
                if (format.nChannels > 1) memcpy(&rightSample, source + sizeof(INT16), sizeof(rightSample));
                else rightSample = leftSample;
                left = leftSample; right = rightSample;
            }
            else
                break;

            int mixedLeft = output[frame * 2] + static_cast<int>(left * leftGain);
            int mixedRight = output[frame * 2 + 1] + static_cast<int>(right * rightGain);
            if (mixedLeft < -32768) mixedLeft = -32768; else if (mixedLeft > 32767) mixedLeft = 32767;
            if (mixedRight < -32768) mixedRight = -32768; else if (mixedRight > 32767) mixedRight = 32767;
            output[frame * 2] = static_cast<Sint16>(mixedLeft);
            output[frame * 2 + 1] = static_cast<Sint16>(mixedRight);
            cursorFrame += step;
        }

        if (cursorFrame >= sourceFrames)
        {
            if (looping) { cursorFrame = fmod(cursorFrame, static_cast<double>(sourceFrames)); wrapped = true; }
            else { cursorFrame = sourceFrames; playing = false; }
        }
        position = static_cast<DWORD>(cursorFrame) * frameBytes;
        for (size_t index = 0; index < notifications.size(); ++index)
        {
            const DWORD offset = notifications[index].dwOffset;
            if ((!wrapped && oldPosition <= offset && position > offset) ||
                (wrapped && (offset >= oldPosition || offset < position)))
                SetEvent(notifications[index].hEventNotify);
        }
    }
  private:
    DWORD FrameBytes() const
    { return hasFormat && format.nBlockAlign != 0 ? format.nBlockAlign : 0; }
    ULONG refs;
    std::vector<BYTE> bytes;
    bool playing, looping;
    DWORD position;
    double cursorFrame;
    LONG volume, pan;
    WAVEFORMATEX format;
    bool hasFormat, locked;
    std::vector<DSBPOSITIONNOTIFY> notifications;
};

HRESULT SwitchSoundNotify::SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions)
{ if (buffer == NULL || (count != 0 && positions == NULL)) return E_INVALIDARG; buffer->SetNotifications(count, positions); return S_OK; }

void AudioCallback(void *, Uint8 *stream, int length)
{
    memset(stream, 0, length);
    Sint16 *output = reinterpret_cast<Sint16 *>(stream);
    const int frames = length / (sizeof(Sint16) * 2);
    for (size_t index = 0; index < g_soundBuffers.size(); ++index)
        g_soundBuffers[index]->Mix(output, frames);
}

void EnsureAudio()
{
    if (g_audioDevice != 0) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    { fprintf(stderr, "th08-switch: SDL audio initialization failed: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec requested, obtained;
    memset(&requested, 0, sizeof(requested));
    requested.freq = 44100; requested.format = AUDIO_S16SYS; requested.channels = 2;
    requested.samples = 1024; requested.callback = AudioCallback;
    g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &requested, &obtained, 0);
    if (g_audioDevice == 0)
    { fprintf(stderr, "th08-switch: SDL audio device unavailable: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_audioDevice, 0);
}

void ShutdownAudio()
{
    if (g_audioDevice == 0) return;
    SDL_CloseAudioDevice(g_audioDevice); g_audioDevice = 0;
}

class SwitchDirectSound : public IDirectSound8
{
  public:
    SwitchDirectSound() : refs(1) { EnsureAudio(); }
    ~SwitchDirectSound() { ShutdownAudio(); }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, IDirectSoundBuffer **out, LPVOID) { *out = new SwitchSoundBuffer(desc); return S_OK; }
    HRESULT DuplicateSoundBuffer(IDirectSoundBuffer *source, IDirectSoundBuffer **out) { *out = new SwitchSoundBuffer(*static_cast<SwitchSoundBuffer *>(source)); return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectSoundCreate8(const GUID *, LPDIRECTSOUND8 *out, LPVOID) { *out = new SwitchDirectSound(); return S_OK; }
