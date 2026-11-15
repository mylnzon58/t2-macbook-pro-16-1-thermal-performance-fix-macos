/*
 * kext/A2141AsymmetricHWP.hpp
 * MacBook Pro 16,1 (A2141 - Intel Core i7-9750H) Asymmetric HWP & Thermal Driver
 * Author: Juan José Monzón (@mylnzon58)
 */

#ifndef A2141_ASYMMETRIC_HWP_HPP
#define A2141_ASYMMETRIC_HWP_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include "../include/a2141_shared.h"

class com_juanmonzon_driver_A2141AsymmetricHWP : public IOService {
    OSDeclareDefaultStructors(com_juanmonzon_driver_A2141AsymmetricHWP)

private:
    IOWorkLoop          *fWorkLoop;
    IOTimerEventSource  *fTimerSource;
    bool                fSysctlRegistered;

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    void enforceHardwareState(void);

public:
    virtual bool init(OSDictionary *dictionary = nullptr) override;
    virtual void free(void) override;
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;
};

#endif /* A2141_ASYMMETRIC_HWP_HPP */
