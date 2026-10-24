package httpapi

import (
	"encoding/json"
	"errors"
	"unicode/utf8"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

// The JSON decoder replaces invalid UTF-8 by default. Validate this new field
// before decoding so an invalid purpose cannot be silently changed and saved.
type groupPurpose string

func (p *groupPurpose) UnmarshalJSON(data []byte) error {
	if !utf8.Valid(data) {
		return errors.New("purpose must be valid UTF-8")
	}
	var value string
	if err := json.Unmarshal(data, &value); err != nil {
		return err
	}
	*p = groupPurpose(value)
	return nil
}

func (s *Server) groupView(group control.Group) control.Group {
	if s.deviceHost != "" {
		group.Purpose = ""
	}
	return group
}

func (s *Server) stateView(state control.State) control.State {
	if s.deviceHost != "" && len(state.Groups) > 0 {
		// Native firmware has a bounded response buffer. Keep its original wire
		// shape without modifying the browser/control directory's purposes.
		state.Groups = append([]control.Group(nil), state.Groups...)
		for i := range state.Groups {
			state.Groups[i] = s.groupView(state.Groups[i])
		}
	}
	return state
}
