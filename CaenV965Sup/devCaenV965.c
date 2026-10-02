/*
 * CaenV965 device support
 *
 * Author: W. Eric Norum
 * "2011/02/22 17:11:35 (UTC)"
 *
 * INFN changes:
 *  - register access with epicsMMIO native accessors instead of the RTEMS
 *    PowerPC in_be/out_be macros: correct on big endian CPUs and on boards
 *    with hardware VME byte swapping (e.g. VMIVME-7750, RTEMS-pc686)
 *  - A24 or A32 base address (A32 if above 0xFFFFFF)
 *  - CAEN V792 (32 channel QDC, single range)
 *  - event counter on its own asyn address (A_EVENT_COUNT), still also on
 *    address 16 for 16 channel cards
 */

/************************************************************************\
* Copyright (c) 2011 Lawrence Berkeley National Laboratory, Accelerator
* Technology Group, Engineering Division
* This code is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <epicsStdio.h>
#include <epicsString.h>
#include <epicsEvent.h>
#include <epicsThread.h>
#include <epicsMessageQueue.h>
#include <epicsMutex.h>
#include <epicsInterrupt.h>
#include <epicsExport.h>
#include <cantProceed.h>
#include <iocsh.h>
#include <errlog.h>
#include <devLib.h>
#include <asynDriver.h>
#include <asynInt32.h>
#include <epicsMMIO.h>

#define MAX_CHANNEL_COUNT   32
#define MESSAGE_QUEUE_COUNT ((2 * MAX_CHANNEL_COUNT) + 2)

/*
 * ASYN subaddress assignments
 */
#define A_FIRMWARE_REVISION     1000
#define A_SERIAL_NUMBER         1001
#define A_VERSION               1002
#define A_MOTHERBOARD_REVISION  1003
#define A_PIGGYBACK_REVISION    1004
#define A_PEDESTAL_CURRENT      1008
#define A_SOFTWARE_TRIGGER      1009
#define A_EVENT_COUNT           1005
#define A_EVENT_COUNT_V965      16    /* compatibility with CaenV965 1.0 */
#define A_DEBUGGING             2000

/*
 * Multi-event buffer bits
 * Don't bother with crate or geographic addressing!
 */
#define MEB_OPCODE_MASK                0x07000000
#define   MEB_OPCODE_DATA              0x00000000
#define   MEB_OPCODE_START             0x02000000
#define   MEB_OPCODE_END               0x04000000
#define   MEB_OPCODE_EMPTY             0x06000000
#define MEB_COUNT_SHIFT                8
#define MEB_SHIFTED_COUNT_MASK         0x3F
#define MEB_CHANNEL_SHIFT_V965         17
#define MEB_SHIFTED_CHANNEL_MASK_V965  0xF
#define MEB_RANGE_MASK_V965            0x00010000
#define MEB_CHANNEL_SHIFT_V965A        18
#define MEB_SHIFTED_CHANNEL_MASK_V965A 0x7
#define MEB_RANGE_MASK_V965A           0x00020000
#define MEB_CHANNEL_SHIFT_V792         16
#define MEB_SHIFTED_CHANNEL_MASK_V792  0x1F
#define MEB_UNDERFLOW_MASK             0x00002000
#define MEB_OVERFLOW_MASK              0x00001000
#define MEB_DATA_MASK                  0x00000FFF
#define MEB_EVENT_COUNTER_MASK         0x00FFFFFF

/*
 * Register offsets
 */
#define R_MULTI_EVENT_BUFFER        0x0000
#define R_FIRMWARE_REVISION         0x1000
#define R_BITSET1                   0x1006
#define R_BITCLEAR1                 0x1008
#define R_INTERRUPT_LEVEL           0x100A
#define R_STATUS1                   0x100E
#define R_INTERRUPT_VECTOR          0x100C
#define R_EVENT_TRIGGER             0x1020
#define R_STATUS2                   0x1022
#define R_BITSET2                   0x1032
#define R_BITCLEAR2                 0x1034
#define R_PEDESTAL_CURRENT          0x1060
#define R_SOFTWARE_TRIGGER          0x1068
#define R_SLIDE_CONSTANT            0x106A
#define R_VERSION                   0x8032
#define R_MOTHERBOARD_REVISION      0x804E
#define R_PIGGYBACK_REVISION        0x8052
#define R_SERIAL_NUMBER_MSB         0x8F02
#define R_SERIAL_NUMBER_LSB         0x8F06

/*
 * Set/Clear register bits
 */
#define B1_SOFTWARE_RESET            0x0080
#define B2_OVERRANGE_ENABLE          0x0008
#define B2_SLIDE_ENABLE              0x0080

struct dpvt {
    epicsUInt32             vmeAddress;
    epicsAddressType        addressType;
    char                   *baseAddress;   /* Register base in CPU space */
    enum                    cardType { v965, v965a, v792 } cardType;
    int                     channelCount;
    int                     channelShift;
    int                     shiftedChannelMask;
    epicsUInt32             rangeMask;

    const char             *portName;
    asynInterface           asynCommon;
    asynInterface           asynInt32;
    void                   *asynInt32InterruptPvt;
    asynUser               *debugAsynUser;

    epicsMessageQueueId     interruptMessageQueue;

    unsigned long           messageBufferOverflowCount;
    unsigned long           messageQueueOverflowCount;
    unsigned long           interruptCount;
};

/*
 * Low-level register access
 * All I/O is performed through these routines
 */
static epicsUInt16
get16(struct dpvt *dpvt, int offset)
{
    return nat_ioread16(dpvt->baseAddress + offset);
}
static void
put16(struct dpvt *dpvt, int offset, epicsUInt16 value)
{
    nat_iowrite16(dpvt->baseAddress + offset, value);
}
static epicsUInt32
get32(struct dpvt *dpvt, int offset)
{
    return nat_ioread32(dpvt->baseAddress + offset);
}

/*
 * Handle weird mapping of threshold registers
 */
static
int highRangeThresholdRegister(struct dpvt *dpvt, int channel)
{
    /* V792: one threshold per channel, 0x1080..0x10BF */
    if (dpvt->cardType == v792) return 0x1080+(2*channel);
    if (dpvt->cardType == v965) return 0x1080+(4*channel);
    else                        return 0x1080+(8*channel);
}
static
int lowRangeThresholdRegister(struct dpvt *dpvt, int channel)
{
    /* single range cards have no low range threshold */
    if (dpvt->cardType == v792) return -1;
    if (dpvt->cardType == v965) return 0x1082+(4*channel);
    else                        return 0x1084+(8*channel);
}

/*
 * Interrupt handler
 * Minimize processing here by sending all values up to the handler thread
 * even though only one of the possible two values per channel is going to
 * be used.
 */
static void
interruptHandler(void *arg)
{
    struct dpvt *dpvt = (struct dpvt *)arg;
    epicsUInt32 v;
    epicsUInt32 qbuf[MESSAGE_QUEUE_COUNT];
    int i = 0;

//printk("\r\n=== ");
    dpvt->interruptCount++;
    do {
        v = get32(dpvt, R_MULTI_EVENT_BUFFER);
        if (i < MESSAGE_QUEUE_COUNT) {
            qbuf[i++] = v;
        }
        else {
//printk("\r\n=== CAEN V965 readout lockup -- disabling interrupts\r\n");
            epicsInterruptContextMessage("CAEN V965 readout lockup -- disabling interrupts");
            put16(dpvt, R_INTERRUPT_LEVEL, 0);
            break;
        }
    } while ((v & MEB_OPCODE_MASK) != MEB_OPCODE_END);
    if (i <= MESSAGE_QUEUE_COUNT) {
        if (epicsMessageQueueTrySend(dpvt->interruptMessageQueue,
                                     qbuf,
                                     i * sizeof(qbuf[0])) != 0)
            dpvt->messageQueueOverflowCount++;
    }
    else {
        dpvt->messageBufferOverflowCount++;
    }
}

/*
 * Thread-level interrupt handling
 */
static void
handlerThread(void *arg)
{
    struct dpvt *dpvt = (struct dpvt *)arg;
    int i, n;
    epicsUInt32 v;
    epicsUInt32 qbuf[MESSAGE_QUEUE_COUNT];
    epicsUInt32 hiRangeRaw[MAX_CHANNEL_COUNT];
    epicsUInt32 lowRangeRaw[MAX_CHANNEL_COUNT];
    epicsUInt32 eventCount;
    epicsUInt32 haveLoRangeRaw, haveHiRangeRaw;
    int haveEventCount;
    ELLLIST *pclientList;
    interruptNode *pnode;


    for (;;) {
        n = epicsMessageQueueReceive(dpvt->interruptMessageQueue, qbuf, sizeof qbuf);
//printk(" %d",n);
        if ((n % sizeof qbuf[0]) != 0) {
            errlogPrintf("CAEN V965 handler thread -- received %d-byte message!\n", n);
            continue;
        }
        n /= sizeof qbuf[0];
        haveLoRangeRaw = haveHiRangeRaw = haveEventCount = 0;
        for (i = 0 ; i < n ; i++) {
            int channel;
            v = qbuf[i];
            switch (v & MEB_OPCODE_MASK) {
            case MEB_OPCODE_START:
                asynPrint(dpvt->debugAsynUser, ASYN_TRACEIO_DRIVER,
                                            "%s: START -- COUNT %d\n",
                                            dpvt->portName, (v >> 8) & 0x3F);
                break;  /* the header is not a data word (was a fall through) */

            case MEB_OPCODE_DATA:
                channel = (v >> dpvt->channelShift) & dpvt->shiftedChannelMask;
                if (v & dpvt->rangeMask) {
                    lowRangeRaw[channel] = v;
                    haveLoRangeRaw |= (1u << channel);
                }
                else {
                    hiRangeRaw[channel] = v;
                    haveHiRangeRaw |= (1u << channel);
                }
                asynPrint(dpvt->debugAsynUser, ASYN_TRACEIO_DRIVER,
                                    "%s: %8.8X CHAN %d %s%s%s: %d\n",
                                    dpvt->portName, v, channel,
                                    v & dpvt->rangeMask ? "LO" : "HI",
                                    v & MEB_UNDERFLOW_MASK ? " Underflow" : "",
                                    v & MEB_OVERFLOW_MASK ? " Overflow" : "",
                                    v & MEB_DATA_MASK);
            break;

            case MEB_OPCODE_END:
                eventCount = v & MEB_EVENT_COUNTER_MASK;
                haveEventCount = 1;
                asynPrint(dpvt->debugAsynUser, ASYN_TRACEIO_DRIVER,
                                                "%s: END -- EVENT COUNT %d\n",
                                                dpvt->portName, eventCount);
                break;
            }
        }

        /*
         * Push values into records
         */
        pasynManager->interruptStart(dpvt->asynInt32InterruptPvt, &pclientList);
        pnode = (interruptNode *)ellFirst(pclientList);
        while (pnode) {
            asynInt32Interrupt *pint32Interrupt = pnode->drvPvt;
            unsigned int addr = pint32Interrupt->addr;
            epicsUInt32 bit = (addr < 32) ? (1u << addr) : 0;
            if ((addr < (unsigned int) dpvt->channelCount)
             && ((haveHiRangeRaw & bit) || (haveLoRangeRaw & bit))) {
                int value = 32768;
                if ((haveLoRangeRaw & bit)
                 && (((v = lowRangeRaw[addr]) & MEB_OVERFLOW_MASK) == 0)) {
                    value = v & MEB_DATA_MASK;
                }
                else if ((haveHiRangeRaw & bit)
                      && (((v = hiRangeRaw[addr]) & MEB_OVERFLOW_MASK) == 0)) {
                    value = (v & MEB_DATA_MASK);
                    if (dpvt->rangeMask)
                        value *= 8;   /* V965 high range is 8 times coarser */
                }
                pint32Interrupt->callback(pint32Interrupt->userPvt,
                                          pint32Interrupt->pasynUser,
                                          value);
            }
            else if (((addr == A_EVENT_COUNT)
                   || ((addr == A_EVENT_COUNT_V965) && (dpvt->channelCount <= 16)))
                  && haveEventCount) {
                pint32Interrupt->callback(pint32Interrupt->userPvt,
                                          pint32Interrupt->pasynUser,
                                          eventCount);
            }
            pnode = (interruptNode *)ellNext(&pnode->node);
        }
        pasynManager->interruptEnd(dpvt->asynInt32InterruptPvt);
    }
}

/*
 * asynCommon methods
 */
static asynStatus
caenConnect(void *drvPvt, asynUser *pasynUser)
{
    pasynManager->exceptionConnect(pasynUser);
    return asynSuccess;
}
static asynStatus
caenDisconnect(void *drvPvt, asynUser *pasynUser)
{
    pasynManager->exceptionDisconnect(pasynUser);
    return asynSuccess;
}

static void
showCount( FILE *fp, const char *name, unsigned long value)
{
    if (value)
        fprintf(fp, "%25s count: %lu\n", name, value);
}

void
report(void *drvPvt, FILE *fp, int details)
{
    struct dpvt *dpvt = (struct dpvt *)drvPvt;
    int i;

    fprintf(fp, "Port:%s  %s address:%#X  CPU space address:%p  V%s\n",
                                    dpvt->portName,
                                    dpvt->addressType == atVMEA32 ? "A32" : "A24",
                                    (unsigned) dpvt->vmeAddress,
                                    dpvt->baseAddress,
                                    dpvt->cardType == v792 ? "792" :
                                    dpvt->cardType == v965a ? "965A" : "965");
    if (details >= 1) {
        showCount (fp, "Message buffer overflow", dpvt->messageBufferOverflowCount);
        showCount (fp, "Message queue overflow", dpvt->messageQueueOverflowCount);
        showCount (fp, "Interrupt", dpvt->interruptCount);
    }
    if (details >= 2) {
            fprintf(fp, "           R_STATUS1: %#x\n", get16(dpvt, R_STATUS1));
            fprintf(fp, "           R_STATUS2: %#x\n", get16(dpvt, R_STATUS2));
            fprintf(fp, "           R_BITSET1: %#x\n", get16(dpvt, R_BITSET1));
            fprintf(fp, "           R_BITSET2: %#x\n", get16(dpvt, R_BITSET2));
            fprintf(fp, "  R_PEDESTAL_CURRENT: %d\n", get16(dpvt, R_PEDESTAL_CURRENT));
    }
    if (details >= 3) {
            fprintf(fp, "   R_INTERRUPT_LEVEL: %d\n", get16(dpvt, R_INTERRUPT_LEVEL) & 0x7);
            fprintf(fp, "  R_INTERRUPT_VECTOR: %d\n", get16(dpvt, R_INTERRUPT_VECTOR));
        fprintf(fp, "             Thresholds\n");
        fprintf(fp, "   Chan  Lo-range  Hi-range\n");
        for (i = 0 ; i < dpvt->channelCount ; i++) {
            if (dpvt->rangeMask)
                fprintf(fp, "    %3d    %#6.4X    %#6.4X\n", i,
                            get16(dpvt, lowRangeThresholdRegister(dpvt, i)),
                            get16(dpvt, highRangeThresholdRegister(dpvt, i)));
            else
                fprintf(fp, "    %3d         -    %#6.4X\n", i,
                            get16(dpvt, highRangeThresholdRegister(dpvt, i)));
        }
    }
}
static asynCommon asynCommonMethods = { report, caenConnect, caenDisconnect };

   
/*
 * asynInt32 methods
 */
static asynStatus
int32Read(void *drvPvt, asynUser *pasynUser, epicsInt32 *value)
{
    struct dpvt *dpvt = (struct dpvt *)drvPvt;
    int addr;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser, &addr);
    if (status != asynSuccess) {
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                                      "caenV965::int32Read Can't get address");
        return status;
    }
    switch(addr) {
    default:
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                               "caenV965::int32Read Invalid address %d", addr);
        return asynError;

    case A_FIRMWARE_REVISION:
        *value = get16(dpvt, R_FIRMWARE_REVISION) & 0xFF;
        break;

    case A_SERIAL_NUMBER:
        *value = ((get16(dpvt, R_SERIAL_NUMBER_MSB) & 0xFF) << 8) |
                  (get16(dpvt, R_SERIAL_NUMBER_LSB) & 0xFF);
        break;

    case A_VERSION:
        *value = get16(dpvt, R_VERSION) & 0xFF;
        break;

    case A_MOTHERBOARD_REVISION:
        *value = get16(dpvt, R_MOTHERBOARD_REVISION) & 0xFF;
        break;

    case A_PIGGYBACK_REVISION:
        *value = get16(dpvt, R_PIGGYBACK_REVISION) & 0xFF;
        break;

    case A_PEDESTAL_CURRENT:
        *value = get16(dpvt, R_PEDESTAL_CURRENT) & 0xFF;
        break;
    }
    return status;
}

static asynStatus
int32Write(void *drvPvt, asynUser *pasynUser, epicsInt32 value)
{
    struct dpvt *dpvt = (struct dpvt *)drvPvt;
    int addr;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser, &addr);
    if (status != asynSuccess) {
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                                      "caenV965::int32Write Can't get address");
        return status;
    }
    switch(addr) {
    default:
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                               "caenV965::int32Write Invalid address %d", addr);
        return asynError;

    case A_PEDESTAL_CURRENT:
        put16(dpvt, R_PEDESTAL_CURRENT, value);
        break;

    case A_SOFTWARE_TRIGGER:
        put16(dpvt, R_SOFTWARE_TRIGGER, 0);
        break;
    }
    return status;
}

static asynStatus
getBounds(void *drvPvt, asynUser *pasynUser, epicsInt32 *low, epicsInt32 *high)
{
    *low = 0;
    *high = 32768;
    return asynSuccess ;
}
static asynInt32 asynInt32Methods = { int32Write, int32Read, getBounds };

/*
 * Configure a card
 */
static void
caenV965Configure(const char *portName, const char *ct, epicsUInt32 vmeAddress, epicsInt32 vector, epicsInt32 level, unsigned int priority)
{
    struct dpvt *dpvt;
    int i;
    volatile void *addr;
    epicsUInt16 rbuf;
    asynStatus status;
    enum cardType cardType;
    epicsAddressType addressType;
    int boardId;

    /*
     * Check arguments
     */
    if (epicsStrCaseCmp(ct, "V965") == 0) cardType = v965;
    else if (epicsStrCaseCmp(ct, "V965A") == 0) cardType = v965a;
    else if (epicsStrCaseCmp(ct, "V792") == 0) cardType = v792;
    else {
        errlogPrintf("Card type must be V965, V965A or V792.\n");
        return;
    }
    if ((vmeAddress & 0xFFFF) != 0) {
        errlogPrintf("Address must be a multiple of 65536 (0x10000).\n");
        return;
    }
    /* A24 (rotary switches A23..A16) or A32 (all switches) */
    addressType = (vmeAddress > 0xFF0000) ? atVMEA32 : atVMEA24;
    if (devRegisterAddress("caenV965", addressType, vmeAddress, 65536, &addr) != 0) {
        errlogPrintf("Can't register VME address.\n");
        return;
    }
    if (devReadProbe(sizeof rbuf, (char *)addr + R_FIRMWARE_REVISION, &rbuf) != 0) {
        errlogPrintf("No device at that address.\n");
        return;
    }
    if ((vector <= 0) || (vector >= 256)) {
        errlogPrintf("Vector %d out of range.\n", vector);
        return;
    }
    if (level == 0) level = 5;
    if ((level <= 0) || (level >= 7)) {
        errlogPrintf("Level %d out of range.\n", level);
        return;
    }
    if (devEnableInterruptLevelVME(level) != 0) {
        errlogPrintf("Can't enable interrupts on that level.\n");
        return;
    }
    if (priority == 0)
        epicsThreadLowestPriorityLevelAbove(epicsThreadPriorityMedium, &priority);

    /*
     * Set up private data 
     */
    dpvt = (struct dpvt *)callocMustSucceed(1, sizeof *dpvt, "caenV965");
    dpvt->portName = epicsStrDup(portName);
    dpvt->vmeAddress = vmeAddress;
    dpvt->addressType = addressType;
    dpvt->baseAddress = (char *)addr;
    dpvt->interruptMessageQueue = epicsMessageQueueCreate(100,
                                    MESSAGE_QUEUE_COUNT * sizeof(epicsUInt32));
    dpvt->cardType = cardType;
    switch (dpvt->cardType) {
    case v965:
        dpvt->channelCount = 16;
        dpvt->channelShift = MEB_CHANNEL_SHIFT_V965;
        dpvt->shiftedChannelMask = MEB_SHIFTED_CHANNEL_MASK_V965;
        dpvt->rangeMask = MEB_RANGE_MASK_V965;
        break;
        
    case v965a:
        dpvt->channelCount = 8;
        dpvt->channelShift = MEB_CHANNEL_SHIFT_V965A;
        dpvt->shiftedChannelMask = MEB_SHIFTED_CHANNEL_MASK_V965A;
        dpvt->rangeMask = MEB_RANGE_MASK_V965A;
        break;

    case v792:
        dpvt->channelCount = 32;
        dpvt->channelShift = MEB_CHANNEL_SHIFT_V792;
        dpvt->shiftedChannelMask = MEB_SHIFTED_CHANNEL_MASK_V792;
        dpvt->rangeMask = 0;     /* single range */
        break;
    }

    /*
     * Hardware checks
     */
    if ((devReadProbe(sizeof rbuf, (char *)addr + (i = 0x8026), &rbuf) != 0)
     || ((rbuf & 0xFF) != 0x00)
     || (devReadProbe(sizeof rbuf, (char *)addr + (i = 0x802A), &rbuf) != 0)
     || ((rbuf & 0xFF) != 0x40)
     || (devReadProbe(sizeof rbuf, (char *)addr + (i = 0x802E), &rbuf) != 0)
     || ((rbuf & 0xFF) != 0xE6)
     || (devReadProbe(sizeof rbuf, (char *)addr + (i = 0x8036), &rbuf) != 0)
     || ((rbuf & 0xFF) != 0x00)
     || (devReadProbe(sizeof rbuf, (char *)addr + (i = 0x803A), &rbuf) != 0)
     || ((rbuf & 0xFF) != 0x03)
     || (devReadProbe(sizeof rbuf, (char *)addr + (i = 0x803E), &rbuf) != 0)) {
        errlogPrintf("Card at that address is not a CAEN board (offset:%#X value:%#X).\n", i, rbuf);
        return;
     }
    boardId = 0x300 | (rbuf & 0xFF);
    if (boardId != ((cardType == v792) ? 792 : 965)) {
        errlogPrintf("Card at that address is a CAEN V%d, not a %s.\n", boardId, ct);
        return;
    }

    /*
     * Register with ASYN
     */
    status = pasynManager->registerPort(portName,
                                        ASYN_MULTIDEVICE,
                                        1,  /* autoconnect */
                                        0,  /* ignored -- we don't block */
                                        0); /* default stack size */
    if (status != asynSuccess) {
        errlogPrintf("Can't register ASYN port.\n");
        return;
    }
    dpvt->asynCommon.interfaceType = asynCommonType;
    dpvt->asynCommon.pinterface  = (void *)&asynCommonMethods;
    dpvt->asynCommon.drvPvt = dpvt;
    status = pasynManager->registerInterface(portName, &dpvt->asynCommon);
    if (status != asynSuccess) {
        errlogPrintf("Can't register common.\n");
        return;
    }
    dpvt->asynInt32.interfaceType = asynInt32Type;
    dpvt->asynInt32.pinterface  = (void *)&asynInt32Methods;
    dpvt->asynInt32.drvPvt = dpvt;
    status = pasynInt32Base->initialize(dpvt->portName, &dpvt->asynInt32);
    if (status != asynSuccess) {
        errlogPrintf("Can't register int32.\n");
        return;
    }
    pasynManager->registerInterruptSource(portName,
                                         &dpvt->asynInt32,
                                         &dpvt->asynInt32InterruptPvt);
    if (epicsThreadCreate(portName,
                           priority,
                           epicsThreadGetStackSize(epicsThreadStackSmall),
                           handlerThread,
                           dpvt) == 0) {
        errlogPrintf("Can't start handler thread.\n");
        return;
    }
    if (devConnectInterruptVME(vector, interruptHandler, (void *)dpvt) != 0) {
        errlogPrintf("Can't attach VME interrupt.\n");
        return;
    }

    /*
     * Creater a dummy asynUser structure to control interrupt thread
     * diagnostic messages
     */
    dpvt->debugAsynUser = pasynManager->createAsynUser(0, 0);
    status = pasynManager->connectDevice(dpvt->debugAsynUser, portName, A_DEBUGGING);
    if (status != asynSuccess) {
        errlogPrintf("Can't connect diagnostic pasynUser.\n");
        return;
    }

    /************************ Set up hardware ************************/
    put16(dpvt, R_BITSET1,   B1_SOFTWARE_RESET);   /* Reset hardware */
    put16(dpvt, R_BITCLEAR1, B1_SOFTWARE_RESET);
    put16(dpvt, R_BITSET2,   B2_OVERRANGE_ENABLE); /* Store overrange values */
    put16(dpvt, R_BITCLEAR2, B2_SLIDE_ENABLE);     /* No sliding */
    put16(dpvt, R_SLIDE_CONSTANT, 0); /* No adjustment */

    /*
     * Set thresholds to kill high-range channels with counts below 496
     * Speeds up readout of low-current channels.
     */
    for (i = 0 ; i < dpvt->channelCount ; i++) {
        if (dpvt->rangeMask) {
            put16(dpvt, lowRangeThresholdRegister(dpvt, i), 0);
            put16(dpvt, highRangeThresholdRegister(dpvt, i), 0x100 | (496/16));
        }
        else {
            /* single range: keep all conversions (the 0x100 kill bit
             * would disable the channel) */
            put16(dpvt, highRangeThresholdRegister(dpvt, i), 0);
        }
    }
    put16(dpvt, R_INTERRUPT_LEVEL, level);
    put16(dpvt, R_INTERRUPT_VECTOR, vector);
    put16(dpvt, R_EVENT_TRIGGER, 1); /* Interrupt when anything in buffer */
}

/*
 * IOC shell command
 */
static const iocshArg caenV965ConfigureArg0 = { "Portname",iocshArgString};
static const iocshArg caenV965ConfigureArg1 = { "Card Type",iocshArgString};
static const iocshArg caenV965ConfigureArg2 = { "A24 or A32 address",iocshArgInt};
static const iocshArg caenV965ConfigureArg3 = { "interrupt vector",iocshArgInt};
static const iocshArg caenV965ConfigureArg4 = { "interrupt level",iocshArgInt};
static const iocshArg caenV965ConfigureArg5 = { "priority",iocshArgInt};
static const iocshArg *caenV965ConfigureArgs[] = {
                    &caenV965ConfigureArg0, &caenV965ConfigureArg1,
                    &caenV965ConfigureArg2, &caenV965ConfigureArg3,
                    &caenV965ConfigureArg4, &caenV965ConfigureArg5 };
static const iocshFuncDef caenV965ConfigureFuncDef =
      {"CaenV965Configure",6,caenV965ConfigureArgs};
static void caenV965ConfigureCallFunc(const iocshArgBuf *args)
{
    caenV965Configure(args[0].sval, args[1].sval, args[2].ival, args[3].ival, args[4].ival, args[5].ival);
}

static void
caenV965_RegisterCommands(void)
{
    iocshRegister(&caenV965ConfigureFuncDef,caenV965ConfigureCallFunc);
}
epicsExportRegistrar(caenV965_RegisterCommands);

