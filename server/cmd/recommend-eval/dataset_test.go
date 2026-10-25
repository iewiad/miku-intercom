package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

func encodeDataset(t *testing.T, data dataset) []byte {
	t.Helper()
	encoded, err := json.Marshal(data)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}
func TestDatasetStrictSchemaBoundsAndAuthorizationLabels(t *testing.T) {
	data := fixtureDataset()
	if _, err := readDataset(bytes.NewReader(encodeDataset(t, data))); err != nil {
		t.Fatal("valid synthetic dataset rejected")
	}
	for _, mutate := range []func(*dataset){
		func(d *dataset) { d.Version = 2 },
		func(d *dataset) { d.Cases[0].Need = strings.Repeat("中", 513) },
		func(d *dataset) { d.Cases[0].Need = "含\n控制" },
		func(d *dataset) { d.Cases[0].ID = "PRIVATE/path" },
		func(d *dataset) { d.Cases[0].AcceptableKeys = []string{"foreign"} },
		func(d *dataset) { d.Cases[0].Expected = "abstain" },
		func(d *dataset) { d.Cases[0].AcceptableKeys = nil },
		func(d *dataset) { d.Cases = append(d.Cases, d.Cases[0]) },
		func(d *dataset) {
			for len(d.Cases) < 61 {
				c := d.Cases[0]
				c.ID += strings.Repeat("x", len(d.Cases))
				d.Cases = append(d.Cases, c)
			}
		},
		func(d *dataset) {
			p := d.Profiles["synthetic_profile"]
			p.Groups[0].Name = p.Groups[1].Name
			d.Profiles["synthetic_profile"] = p
		},
	} {
		fresh := fixtureDataset()
		mutate(&fresh)
		if _, err := readDataset(bytes.NewReader(encodeDataset(t, fresh))); err == nil {
			t.Fatal("invalid dataset or unauthorized expected label accepted")
		}
	}
	for _, raw := range []string{`{"version":1,"version":1,"profiles":{},"cases":[]}`, `{"version":1,"profiles":{},"cases":[],"PRIVATE_UNKNOWN":true}`, strings.Repeat(" ", maxDatasetBytes+1), `{"profiles":{` + strings.Repeat("[", 20)} {
		if _, err := readDataset(strings.NewReader(raw)); err == nil {
			t.Fatal("duplicate, unknown, oversized or deep dataset accepted")
		}
	}
}

func TestCaseSelectionIsBoundedAndRetainsDatasetOrder(t *testing.T) {
	data := fixtureDataset()
	data.Cases = append(data.Cases, data.Cases[0])
	data.Cases[1].ID = "C02"
	cases, err := selectCases(data, 60, []string{"C02", "C01"})
	if err != nil || len(cases) != 2 || cases[0].ID != "C01" || cases[1].ID != "C02" {
		t.Fatal("case subset changed dataset order")
	}
	for _, limit := range []int{0, 61, -1} {
		if _, err = selectCases(data, limit, nil); err == nil {
			t.Fatal("unbounded case limit accepted")
		}
	}
	for _, ids := range [][]string{{"missing"}, {"C01", "C01"}} {
		if _, err = selectCases(data, 60, ids); err == nil {
			t.Fatal("unknown/duplicate selection accepted")
		}
	}
}
