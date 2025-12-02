#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "file_manager.h"

// Save file registry to disk
int save_file_registry(const char *filename);

// Load file registry from disk
int load_file_registry(const char *filename);

#endif // PERSISTENCE_H
