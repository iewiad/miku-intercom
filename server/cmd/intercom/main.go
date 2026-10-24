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

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/httpapi"
	"github.com/iewiad/miku-intercom/server/internal/lan"
	"github.com/iewiad/miku-intercom/server/internal/media"
	"github.com/iewiad/miku-intercom/server/internal/persist"
	"github.com/iewiad/miku-intercom/server/internal/recommendmodel"
	"github.com/iewiad/miku-intercom/server/internal/rta"
)

func run() error {
	addr := flag.String("addr", "127.0.0.1:8080", "loopback HTTP listen address")
	data := flag.String("data", "../work/intercom/state.json", "private snapshot path (parent directory must be 0700)")
	diagnostics := flag.Bool("media-diagnostics", false, "enable localhost synthetic audio diagnostics")
	deviceAddr := flag.String("device-addr", "", "opt-in native HTTPS listener: assigned RFC1918 IPv4:port")
	deviceCert := flag.String("device-cert", "", "device HTTPS certificate PEM file (IPv4 SAN required)")
	deviceKey := flag.String("device-key", "", "device HTTPS private key PEM file")
	deviceUDPMin := flag.Int("device-udp-min", 0, "device media minimum UDP port")
	deviceUDPMax := flag.Int("device-udp-max", 0, "device media maximum UDP port")
	voiceConfig := flag.String("voice-config", "", "opt-in private JSON RTA configuration; browser signalling stays local")
	recommendationModel := flag.Bool("recommendation-model", false, "opt-in bounded text recommendation using the same approved voice-config route")
	flag.Parse()
	if err := validateBrowserAddress(*addr); err != nil {
		return err
	}
	var device *deviceConfiguration
	if *deviceAddr != "" || *deviceCert != "" || *deviceKey != "" || *deviceUDPMin != 0 || *deviceUDPMax != 0 {
		local, err := lan.LocalIPv4s()
		if err != nil {
			return err
		}
		device, err = prepareDeviceConfiguration(deviceFlags{*deviceAddr, *deviceCert, *deviceKey, *deviceUDPMin, *deviceUDPMax}, local)
		if err != nil {
			return err
		}
	}
	store, err := persist.Open(*data)
	if err != nil {
		return err
	}
	defer store.Close()
	snapshot, err := store.Load()
	if err != nil {
		return err
	}
	var voice *rta.Proxy
	if *voiceConfig != "" {
		voice, err = rta.Load(*voiceConfig, *data+".voice.json")
		if err != nil {
			return err
		}
		defer voice.Close()
	}
	opts := control.Options{}
	if *recommendationModel {
		if voice == nil {
			return errors.New("recommendation-model requires the existing private voice-config")
		}
		opts.Recommender = recommendmodel.New(voice)
	}
	service, err := control.New(snapshot, store, opts)
	if err != nil {
		return err
	}
	mediaOptions := media.Options{Diagnostics: *diagnostics}
	if device != nil {
		mediaOptions.DeviceIP = device.address.Addr()
		mediaOptions.DeviceUDPMin = device.udpMin
		mediaOptions.DeviceUDPMax = device.udpMax
	}
	hub, err := media.New(service, mediaOptions)
	if err != nil {
		return err
	}
	defer hub.Close()
	browserServer := httpServer(*addr, httpapi.NewWithVoice(service, hub, voice))
	var deviceServer *http.Server
	if device != nil {
		handler, err := httpapi.NewDevice(service, hub, device.address.String())
		if err != nil {
			return err
		}
		deviceServer = httpServer(device.address.String(), handler)
		deviceServer.TLSConfig = device.tls
	}
	servers, err := prepareListeners(browserServer, deviceServer)
	if err != nil {
		return err
	}
	defer func() {
		for _, entry := range servers {
			_ = entry.listener.Close()
		}
	}()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	log.Printf("intercom browser audio listening on %s", *addr)
	if device != nil {
		log.Printf("intercom native device HTTPS listening on %s", device.address)
	}
	return serveListeners(ctx, servers)
}
func main() {
	if err := run(); err != nil {
		log.Printf("intercom: %v", err)
		os.Exit(1)
	}
}
