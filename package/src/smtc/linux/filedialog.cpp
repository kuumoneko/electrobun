#include "tinyfiledialogs/tinyfiledialogs.h"
#include <cstring>
#include <cstdio>

static char g_result[4096] = {};

extern "C" const char* open_folder_dialog(const char* startingFolder)
{
    g_result[0] = '\0';

    const char *defaultPath = (startingFolder && startingFolder[0]) ? startingFolder : nullptr;

    char *selected = tinyfd_selectFolderDialog("Select Folder", defaultPath);
    if (!selected || !selected[0])
        return nullptr;

    size_t len = strlen(selected);
    if (len >= sizeof(g_result))
        len = sizeof(g_result) - 1;

    memcpy(g_result, selected, len);
    g_result[len] = '\0';

    return g_result;
}
