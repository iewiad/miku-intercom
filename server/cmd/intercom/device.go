package main

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"net"
	"net/netip"
	"strconv"
	"strings"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/lan"
)

type deviceFlags struct {
	address, certificate, key string
	udpMin, udpMax            int
}
type deviceConfiguration struct {
	address        netip.AddrPort
	udpMin, udpMax int
	tls            *tls.Config
}

func validateBrowserAddress(address string) error {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return fmt.Errorf("HTTP address must use a loopback host and port 1-65535")
	}
	number, err := strconv.Atoi(port)
	ip := net.ParseIP(host)
	if err != nil || number < 1 || number > 65535 || (!strings.EqualFold(host, "localhost") && (ip == nil || !ip.IsLoopback())) {
		return fmt.Errorf("HTTP address must use a loopback host and port 1-65535")
	}
	return nil
}

func prepareDeviceConfiguration(flags deviceFlags, local []netip.Addr) (*deviceConfiguration, error) {
	if flags.address == "" && flags.certificate == "" && flags.key == "" && flags.udpMin == 0 && flags.udpMax == 0 {
		return nil, nil
	}
	if flags.address == "" || flags.certificate == "" || flags.key == "" || flags.udpMin == 0 || flags.udpMax == 0 {
		return nil, fmt.Errorf("device-addr, device-cert, device-key, device-udp-min and device-udp-max must be configured together")
	}
	address, err := lan.ParseAddress(flags.address)
	if err != nil {
		return nil, err
	}
	if err = lan.ValidateMedia(address.Addr(), flags.udpMin, flags.udpMax); err != nil {
		return nil, err
	}
	if err = lan.RequireLocal(address.Addr(), local); err != nil {
		return nil, err
	}
	certificate, err := tls.LoadX509KeyPair(flags.certificate, flags.key)
	if err != nil {
		return nil, fmt.Errorf("device TLS certificate/key: %w", err)
	}
	leaf, err := x509.ParseCertificate(certificate.Certificate[0])
	if err != nil {
		return nil, fmt.Errorf("device TLS certificate: %w", err)
	}
	if err = leaf.VerifyHostname(address.Addr().String()); err != nil {
		return nil, fmt.Errorf("device TLS certificate must cover the configured IPv4: %w", err)
	}
	now := time.Now()
	if now.Before(leaf.NotBefore) || !now.Before(leaf.NotAfter) {
		return nil, fmt.Errorf("device TLS certificate is not currently valid")
	}
	return &deviceConfiguration{address: address, udpMin: flags.udpMin, udpMax: flags.udpMax,
		tls: &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{certificate}}}, nil
}
