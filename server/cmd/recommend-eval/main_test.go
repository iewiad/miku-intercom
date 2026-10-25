package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/rta"
)

func cliDatasetFile(t *testing.T) (string, string) {
	t.Helper()
	dir := t.TempDir()
	if err := os.Chmod(dir, 0700); err != nil {
		t.Fatal(err)
	}
	data := fixtureDataset()
	for _, id := range []string{"C02", "C03"} {
		c := data.Cases[0]
		c.ID = id
		data.Cases = append(data.Cases, c)
	}
	path := filepath.Join(dir, "synthetic-dataset.json")
	if err := os.WriteFile(path, encodeDataset(t, data), 0600); err != nil {
		t.Fatal(err)
	}
	return dir, path
}

func cliErrorCode(t *testing.T, stream *bytes.Buffer, want string) {
	t.Helper()
	var got map[string]string
	if json.Unmarshal(stream.Bytes(), &got) != nil || len(got) != 1 || got["error_code"] != want {
		t.Fatalf("expected fixed error code %q", want)
	}
}

func cliNoPrivateEcho(t *testing.T, output ...[]byte) {
	t.Helper()
	for _, data := range output {
		for _, secret := range []string{"PRIVATE", "fixture-route-secret", "fixture-existing-bytes", "synthetic-dataset.json", "voice-secret-config.json"} {
			if bytes.Contains(data, []byte(secret)) {
				t.Fatal("CLI/report echoed private fixture input or path")
			}
		}
	}
}

func TestCLIDefaultOnlyValidatesAndNeverOpensRouteOrCreatesFiles(t *testing.T) {
	dir, datasetPath := cliDatasetFile(t)
	for _, selection := range []struct {
		name string
		args []string
		want int
	}{
		{"default", nil, 3},
		{"bounded", []string{"--max-cases", "1"}, 1},
		{"comma_subset", []string{"--case-id", "C03,C01"}, 2},
		{"repeat_subset", []string{"--case-id", "C03", "--case-id", "C01", "--max-cases", "1"}, 1},
	} {
		t.Run(selection.name, func(t *testing.T) {
			missing := filepath.Join(dir, "PRIVATE-not-created", selection.name)
			args := []string{"--dataset", datasetPath, "--voice-config", filepath.Join(missing, "voice-secret-config.json"), "--journal", filepath.Join(missing, "journal.json"), "--output", filepath.Join(missing, "report.json")}
			args = append(args, selection.args...)
			var stdout, stderr bytes.Buffer
			loadCalls := 0
			code := execute(context.Background(), args, &stdout, &stderr, func(string, string) (*rta.Proxy, error) {
				loadCalls++
				return nil, errors.New("fixture-route-secret")
			})
			if code != 0 || loadCalls != 0 || stderr.Len() != 0 {
				t.Fatal("validation attempted route setup or failed")
			}
			var report struct {
				Stage         string `json:"stage"`
				CasesSelected int    `json:"cases_selected"`
				ModelCalls    int    `json:"model_calls"`
			}
			if json.Unmarshal(stdout.Bytes(), &report) != nil || report.Stage != "validated_not_run" || report.CasesSelected != selection.want || report.ModelCalls != 0 {
				t.Fatal("validation/selection metadata differs")
			}
			if _, err := os.Lstat(missing); !errors.Is(err, os.ErrNotExist) {
				t.Fatal("dry-run created a private output/config/journal parent")
			}
			cliNoPrivateEcho(t, stdout.Bytes(), stderr.Bytes())
		})
	}
}

func TestCLIInvalidArgumentsAndInputsUseFixedErrors(t *testing.T) {
	dir, datasetPath := cliDatasetFile(t)
	for _, test := range []struct {
		name, want string
		args       []string
	}{
		{"empty", "invalid_arguments", nil},
		{"unknown_flag", "invalid_arguments", []string{"--PRIVATE-secret=fixture-route-secret"}},
		{"positional", "invalid_arguments", []string{"--dataset", datasetPath, "PRIVATE-positional"}},
		{"bad_number", "invalid_arguments", []string{"--dataset", datasetPath, "--max-cases", "PRIVATE-number"}},
		{"zero_limit", "invalid_arguments", []string{"--dataset", datasetPath, "--max-cases", "0"}},
		{"large_limit", "invalid_arguments", []string{"--dataset", datasetPath, "--max-cases", "61"}},
		{"missing_live_paths", "invalid_arguments", []string{"--dataset", datasetPath, "--live"}},
		{"conflicting_modes", "invalid_arguments", []string{"--live", "--cleanup-only"}},
		{"cleanup_dataset", "invalid_arguments", []string{"--cleanup-only", "--dataset", datasetPath}},
		{"unknown_case", "case_selection_invalid", []string{"--dataset", datasetPath, "--case-id", "PRIVATE-case"}},
		{"duplicate_case", "case_selection_invalid", []string{"--dataset", datasetPath, "--case-id", "C01,C01"}},
		{"missing_dataset", "dataset_unavailable", []string{"--dataset", filepath.Join(dir, "PRIVATE-missing")}},
		{"directory_dataset", "dataset_unavailable", []string{"--dataset", dir}},
		{"aliased_paths", "paths_must_be_distinct", []string{"--dataset", datasetPath, "--output", filepath.Join(dir, "unused", "..", filepath.Base(datasetPath))}},
	} {
		t.Run(test.name, func(t *testing.T) {
			var stdout, stderr bytes.Buffer
			called := false
			code := execute(context.Background(), test.args, &stdout, &stderr, func(string, string) (*rta.Proxy, error) {
				called = true
				return nil, errors.New("fixture-route-secret")
			})
			if code != 2 || called || stdout.Len() != 0 {
				t.Fatal("invalid CLI input reached the loader or produced non-error output")
			}
			cliErrorCode(t, &stderr, test.want)
			cliNoPrivateEcho(t, stdout.Bytes(), stderr.Bytes())
		})
	}
	t.Run("help", func(t *testing.T) {
		var stdout, stderr bytes.Buffer
		code := execute(context.Background(), []string{"--help", "PRIVATE-unparsed"}, &stdout, &stderr, func(string, string) (*rta.Proxy, error) { t.Fatal("help loaded route"); return nil, nil })
		if code != 0 || stderr.Len() != 0 || !strings.Contains(stdout.String(), "Without --live") {
			t.Fatal("help should be a bounded fixed usage message")
		}
		cliNoPrivateEcho(t, stdout.Bytes(), stderr.Bytes())
	})
	for _, name := range []string{"symlink", "malformed"} {
		t.Run(name+"_dataset", func(t *testing.T) {
			path := filepath.Join(dir, "PRIVATE-"+name)
			var err error
			if name == "symlink" {
				err = os.Symlink(datasetPath, path)
			} else {
				err = os.WriteFile(path, []byte(`{"PRIVATE-invalid":`), 0600)
			}
			if err != nil {
				t.Fatal(err)
			}
			var stdout, stderr bytes.Buffer
			code := execute(context.Background(), []string{"--dataset", path}, &stdout, &stderr, func(string, string) (*rta.Proxy, error) { t.Fatal("invalid dataset loaded route"); return nil, nil })
			if code != 2 {
				t.Fatal("invalid dataset accepted")
			}
			want := "dataset_invalid"
			if name == "symlink" {
				want = "dataset_unavailable"
			}
			cliErrorCode(t, &stderr, want)
			cliNoPrivateEcho(t, stdout.Bytes(), stderr.Bytes())
		})
	}
}

func TestCLILiveSetupReservesPrivateFilesAndSanitizesLoaderFailure(t *testing.T) {
	dir, datasetPath := cliDatasetFile(t)
	output, journal, config := filepath.Join(dir, "report.json"), filepath.Join(dir, "journal.json"), filepath.Join(dir, "voice-secret-config.json")
	var stdout, stderr bytes.Buffer
	calls := 0
	code := execute(context.Background(), []string{"--dataset", datasetPath, "--live", "--voice-config", config, "--journal", journal, "--output", output}, &stdout, &stderr, func(gotConfig, gotJournal string) (*rta.Proxy, error) {
		calls++
		if gotConfig != config || gotJournal != journal {
			t.Fatal("loader did not receive explicit paths")
		}
		for _, path := range []string{output, journal} {
			info, err := os.Stat(path)
			if err != nil || info.Mode().Perm() != 0600 || !info.Mode().IsRegular() {
				t.Fatal("files were not exclusively reserved as 0600 before loader")
			}
		}
		return nil, errors.New("PRIVATE fixture-route-secret " + config)
	})
	if code != 2 || calls != 1 || stdout.Len() != 0 {
		t.Fatal("offline injected setup failure did not stop execution")
	}
	cliErrorCode(t, &stderr, "approved_route_unavailable")
	contents, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	var report map[string]string
	if json.Unmarshal(contents, &report) != nil || len(report) != 1 || report["error_code"] != "approved_route_unavailable" {
		t.Fatal("setup failure report was not fixed metadata")
	}
	if info, err := os.Stat(journal); err != nil || info.Size() != 0 {
		t.Fatal("fake loader setup mutated the empty reserved journal")
	}
	if _, err := os.Lstat(config); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("CLI unexpectedly created/read a real config in the offline test")
	}
	cliNoPrivateEcho(t, contents, stdout.Bytes(), stderr.Bytes())
}

func TestCLIExclusiveFilesAndPrivateParentsNeverOverwrite(t *testing.T) {
	for _, scenario := range []string{"existing_output", "existing_journal", "symlink_output", "symlink_journal", "public_output_parent", "public_journal_parent", "symlink_parent"} {
		t.Run(scenario, func(t *testing.T) {
			dir, datasetPath := cliDatasetFile(t)
			output, journal := filepath.Join(dir, "report.json"), filepath.Join(dir, "journal.json")
			keep := filepath.Join(dir, "existing.json")
			const original = "fixture-existing-bytes"
			if err := os.WriteFile(keep, []byte(original), 0600); err != nil {
				t.Fatal(err)
			}
			want := "output_unavailable"
			switch scenario {
			case "existing_output":
				output = keep
			case "existing_journal":
				journal, want = keep, "new_journal_required"
			case "symlink_output", "symlink_journal":
				link := filepath.Join(dir, "link.json")
				if err := os.Symlink(keep, link); err != nil {
					t.Fatal(err)
				}
				if scenario == "symlink_output" {
					output = link
				} else {
					journal, want = link, "new_journal_required"
				}
			case "public_output_parent", "public_journal_parent":
				public := filepath.Join(dir, "public")
				if err := os.Mkdir(public, 0755); err != nil {
					t.Fatal(err)
				}
				if err := os.Chmod(public, 0755); err != nil {
					t.Fatal(err)
				}
				if scenario == "public_output_parent" {
					output = filepath.Join(public, "report.json")
				} else {
					journal, want = filepath.Join(public, "journal.json"), "new_journal_required"
				}
			case "symlink_parent":
				link := filepath.Join(dir, "directory-link")
				if err := os.Symlink(dir, link); err != nil {
					t.Fatal(err)
				}
				output = filepath.Join(link, "report.json")
			}
			var stdout, stderr bytes.Buffer
			called := false
			code := execute(context.Background(), []string{"--dataset", datasetPath, "--live", "--voice-config", filepath.Join(dir, "voice-secret-config.json"), "--journal", journal, "--output", output}, &stdout, &stderr, func(string, string) (*rta.Proxy, error) {
				called = true
				return nil, errors.New("fixture-route-secret")
			})
			if code != 2 || called {
				t.Fatal("unsafe setup reached the route loader")
			}
			cliErrorCode(t, &stderr, want)
			contents, err := os.ReadFile(keep)
			if err != nil || string(contents) != original {
				t.Fatal("existing output/journal or symlink target was overwritten")
			}
			cliNoPrivateEcho(t, stdout.Bytes(), stderr.Bytes())
		})
	}
}

func TestCLICleanupOnlyRejectsUnrelatedOrNonPrivateJournalBeforeLoading(t *testing.T) {
	for _, scenario := range []string{"unrelated_device", "missing_binding", "missing_records", "malformed", "public_journal", "valid_evaluation_journal"} {
		t.Run(scenario, func(t *testing.T) {
			dir := t.TempDir()
			if err := os.Chmod(dir, 0700); err != nil {
				t.Fatal(err)
			}
			journal, output := filepath.Join(dir, "journal.json"), filepath.Join(dir, "report.json")
			body := `{"binding":"PRIVATE-binding","records":{"PRIVATE-record":{"device":"recommendation_model"}}}`
			mode := os.FileMode(0600)
			switch scenario {
			case "unrelated_device":
				body = `{"binding":"PRIVATE-binding","records":{"PRIVATE-record":{"device":"ordinary_product_device"}}}`
			case "missing_binding":
				body = `{"records":{}}`
			case "missing_records":
				body = `{"binding":"PRIVATE-binding"}`
			case "malformed":
				body = `{"PRIVATE-unfinished":`
			case "public_journal":
				mode = 0644
			}
			if err := os.WriteFile(journal, []byte(body), mode); err != nil {
				t.Fatal(err)
			}
			if err := os.Chmod(journal, mode); err != nil {
				t.Fatal(err)
			}
			var stdout, stderr bytes.Buffer
			calls := 0
			code := execute(context.Background(), []string{"--cleanup-only", "--voice-config", filepath.Join(dir, "voice-secret-config.json"), "--journal", journal, "--output", output}, &stdout, &stderr, func(string, string) (*rta.Proxy, error) {
				calls++
				return nil, errors.New("PRIVATE fixture-route-secret")
			})
			want := "evaluation_journal_required"
			if scenario == "valid_evaluation_journal" {
				want = "approved_route_unavailable"
				if calls != 1 {
					t.Fatal("valid evaluation journal did not reach injected loader")
				}
			} else if calls != 0 {
				t.Fatal("unrelated or unsafe journal reached route loader")
			}
			if code != 2 {
				t.Fatal("offline cleanup setup should return the fixed failure")
			}
			cliErrorCode(t, &stderr, want)
			contents, err := os.ReadFile(journal)
			if err != nil || string(contents) != body {
				t.Fatal("cleanup setup changed the existing journal")
			}
			report, err := os.ReadFile(output)
			if err != nil {
				t.Fatal(err)
			}
			cliNoPrivateEcho(t, report, stdout.Bytes(), stderr.Bytes())
		})
	}
}

type mainTestCleanupProxy struct {
	t           *testing.T
	originalID  string
	activeID    string
	unknown     bool
	successAt   int
	stopCalls   int
	closeCalls  int
	statusCalls int
}

func (p *mainTestCleanupProxy) Status(device string) rta.Status {
	p.t.Helper()
	if device != "recommendation_model" {
		p.t.Fatal("cleanup requested an unrelated identity")
	}
	p.statusCalls++
	phase := "stop_unconfirmed"
	if p.unknown {
		phase = "creating"
	}
	return rta.Status{Available: true, CommandID: p.activeID, Phase: phase}
}
func (p *mainTestCleanupProxy) StopContext(ctx context.Context, device, id string) error {
	p.t.Helper()
	if device != "recommendation_model" || id != p.originalID || ctx == nil {
		p.t.Fatal("cleanup changed the original in-memory identity or command")
	}
	p.stopCalls++
	if !p.unknown && p.successAt > 0 && p.stopCalls >= p.successAt {
		p.activeID = ""
		return nil
	}
	return errors.New("PRIVATE cleanup error fixture-route-secret")
}
func (p *mainTestCleanupProxy) Close() { p.closeCalls++ }

func TestCleanupOnlyKnownCommandRetriesAndConfirmsWithoutIdentifiers(t *testing.T) {
	const original = "PRIVATE-original-command"
	proxy := &mainTestCleanupProxy{t: t, originalID: original, activeID: original, successAt: 3}
	clock := time.Unix(100, 0)
	waits := 0
	report := cleanupOnly(context.Background(), proxy, func() time.Time { return clock }, func(ctx context.Context, duration time.Duration) error {
		if ctx.Err() != nil || duration != 250*time.Millisecond {
			t.Fatal("cleanup retry did not respect bounded cancellable wait")
		}
		waits++
		clock = clock.Add(duration)
		return nil
	})
	if !report.HadActive || !report.Attempted || !report.StopConfirmed || report.StillPending || report.ModelCalls != 0 || report.Stage != "cleanup_only" || report.ElapsedMS != 500 || proxy.stopCalls != 3 || proxy.closeCalls != 1 || waits != 2 {
		t.Fatal("known pending cleanup did not retry then confirm")
	}
	encoded, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	cliNoPrivateEcho(t, encoded)
	for _, forbidden := range []string{original, "command_id", "session_id", "token", "device"} {
		if bytes.Contains(encoded, []byte(forbidden)) {
			t.Fatal("cleanup report contains an identifier or credential field")
		}
	}
}

func TestCleanupOnlyUnknownCreationStaysPendingAndNeverCreatesModel(t *testing.T) {
	const original = "PRIVATE-unknown-create"
	proxy := &mainTestCleanupProxy{t: t, originalID: original, activeID: original, unknown: true}
	clock := time.Unix(100, 0)
	waits := 0
	report := cleanupOnly(context.Background(), proxy, func() time.Time { return clock }, func(context.Context, time.Duration) error {
		waits++
		clock = clock.Add(250 * time.Millisecond)
		if waits == 2 {
			return context.DeadlineExceeded
		}
		return nil
	})
	if !report.HadActive || !report.Attempted || report.StopConfirmed || !report.StillPending || report.ModelCalls != 0 || proxy.stopCalls != 2 || proxy.closeCalls != 1 || proxy.activeID != original {
		t.Fatal("unknown creation was lost or counted as confirmed cleanup")
	}
	encoded, _ := json.Marshal(report)
	cliNoPrivateEcho(t, encoded)
}

func TestCleanupOnlyCancellationAlwaysClosesOriginalProxy(t *testing.T) {
	for _, alreadyCanceled := range []bool{false, true} {
		t.Run(map[bool]string{false: "during_retry", true: "before_cleanup"}[alreadyCanceled], func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			proxy := &mainTestCleanupProxy{t: t, originalID: "PRIVATE-original-command", activeID: "PRIVATE-original-command", unknown: true}
			if alreadyCanceled {
				cancel()
			}
			report := cleanupOnly(ctx, proxy, time.Now, func(context.Context, time.Duration) error { cancel(); return context.Canceled })
			wantStops := 1
			if alreadyCanceled {
				wantStops = 0
			}
			if proxy.closeCalls != 1 || proxy.stopCalls != wantStops || report.ModelCalls != 0 || !report.StillPending || report.StopConfirmed {
				t.Fatal("cancellation skipped Close or created/replaced a pending session")
			}
		})
	}
	proxy := &mainTestCleanupProxy{t: t}
	report := cleanupOnly(context.Background(), proxy, time.Now, func(context.Context, time.Duration) error { t.Fatal("empty cleanup waited"); return nil })
	if report.HadActive || report.Attempted || !report.StopConfirmed || report.StillPending || report.ModelCalls != 0 || proxy.stopCalls != 0 || proxy.closeCalls != 1 {
		t.Fatal("no-active cleanup should close without a stop or model call")
	}
}
