// Package lan validates the explicit, opt-in device network boundary.
package lan

import (
	"fmt"
	"net"
	"net/netip"
)

// ParseAddress requires a canonical RFC1918 IPv4 literal and a nonzero port.
func ParseAddress(value string) (netip.AddrPort, error) {
	address, err := netip.ParseAddrPort(value)
	if err != nil || address.String() != value || address.Port() == 0 || !PrivateIPv4(address.Addr()) {
		return netip.AddrPort{}, fmt.Errorf("device address must be an RFC1918 IPv4 literal with port 1-65535")
	}
	return address, nil
}

func PrivateIPv4(ip netip.Addr) bool { return ip.Is4() && ip.IsPrivate() }

// ValidateMedia rejects partial configuration and preserves the zero-value loopback policy.
func ValidateMedia(ip netip.Addr, minimum, maximum int) error {
	if !ip.IsValid() && minimum == 0 && maximum == 0 {
		return nil
	}
	if !PrivateIPv4(ip) || minimum < 1 || maximum > 65535 || minimum > maximum {
		return fmt.Errorf("device media requires an RFC1918 IPv4 and UDP range 1-65535 with min <= max")
	}
	return nil
}

func RequireLocal(ip netip.Addr, local []netip.Addr) error {
	for _, assigned := range local {
		if ip == assigned {
			return nil
		}
	}
	return fmt.Errorf("device IPv4 must be assigned to an active local interface")
}

func LocalIPv4s() ([]netip.Addr, error) {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil, err
	}
	var result []netip.Addr
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addresses, err := iface.Addrs()
		if err != nil {
			return nil, err
		}
		for _, address := range addresses {
			prefix, err := netip.ParsePrefix(address.String())
			if err == nil && prefix.Addr().Is4() {
				result = append(result, prefix.Addr())
			}
		}
	}
	return result, nil
}

// CandidateAllowed constrains advertised candidates, not every underlying socket.
func CandidateAllowed(selected netip.Addr, candidate net.IP) bool {
	if candidate.IsLoopback() {
		return true
	}
	ip, ok := netip.AddrFromSlice(candidate)
	return ok && PrivateIPv4(selected) && ip.Unmap() == selected
}
