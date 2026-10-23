/* SPDX-License-Identifier: BSD-2-Clause
 * Independent GICv3 implementation for the XNU 11417 IOInterruptController ABI.
 */
#include "AppleARMGIC.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>

#define super IOInterruptController
OSDefineMetaClassAndStructors(AppleARMGICv3, IOInterruptController);
static_assert(sizeof(IOInterruptVectorNumber) == sizeof(uint32_t), "XNU arm64 vector ABI");
static_assert(kIOInterruptTypeEdge == 0 && kIOInterruptTypeLevel == 1, "Apple interrupt type ABI");

uint8_t AppleARMGICv3::Registers::read8(uint32_t offset) { return base[offset]; }
uint32_t AppleARMGICv3::Registers::read32(uint32_t offset)
{
	return *reinterpret_cast<volatile uint32_t *>(base + offset);
}
uint64_t AppleARMGICv3::Registers::read64(uint32_t offset)
{
	return *reinterpret_cast<volatile uint64_t *>(base + offset);
}
void AppleARMGICv3::Registers::write8(uint32_t offset, uint8_t value)
{
	base[offset] = value;
	OSSynchronizeIO();
}
void AppleARMGICv3::Registers::write32(uint32_t offset, uint32_t value)
{
	*reinterpret_cast<volatile uint32_t *>(base + offset) = value;
	OSSynchronizeIO();
}
void AppleARMGICv3::Registers::write64(uint32_t offset, uint64_t value)
{
	*reinterpret_cast<volatile uint64_t *>(base + offset) = value;
	OSSynchronizeIO();
}
void AppleARMGICv3::Registers::relax(void) { __asm__ volatile("yield"); }

bool
AppleARMGICv3::mapGICMemory(IOService *provider)
{
	if (provider->getDeviceMemoryCount() != 2) {
		IOLog("AppleARMGICv3: expected distributor and redistributor resources\n");
		return false;
	}
	distributorMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
	redistributorMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
	if (!distributorMap || !redistributorMap ||
	    distributorMap->getLength() < GICv3::kDistributorSize ||
	    redistributorMap->getLength() < GICv3::kRedistributorStride ||
	    redistributorMap->getLength() > UINT32_MAX ||
	    (redistributorMap->getLength() % GICv3::kRedistributorStride) ||
	    (distributorMap->getPhysicalAddress() & 0xffff) ||
	    (redistributorMap->getPhysicalAddress() & 0xffff)) {
		IOLog("AppleARMGICv3: invalid GIC memory resources\n");
		return false;
	}
	distributor.base = reinterpret_cast<volatile uint8_t *>(distributorMap->getVirtualAddress());
	Registers redist = { reinterpret_cast<volatile uint8_t *>(redistributorMap->getVirtualAddress()) };
	if (!distributor.base || !redist.base ||
	    ((distributor.read32(GICv3::kPIDR2) >> 4) & 15) != 3) {
		IOLog("AppleARMGICv3: GICv3 register interface required\n");
		return false;
	}
	/* Never change DS, secure group ownership, or affinity-routing mode. */
	uint32_t ctlr = distributor.read32(GICv3::kCTLR);
	if ((ctlr & (GICv3::kARE | GICv3::kEnableGroup1A)) !=
	    (GICv3::kARE | GICv3::kEnableGroup1A) || !GICv3::waitForWrites(distributor)) {
		IOLog("AppleARMGICv3: firmware must enable ARE_NS and Group1NS\n");
		return false;
	}
	numVectors = GICv3::vectorCount(distributor.read32(GICv3::kTYPER));
	const ml_topology_info_t *topology = ml_get_topology_info();
	if (!topology || !topology->boot_cpu || numVectors <= GICv3::kFirstSPI) {
		return false;
	}
	bootAffinity = GICv3::route(topology->boot_cpu->phys_id);
	bool found = false;
	for (IOByteCount offset = 0; offset < redistributorMap->getLength();
	    offset += GICv3::kRedistributorStride) {
		uint64_t typer = redist.read64(uint32_t(offset + 8));
		/* GIC-500 has 128 KiB frames; a VLPIS frame needs another stride. */
		if (typer & 2) {
			return false;
		}
		if (uint32_t(typer >> 32) == GICv3::affinity(bootAffinity)) {
			found = true;
		}
		if (typer & 16) {
			break;
		}
	}
	if (!found) {
		IOLog("AppleARMGICv3: no redistributor for the boot CPU\n");
	}
	return found;
}

bool
AppleARMGICv3::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	OSData *phandle = OSDynamicCast(OSData, provider->getProperty("AAPL,phandle"));
	OSData *cells = OSDynamicCast(OSData, provider->getProperty("#interrupt-cells"));
	uint32_t count = 0;
	if (cells && cells->getLength() == sizeof(count)) {
		bcopy(cells->getBytesNoCopy(), &count, sizeof(count));
	}
	if (!phandle || phandle->getLength() != sizeof(uint32_t) || count != 2 ||
	    !mapGICMemory(provider)) {
		goto fail;
	}
	controllerName = IODTInterruptControllerName(provider);
	vectors = IONewZero(IOInterruptVector, numVectors);
	controllerLock = IOSimpleLockAlloc();
	if (!controllerName || !vectors || !controllerLock) {
		goto fail;
	}
	for (uint32_t i = 0; i < numVectors; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		if (!vectors[i].interruptLock) {
			goto fail;
		}
	}
	{
		/* The platform publishes an Apple PassthruInterruptController.
		 * Its callback carries the already-acknowledged INTID as source. */
		IOInterruptController *parent =
		    getPlatform()->lookUpInterruptController(gPlatformInterruptControllerName);
		getPlatform()->setCPUInterruptProperties(this);
		if (parent->registerInterrupt(this, 0, this, parentInterrupt, NULL) != kIOReturnSuccess) {
			goto fail;
		}
	}
	/* Parent registration retains this service for the kernel lifetime. */
	published = true;
	getPlatform()->registerInterruptController(const_cast<OSSymbol *>(controllerName), this);
	registerService();
	IOLog("AppleARMGICv3: %u INTIDs, peripheral IRQs routed to affinity 0x%llx\n",
	    numVectors, bootAffinity);
	return true;
fail:
	releaseResources();
	super::stop(provider);
	return false;
}

void
AppleARMGICv3::releaseResources(void)
{
	if (vectors) {
		for (uint32_t i = 0; i < numVectors; i++) {
			if (vectors[i].interruptLock) {
				IOLockFree(vectors[i].interruptLock);
			}
		}
		IODelete(vectors, IOInterruptVector, numVectors);
		vectors = NULL;
	}
	if (controllerLock) {
		IOSimpleLockFree(controllerLock);
		controllerLock = NULL;
	}
	OSSafeReleaseNULL(controllerName);
	OSSafeReleaseNULL(distributorMap);
	OSSafeReleaseNULL(redistributorMap);
	distributor.base = NULL;
}

void AppleARMGICv3::free(void)
{
	releaseResources();
	super::free();
}

bool AppleARMGICv3::requestTerminate(IOService *, IOOptionBits)
{
	/* The primary interrupt controller must live as long as the kernel. */
	return !published;
}

bool
AppleARMGICv3::decodeSource(IOService *nub, int source, uint32_t &id, uint32_t &type) const
{
	if (!nub || source < 0 || source >= nub->_numInterruptSources || !nub->_interruptSources) {
		return false;
	}
	OSData *data = nub->_interruptSources[source].vectorData;
	return data && GICv3::decode(data->getBytesNoCopy(), data->getLength(), numVectors, id, type);
}

bool
AppleARMGICv3::ownsSource(IOService *nub, int source, uint32_t &id) const
{
	uint32_t type;
	return decodeSource(nub, source, id, type) && vectors[id].interruptRegistered &&
	    vectors[id].nub == nub && vectors[id].source == source;
}

IOReturn
AppleARMGICv3::registerInterrupt(IOService *nub, int source, void *target,
    IOInterruptHandler handler, void *refCon)
{
	uint32_t id, type;
	if (!handler || !decodeSource(nub, source, id, type)) {
		return kIOReturnBadArgument;
	}
	IOInterruptVector *vector = &vectors[id];
	IOLockLock(vector->interruptLock);
	if (vector->interruptRegistered) {
		IOLockUnlock(vector->interruptLock);
		return kIOReturnNoResources;
	}
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	bool configured = GICv3::configure(distributor, id, type, bootAffinity);
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	if (!configured) {
		IOLockUnlock(vector->interruptLock);
		IOLog("AppleARMGICv3: cannot configure Group1NS INTID %u\n", id);
		return kIOReturnUnsupported;
	}
	vector->handler = handler;
	vector->nub = nub;
	vector->source = source;
	vector->target = target;
	vector->refCon = refCon;
	vector->interruptDisabledSoft = 1;
	vector->interruptDisabledHard = 1;
	OSMemoryBarrier();
	vector->interruptRegistered = 1;
	IOLockUnlock(vector->interruptLock);
	return kIOReturnSuccess;
}

IOReturn AppleARMGICv3::unregisterInterrupt(IOService *nub, int source)
{
	uint32_t id;
	return ownsSource(nub, source, id) ? super::unregisterInterrupt(nub, source) : kIOReturnBadArgument;
}
IOReturn AppleARMGICv3::enableInterrupt(IOService *nub, int source)
{
	uint32_t id;
	return ownsSource(nub, source, id) ? super::enableInterrupt(nub, source) : kIOReturnBadArgument;
}
IOReturn AppleARMGICv3::disableInterrupt(IOService *nub, int source)
{
	uint32_t id;
	return ownsSource(nub, source, id) ? super::disableInterrupt(nub, source) : kIOReturnBadArgument;
}
IOReturn AppleARMGICv3::causeInterrupt(IOService *nub, int source)
{
	uint32_t id;
	return ownsSource(nub, source, id) ? super::causeInterrupt(nub, source) : kIOReturnBadArgument;
}
IOReturn AppleARMGICv3::getInterruptType(IOService *nub, int source, int *type)
{
	uint32_t id, decoded;
	if (!type || !decodeSource(nub, source, id, decoded)) {
		return kIOReturnBadArgument;
	}
	*type = int(decoded);
	return kIOReturnSuccess;
}
int AppleARMGICv3::getVectorType(IOInterruptVectorNumber, IOInterruptVector *vector)
{
	int type = kIOInterruptTypeLevel;
	(void)getInterruptType(vector->nub, vector->source, &type);
	return type;
}
void AppleARMGICv3::initVector(IOInterruptVectorNumber number, IOInterruptVector *vector)
{
	disableVectorHard(number, vector);
}
void AppleARMGICv3::disableVectorHard(IOInterruptVectorNumber number, IOInterruptVector *)
{
	if (!GICv3::disable(distributor, uint32_t(number))) {
		panic("AppleARMGICv3: distributor RWP timeout masking INTID %d", number);
	}
}
void AppleARMGICv3::enableVector(IOInterruptVectorNumber number, IOInterruptVector *)
{
	distributor.write32(GICv3::kISENABLER + GICv3::bank(number), GICv3::bit(number));
}
void AppleARMGICv3::causeVector(IOInterruptVectorNumber number, IOInterruptVector *)
{
	distributor.write32(GICv3::kISPENDR + GICv3::bank(number), GICv3::bit(number));
}

IOInterruptAction AppleARMGICv3::getInterruptHandlerAddress(void)
{
	return OSMemberFunctionCast(IOInterruptAction, this, &AppleARMGICv3::handleInterrupt);
}
void AppleARMGICv3::parentInterrupt(void *target, void *refCon, void *nub, int source)
{
	(void)static_cast<AppleARMGICv3 *>(target)->handleInterrupt(refCon, static_cast<IOService *>(nub), source);
}
IOReturn AppleARMGICv3::handleInterrupt(void *, IOService *, int source)
{
	/* XNU owns ICC_IAR1/EOIR1, timer and SGI0. Never acknowledge twice. */
	if (source < int(GICv3::kFirstSPI) || uint32_t(source) >= numVectors) {
		return kIOReturnBadArgument;
	}
	IOInterruptVector *vector = &vectors[source];
	vector->interruptActive = 1;
	OSMemoryBarrier();
	if (vector->interruptRegistered && !vector->interruptDisabledSoft) {
		timeStampInterruptHandlerStart(source, vector);
		vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
		timeStampInterruptHandlerEnd(source, vector);
	} else {
		vector->interruptDisabledHard = 1;
		disableVectorHard(source, vector);
		timeStampSpuriousInterrupt();
	}
	OSMemoryBarrier();
	vector->interruptActive = 0;
	return kIOReturnSuccess;
}

void AppleARMGICv3::sendIPI(unsigned int cpu, bool)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	if (!topology || cpu >= topology->num_cpus) {
		panic("AppleARMGICv3: invalid logical IPI CPU %u", cpu);
	}
	uint64_t value;
	if (!GICv3::sgi(topology->cpus[cpu].phys_id, 0, value)) {
		panic("AppleARMGICv3: unsupported IPI affinity");
	}
	__asm__ volatile("dsb ishst" ::: "memory");
	__builtin_arm_wsr64("ICC_SGI1R_EL1", value);
	__asm__ volatile("isb" ::: "memory");
}
void AppleARMGICv3::cancelDeferredIPI(unsigned int)
{
	/* Deferred IPIs are immediate. XNU cancels the software signal bit;
	 * clearing SGI0 here could discard a concurrent immediate IPI. */
}
