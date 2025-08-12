#ifndef QEMU_PATH_H
#define QEMU_PATH_H

void init_paths(const char *prefix);
void init_paths_nb(const char *prefix);
const char *path(const char *pathname);

#endif
