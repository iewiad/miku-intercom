package httpapi

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

func TestHTTPGroupPurposeCompatibilityAndValidation(t *testing.T) {
	for _, tc := range []struct {
		name, field, want, code string
		status                  int
	}{
		{name: "legacy-body", status: 201},
		{name: "null", field: `,"purpose":null`, status: 201},
		{name: "whitespace", field: `,"purpose":" \t\n　"`, status: 201},
		{name: "trimmed", field: `,"purpose":"  设备维修  "`, want: "设备维修", status: 201},
		{name: "unicode-limit", field: `,"purpose":"` + strings.Repeat("修", 256) + `"`, want: strings.Repeat("修", 256), status: 201},
		{name: "unicode-over-limit", field: `,"purpose":"` + strings.Repeat("修", 257) + `"`, status: 400, code: "invalid_purpose"},
		{name: "internal-control", field: `,"purpose":"a\u0085b"`, status: 400, code: "invalid_purpose"},
		{name: "internal-newline", field: `,"purpose":"a\nb"`, status: 400, code: "invalid_purpose"},
		{name: "wrong-type", field: `,"purpose":[]`, status: 400, code: "invalid_json"},
		{name: "invalid-utf8", field: ",\"purpose\":\"" + string([]byte{0xff}) + "\"", status: 400, code: "invalid_json"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := fixture(t)
			w := request(f.h, "POST", "/api/v1/groups", `{"name":"维修组"`+tc.field+`}`, f.tokenA, nil)
			if tc.code != "" {
				errorCode(t, w, tc.status, tc.code)
				return
			}
			out := response(t, w, tc.status)
			var group map[string]json.RawMessage
			if err := json.Unmarshal(out["group"], &group); err != nil {
				t.Fatal(err)
			}
			var got string
			if raw, ok := group["purpose"]; ok {
				if tc.want == "" {
					t.Fatal("empty purpose must be omitted")
				}
				if err := json.Unmarshal(raw, &got); err != nil {
					t.Fatal(err)
				}
			}
			if got != tc.want {
				t.Fatalf("purpose=%q want=%q", got, tc.want)
			}
		})
	}
}

func TestHTTPGroupPurposeVisibleOnlyAfterJoining(t *testing.T) {
	f := fixture(t)
	out := response(t, request(f.h, "POST", "/api/v1/groups", `{"name":"维修组","purpose":"设备维修"}`, f.tokenA, nil), 201)
	var code string
	if err := json.Unmarshal(out["code"], &code); err != nil {
		t.Fatal(err)
	}
	response(t, request(f.h, "GET", "/api/v1/state", "", f.tokenA, nil), 200)
	other := request(f.h, "GET", "/api/v1/state", "", f.tokenB, nil)
	response(t, other, 200)
	if strings.Contains(other.Body.String(), "设备维修") || strings.Contains(other.Body.String(), "维修组") {
		t.Fatal("non-member received a group or its purpose")
	}
	body, err := json.Marshal(map[string]string{"code": code})
	if err != nil {
		t.Fatal(err)
	}
	joined := request(f.h, "POST", "/api/v1/memberships", string(body), f.tokenB, nil)
	response(t, joined, 200)
	if !strings.Contains(joined.Body.String(), `"purpose":"设备维修"`) {
		t.Fatal("authorized member did not receive group purpose")
	}
}

func TestDevicePurposeViewsPreserveBrowserDirectory(t *testing.T) {
	f := fixture(t)
	device, err := NewDevice(f.s, nil, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	create := nativeRequest(device, "POST", "/api/v1/groups", `{"name":"维修组","purpose":"设备维修"}`, f.tokenA, nil)
	response(t, create, 201)
	if strings.Contains(create.Body.String(), `"purpose"`) {
		t.Fatal("native group or state exposed purpose")
	}
	out := response(t, request(f.h, "POST", "/api/v1/groups", `{"name":"后勤组","purpose":"物资运输"}`, f.tokenA, nil), 201)
	var group control.Group
	var code string
	if err := json.Unmarshal(out["group"], &group); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(out["code"], &code); err != nil {
		t.Fatal(err)
	}
	if group.Purpose != "物资运输" {
		t.Fatal("browser creation lost purpose")
	}
	join, _ := json.Marshal(map[string]string{"code": code})
	switchBody, _ := json.Marshal(control.SwitchRequest{OperationID: "purpose_switch", GroupID: group.ID, ExpectedVersion: 1})
	for _, call := range []struct {
		method, path, body, token string
	}{
		{"GET", "/api/v1/state", "", f.tokenA},
		{"POST", "/api/v1/memberships", string(join), f.tokenB},
		{"PUT", "/api/v1/current-group", string(switchBody), f.tokenA},
	} {
		w := nativeRequest(device, call.method, call.path, call.body, call.token, nil)
		response(t, w, 200)
		if strings.Contains(w.Body.String(), `"purpose"`) {
			t.Fatalf("native %s exposed purpose", call.path)
		}
	}
	browser := request(f.h, "GET", "/api/v1/state", "", f.tokenA, nil)
	response(t, browser, 200)
	if !strings.Contains(browser.Body.String(), `"purpose":"设备维修"`) || !strings.Contains(browser.Body.String(), `"purpose":"物资运输"`) {
		t.Fatal("native view modified the shared browser directory")
	}
	// Directly protect the helper's input slice too: control.State is a value
	// containing a slice, so copying only the outer struct would still mutate it.
	st, err := f.s.State(f.a.ID)
	if err != nil {
		t.Fatal(err)
	}
	_ = device.(*Server).stateView(st)
	for _, g := range st.Groups {
		if g.Purpose == "" {
			t.Fatal("native view mutated its input state's group slice")
		}
	}
}

func TestDevicePurposeDirectoryKeepsOriginal128GroupResponseSize(t *testing.T) {
	const firmwareResponseMax = 48 * 1024
	var lengths []int
	for _, purpose := range []string{"", strings.Repeat("修", 256)} {
		s, err := control.New(nil, nil, control.Options{Now: func() time.Time {
			return time.Date(2026, 10, 25, 1, 2, 3, 0, time.UTC)
		}})
		if err != nil {
			t.Fatal(err)
		}
		d, token, err := s.CreateDevice("设备")
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 128; i++ {
			name := strings.Repeat("群", 31) + string(rune('一'+i))
			if _, _, _, err := s.CreateGroupWithPurpose(d.ID, name, purpose); err != nil {
				t.Fatal(err)
			}
		}
		device, err := NewDevice(s, nil, deviceHost)
		if err != nil {
			t.Fatal(err)
		}
		w := nativeRequest(device, "GET", "/api/v1/state", "", token, nil)
		response(t, w, 200)
		if strings.Contains(w.Body.String(), `"purpose"`) || w.Body.Len() >= firmwareResponseMax {
			t.Fatal("native directory expanded beyond its original response budget")
		}
		lengths = append(lengths, w.Body.Len())
		if purpose != "" {
			browser := request(New(s), "GET", "/api/v1/state", "", token, nil)
			response(t, browser, 200)
			if strings.Count(browser.Body.String(), `"purpose"`) != 128 || browser.Body.Len() <= firmwareResponseMax {
				t.Fatal("browser directory did not preserve all long purposes")
			}
		}
	}
	if lengths[0] != lengths[1] {
		t.Fatalf("native response changed with purposes: %v", lengths)
	}
}
