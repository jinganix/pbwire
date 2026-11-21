// Well-known-type helpers over the dynamic core. Mirrors the C core's
// wkt.c and the Java runtime's Wkt: google.protobuf.Any packing plus
// Timestamp/Duration millisecond conversion and FieldMask paths. Field
// numbers only (Any 1/2, seconds/nanos 1/2, paths 1), so any descriptor
// with the same shape works.
package runtime_test

import (
	"errors"
	"reflect"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/known/anypb"
	"google.golang.org/protobuf/types/known/durationpb"
	"google.golang.org/protobuf/types/known/fieldmaskpb"
	"google.golang.org/protobuf/types/known/timestamppb"

	"pbwire/runtime"
)

// loadWKTSchema builds a Schema holding the real google.protobuf.Any,
// Timestamp, Duration and FieldMask descriptors, so the helpers run
// against true WKT descriptor data.
func loadWKTSchema(t *testing.T) *runtime.Schema {
	t.Helper()
	wkts := []protoreflect.FileDescriptor{
		anypb.File_google_protobuf_any_proto,
		timestamppb.File_google_protobuf_timestamp_proto,
		durationpb.File_google_protobuf_duration_proto,
		fieldmaskpb.File_google_protobuf_field_mask_proto,
	}
	set := &descriptorpb.FileDescriptorSet{}
	for _, fd := range wkts {
		set.File = append(set.File, protodesc.ToFileDescriptorProto(fd))
	}
	raw, err := proto.Marshal(set)
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func mustWKTType(t *testing.T, s *runtime.Schema, full string) *runtime.DType {
	t.Helper()
	typ, err := s.Type(full)
	if err != nil {
		t.Fatal(err)
	}
	return typ
}

func TestWKTAnyRoundTrip(t *testing.T) {
	s := loadWKTSchema(t)
	anyType := mustWKTType(t, s, "google.protobuf.Any")
	tsType := mustWKTType(t, s, "google.protobuf.Timestamp")

	payload, err := tsType.Decode([]byte{0x08, 0x96, 0x01})
	if err != nil {
		t.Fatal(err)
	}
	wire, err := payload.Encode()
	if err != nil {
		t.Fatal(err)
	}
	url := "type.googleapis.com/google.protobuf.Timestamp"
	packed, err := runtime.AnyPack(anyType, url, wire)
	if err != nil {
		t.Fatalf("AnyPack: %v", err)
	}
	if got := runtime.AnyTypeURL(packed); got != url {
		t.Fatalf("AnyTypeURL = %q, want %q", got, url)
	}
	if got := runtime.AnyPayload(packed); !reflect.DeepEqual(got, wire) {
		t.Fatalf("AnyPayload = %x, want %x", got, wire)
	}
	back, err := runtime.AnyUnpack(packed, tsType)
	if err != nil {
		t.Fatalf("AnyUnpack: %v", err)
	}
	rewire, err := back.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(rewire, wire) {
		t.Fatalf("unpacked wire = %x, want %x", rewire, wire)
	}
	// A bare type name without any URL prefix matches by suffix too.
	bare, err := runtime.AnyPack(anyType, "google.protobuf.Timestamp", wire)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.AnyUnpack(bare, tsType); err != nil {
		t.Fatalf("AnyUnpack bare name: %v", err)
	}
	// An empty type URL skips the check, like the Java helper.
	nourl, err := runtime.AnyPack(anyType, "", wire)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.AnyUnpack(nourl, tsType); err != nil {
		t.Fatalf("AnyUnpack empty URL: %v", err)
	}
}

func TestWKTAnyUnpackWrongType(t *testing.T) {
	s := loadWKTSchema(t)
	anyType := mustWKTType(t, s, "google.protobuf.Any")
	tsType := mustWKTType(t, s, "google.protobuf.Timestamp")
	durType := mustWKTType(t, s, "google.protobuf.Duration")

	packed, err := runtime.AnyPack(anyType,
		"type.googleapis.com/google.protobuf.Timestamp", []byte{0x08, 0x01})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.AnyUnpack(packed, durType); err == nil {
		t.Fatal("AnyUnpack with wrong type should fail")
	}
	// A mere suffix of the wrong message must not match either.
	sneaky, err := runtime.AnyPack(anyType,
		"type.googleapis.com/myapp.Timestamp", []byte{0x08, 0x01})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.AnyUnpack(sneaky, tsType); err == nil {
		t.Fatal("AnyUnpack with mismatched suffix should fail")
	}
	// Missing payload fails instead of decoding empty input: an Any with
	// only the type URL set has no field 2 at all.
	bare2 := anyType.New()
	if err := bare2.SetString("type_url", "type.googleapis.com/google.protobuf.Duration"); err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.AnyUnpack(bare2, durType); err == nil {
		t.Fatal("AnyUnpack without payload should fail")
	}
}

func TestWKTTimestampMillis(t *testing.T) {
	s := loadWKTSchema(t)
	tsType := mustWKTType(t, s, "google.protobuf.Timestamp")

	cases := []struct {
		name  string
		sec   int64
		nanos int64
		want  int64
	}{
		{"epoch", 0, 0, 0},
		{"positive", 1, 500000000, 1500},
		{"negative borrowing", -1, 999000000, -1},
		{"exact negative", -2, 0, -2000},
		{"max int64", 9223372036854775, 807000000, 9223372036854775807},
	}
	for _, c := range cases {
		m := tsType.New()
		if err := m.SetInt("seconds", c.sec); err != nil {
			t.Fatal(err)
		}
		if err := m.SetInt("nanos", c.nanos); err != nil {
			t.Fatal(err)
		}
		got, err := runtime.TimestampMillis(m)
		if err != nil {
			t.Fatalf("%s: TimestampMillis: %v", c.name, err)
		}
		if got != c.want {
			t.Fatalf("%s: TimestampMillis = %d, want %d", c.name, got, c.want)
		}
		// The value round-trips back through FromMillis.
		back, err := runtime.TimestampFromMillis(tsType, got)
		if err != nil {
			t.Fatalf("%s: TimestampFromMillis: %v", c.name, err)
		}
		again, err := runtime.TimestampMillis(back)
		if err != nil {
			t.Fatalf("%s: re-read: %v", c.name, err)
		}
		if again != got {
			t.Fatalf("%s: round trip = %d, want %d", c.name, again, got)
		}
	}
	// MinInt64 ms is not a Timestamp: flooring the negative remainder
	// borrows a second below the representable range, like the C core.
	if _, err := runtime.TimestampFromMillis(tsType, -9223372036854775808); err == nil {
		t.Fatal("TimestampFromMillis(MinInt64) should fail")
	}
}

func TestWKTDurationMillis(t *testing.T) {
	s := loadWKTSchema(t)
	durType := mustWKTType(t, s, "google.protobuf.Duration")

	m := durType.New()
	if err := m.SetInt("seconds", -1); err != nil {
		t.Fatal(err)
	}
	if err := m.SetInt("nanos", -500000000); err != nil {
		t.Fatal(err)
	}
	got, err := runtime.DurationMillis(m)
	if err != nil {
		t.Fatalf("DurationMillis: %v", err)
	}
	if got != -1500 {
		t.Fatalf("DurationMillis = %d, want -1500", got)
	}
	back, err := runtime.DurationFromMillis(durType, got)
	if err != nil {
		t.Fatalf("DurationFromMillis: %v", err)
	}
	again, err := runtime.DurationMillis(back)
	if err != nil {
		t.Fatalf("re-read: %v", err)
	}
	if again != got {
		t.Fatalf("round trip = %d, want %d", again, got)
	}
	// Durations keep the sign in both fields, so MinInt64 ms round-trips.
	mn, err := runtime.DurationFromMillis(durType, -9223372036854775808)
	if err != nil {
		t.Fatalf("DurationFromMillis(MinInt64): %v", err)
	}
	mg, err := runtime.DurationMillis(mn)
	if err != nil {
		t.Fatalf("DurationMillis(MinInt64): %v", err)
	}
	if mg != -9223372036854775808 {
		t.Fatalf("MinInt64 round trip = %d", mg)
	}
}

func TestWKTMillisErrors(t *testing.T) {
	s := loadWKTSchema(t)
	tsType := mustWKTType(t, s, "google.protobuf.Timestamp")
	durType := mustWKTType(t, s, "google.protobuf.Duration")

	// Nanos past +/-999999999 are malformed wire data.
	for _, nanos := range []int64{1000000000, -1000000000} {
		m := durType.New()
		if err := m.SetInt("seconds", 0); err != nil {
			t.Fatal(err)
		}
		if err := m.SetInt("nanos", nanos); err != nil {
			t.Fatal(err)
		}
		if _, err := runtime.DurationMillis(m); !errors.Is(err, runtime.ErrWire) {
			t.Fatalf("nanos %d: want ErrWire, got %v", nanos, err)
		}
		if _, err := runtime.TimestampMillis(m); !errors.Is(err, runtime.ErrWire) {
			t.Fatalf("nanos %d: want ErrWire, got %v", nanos, err)
		}
	}
	// Missing seconds fail instead of silently reading the epoch.
	if _, err := runtime.TimestampMillis(tsType.New()); err == nil {
		t.Fatal("missing seconds should fail")
	}
	// Seconds that cannot scale to millis overflow.
	m := tsType.New()
	if err := m.SetInt("seconds", 9223372036854775+1); err != nil {
		t.Fatal(err)
	}
	if err := m.SetInt("nanos", 0); err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.TimestampMillis(m); err == nil {
		t.Fatal("overflowing seconds should fail")
	}
	// A boundary pair just past the limit fails too.
	m = tsType.New()
	if err := m.SetInt("seconds", 9223372036854775); err != nil {
		t.Fatal(err)
	}
	if err := m.SetInt("nanos", 808000000); err != nil {
		t.Fatal(err)
	}
	if _, err := runtime.TimestampMillis(m); err == nil {
		t.Fatal("boundary overflow should fail")
	}
}

func TestWKTFieldMask(t *testing.T) {
	s := loadWKTSchema(t)
	fmType := mustWKTType(t, s, "google.protobuf.FieldMask")

	paths := []string{"foo.bar", "baz", "a.b.c"}
	m := runtime.NewFieldMask(fmType, paths)
	if got := runtime.FieldMaskPaths(m); !reflect.DeepEqual(got, paths) {
		t.Fatalf("FieldMaskPaths = %q, want %q", got, paths)
	}
	wire, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	back, err := fmType.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	if got := runtime.FieldMaskPaths(back); !reflect.DeepEqual(got, paths) {
		t.Fatalf("decoded paths = %q, want %q", got, paths)
	}
	if got := runtime.FieldMaskPaths(fmType.New()); len(got) != 0 {
		t.Fatalf("empty mask paths = %q, want none", got)
	}
}
