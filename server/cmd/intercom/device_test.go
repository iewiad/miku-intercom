package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func syntheticCertificate(t *testing.T, ip string, before, after time.Time) (string, string, *x509.CertPool) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "synthetic device test"}, NotBefore: before, NotAfter: after,
		KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}, BasicConstraintsValid: true, IsCA: true, IPAddresses: []net.IP{net.ParseIP(ip)}}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	certPath := filepath.Join(directory, "synthetic-cert.pem")
	keyPath := filepath.Join(directory, "synthetic-key.pem")
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	if err = os.WriteFile(certPath, certPEM, 0600); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0600); err != nil {
		t.Fatal(err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(certPEM) {
		t.Fatal("invalid synthetic root")
	}
	return certPath, keyPath, roots
}
func validDeviceFlags(t *testing.T) (deviceFlags, *x509.CertPool) {
	t.Helper()
	cert, key, roots := syntheticCertificate(t, "192.168.50.5", time.Now().Add(-time.Hour), time.Now().Add(time.Hour))
	return deviceFlags{"192.168.50.5:8443", cert, key, 40000, 40100}, roots
}
func TestDeviceConfigurationFailClosed(t *testing.T) {
	local := []netip.Addr{netip.MustParseAddr("192.168.50.5")}
	if config, err := prepareDeviceConfiguration(deviceFlags{}, nil); err != nil || config != nil {
		t.Fatal("default device listener enabled")
	}
	flags, _ := validDeviceFlags(t)
	config, err := prepareDeviceConfiguration(flags, local)
	if err != nil {
		t.Fatal(err)
	}
	if config.tls.MinVersion != tls.VersionTLS12 || len(config.tls.Certificates) != 1 {
		t.Fatal("TLS minimum/certificate not configured")
	}
	for _, test := range []struct {
		name   string
		change func(*deviceFlags)
	}{
		{"missing_address", func(f *deviceFlags) { f.address = "" }}, {"missing_cert", func(f *deviceFlags) { f.certificate = "" }}, {"missing_key", func(f *deviceFlags) { f.key = "" }}, {"missing_min", func(f *deviceFlags) { f.udpMin = 0 }}, {"missing_max", func(f *deviceFlags) { f.udpMax = 0 }},
		{"unassigned_ip", func(f *deviceFlags) { f.address = "192.168.50.6:8443" }}, {"wildcard", func(f *deviceFlags) { f.address = "0.0.0.0:8443" }}, {"hostname", func(f *deviceFlags) { f.address = "localhost:8443" }}, {"mapped_ipv6", func(f *deviceFlags) { f.address = "[::ffff:192.168.50.5]:8443" }}, {"port_zero", func(f *deviceFlags) { f.address = "192.168.50.5:0" }},
		{"reversed_udp", func(f *deviceFlags) { f.udpMin = 40101 }}, {"negative_udp", func(f *deviceFlags) { f.udpMin = -1 }}, {"udp_overflow", func(f *deviceFlags) { f.udpMax = 65536 }}, {"missing_file", func(f *deviceFlags) { f.certificate = filepath.Join(t.TempDir(), "absent.pem") }},
	} {
		t.Run(test.name, func(t *testing.T) {
			value := flags
			test.change(&value)
			if _, err := prepareDeviceConfiguration(value, local); err == nil {
				t.Fatal("incomplete/unsafe configuration accepted")
			}
		})
	}
	for _, test := range []struct {
		name, ip      string
		before, after time.Time
	}{
		{"wrong_SAN", "192.168.50.6", time.Now().Add(-time.Hour), time.Now().Add(time.Hour)},
		{"expired", "192.168.50.5", time.Now().Add(-2 * time.Hour), time.Now().Add(-time.Hour)},
		{"future", "192.168.50.5", time.Now().Add(time.Hour), time.Now().Add(2 * time.Hour)},
	} {
		t.Run(test.name, func(t *testing.T) {
			value := flags
			value.certificate, value.key, _ = syntheticCertificate(t, test.ip, test.before, test.after)
			if _, err := prepareDeviceConfiguration(value, local); err == nil {
				t.Fatal("invalid certificate accepted")
			}
		})
	}
}
func TestBrowserAddressRemainsLoopback(t *testing.T) {
	for _, address := range []string{"127.0.0.1:8080", "localhost:8080", "[::1]:8080"} {
		if err := validateBrowserAddress(address); err != nil {
			t.Fatal(err)
		}
	}
	for _, address := range []string{"0.0.0.0:8080", ":8080", "192.168.50.5:8080", "[::]:8080", "evil.example:8080", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0.1:-1"} {
		if validateBrowserAddress(address) == nil {
			t.Fatal("non-loopback/invalid HTTP address accepted")
		}
	}
}
func TestDeviceTLSMinimumActualHandshake(t *testing.T) {
	flags, roots := validDeviceFlags(t)
	config, err := prepareDeviceConfiguration(flags, []netip.Addr{netip.MustParseAddr("192.168.50.5")})
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(200) }))
	server.Config.ErrorLog = log.New(io.Discard, "", 0)
	server.TLS = config.tls.Clone()
	server.StartTLS()
	defer server.Close()
	for _, test := range []struct {
		version uint16
		allowed bool
	}{{tls.VersionTLS11, false}, {tls.VersionTLS12, true}, {tls.VersionTLS13, true}} {
		transport := &http.Transport{TLSClientConfig: &tls.Config{RootCAs: roots, ServerName: "192.168.50.5", MinVersion: tls.VersionTLS10, MaxVersion: test.version}}
		client := &http.Client{Transport: transport, Timeout: time.Second}
		response, err := client.Get(server.URL)
		transport.CloseIdleConnections()
		if response != nil {
			_ = response.Body.Close()
		}
		if (err == nil) != test.allowed {
			t.Fatalf("version %x allowed=%v error=%v", test.version, test.allowed, err)
		}
	}
}
func TestDualListenerPreparationFailureClosesHTTP(t *testing.T) {
	occupied, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer occupied.Close()
	probe, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := probe.Addr().String()
	_ = probe.Close()
	if _, err := prepareListeners(httpServer(address, http.NotFoundHandler()), httpServer(occupied.Addr().String(), http.NotFoundHandler())); err == nil {
		t.Fatal("occupied device address accepted")
	}
	retry, err := net.Listen("tcp4", address)
	if err != nil {
		t.Fatal("HTTP listener leaked on failed device bind")
	}
	_ = retry.Close()
}
func TestDualListenersShutdownOnCancelOrServeFailure(t *testing.T) {
	for _, fail := range []bool{false, true} {
		t.Run(map[bool]string{false: "cancel", true: "serve_failure"}[fail], func(t *testing.T) {
			flags, roots := validDeviceFlags(t)
			config, err := prepareDeviceConfiguration(flags, []netip.Addr{netip.MustParseAddr("192.168.50.5")})
			if err != nil {
				t.Fatal(err)
			}
			handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(200) })
			device := httpServer("127.0.0.1:0", handler)
			device.TLSConfig = config.tls
			servers, err := prepareListeners(httpServer("127.0.0.1:0", handler), device)
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan error, 1)
			go func() { done <- serveListeners(ctx, servers) }()
			response, err := http.Get("http://" + servers[0].listener.Addr().String())
			if err != nil {
				t.Fatal(err)
			}
			_ = response.Body.Close()
			transport := &http.Transport{TLSClientConfig: &tls.Config{RootCAs: roots, ServerName: "192.168.50.5", MinVersion: tls.VersionTLS12}}
			defer transport.CloseIdleConnections()
			client := &http.Client{Transport: transport, Timeout: time.Second}
			response, err = client.Get("https://" + servers[1].listener.Addr().String())
			if err != nil {
				t.Fatal(err)
			}
			_ = response.Body.Close()
			if fail {
				_ = servers[1].listener.Close()
			} else {
				cancel()
			}
			select {
			case err := <-done:
				if fail == (err == nil) {
					t.Fatalf("unexpected shutdown error %v", err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("dual listeners failed to stop")
			}
			for _, entry := range servers {
				connection, err := net.DialTimeout("tcp", entry.listener.Addr().String(), time.Second)
				if err == nil {
					_ = connection.Close()
					t.Fatal("listener left open")
				}
			}
		})
	}
}
