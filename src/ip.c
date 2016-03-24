#include "ip.h"

#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "log.h"

#if IP4_ADDR_BUFLEN < INET_ADDRSTRLEN
#error "IPv4 address buffer length is incorrect for this platform"
#endif

bool ip4GetAddr(const char* str, ip4Addr* addr) {
	struct in_addr inAddr;
	if (inet_pton(AF_INET, str, &inAddr) != 1) {
		lprintf(LogError, "Invalid IPv4 address: %s\n", str);
		return false;
	}
	*addr = inAddr.s_addr;
	return true;
}

int ip4AddrToString(ip4Addr addr, char* buffer) {
	struct in_addr inAddr = { .s_addr = addr };
	errno = 0;
	if (inet_ntop(AF_INET, &inAddr, buffer, IP4_ADDR_BUFLEN) == NULL) {
		lprintf(LogError, "Could not convert IPv4 address to string: %s\n", strerror(errno));
		strcpy(buffer, "(bad ip)");
		return errno;
	}
	return 0;
}

bool ip4GetSubnet(const char* str, ip4Subnet* subnet) {
	size_t len = strlen(str);
	char strCopy[len+1];
	strncpy(strCopy, str, len);
	strCopy[len] = '\0';

	char* slash = strchr(strCopy, '/');
	if (slash == NULL) {
		lprintf(LogError, "Invalid CIDR notation (no slash found): %s\n", str);
		return false;
	}
	*slash = '\0';

	if (!ip4GetAddr(strCopy, &subnet->addr)) {
		lprintf(LogError, "Invalid CIDR notation (invalid IPv4 address): %s\n", str);
		return false;
	}

	const char* suffix = slash+1;
	char* end;
	long prefixLen = strtol(suffix, &end, 10);
	if (end == suffix || *end != '\0' || prefixLen < 0 || prefixLen > 32) {
		lprintf(LogError, "Invalid CIDR notation (invalid prefix length): %s\n", str);
		return false;
	}

	subnet->prefixLen = (uint8_t)prefixLen;
	subnet->addr &= ip4SubnetMask(subnet);
	return true;
}

ip4Addr ip4SubnetMask(const ip4Subnet* subnet) {
	return ~ip4HostMask(subnet);
}

ip4Addr ip4HostMask(const ip4Subnet* subnet) {
	return (ip4Addr)ntohl((uint32_t)(((uint64_t)1 << (uint64_t)(32 - subnet->prefixLen)) - 1));
}

ip4Addr ip4SubnetStart(const ip4Subnet* subnet) {
	return subnet->addr;
}

ip4Addr ip4SubnetEnd(const ip4Subnet* subnet) {
	return subnet->addr | ip4HostMask(subnet);
}

uint32_t ip4SubnetSize(const ip4Subnet* subnet, bool excludeReserved) {
	uint32_t count = 1U << (32 - subnet->prefixLen);
	if (excludeReserved && count > 2) count -= 2;
	return count;
}

int ip4SubnetToString(const ip4Subnet* subnet, char* buffer) {
	char ip[IP4_ADDR_BUFLEN];
	int res = ip4AddrToString(subnet->addr, ip);
	if (res != 0) {
		strcpy(buffer, ip); // IP error message
		return res;
	}
	res = snprintf(buffer, IP4_CIDR_BUFLEN, "%s/%u", ip, subnet->prefixLen);
	if (res < 0 || res >= IP4_CIDR_BUFLEN) {
		strcpy(buffer, "(print fail)");
		return ENOSPC;
	}
	return 0;
}

typedef struct {
	int64_t start;
	int64_t end;
} ignoreRange;

static int ignoreRangeCompare(const void* r1, const void* r2) {
	const ignoreRange* range1 = r1;
	const ignoreRange* range2 = r2;
	if (range1->start < range2->start) return -1;
	else if (range1->start > range2->start) return 1;
	else {
		// Sort the ranges so that we skip over the maximum possible (largest
		// comes first)
		if (range1->end > range2->end) return -1;
		else if (range1->end == range2->end) return 0;
		else return 1;
	}
}

struct ip4Iter_s {
	int64_t currentAddr; // Host order
	int64_t finalAddr; // Host order
	ignoreRange* ignores;
	size_t ignoreCount;
	size_t currentIgnoreNum;
	ignoreRange* currentIgnore; // Cached for performance
};

ip4Iter* ip4NewIter(const ip4Subnet* subnet, const ip4Subnet** avoidSubnets) {
	ip4Iter* it = malloc(sizeof(ip4Iter));
	it->currentAddr = (int64_t)(ntohl(ip4SubnetStart(subnet)))-1;
	it->finalAddr = ntohl(ip4SubnetEnd(subnet));
	it->ignoreCount = 0;
	if (avoidSubnets != NULL) {
		for (const ip4Subnet** net = avoidSubnets; *net != NULL; ++net) {
			++it->ignoreCount;
		}
	}
	if (it->ignoreCount == 0) {
		it->ignores = NULL;
	} else {
		it->ignores = malloc(it->ignoreCount * sizeof(ignoreRange));
		for (size_t i = 0; i < it->ignoreCount; ++i) {
			it->ignores[i].start = ntohl(ip4SubnetStart(avoidSubnets[i]));
			it->ignores[i].end = ntohl(ip4SubnetEnd(avoidSubnets[i]));
		}
		qsort(it->ignores, it->ignoreCount, sizeof(ignoreRange), &ignoreRangeCompare);
	}
	it->currentIgnoreNum = 0;
	it->currentIgnore = it->ignores;

	return it;
}

bool ip4IterNext(ip4Iter* it) {
	if (it->currentAddr >= it->finalAddr) return false;
	++it->currentAddr;

	while (true) {
		#define IGNORES_REMAIN (it->currentIgnoreNum < it->ignoreCount)

		// We need to check containment in case ignore ranges overlap
		bool insideIgnore = IGNORES_REMAIN && (it->currentAddr >= it->currentIgnore->start && it->currentAddr <= it->currentIgnore->end);
		if (!insideIgnore) break;

		// Skip the ignored range
		it->currentAddr = it->currentIgnore->end + 1;

		// We may have skipped over multiple ranges
		do {
			++it->currentIgnoreNum;
			++it->currentIgnore;
		} while (IGNORES_REMAIN && it->currentAddr > it->currentIgnore->end);
	}
	return (it->currentAddr <= it->finalAddr);
}

ip4Addr ip4IterAddr(const ip4Iter* it) {
	return (ip4Addr)htonl((uint32_t)it->currentAddr);
}

void ip4FreeIter(ip4Iter* it) {
	if (it->ignoreCount > 0) {
		free(it->ignores);
	}
	free(it);
}

struct ip4FragIter_s {
	bool first;
	uint32_t currentAddr; // Host order
	uint32_t smallIncrement;
	uint8_t smallPrefixLen;
	uint32_t largeFragmentsRemaining;
	uint32_t fragmentsRemaining;
};

ip4FragIter* ip4FragmentSubnet(const ip4Subnet* subnet, uint32_t fragmentCount) {
	uint32_t parentSize = ip4SubnetSize(subnet, false);
	if (parentSize < fragmentCount) return NULL;

	// Our strategy is to split the subnet into "small" and "large" fragments.
	// Large fragments are exactly twice as large as small fragments. We simply
	// choose the largest possible size for the small fragments so that we have
	// enough space, and then grant some fragments twice as much space until we
	// have used up the leftover addresses.

	double idealFragmentSize = floor((double)parentSize / (double)fragmentCount);
	fesetround(FE_DOWNWARD);
	uint8_t smallerPow2 = (uint8_t)llrint(log2(idealFragmentSize));
	uint32_t smallSize = 1U << smallerPow2;
	uint32_t totalSmallSize = smallSize * fragmentCount;
	uint32_t leftoverSize = parentSize - totalSmallSize;
	uint32_t largeFragments = (uint32_t)llrint((double)leftoverSize / (double)smallSize);

	ip4FragIter* it = malloc(sizeof(ip4FragIter));
	it->first = true;
	it->currentAddr = ntohl(subnet->addr);
	it->smallIncrement = smallSize;
	it->smallPrefixLen = (uint8_t)(32 - smallerPow2);
	it->largeFragmentsRemaining = largeFragments;
	it->fragmentsRemaining = fragmentCount;
	return it;
}

bool ip4FragIterNext(ip4FragIter* it) {
	if (it->fragmentsRemaining == 0) return false;
	if (it->first) {
		it->first = false;
		return true;
	}
	bool isLarge = (it->largeFragmentsRemaining > 0);
	if (isLarge) {
		--it->largeFragmentsRemaining;
	}
	it->currentAddr += it->smallIncrement * (isLarge ? 2 : 1);
	--it->fragmentsRemaining;
	return it->fragmentsRemaining > 0;
}

void ip4FragIterSubnet(const ip4FragIter* it, ip4Subnet* fragment) {
	fragment->addr = htonl(it->currentAddr);
	fragment->prefixLen = (uint8_t)(it->largeFragmentsRemaining > 0 ? it->smallPrefixLen - 1 : it->smallPrefixLen);
}

void ip4FreeFragIter(ip4FragIter* it) {
	free(it);
}

bool macGetAddr(const char* str, macAddr* addr) {
	int octets[MAC_ADDR_BYTES];
	char overflow;
	int foundOctets = sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x%c", &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5], &overflow);
	if (foundOctets != MAC_ADDR_BYTES) return false;
	for (int i = 0; i < MAC_ADDR_BYTES; ++i) {
		addr->octets[i] = (uint8_t)octets[i];
	}
	return true;
}

bool macNextAddr(macAddr* addr) {
	for (size_t i = MAC_ADDR_BYTES; i > 0; --i) {
		if (++addr->octets[i-1] != 0) return true;
	}
	return false;
}

bool macNextAddrs(macAddr* nextAddr, macAddr buffer[], size_t count) {
	bool unwrapped = true;
	for (size_t i = 0; i < count; ++i) {
		memcpy(buffer[i].octets, nextAddr->octets, MAC_ADDR_BYTES);
		if (!macNextAddr(nextAddr)) unwrapped = false;
	}
	return unwrapped;
}

void macAddrToString(const macAddr* addr, char* buffer) {
	sprintf(buffer, "%02x:%02x:%02x:%02x:%02x:%02x", addr->octets[0], addr->octets[1], addr->octets[2], addr->octets[3], addr->octets[4], addr->octets[5]);
}
