/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _APPLE_ARM_GIC_H
#define _APPLE_ARM_GIC_H

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "GICv3.h"

/* GIC-500 / GICv3, Non-secure EL1. Local timer and SGI0 belong to XNU's
 * bootstrap; this service manages shared peripheral interrupts. */
class AppleARMGICv3 : public IOInterruptController
{
	OSDeclareDefaultStructors(AppleARMGICv3);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual bool requestTerminate(IOService *provider, IOOptionBits options)
	    APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerInterrupt(IOService *nub, int source, void *target,
	    IOInterruptHandler handler, void *refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn unregisterInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getInterruptType(IOService *nub, int source, int *type) APPLE_KEXT_OVERRIDE;
	virtual IOReturn enableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	virtual IOReturn disableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	virtual IOReturn causeInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	virtual IOInterruptAction getInterruptHandlerAddress(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source)
	    APPLE_KEXT_OVERRIDE;
	virtual void initVector(IOInterruptVectorNumber number, IOInterruptVector *vector)
	    APPLE_KEXT_OVERRIDE;
	virtual int getVectorType(IOInterruptVectorNumber number, IOInterruptVector *vector)
	    APPLE_KEXT_OVERRIDE;
	virtual void disableVectorHard(IOInterruptVectorNumber number, IOInterruptVector *vector)
	    APPLE_KEXT_OVERRIDE;
	virtual void enableVector(IOInterruptVectorNumber number, IOInterruptVector *vector)
	    APPLE_KEXT_OVERRIDE;
	virtual void causeVector(IOInterruptVectorNumber number, IOInterruptVector *vector)
	    APPLE_KEXT_OVERRIDE;
	virtual void sendIPI(unsigned int cpu, bool deferred) APPLE_KEXT_OVERRIDE;
	virtual void cancelDeferredIPI(unsigned int cpu) APPLE_KEXT_OVERRIDE;

private:
	class Registers {
	public:
		volatile uint8_t *base;
		uint8_t read8(uint32_t offset);
		uint32_t read32(uint32_t offset);
		uint64_t read64(uint32_t offset);
		void write8(uint32_t offset, uint8_t value);
		void write32(uint32_t offset, uint32_t value);
		void write64(uint32_t offset, uint64_t value);
		void relax(void);
	};
	bool mapGICMemory(IOService *provider);
	bool decodeSource(IOService *nub, int source, uint32_t &id, uint32_t &type) const;
	bool ownsSource(IOService *nub, int source, uint32_t &id) const;
	void releaseResources(void);
	static void parentInterrupt(void *target, void *refCon, void *nub, int source);

	IOMemoryMap *distributorMap;
	IOMemoryMap *redistributorMap;
	const OSSymbol *controllerName;
	Registers distributor;
	uint32_t numVectors;
	uint64_t bootAffinity;
	bool published;
};
#endif
