package main

import (
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// Use a real TCP listener: ResponseRecorder would conceal the old 10s socket
// write deadline that silently dropped otherwise valid model results.
func TestModelResponseSurvivesPreviousWriteDeadline(t *testing.T) {
	server := httpServer("127.0.0.1:0", http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		timer := time.NewTimer(10250 * time.Millisecond)
		defer timer.Stop()
		select {
		case <-r.Context().Done():
			return
		case <-timer.C:
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"recommendation":"synthetic delayed result"}`)
	}))
	if server.WriteTimeout != 40*time.Second || server.WriteTimeout <= 35*time.Second {
		t.Fatal("listener cannot contain model + cleanup + client response budget")
	}
	listener, e := net.Listen("tcp", server.Addr)
	if e != nil {
		t.Fatal(e)
	}
	defer server.Close()
	go func() { _ = server.Serve(listener) }()
	client := &http.Client{Timeout: 15 * time.Second}
	response, e := client.Post("http://"+listener.Addr().String()+"/api/v1/recommendations", "application/json", strings.NewReader(`{}`))
	if e != nil {
		t.Fatalf("bounded model response dropped: %v", e)
	}
	defer response.Body.Close()
	body, e := io.ReadAll(response.Body)
	if e != nil || response.StatusCode != 200 || string(body) != `{"recommendation":"synthetic delayed result"}` {
		t.Fatal("delayed response incomplete")
	}
}
