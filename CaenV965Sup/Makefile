TOP=..
include $(TOP)/configure/CONFIG
#=======================================

# Build devCaenV965 as a library for an IOC:
# VME targets only (devLib VME support)
LIBRARY_IOC_RTEMS += devCaenV965
LIBRARY_IOC_vxWorks += devCaenV965

# Library Source files
devCaenV965_SRCS += devCaenV965.c

# Link with the asyn and base libraries
devCaenV965_LIBS += asyn
devCaenV965_LIBS += $(EPICS_BASE_IOC_LIBS)

# Install .dbd and .db files
DBD += devCaenV965.dbd
DB += devCaenV965.db
DB += devCaenV792.db
DB += perCard.template perChannel.template

#=======================================
include $(TOP)/configure/RULES
