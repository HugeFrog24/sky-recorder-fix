#pragma once
#include <cstddef>
#include <cstring>

static bool module_package(const char *path, char *package, size_t capacity) {
    if (!path || !package || !capacity) {
        return false;
    }
    package[0] = 0;
    const char *start = nullptr;
    if (!strncmp(path, "/data/data/", 11)) {
        start = path + 11;
    } else {
        const char *user = nullptr;
        if (!strncmp(path, "/data/user/", 11)) {
            user = path + 11;
        } else if (!strncmp(path, "/data/user_de/", 14)) {
            user = path + 14;
        }
        if (!user || *user < '0' || *user > '9') {
            return false;
        }
        while (*user >= '0' && *user <= '9') {
            ++user;
        }
        if (*user != '/') {
            return false;
        }
        start = user + 1;
    }
    const char *end = strchr(start, '/');
    if (!end || strncmp(end, "/files/mods/", 12) != 0 || !end[12]) {
        return false;
    }
    size_t const length = size_t(end - start);
    if (!length || length >= capacity || start[0] == '.' || end[-1] == '.') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        char const c = start[i];
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') && c != '_' && c != '.') {
            return false;
        }
        if (c == '.' && i && start[i - 1] == '.') {
            return false;
        }
    }
    memcpy(package, start, length);
    package[length] = 0;
    return true;
}
