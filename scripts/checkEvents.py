from epics import ca
from epics import PV
import time 

oldEventCount = -1

def eventChange(pvname=None, value=None, char_value=None, **kw):
    global oldEventCount
    if (oldEventCount < 0):
        oldEventCount = (value - 1) & 0xFFFFFF
    if (value == ((oldEventCount+1) & 0xFFFFFF)):
        oldEventCount = value
    else:
        print 'Expected event' , oldEventCount+1, 'got event', value
        oldEventCount = -1

mypv = PV('CaenV965Test:CaenV965:EventCount')
mypv.add_callback(eventChange)

wasConnected = 0
now = time.time()
time.sleep(0.01)
while 1:
    print 'Event count', oldEventCount
    then = now
    while (now < (then + 100)):
        chid = ca.create_channel('CaenV965Test:CaenV965:chan0', connect=True)
        if (ca.isConnected(chid)):
            if (wasConnected == 0):
                print 'Connected'
            wasConnected = 1
            v = ca.get(chid)
        else:
            if (wasConnected):
                print 'Disconnected'
            wasConnected = 0
        ca.clear_channel(chid)
        time.sleep(1)
        now = time.time()
