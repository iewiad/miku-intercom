package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"regexp"
	"strings"
	"unicode"
	"unicode/utf8"
)

const maxDatasetBytes = 1024 * 1024
const maxCases = 60

var slug = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$`)
var errDataset = errors.New("dataset_invalid")

type group struct {
	Key     string `json:"key"`
	Name    string `json:"name"`
	Purpose string `json:"purpose"`
	Joined  bool   `json:"joined"`
}
type profile struct {
	Groups []group `json:"groups"`
}
type evalCase struct {
	ID             string   `json:"id"`
	Category       string   `json:"category"`
	Profile        string   `json:"profile"`
	Intent         string   `json:"intent,omitempty"`
	Need           string   `json:"need"`
	AcceptableKeys []string `json:"acceptable_keys"`
	Expected       string   `json:"expected"`
	RequireAll     bool     `json:"require_all"`
	Notes          string   `json:"notes"`
}
type dataset struct {
	Version  int                `json:"version"`
	Profiles map[string]profile `json:"profiles"`
	Cases    []evalCase         `json:"cases"`
}

func validText(value string, maximum int, empty bool) bool {
	if !utf8.ValidString(value) || strings.TrimSpace(value) != value || utf8.RuneCountInString(value) > maximum || (!empty && value == "") {
		return false
	}
	for _, r := range value {
		if unicode.IsControl(r) {
			return false
		}
	}
	return true
}

// Reject duplicate keys and excessive structure before typed decoding. Labels
// remain local data; only control.RecommendationInput reaches the model.
func uniqueJSON(data []byte) bool {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	nodes := 0
	var value func(int) bool
	value = func(depth int) bool {
		nodes++
		if depth > 10 || nodes > 20000 {
			return false
		}
		token, err := decoder.Token()
		if err != nil {
			return false
		}
		delimiter, container := token.(json.Delim)
		if !container {
			return true
		}
		if delimiter != '{' && delimiter != '[' {
			return false
		}
		keys := map[string]bool{}
		for decoder.More() {
			if delimiter == '{' {
				key, err := decoder.Token()
				text, ok := key.(string)
				if err != nil || !ok || keys[text] || len(keys) >= 128 {
					return false
				}
				keys[text] = true
			}
			if !value(depth + 1) {
				return false
			}
		}
		end, err := decoder.Token()
		return err == nil && (delimiter == '{' && end == json.Delim('}') || delimiter == '[' && end == json.Delim(']'))
	}
	if !value(0) {
		return false
	}
	_, err := decoder.Token()
	return err == io.EOF
}

func readDataset(reader io.Reader) (dataset, error) {
	var result dataset
	data, err := io.ReadAll(io.LimitReader(reader, maxDatasetBytes+1))
	if err != nil || len(data) > maxDatasetBytes || !utf8.Valid(data) || !uniqueJSON(data) {
		return result, errDataset
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&result) != nil || result.Version != 1 || len(result.Profiles) == 0 || len(result.Profiles) > maxCases || len(result.Cases) == 0 || len(result.Cases) > maxCases {
		return dataset{}, errDataset
	}
	for id, p := range result.Profiles {
		if !slug.MatchString(id) || len(p.Groups) == 0 || len(p.Groups) > 32 {
			return dataset{}, errDataset
		}
		keys, names, joined := map[string]bool{}, map[string]bool{}, 0
		for _, g := range p.Groups {
			if !slug.MatchString(g.Key) || keys[g.Key] || names[g.Name] || !validText(g.Name, 32, false) || !validText(g.Purpose, 256, true) {
				return dataset{}, errDataset
			}
			keys[g.Key], names[g.Name] = true, true
			if g.Joined {
				joined++
			}
		}
		if joined == 0 {
			return dataset{}, errDataset
		}
	}
	ids := map[string]bool{}
	for _, c := range result.Cases {
		p, ok := result.Profiles[c.Profile]
		if !ok || !slug.MatchString(c.ID) || ids[c.ID] || !slug.MatchString(c.Category) || !validText(c.Need, 512, false) || !utf8.ValidString(c.Notes) || utf8.RuneCountInString(c.Notes) > 2000 || len(c.AcceptableKeys) > 3 {
			return dataset{}, errDataset
		}
		if c.Intent != "" && c.Intent != "voice_target" || c.Intent == "voice_target" && (utf8.RuneCountInString(c.Need) > 256 || len(c.Need) > 1024) {
			return dataset{}, errDataset
		}
		ids[c.ID] = true
		if c.Expected != "recommend" && c.Expected != "abstain" || c.Expected == "abstain" && (len(c.AcceptableKeys) != 0 || c.RequireAll) || c.Expected == "recommend" && len(c.AcceptableKeys) == 0 {
			return dataset{}, errDataset
		}
		keys := map[string]bool{}
		for _, key := range c.AcceptableKeys {
			found := false
			for _, g := range p.Groups {
				found = found || g.Key == key && g.Joined
			}
			if !found || keys[key] {
				return dataset{}, errDataset
			}
			keys[key] = true
		}
	}
	return result, nil
}

func selectCases(data dataset, limit int, wanted []string) ([]evalCase, error) {
	if limit < 1 || limit > maxCases {
		return nil, errors.New("case_limit_invalid")
	}
	selected := map[string]bool{}
	for _, id := range wanted {
		if !slug.MatchString(id) || selected[id] {
			return nil, errors.New("case_selection_invalid")
		}
		selected[id] = true
	}
	var cases []evalCase
	for _, c := range data.Cases {
		if len(wanted) == 0 || selected[c.ID] {
			cases = append(cases, c)
			delete(selected, c.ID)
		}
	}
	if len(selected) != 0 || len(cases) == 0 {
		return nil, errors.New("case_selection_invalid")
	}
	if len(cases) > limit {
		cases = cases[:limit]
	}
	return cases, nil
}
