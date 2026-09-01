// direct.h (switch) — перехват _mkdir/_chdir/rename из кода игры.
// Этот каталог include стоит ПЕРЕД src/modern/linux/include, поэтому именно
// эта версия достаётся Supervisor.cpp/TitleScreen.cpp/ResultScreen.cpp.
// Всё, что сюда падает, — относительные пути, их надо переводить в
// <папка данных>/[<виртуальный cwd>/]путь и заворачивать chdir в виртуальный cwd.
#pragma once

#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

int th08_switch_mkdir(const char *path);
int th08_switch_chdir(const char *path);
int th08_switch_rename(const char *oldPath, const char *newPath);

#ifdef __cplusplus
}
#endif

#define _mkdir(path) th08_switch_mkdir(path)
#define _chdir(path) th08_switch_chdir(path)
// Supervisor.cpp делает rename(findData.cFileName, fileNameBuffer) с ОТНОСИТЕЛЬНЫМИ
// именами из виртуального cwd ("backup/") — перехватываем, иначе newlib уйдёт в
// реальный корень.
#define rename(oldPath, newPath) th08_switch_rename(oldPath, newPath)
