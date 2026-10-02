#############################################################################
# Set up environment
< envPaths
epicsEnvSet("P","$(P=CaenV965Test:)")
cd "$(TOP)"

#############################################################################
# Register all support components
dbLoadDatabase("dbd/caenV965Test.dbd")
caenV965Test_registerRecordDeviceDriver(pdbbase)

#############################################################################
# Configure Hardware
CaenV965Configure("QDC0", "V965A", 0x020000, 192, 5)

#############################################################################
# Load record instances
dbLoadRecords("db/devCaenV965.db", "P=$(P),R=CaenV965:,PORT=QDC0")
dbLoadRecords("db/asynRecord.db", "P=$(P),R=asyn,PORT=QDC0,ADDR=2000,OMAX=0,IMAX=0")

#############################################################################
# Start EPICS
iocInit
