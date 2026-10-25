// recommend-eval evaluates synthetic needs through the production recommender
// and HTTP/control validator. Only --live opens the approved model route;
// --cleanup-only only retires an existing evaluation journal. No audio devices,
// decoder, playback, browser, serial, existing service or product state are used.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/recommendmodel"
	"github.com/iewiad/miku-intercom/server/internal/rta"
)

type caseIDs []string

func (ids *caseIDs) String() string { return "" }
func (ids *caseIDs) Set(value string) error {
	*ids = append(*ids, strings.Split(value, ",")...)
	return nil
}

type arguments struct {
	dataset, config, journal, output string
	limit                            int
	ids                              caseIDs
	live, cleanup                    bool
}

func argumentsFor(argv []string) (arguments, error) {
	var result arguments
	flags := flag.NewFlagSet("recommend-eval", flag.ContinueOnError)
	flags.SetOutput(io.Discard) // Never echo paths, unexpected arguments or values.
	flags.StringVar(&result.dataset, "dataset", "", "bounded synthetic dataset")
	flags.StringVar(&result.config, "voice-config", "", "existing approved private config, read in place")
	flags.StringVar(&result.journal, "journal", "", "new cleanup journal; existing only with --cleanup-only")
	flags.StringVar(&result.output, "output", "", "new exclusive 0600 report")
	flags.IntVar(&result.limit, "max-cases", maxCases, "maximum selected cases, 1 through 60")
	flags.Var(&result.ids, "case-id", "repeat or comma-separate case IDs; dataset order is retained")
	flags.BoolVar(&result.live, "live", false, "explicitly run production text model on synthetic data")
	flags.BoolVar(&result.cleanup, "cleanup-only", false, "only stop existing evaluation sessions; no new model calls")
	if err := flags.Parse(argv); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return result, err
		}
		return result, errors.New("invalid_arguments")
	}
	if flags.NArg() != 0 || result.live && result.cleanup || result.limit < 1 || result.limit > maxCases ||
		result.cleanup && (result.dataset != "" || len(result.ids) != 0) || !result.cleanup && result.dataset == "" ||
		(result.live || result.cleanup) && (result.config == "" || result.journal == "" || result.output == "") {
		return result, errors.New("invalid_arguments")
	}
	return result, nil
}

func privateParent(path string) bool {
	info, err := os.Lstat(filepath.Dir(path))
	return err == nil && info.IsDir() && info.Mode().Perm()&0077 == 0
}
func newPrivate(path string) (*os.File, error) {
	if !privateParent(path) {
		return nil, errors.New("private_directory_required")
	}
	return os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
}
func regularReader(path string, private bool) (*os.File, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || private && info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("input_unavailable")
	}
	return os.Open(path)
}
func distinctPaths(paths ...string) bool {
	seen := map[string]bool{}
	for _, path := range paths {
		if path == "" {
			continue
		}
		absolute, err := filepath.Abs(path)
		if err != nil || seen[absolute] {
			return false
		}
		seen[absolute] = true
	}
	return true
}
func writePrivate(file *os.File, report any) error {
	data, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	if _, err = file.Seek(0, io.SeekStart); err != nil {
		return err
	}
	if err = file.Truncate(0); err != nil {
		return err
	}
	if _, err = file.Write(data); err != nil {
		return err
	}
	return file.Sync()
}
func waitContext(ctx context.Context, duration time.Duration) error {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

// Only this fixed identity is used by production recommendmodel.Text. A journal
// containing unrelated device sessions is outside this tool's cleanup scope.
func evaluationJournal(path string) bool {
	file, err := regularReader(path, true)
	if err != nil {
		return false
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, 4*1024*1024+1))
	if err != nil || len(data) == 0 || len(data) > 4*1024*1024 {
		return false
	}
	var snapshot struct {
		Binding string `json:"binding"`
		Records map[string]struct {
			Device string `json:"device"`
		} `json:"records"`
	}
	if json.Unmarshal(data, &snapshot) != nil || snapshot.Binding == "" || snapshot.Records == nil {
		return false
	}
	for _, record := range snapshot.Records {
		if record.Device != "recommendation_model" {
			return false
		}
	}
	return true
}

type cleanupProxy interface {
	Status(string) rta.Status
	StopContext(context.Context, string, string) error
	Close()
}
type cleanupReport struct {
	Version       int    `json:"version"`
	Stage         string `json:"stage"`
	HadActive     bool   `json:"had_active_recommendation"`
	Attempted     bool   `json:"cleanup_attempted"`
	StopConfirmed bool   `json:"stop_confirmed"`
	StillPending  bool   `json:"still_pending"`
	ElapsedMS     int64  `json:"elapsed_ms"`
	ModelCalls    int    `json:"model_calls"`
}

func cleanupOnly(parent context.Context, proxy cleanupProxy, now func() time.Time, wait func(context.Context, time.Duration) error) cleanupReport {
	started := now()
	ctx, cancel := context.WithTimeout(parent, 20*time.Second)
	defer cancel()
	result := cleanupReport{Version: 1, Stage: "cleanup_only"}
	id := proxy.Status("recommendation_model").CommandID // Never emitted or used to create anything.
	result.HadActive = id != ""
	for id != "" && ctx.Err() == nil {
		result.Attempted = true
		if proxy.StopContext(ctx, "recommendation_model", id) == nil {
			break
		}
		if wait(ctx, 250*time.Millisecond) != nil {
			break
		}
	}
	proxy.Close() // Existing proxy cleanup only; at most its original 7s Stop bound.
	result.StillPending = proxy.Status("recommendation_model").CommandID != ""
	result.StopConfirmed = !result.StillPending
	result.ElapsedMS = now().Sub(started).Milliseconds()
	return result
}

type loadRoute func(string, string) (*rta.Proxy, error)

func execute(parent context.Context, argv []string, stdout, stderr io.Writer, load loadRoute) int {
	args, err := argumentsFor(argv)
	if errors.Is(err, flag.ErrHelp) {
		_, _ = io.WriteString(stdout, "recommend-eval --dataset DATASET [--case-id ID] [--max-cases 60] [--live --voice-config CONFIG --journal NEW_JOURNAL --output NEW_REPORT]\nrecommend-eval --cleanup-only --voice-config CONFIG --journal EXISTING_EVAL_JOURNAL --output NEW_REPORT\nWithout --live, only validates the dataset. Report/journal parents must already be private directories (0700).\n")
		return 0
	}
	fail := func(code string) int {
		_ = json.NewEncoder(stderr).Encode(map[string]string{"error_code": code})
		return 2
	}
	if err != nil {
		return fail("invalid_arguments")
	}
	if !distinctPaths(args.dataset, args.config, args.journal, args.output) {
		return fail("paths_must_be_distinct")
	}
	var data dataset
	var cases []evalCase
	if !args.cleanup {
		reader, readErr := regularReader(args.dataset, false)
		if readErr != nil {
			return fail("dataset_unavailable")
		}
		data, err = readDataset(reader)
		_ = reader.Close()
		if err != nil {
			return fail("dataset_invalid")
		}
		cases, err = selectCases(data, args.limit, args.ids)
		if err != nil {
			return fail("case_selection_invalid")
		}
	}
	if !args.live && !args.cleanup {
		_ = json.NewEncoder(stdout).Encode(map[string]any{"stage": "validated_not_run", "dataset_version": data.Version, "cases_selected": len(cases), "model_calls": 0})
		return 0
	}
	output, err := newPrivate(args.output)
	if err != nil {
		return fail("output_unavailable")
	}
	defer output.Close()
	setupFail := func(code string) int {
		_ = writePrivate(output, map[string]string{"error_code": code})
		return fail(code)
	}
	if args.cleanup {
		if !evaluationJournal(args.journal) {
			return setupFail("evaluation_journal_required")
		}
	} else {
		journal, createErr := newPrivate(args.journal)
		if createErr != nil {
			return setupFail("new_journal_required")
		}
		if journal.Close() != nil {
			return setupFail("journal_unavailable")
		}
	}
	proxy, err := load(args.config, args.journal)
	if err != nil {
		return setupFail("approved_route_unavailable")
	}
	if args.cleanup {
		result := cleanupOnly(parent, proxy, time.Now, waitContext)
		if writePrivate(output, result) != nil {
			return fail("output_unavailable")
		}
		_ = json.NewEncoder(stdout).Encode(result)
		if result.StillPending {
			return 1
		}
		return 0
	}
	ctx, cancel := context.WithTimeout(parent, runLimit)
	defer cancel()
	started := time.Now()
	result := runEvaluation(ctx, data, cases, recommendmodel.New(proxy), runOptions{now: time.Now, wait: waitContext,
		cleanupPending: func() bool { return proxy.Status("recommendation_model").CommandID != "" },
		checkpoint: func(report evaluation) error {
			if err := writePrivate(output, report); err != nil {
				return err
			}
			current := report.Cases[len(report.Cases)-1]
			return json.NewEncoder(stdout).Encode(map[string]any{"stage": "case_completed", "case_id": current.CaseID, "protocol_status": current.Protocol,
				"semantic_status": current.Semantic, "transport_status": current.Transport, "cleanup_status": current.Cleanup, "elapsed_ms": current.ElapsedMS})
		}})
	proxy.Close()
	result.PendingAfterClose = proxy.Status("recommendation_model").CommandID != ""
	result.ElapsedMS = time.Since(started).Milliseconds()
	if writePrivate(output, result) != nil {
		return fail("output_unavailable")
	}
	_ = json.NewEncoder(stdout).Encode(map[string]any{"stage": "evaluation_finished", "stop_reason": result.StopReason, "metrics": result.Metrics, "cleanup_pending_after_close": result.PendingAfterClose})
	if result.StopReason != "completed" {
		return 1
	}
	return 0
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	code := execute(ctx, os.Args[1:], os.Stdout, os.Stderr, func(config, journal string) (*rta.Proxy, error) { return rta.Load(config, journal) })
	stop()
	os.Exit(code)
}
