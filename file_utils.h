#ifndef RUNWHENIDLE_FILE_UTILS_H
#define RUNWHENIDLE_FILE_UTILS_H

#include <stdio.h>
int file_is_socket(const char *path);
int file_is_readable_regular_file(const char *path);
int directory_exists_and_accessible(const char *path);
int get_home_directory_for_current_user(char *out_home, size_t out_home_size);

#endif