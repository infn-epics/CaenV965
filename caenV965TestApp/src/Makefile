TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

PROD_IOC_RTEMS = caenV965Test
# caenV965Test.dbd will be created and installed
DBD += caenV965Test.dbd

# caenV965Test.dbd will be made up from these files:
caenV965Test_DBD += base.dbd

# Include dbd files from all support applications:
caenV965Test_DBD += devCaenV965.dbd

# Add all the support libraries needed by this IOC
caenV965Test_LIBS += devCaenV965
caenV965Test_LIBS += asyn

# caenV965Test_registerRecordDeviceDriver.cpp derives from caenV965Test.dbd
caenV965Test_SRCS += caenV965Test_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
caenV965Test_SRCS_DEFAULT += caenV965TestMain.cpp
caenV965Test_SRCS_vxWorks += -nil-

# Add support from base/src/vxWorks if needed
#caenV965Test_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
caenV965Test_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

