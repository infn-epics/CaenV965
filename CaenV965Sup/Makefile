TOP=..
include $(TOP)/configure/CONFIG
#=======================================

# Build devCaenV965 as a library for an IOC:
LIBRARY_IOC_RTEMS += devCaenV965

# Library Source files
devCaenV965_SRCS += devCaenV965.c

# Link with the asyn and base libraries
devCaenV965_LIBS += asyn
devCaenV965_LIBS += $(EPICS_BASE_IOC_LIBS)

# Install .dbd and .db files
DBD += devCaenV965.dbd
DB += devCaenV965.db

#=======================================
include $(TOP)/configure/RULES
