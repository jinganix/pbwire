// Command conformance-go-testee answers the official protobuf conformance
// runner over stdin/stdout: each request is a 4-byte little-endian length
// prefix followed by a serialized conformance.ConformanceRequest, each reply
// the same framing around a conformance.ConformanceResponse. The schema
// comes from the descriptor set path in argv[1], so no message type is
// compiled in: the request names the type, the set answers with the DType.
//
// Supported: binary, JSON and text in, binary, JSON or text out. JSPB and
// unknown output formats answer "skipped", like every other testee here.
//
// Usage: conformance-go-testee build/conformance.desc (from the repo root).
package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"

	"pbwire/runtime"
)

const (
	formatProtobuf = 1
	formatJSON     = 2
	formatText     = 4

	categoryJSONIgnoreUnknown = 3
)

func readFrame(r io.Reader) ([]byte, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	body := make([]byte, binary.LittleEndian.Uint32(hdr[:]))
	if _, err := io.ReadFull(r, body); err != nil {
		return nil, fmt.Errorf("short request body: %w", err)
	}
	return body, nil
}

func writeFrame(w io.Writer, payload []byte) error {
	var hdr [4]byte
	binary.LittleEndian.PutUint32(hdr[:], uint32(len(payload)))
	if _, err := w.Write(hdr[:]); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

// respond encodes one result field and writes the framed response. Encoding
// the response itself cannot fail on these small string/bytes fields; a
// failure is reported as runtime_error through a best-effort second encode.
func respond(w io.Writer, restype *runtime.DType, field string, value string, raw []byte) {
	resp := restype.New()
	var err error
	if raw != nil {
		err = resp.SetBytes(field, raw)
	} else {
		err = resp.SetString(field, value)
	}
	var payload []byte
	if err != nil {
		payload, _ = failsafe(restype, "cannot set response field: "+err.Error())
	} else if payload, err = restype.Encode(resp); err != nil {
		payload, _ = failsafe(restype, "cannot encode response: "+err.Error())
	}
	_ = writeFrame(w, payload)
}

func failsafe(restype *runtime.DType, msg string) ([]byte, error) {
	resp := restype.New()
	_ = resp.SetString("runtime_error", msg)
	return restype.Encode(resp)
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: conformance-go-testee <conformance.desc>")
		os.Exit(2)
	}
	set, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, "cannot read descriptor set:", err)
		os.Exit(2)
	}
	schema, err := runtime.Load(set)
	if err != nil {
		fmt.Fprintln(os.Stderr, "cannot load descriptor set:", err)
		os.Exit(2)
	}
	reqtype, err := schema.Type("conformance.ConformanceRequest")
	if err != nil {
		fmt.Fprintln(os.Stderr, "descriptor set has no ConformanceRequest:", err)
		os.Exit(2)
	}
	restype, err := schema.Type("conformance.ConformanceResponse")
	if err != nil {
		fmt.Fprintln(os.Stderr, "descriptor set has no ConformanceResponse:", err)
		os.Exit(2)
	}
	types := map[string]*runtime.DType{}
	messageOf := func(name string) (*runtime.DType, error) {
		if t, ok := types[name]; ok {
			return t, nil
		}
		t, err := schema.Type(name)
		if err != nil {
			return nil, err
		}
		types[name] = t
		return t, nil
	}

	stdin, stdout := os.Stdin, os.Stdout
	for {
		body, err := readFrame(stdin)
		if err != nil {
			if err == io.EOF || err == io.ErrUnexpectedEOF {
				return
			}
			respond(stdout, restype, "runtime_error", "cannot parse ConformanceRequest: "+err.Error(), nil)
			continue
		}
		req, err := reqtype.Decode(body)
		if err != nil {
			respond(stdout, restype, "runtime_error", "cannot parse ConformanceRequest: "+err.Error(), nil)
			continue
		}
		wanted, _, _ := req.GetInt("requested_output_format")
		name, _, _ := req.GetString("message_type")
		category, _, _ := req.GetInt("test_category")
		printUnknown, _, _ := req.GetBool("print_unknown_fields")

		if wanted != formatProtobuf && wanted != formatJSON && wanted != formatText {
			respond(stdout, restype, "skipped", fmt.Sprintf("output format %d is not supported", wanted), nil)
			continue
		}
		msgtype, err := messageOf(name)
		if err != nil {
			respond(stdout, restype, "runtime_error", fmt.Sprintf("unknown message type %q", name), nil)
			continue
		}
		var parsed *runtime.DMsg
		if has, _ := req.Has("json_payload"); has {
			text, _, _ := req.GetString("json_payload")
			parsed, err = msgtype.UnmarshalJSON(text, category == categoryJSONIgnoreUnknown)
		} else if has, _ := req.Has("text_payload"); has {
			text, _, _ := req.GetString("text_payload")
			parsed, err = msgtype.UnmarshalText(text)
		} else if has, _ := req.Has("protobuf_payload"); has {
			wire, _, _ := req.GetBytes("protobuf_payload")
			parsed, err = msgtype.Decode(wire)
		} else {
			respond(stdout, restype, "skipped", "unsupported payload", nil)
			continue
		}
		if err != nil {
			respond(stdout, restype, "parse_error", err.Error(), nil)
			continue
		}
		switch wanted {
		case formatJSON:
			var text string
			text, err = msgtype.MarshalJSON(parsed)
			if err == nil {
				respond(stdout, restype, "json_payload", text, nil)
			}
		case formatText:
			var text string
			text, err = msgtype.MarshalText(parsed, printUnknown)
			if err == nil {
				respond(stdout, restype, "text_payload", text, nil)
			}
		default:
			var wire []byte
			wire, err = msgtype.Encode(parsed)
			if err == nil {
				respond(stdout, restype, "protobuf_payload", "", wire)
			}
		}
		if err != nil {
			respond(stdout, restype, "serialize_error", err.Error(), nil)
		}
	}
}
