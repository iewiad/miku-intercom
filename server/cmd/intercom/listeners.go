package main

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/http"
	"time"
)

type runningServer struct {
	server   *http.Server
	listener net.Listener
}

func httpServer(address string, handler http.Handler) *http.Server {
	return &http.Server{Addr: address, Handler: handler, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second,
		WriteTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 16 * 1024}
}

// Prepare every listener before serving any request. A failed device bind also closes HTTP.
func prepareListeners(browser *http.Server, device *http.Server) ([]runningServer, error) {
	listener, err := net.Listen("tcp", browser.Addr)
	if err != nil {
		return nil, fmt.Errorf("localhost listener: %w", err)
	}
	servers := []runningServer{{browser, listener}}
	if device != nil {
		listener, err = net.Listen("tcp4", device.Addr)
		if err != nil {
			_ = servers[0].listener.Close()
			return nil, fmt.Errorf("device listener: %w", err)
		}
		servers = append(servers, runningServer{device, tls.NewListener(listener, device.TLSConfig)})
	}
	return servers, nil
}

func serveListeners(ctx context.Context, servers []runningServer) error {
	results := make(chan error, len(servers))
	for _, entry := range servers {
		go func() { results <- entry.server.Serve(entry.listener) }()
	}
	consumed := 0
	var result error
	select {
	case <-ctx.Done():
	case result = <-results:
		consumed++
	}
	shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for _, entry := range servers {
		if err := entry.server.Shutdown(shutdown); err != nil {
			_ = entry.server.Close()
		}
		_ = entry.listener.Close()
	}
	for consumed < len(servers) {
		<-results
		consumed++
	}
	if errors.Is(result, http.ErrServerClosed) {
		return nil
	}
	return result
}
