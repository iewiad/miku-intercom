// recommend-probe performs a synthetic text-only turn on the existing approved
// RTA route. It does not open microphone/audio/serial devices or launch a browser.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"os"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/recommendmodel"
	"github.com/iewiad/miku-intercom/server/internal/rta"
)

func main() {
	config := flag.String("voice-config", "", "existing private RTA config, read in place")
	journal := flag.String("journal", "", "new private cleanup journal path")
	throughControl := flag.Bool("through-control", false, "exercise real HTTP recommendation against synthetic private groups")
	state := flag.String("state", "", "new empty private synthetic control snapshot path")
	confirm := flag.Bool("confirm", false, "explicitly confirm a synthetic recommendation, then verify durable replay")
	flag.Parse()
	p, e := rta.Load(*config, *journal)
	if e != nil {
		_ = json.NewEncoder(os.Stdout).Encode(map[string]string{"stage": "config_unavailable"})
		return
	}
	defer p.Close()
	if *throughControl {
		_ = json.NewEncoder(os.Stdout).Encode(probeControl(p, *state, *confirm))
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	output, report, e := recommendmodel.Text(ctx, p, `Return JSON only: {"suggestions":[{"candidate_id":"c1","reason":"brief reason"}]}. Need: a broken speaker needs repair. Candidates: c1 repair team, equipment faults; c2 catering, food and transport. Select at most one supplied candidate; empty array if none fits.`)
	var parsed struct {
		Suggestions []struct {
			ID     string `json:"candidate_id"`
			Reason string `json:"reason"`
		} `json:"suggestions"`
	}
	valid := e == nil && json.Unmarshal([]byte(output), &parsed) == nil && len(parsed.Suggestions) == 1 && parsed.Suggestions[0].ID == "c1" && len(parsed.Suggestions[0].Reason) > 0
	_ = json.NewEncoder(os.Stdout).Encode(struct {
		Report recommendmodel.Report `json:"report"`
		Valid  bool                  `json:"exact_candidate_json"`
	}{report, valid})
}
