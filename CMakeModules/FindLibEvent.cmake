# Finding LibEvent (https://libevent.org/)
# Defining:
# LIBEVENT_LIB
# LIBEVENT_INCLUDE_DIR

FIND_PATH(LIBEVENT_INCLUDE_DIR event2/event.h PATH_SUFFIXES include HINTS ${ADDITIONAL_LIBRARY_PATHS})

FIND_LIBRARY(LIBEVENT_LIB NAMES event_core PATH_SUFFIXES lib HINTS ${ADDITIONAL_LIBRARY_PATHS})

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBEVENT DEFAULT_MSG
	LIBEVENT_INCLUDE_DIR LIBEVENT_LIB)