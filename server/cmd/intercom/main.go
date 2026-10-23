package main

import (
	"context"
	"errors"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/httpapi"
	"github.com/iewiad/miku-intercom/server/internal/media"
	"github.com/iewiad/miku-intercom/server/internal/persist"
)

func run() error {
	addr := flag.String("addr", "127.0.0.1:8080", "HTTP listen address")
	data := flag.String("data", "../work/intercom/state.json", "private snapshot path (parent directory must be 0700)")
	diagnostics := flag.Bool("media-diagnostics", false, "enable localhost synthetic audio diagnostics")
	flag.Parse()
	store, err := persist.Open(*data)
	if err != nil {
		return err
	}
	defer store.Close()
	snapshot, err := store.Load()
	if err != nil {
		return err
	}
	service, err := control.New(snapshot, store, control.Options{})
	if err != nil {
		return err
	}
	hub, err := media.New(service, media.Options{Diagnostics: *diagnostics})
	if err != nil {
		return err
	}
	defer hub.Close()
	server := &http.Server{Addr: *addr, Handler: httpapi.New(service, hub), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 16 * 1024}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	done := make(chan struct{})
	go func() {
		defer close(done)
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdown)
	}()
	log.Printf("intercom browser audio listening on %s", *addr)
	err = server.ListenAndServe()
	stop()
	<-done
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}
func main() {
	if err := run(); err != nil {
		log.Printf("intercom: %v", err)
		os.Exit(1)
	}
}
