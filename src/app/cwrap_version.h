#ifndef CWRAP_VERSION_H
#define CWRAP_VERSION_H

#include "shared/str.h"

/**
 * @brief Returns the version string fixed when the build tree was configured.
 *
 * @return The non-empty, statically allocated version string.
 */
struct Str cwrap_version_string(void);

#endif
