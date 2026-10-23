package media

import "errors"

// opusSamples validates the RFC 6716 packet framing and returns its duration in
// the mandatory 48 kHz RTP clock. No samples are decoded or transcoded.
func opusSamples(payload []byte) (uint32, error) {
	bad := errors.New("invalid Opus packet")
	if len(payload) == 0 {
		return 0, bad
	}
	config := payload[0] >> 3
	var per int
	switch {
	case config >= 16:
		per = 120 << uint(config&3)
	case config >= 12:
		per = 480 << uint(config&1)
	case config&3 == 3:
		per = 2880
	default:
		per = 480 << uint(config&3)
	}
	data := payload[1:]
	count := 1
	frameOK := func(n int) bool { return n >= 0 && n <= 1275 }
	readLength := func(b []byte) (int, int, bool) {
		if len(b) == 0 {
			return 0, 0, false
		}
		if b[0] < 252 {
			return int(b[0]), 1, true
		}
		if len(b) < 2 {
			return 0, 0, false
		}
		return int(b[0]) + 4*int(b[1]), 2, true
	}
	switch payload[0] & 3 {
	case 0:
		if !frameOK(len(data)) {
			return 0, bad
		}
	case 1:
		count = 2
		if len(data)%2 != 0 || !frameOK(len(data)/2) {
			return 0, bad
		}
	case 2:
		count = 2
		n, used, ok := readLength(data)
		if !ok || n > len(data)-used || !frameOK(n) || !frameOK(len(data)-used-n) {
			return 0, bad
		}
	case 3:
		if len(data) == 0 {
			return 0, bad
		}
		flags := data[0]
		data = data[1:]
		count = int(flags & 63)
		if count == 0 || count > 48 || per*count > 5760 {
			return 0, bad
		}
		if flags&64 != 0 {
			padding := 0
			for {
				if len(data) == 0 {
					return 0, bad
				}
				n := int(data[0])
				data = data[1:]
				if n == 255 {
					padding += 254
				} else {
					padding += n
				}
				if padding > len(data) {
					return 0, bad
				}
				if n != 255 {
					break
				}
			}
			data = data[:len(data)-padding]
		}
		if flags&128 == 0 {
			if len(data)%count != 0 || !frameOK(len(data)/count) {
				return 0, bad
			}
		} else {
			total := 0
			for i := 0; i < count-1; i++ {
				n, used, ok := readLength(data)
				if !ok || !frameOK(n) {
					return 0, bad
				}
				data = data[used:]
				total += n
				if total > len(data) {
					return 0, bad
				}
			}
			if !frameOK(len(data) - total) {
				return 0, bad
			}
		}
	}
	if per*count > 5760 {
		return 0, bad
	}
	return uint32(per * count), nil
}
